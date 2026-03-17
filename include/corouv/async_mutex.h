#pragma once

#include <async_simple/Signal.h>
#include <async_simple/coro/Lazy.h>

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "corouv/detail/awaiter_utils.h"
#include "corouv/task.h"

namespace corouv {

class AsyncMutex {
public:
    class ScopedLock {
    public:
        ScopedLock() = default;
        explicit ScopedLock(AsyncMutex* mutex) : _mutex(mutex) {}

        ~ScopedLock() { release(); }

        ScopedLock(const ScopedLock&) = delete;
        ScopedLock& operator=(const ScopedLock&) = delete;

        ScopedLock(ScopedLock&& other) noexcept
            : _mutex(std::exchange(other._mutex, nullptr)) {}
        ScopedLock& operator=(ScopedLock&& other) noexcept {
            if (this != &other) {
                release();
                _mutex = std::exchange(other._mutex, nullptr);
            }
            return *this;
        }

        void release() noexcept {
            if (!_mutex) {
                return;
            }
            AsyncMutex* m = std::exchange(_mutex, nullptr);
            try {
                m->unlock();
            } catch (...) {
                std::terminate();
            }
        }

        [[nodiscard]] bool owns_lock() const noexcept { return _mutex != nullptr; }

    private:
        AsyncMutex* _mutex = nullptr;
    };

    AsyncMutex() = default;

    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;

    [[nodiscard]] bool try_lock() noexcept {
        std::lock_guard<std::mutex> lk(_mu);
        compact_waiters_locked();
        if (_locked) {
            return false;
        }
        _locked = true;
        return true;
    }

    Task<void> lock() {
        auto* slot = co_await async_simple::coro::CurrentSlot{};
        co_await Awaiter(this, slot);
        co_return;
    }

    Task<ScopedLock> scoped_lock() {
        co_await lock();
        co_return ScopedLock(this);
    }

    template <class F>
    auto with_lock(F&& fn) -> Task<std::invoke_result_t<F&>> {
        auto guard = co_await scoped_lock();

        if constexpr (std::is_void_v<std::invoke_result_t<F&>>) {
            std::invoke(std::forward<F>(fn));
            co_return;
        } else {
            co_return std::invoke(std::forward<F>(fn));
        }
    }

    void unlock() {
        std::shared_ptr<Waiter> next;

        {
            std::lock_guard<std::mutex> lk(_mu);
            if (!_locked) {
                throw std::logic_error(
                    "corouv::AsyncMutex::unlock on unlocked mutex");
            }

            compact_waiters_locked();
            while (!_waiters.empty()) {
                auto candidate = std::move(_waiters.front());
                _waiters.pop_front();
                if (!candidate) {
                    continue;
                }
                if (!candidate->try_mark_granted()) {
                    continue;
                }
                next = std::move(candidate);
                break;
            }

            if (!next) {
                _locked = false;
                return;
            }
        }

        next->resume_after_marked_done();
    }

    [[nodiscard]] bool locked() const noexcept {
        std::lock_guard<std::mutex> lk(_mu);
        return _locked;
    }

private:
    struct Waiter : std::enable_shared_from_this<Waiter> {
        AsyncMutex* owner = nullptr;
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
        Awaiter(AsyncMutex* mutex, async_simple::Slot* slot) noexcept
            : _mutex(mutex), _slot(slot) {}

        bool await_ready() const noexcept {
            if (async_simple::signalHelper{async_simple::Terminate}.hasCanceled(
                    _slot)) {
                return true;
            }
            return _mutex->try_lock();
        }

        bool await_suspend(std::coroutine_handle<> h) {
            _waiter = std::make_shared<Waiter>();
            _waiter->owner = _mutex;
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

            return _mutex->enqueue_waiter(_waiter);
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
                _slot, "corouv async mutex lock canceled");

            if (waiter) {
                throw std::runtime_error("corouv::AsyncMutex lock failed");
            }
        }

        ~Awaiter() {
            if (_waiter) {
                _waiter->abandoned.store(true, std::memory_order_release);
                _waiter->continuation = std::coroutine_handle<>{};
                _waiter->done.store(true, std::memory_order_release);
                _mutex->remove_waiter(_waiter);
            }
        }

    private:
        AsyncMutex* _mutex = nullptr;
        async_simple::Slot* _slot = nullptr;
        std::shared_ptr<Waiter> _waiter;
    };

    bool enqueue_waiter(const std::shared_ptr<Waiter>& waiter) noexcept {
        std::lock_guard<std::mutex> lk(_mu);
        compact_waiters_locked();

        if (waiter->done.load(std::memory_order_acquire)) {
            return false;
        }

        if (!_locked) {
            _locked = true;
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
    bool _locked{false};
    std::deque<std::shared_ptr<Waiter>> _waiters;
};

}  // namespace corouv
