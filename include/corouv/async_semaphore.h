#pragma once

#include <async_simple/Signal.h>
#include <async_simple/coro/Lazy.h>

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "corouv/detail/awaiter_utils.h"
#include "corouv/task.h"

namespace corouv {

class AsyncSemaphore {
public:
    explicit AsyncSemaphore(std::size_t initial_permits = 0)
        : _permits(initial_permits) {}

    AsyncSemaphore(const AsyncSemaphore&) = delete;
    AsyncSemaphore& operator=(const AsyncSemaphore&) = delete;

    [[nodiscard]] bool try_acquire() noexcept {
        std::lock_guard<std::mutex> lk(_mu);
        compact_waiters_locked();
        if (_permits == 0) {
            return false;
        }
        --_permits;
        return true;
    }

    Task<void> acquire() {
        auto* slot = co_await async_simple::coro::CurrentSlot{};
        co_await Awaiter(this, slot);
        co_return;
    }

    void release(std::size_t count = 1) {
        if (count == 0) {
            return;
        }

        std::vector<std::shared_ptr<Waiter>> wakeups;

        {
            std::lock_guard<std::mutex> lk(_mu);
            if (count > std::numeric_limits<std::size_t>::max() - _permits) {
                throw std::overflow_error(
                    "corouv::AsyncSemaphore permit overflow");
            }

            _permits += count;
            compact_waiters_locked();

            while (_permits > 0 && !_waiters.empty()) {
                auto candidate = std::move(_waiters.front());
                _waiters.pop_front();
                if (!candidate) {
                    continue;
                }
                if (!candidate->try_mark_granted()) {
                    continue;
                }
                --_permits;
                wakeups.push_back(std::move(candidate));
            }
        }

        for (auto& waiter : wakeups) {
            waiter->resume_after_marked_done();
        }
    }

    [[nodiscard]] std::size_t available_permits() const noexcept {
        std::lock_guard<std::mutex> lk(_mu);
        return _permits;
    }

private:
    struct Waiter : std::enable_shared_from_this<Waiter> {
        AsyncSemaphore* owner = nullptr;
        async_simple::Executor* executor = nullptr;
        std::coroutine_handle<> continuation{};

        std::atomic<bool> done{false};
        std::atomic<bool> granted{false};
        std::atomic<bool> abandoned{false};
        std::atomic<bool> cancel_requested{false};

        [[nodiscard]] bool try_mark_granted() noexcept {
            bool expected = false;
            if (!done.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                return false;
            }
            granted.store(true, std::memory_order_release);
            return true;
        }

        [[nodiscard]] bool try_mark_canceled() noexcept {
            bool expected = false;
            return done.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire);
        }

        void resume_after_marked_done() noexcept {
            if (abandoned.load(std::memory_order_acquire)) {
                return;
            }
            auto h = std::exchange(continuation, std::coroutine_handle<>{});
            detail::resume_handle(executor, h);
        }

        void cancel_on_executor() noexcept {
            if (!cancel_requested.load(std::memory_order_acquire)) {
                return;
            }
            if (!try_mark_canceled()) {
                return;
            }
            if (owner) {
                owner->remove_waiter(shared_from_this());
            }
            resume_after_marked_done();
        }

        void request_cancel_from_any_thread() noexcept {
            cancel_requested.store(true, std::memory_order_release);
            auto self = shared_from_this();
            if (executor) {
                const bool scheduled =
                    executor->schedule([self]() noexcept { self->cancel_on_executor(); });
                if (scheduled) {
                    return;
                }
            }
            self->cancel_on_executor();
        }
    };

    class Awaiter {
    public:
        Awaiter(AsyncSemaphore* sem, async_simple::Slot* slot) noexcept
            : _sem(sem), _slot(slot) {}

        bool await_ready() const noexcept {
            if (async_simple::signalHelper{async_simple::Terminate}.hasCanceled(
                    _slot)) {
                return true;
            }
            return _sem->try_acquire();
        }

        bool await_suspend(std::coroutine_handle<> h) {
            _waiter = std::make_shared<Waiter>();
            _waiter->owner = _sem;
            _waiter->executor = detail::current_executor_from_handle(h);
            _waiter->continuation = h;

            if (_slot) {
                const bool ok = async_simple::signalHelper{
                                    async_simple::Terminate}
                                    .tryEmplace(
                                        _slot,
                                        [waiter = _waiter](async_simple::SignalType,
                                                           async_simple::Signal*) noexcept {
                                            waiter->request_cancel_from_any_thread();
                                        });
                if (!ok) {
                    return false;
                }
            }

            return _sem->enqueue_waiter(_waiter);
        }

        void await_resume() {
            auto waiter = std::move(_waiter);

            if (waiter && waiter->granted.load(std::memory_order_acquire)) {
                if (_slot) {
                    (void)_slot->clear(async_simple::Terminate);
                }
                return;
            }

            async_simple::signalHelper{async_simple::Terminate}.checkHasCanceled(
                _slot, "corouv async semaphore acquire canceled");

            if (waiter) {
                throw std::runtime_error(
                    "corouv::AsyncSemaphore acquire failed");
            }
        }

        ~Awaiter() {
            if (_waiter) {
                _waiter->abandoned.store(true, std::memory_order_release);
                _waiter->continuation = std::coroutine_handle<>{};
                _waiter->done.store(true, std::memory_order_release);
                _sem->remove_waiter(_waiter);
            }
        }

    private:
        AsyncSemaphore* _sem = nullptr;
        async_simple::Slot* _slot = nullptr;
        std::shared_ptr<Waiter> _waiter;
    };

    bool enqueue_waiter(const std::shared_ptr<Waiter>& waiter) noexcept {
        std::lock_guard<std::mutex> lk(_mu);
        compact_waiters_locked();

        if (waiter->done.load(std::memory_order_acquire)) {
            return false;
        }

        if (_permits > 0) {
            --_permits;
            waiter->granted.store(true, std::memory_order_release);
            waiter->done.store(true, std::memory_order_release);
            return false;
        }

        _waiters.push_back(waiter);
        return true;
    }

    void remove_waiter(const std::shared_ptr<Waiter>& waiter) noexcept {
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
    std::size_t _permits{0};
    std::deque<std::shared_ptr<Waiter>> _waiters;
};

}  // namespace corouv
