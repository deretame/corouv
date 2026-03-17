#pragma once

#include <uv.h>

#include <async_simple/Executor.h>
#include <async_simple/Signal.h>
#include <async_simple/coro/Lazy.h>

#include <atomic>
#include <coroutine>
#include <exception>
#include <optional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "corouv/executor.h"
#include "corouv/uv_error.h"

namespace corouv::blocking {

namespace detail {

template <class R>
struct WorkResult {
    std::optional<R> value;
    std::exception_ptr ep;
};

template <>
struct WorkResult<void> {
    std::exception_ptr ep;
};

template <class F>
class WorkAwaiter {
public:
    using Fn = std::decay_t<F>;
    using R = std::invoke_result_t<Fn&>;

    WorkAwaiter(UvExecutor& ex, Fn fn, async_simple::Slot* slot)
        : _ex(&ex), _loop(ex.loop()), _slot(slot), _fn(std::move(fn)) {}

    bool await_ready() const noexcept {
        return async_simple::signalHelper{async_simple::Terminate}.hasCanceled(
            _slot);
    }

    bool await_suspend(std::coroutine_handle<> h) {
        _op = std::make_shared<Op>();
        _op->ex = _ex;
        _op->loop = _loop;
        _op->slot = _slot;
        _op->fn = std::move(_fn);
        _op->h = h;
        _op->req.data = new std::shared_ptr<Op>(_op);

        if (_slot) {
            const bool ok =
                async_simple::signalHelper{async_simple::Terminate}.tryEmplace(
                    _slot, [op = _op](async_simple::SignalType,
                                       async_simple::Signal*) noexcept {
                        op->cancel_requested.store(true,
                                                   std::memory_order_release);
                        if (auto* ex = op->ex) {
                            (void)ex->schedule([op]() noexcept {
                                op->try_cancel_on_loop();
                            });
                        }
                    });
            if (!ok) {
                // Already canceled before we even started.
                delete static_cast<std::shared_ptr<Op>*>(_op->req.data);
                _op->req.data = nullptr;
                return false;
            }
        }

        const int rc = uv_queue_work(_loop, &_op->req, &Op::on_work,
                                     &Op::on_after_work);
        if (rc != 0) {
            _op->start_rc = rc;
            delete static_cast<std::shared_ptr<Op>*>(_op->req.data);
            _op->req.data = nullptr;
            return false;
        }

        _op->queued.store(true, std::memory_order_release);

        if (_op->cancel_requested.load(std::memory_order_acquire)) {
            _op->try_cancel_on_loop();
        }

        return true;
    }

    R await_resume() {
        auto op = std::move(_op);

        // Clear cancel handler (if any) and throw if canceled.
        async_simple::signalHelper{async_simple::Terminate}.checkHasCanceled(
            _slot, "corouv blocking::run canceled");

        if (!op) {
            throw std::logic_error("corouv blocking::run: missing state");
        }

        // If queueing failed, translate to an exception.
        if (op->start_rc != 0) {
            const int rc = op->start_rc;
            throw_uv_error(rc, "uv_queue_work");
        }

        // Cancellation through uv_cancel results in UV_ECANCELED status.
        if (op->after_status == UV_ECANCELED) {
            throw std::runtime_error("corouv blocking::run canceled (UV_ECANCELED)");
        }

        if constexpr (std::is_void_v<R>) {
            if (op->result.ep) {
                std::rethrow_exception(op->result.ep);
            }
            return;
        } else {
            if (op->result.ep) {
                std::rethrow_exception(op->result.ep);
            }
            return std::move(*op->result.value);
        }
    }

    ~WorkAwaiter() {
        // If the coroutine frame is destroyed while suspended, avoid resuming an
        // invalid continuation.
        if (_op) {
            _op->abandoned.store(true, std::memory_order_release);
            _op->h = std::coroutine_handle<>{};
            if (auto* ex = _op->ex) {
                auto op = _op;
                (void)ex->schedule([op]() noexcept {
                    op->try_cancel_on_loop();
                });
            }
        }
    }

private:
    struct Op {
        UvExecutor* ex = nullptr;
        uv_loop_t* loop = nullptr;
        async_simple::Slot* slot = nullptr;
        Fn fn;
        uv_work_t req{};
        std::coroutine_handle<> h{};

        std::atomic<bool> queued{false};
        std::atomic<bool> cancel_requested{false};
        std::atomic<bool> abandoned{false};

        int start_rc = 0;
        int after_status = 0;
        WorkResult<R> result;

        void try_cancel_on_loop() noexcept {
            if (!queued.load(std::memory_order_acquire)) {
                return;
            }
            (void)uv_cancel(reinterpret_cast<uv_req_t*>(&req));
        }

        static void on_work(uv_work_t* req) noexcept {
            auto* holder = static_cast<std::shared_ptr<Op>*>(req->data);
            auto self = *holder;
            try {
                if constexpr (std::is_void_v<R>) {
                    self->fn();
                } else {
                    self->result.value.emplace(self->fn());
                }
            } catch (...) {
                self->result.ep = std::current_exception();
            }
        }

        static void on_after_work(uv_work_t* req, int status) noexcept {
            auto* holder = static_cast<std::shared_ptr<Op>*>(req->data);
            auto self = *holder;
            delete holder;
            req->data = nullptr;

            self->after_status = status;

            if (self->abandoned.load(std::memory_order_acquire)) {
                return;
            }

            if (!self->h) {
                return;
            }

            try {
                self->h.resume();
            } catch (...) {
                std::terminate();
            }
        }
    };

    UvExecutor* _ex = nullptr;
    uv_loop_t* _loop = nullptr;
    async_simple::Slot* _slot = nullptr;
    Fn _fn;
    std::shared_ptr<Op> _op;
};

}  // namespace detail

// Run a potentially blocking function on libuv's threadpool, resume on the loop
// thread, and return the result.
//
// Cancellation:
// - If the current coroutine has a Slot and receives Terminate, we will call
//   uv_cancel() best-effort. If the work has started already, cancellation may
//   not take effect until completion.
template <class F>
auto run(UvExecutor& ex, F&& fn)
    -> async_simple::coro::Lazy<std::invoke_result_t<std::decay_t<F>&>> {
    using Fn = std::decay_t<F>;
    using R = std::invoke_result_t<Fn&>;
    auto* slot = co_await async_simple::coro::CurrentSlot{};
    if constexpr (std::is_void_v<R>) {
        co_await detail::WorkAwaiter<Fn>(ex, Fn(std::forward<F>(fn)), slot);
        co_return;
    } else {
        co_return co_await detail::WorkAwaiter<Fn>(ex, Fn(std::forward<F>(fn)),
                                                   slot);
    }
}

// Convenience overload: infer UvExecutor from CurrentExecutor.
template <class F>
auto run(F&& fn)
    -> async_simple::coro::Lazy<std::invoke_result_t<std::decay_t<F>&>> {
    using Fn = std::decay_t<F>;
    using R = std::invoke_result_t<Fn&>;
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::blocking::run requires CurrentExecutor to be UvExecutor");
    }
    if constexpr (std::is_void_v<R>) {
        co_await run(*uvex, std::forward<F>(fn));
        co_return;
    } else {
        co_return co_await run(*uvex, std::forward<F>(fn));
    }
}

}  // namespace corouv::blocking
