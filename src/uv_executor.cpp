#include "corouv/executor.h"

#include <async_simple/Signal.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "corouv/uv_error.h"

namespace corouv {

struct UvExecutor::State {
    uv_loop_t* loop = nullptr;

    uv_async_t async{};
    std::shared_ptr<State>* holder = nullptr;  // stored in async.data

    std::mutex mu;
    std::deque<Func> q;
    std::atomic<size_t> pending{0};
    std::atomic<bool> closing{false};
    std::thread::id loop_thread;

    bool enqueue(Func f) {
        if (closing.load(std::memory_order_acquire)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            q.push_back(std::move(f));
            pending.fetch_add(1, std::memory_order_relaxed);
        }
        uv_async_send(&async);
        return true;
    }

    static void on_async(uv_async_t* h) {
        auto* holder = static_cast<std::shared_ptr<State>*>(h->data);
        if (!holder) {
            return;
        }

        // Keep the state alive while we drain.
        auto st = *holder;

        std::deque<Func> local;
        {
            std::lock_guard<std::mutex> lk(st->mu);
            local.swap(st->q);
        }

        st->pending.fetch_sub(local.size(), std::memory_order_relaxed);

        for (auto& f : local) {
            try {
                f();
            } catch (...) {
                // Never throw through a C callback.
                std::terminate();
            }
        }
    }

    static void on_async_close(uv_handle_t* h) noexcept {
        auto* holder = static_cast<std::shared_ptr<State>*>(h->data);
        // The holder can be the last shared_ptr reference. Avoid touching `h`
        // after dropping it, since the handle memory lives inside `State`.
        h->data = nullptr;
        delete holder;  // releases the handle-side shared_ptr reference
    }
};

UvExecutor::UvExecutor(uv_loop_t* loop, std::string name)
    : async_simple::Executor(std::move(name)) {
    if (!loop) {
        throw std::invalid_argument("UvExecutor: loop is null");
    }

    _state = std::make_shared<State>();
    _state->loop = loop;
    _state->loop_thread = std::this_thread::get_id();

    _state->holder = new std::shared_ptr<State>(_state);
    _state->async.data = _state->holder;

    const int rc = uv_async_init(loop, &_state->async, &State::on_async);
    if (rc != 0) {
        delete _state->holder;
        _state.reset();
        throw_uv_error(rc, "uv_async_init");
    }
}

UvExecutor::~UvExecutor() {
    // Best-effort: for deterministic cleanup prefer calling shutdown() on the
    // loop thread.
    if (!_state) {
        return;
    }
    if (currentThreadInExecutor()) {
        try {
            shutdown();
        } catch (...) {
        }
    } else {
        _state->closing.store(true, std::memory_order_release);
    }
}

uv_loop_t* UvExecutor::loop() const noexcept {
    return _state ? _state->loop : nullptr;
}

bool UvExecutor::schedule(Func func) {
    auto st = _state;
    if (!st) {
        return false;
    }
    return st->enqueue(std::move(func));
}

bool UvExecutor::currentThreadInExecutor() const {
    auto st = _state;
    if (!st) {
        return false;
    }
    return std::this_thread::get_id() == st->loop_thread;
}

async_simple::ExecutorStat UvExecutor::stat() const {
    async_simple::ExecutorStat s;
    auto st = _state;
    s.pendingTaskCount = st ? st->pending.load(std::memory_order_relaxed) : 0;
    return s;
}

void UvExecutor::shutdown() {
    auto st = _state;
    if (!st) {
        return;
    }
    if (!currentThreadInExecutor()) {
        throw std::logic_error(
            "UvExecutor::shutdown must be called on the loop thread");
    }

    bool expected = false;
    if (!st->closing.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel)) {
        _state.reset();
        return;
    }

    uv_close(reinterpret_cast<uv_handle_t*>(&st->async), &State::on_async_close);

    // Drop our reference. The handle keeps a shared_ptr copy in async.data until
    // the close callback runs.
    _state.reset();
}

void UvExecutor::schedule(Func func, Duration dur, uint64_t /*schedule_info*/,
                          async_simple::Slot* slot) {
    auto st = _state;
    if (!st) {
        return;
    }

    struct TimerOp : std::enable_shared_from_this<TimerOp> {
        std::shared_ptr<State> st;
        uv_timer_t timer{};
        async_simple::Executor::Func func;

        std::atomic<bool> done{false};
        std::atomic<bool> timer_started{false};

        static void on_timeout(uv_timer_t* h) {
            auto* holder = static_cast<std::shared_ptr<TimerOp>*>(h->data);
            auto self = *holder;
            self->complete_on_loop();
        }

        static void on_timer_close(uv_handle_t* h) noexcept {
            auto* holder = static_cast<std::shared_ptr<TimerOp>*>(h->data);
            h->data = nullptr;
            delete holder;
        }

        void start_timer_on_loop(uint64_t timeout_ms) {
            // If cancellation completed before we got a chance to init the
            // timer, simply drop this op here (no uv handles to clean up).
            if (done.load(std::memory_order_acquire)) {
                return;
            }

            timer.data = new std::shared_ptr<TimerOp>(shared_from_this());

            int rc = uv_timer_init(st->loop, &timer);
            if (rc != 0) {
                throw_uv_error(rc, "uv_timer_init");
            }

            rc = uv_timer_start(&timer, &TimerOp::on_timeout, timeout_ms, 0);
            if (rc != 0) {
                throw_uv_error(rc, "uv_timer_start");
            }

            timer_started.store(true, std::memory_order_release);
        }

        void request_cancel_from_any_thread() noexcept {
            // The cancellation handler may run on an arbitrary thread.
            // We enqueue a loop-thread completion to stop/close the timer
            // safely.
            auto self = shared_from_this();
            (void)st->enqueue([self]() { self->complete_on_loop(); });
        }

        void complete_on_loop() noexcept {
            if (done.exchange(true, std::memory_order_acq_rel)) {
                return;
            }

            auto f = std::move(func);

            if (timer_started.load(std::memory_order_acquire)) {
                uv_timer_stop(&timer);
                uv_close(reinterpret_cast<uv_handle_t*>(&timer),
                         &TimerOp::on_timer_close);
            }

            if (f) {
                f();
            }
            // If timer wasn't started yet, `start_timer_on_loop()` will observe
            // `done=true` and simply return without touching libuv.
        }
    };

    auto op = std::make_shared<TimerOp>();
    op->st = st;
    op->func = std::move(func);

    if (slot) {
        const bool ok = async_simple::signalHelper{async_simple::Terminate}
                            .tryEmplace(slot, [op](async_simple::SignalType,
                                                   async_simple::Signal*) {
                                op->request_cancel_from_any_thread();
                            });
        if (!ok) {
            // Already canceled: schedule immediately (same semantics as the
            // default Executor implementation).
            auto f = std::move(op->func);
            (void)st->enqueue(std::move(f));
            return;
        }
    }

    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
    const uint64_t timeout_ms = ms <= 0 ? 0 : static_cast<uint64_t>(ms);

    if (currentThreadInExecutor()) {
        op->start_timer_on_loop(timeout_ms);
    } else {
        const bool enqueued = st->enqueue(
            [op, timeout_ms]() { op->start_timer_on_loop(timeout_ms); });
        if (!enqueued) {
            // Executor is closing. Best-effort: resume immediately.
            op->complete_on_loop();
        }
    }
}

}  // namespace corouv
