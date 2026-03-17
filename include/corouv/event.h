#pragma once

#include <async_simple/Executor.h>
#include <async_simple/Signal.h>
#include <async_simple/coro/Lazy.h>

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "corouv/detail/awaiter_utils.h"
#include "corouv/task.h"

namespace corouv {

class ManualResetEvent;

namespace detail {

struct ManualResetEventOp
    : public std::enable_shared_from_this<ManualResetEventOp> {
    corouv::ManualResetEvent* event = nullptr;
    async_simple::Executor* executor = nullptr;
    std::coroutine_handle<> continuation{};
    std::atomic<bool> done{false};
    std::atomic<bool> abandoned{false};

    void request_resume() noexcept;
};

}  // namespace detail

class ManualResetEvent {
public:
    explicit ManualResetEvent(bool signaled = false) noexcept
        : _signaled(signaled) {}

    void set() noexcept {
        std::vector<std::shared_ptr<detail::ManualResetEventOp>> waiters;
        {
            std::lock_guard<std::mutex> lk(_mu);
            _signaled.store(true, std::memory_order_release);
            compact_waiters_locked();
            waiters.swap(_waiters);
        }

        for (auto& waiter : waiters) {
            waiter->request_resume();
        }
    }

    void reset() noexcept {
        std::lock_guard<std::mutex> lk(_mu);
        _signaled.store(false, std::memory_order_release);
        compact_waiters_locked();
    }

    bool is_set() const noexcept {
        return _signaled.load(std::memory_order_acquire);
    }

    Task<void> wait() {
        auto* slot = co_await async_simple::coro::CurrentSlot{};
        co_await Awaiter(this, slot);
        co_return;
    }

private:
    friend struct detail::ManualResetEventOp;

    class Awaiter {
    public:
        Awaiter(ManualResetEvent* event, async_simple::Slot* slot) noexcept
            : _event(event), _slot(slot) {}

        bool await_ready() const noexcept {
            return _event->is_set() ||
                   async_simple::signalHelper{async_simple::Terminate}
                       .hasCanceled(_slot);
        }

        bool await_suspend(std::coroutine_handle<> h) {
            _op = std::make_shared<detail::ManualResetEventOp>();
            _op->event = _event;
            _op->executor = detail::current_executor_from_handle(h);
            _op->continuation = h;

            if (_slot) {
                const bool ok = async_simple::signalHelper{
                                    async_simple::Terminate}
                                    .tryEmplace(
                                        _slot,
                                        [op = _op](async_simple::SignalType,
                                                   async_simple::Signal*) {
                                            op->request_resume();
                                        });
                if (!ok) {
                    return false;
                }
            }

            return _event->register_waiter(_op);
        }

        void await_resume() {
            async_simple::signalHelper{async_simple::Terminate}
                .checkHasCanceled(_slot, "corouv event canceled");
        }

        ~Awaiter() {
            if (_op) {
                _op->abandoned.store(true, std::memory_order_release);
                _op->continuation = std::coroutine_handle<>{};
                _op->done.store(true, std::memory_order_release);
                _event->remove_waiter(_op);
            }
        }

    private:
        ManualResetEvent* _event = nullptr;
        async_simple::Slot* _slot = nullptr;
        std::shared_ptr<detail::ManualResetEventOp> _op;
    };

    bool register_waiter(
        const std::shared_ptr<detail::ManualResetEventOp>& waiter) noexcept {
        std::lock_guard<std::mutex> lk(_mu);
        compact_waiters_locked();
        if (_signaled.load(std::memory_order_acquire)) {
            return false;
        }
        _waiters.push_back(waiter);
        return true;
    }

    void remove_waiter(
        const std::shared_ptr<detail::ManualResetEventOp>& waiter) noexcept {
        std::lock_guard<std::mutex> lk(_mu);
        std::erase_if(_waiters, [&](const auto& item) {
            return !item || item == waiter ||
                   item->done.load(std::memory_order_acquire);
        });
    }

    void compact_waiters_locked() noexcept {
        std::erase_if(_waiters, [](const auto& item) {
            return !item || item->done.load(std::memory_order_acquire);
        });
    }

    mutable std::mutex _mu;
    std::atomic<bool> _signaled{false};
    std::vector<std::shared_ptr<detail::ManualResetEventOp>> _waiters;
};

inline void detail::ManualResetEventOp::request_resume() noexcept {
    if (done.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (event) {
        event->remove_waiter(shared_from_this());
    }

    if (abandoned.load(std::memory_order_acquire)) {
        return;
    }

    auto h = std::exchange(continuation, std::coroutine_handle<>{});
    if (!h) {
        return;
    }

    if (executor) {
        const bool scheduled = executor->schedule([h]() mutable { h.resume(); });
        if (scheduled) {
            return;
        }
    }

    try {
        h.resume();
    } catch (...) {
        std::terminate();
    }
}

}  // namespace corouv
