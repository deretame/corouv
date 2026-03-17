#pragma once

#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "corouv/async_semaphore.h"
#include "corouv/task.h"

namespace corouv {

template <class T>
class AsyncQueue {
public:
    static constexpr std::size_t kUnbounded =
        std::numeric_limits<std::size_t>::max();

    explicit AsyncQueue(std::size_t capacity = kUnbounded)
        : _capacity(capacity), _items(0) {
        if (capacity != kUnbounded) {
            _slots.emplace(capacity);
        }
    }

    AsyncQueue(const AsyncQueue&) = delete;
    AsyncQueue& operator=(const AsyncQueue&) = delete;

    Task<void> push(T value) {
        if (_slots) {
            co_await _slots->acquire();
        }

        bool pushed = false;
        try {
            std::lock_guard<std::mutex> lk(_mu);
            _queue.push_back(std::move(value));
            pushed = true;
        } catch (...) {
            if (_slots && !pushed) {
                _slots->release();
            }
            throw;
        }

        _items.release();
        co_return;
    }

    template <class... Args>
    Task<void> emplace(Args&&... args) {
        co_await push(T(std::forward<Args>(args)...));
        co_return;
    }

    Task<T> pop() {
        co_await _items.acquire();

        std::optional<T> out;
        {
            std::lock_guard<std::mutex> lk(_mu);
            if (_queue.empty()) {
                _items.release();
                throw std::logic_error("corouv::AsyncQueue internal underflow");
            }
            out.emplace(std::move(_queue.front()));
            _queue.pop_front();
        }

        if (_slots) {
            _slots->release();
        }

        co_return std::move(*out);
    }

    bool try_push(T value) {
        if (_slots && !_slots->try_acquire()) {
            return false;
        }

        bool pushed = false;
        try {
            std::lock_guard<std::mutex> lk(_mu);
            _queue.push_back(std::move(value));
            pushed = true;
        } catch (...) {
            if (_slots && !pushed) {
                _slots->release();
            }
            throw;
        }

        _items.release();
        return true;
    }

    std::optional<T> try_pop() {
        if (!_items.try_acquire()) {
            return std::nullopt;
        }

        std::optional<T> out;
        {
            std::lock_guard<std::mutex> lk(_mu);
            if (_queue.empty()) {
                _items.release();
                return std::nullopt;
            }
            out.emplace(std::move(_queue.front()));
            _queue.pop_front();
        }

        if (_slots) {
            _slots->release();
        }
        return out;
    }

    [[nodiscard]] bool empty() const noexcept {
        std::lock_guard<std::mutex> lk(_mu);
        return _queue.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard<std::mutex> lk(_mu);
        return _queue.size();
    }

    [[nodiscard]] bool bounded() const noexcept { return _slots.has_value(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return _capacity; }

private:
    const std::size_t _capacity;
    AsyncSemaphore _items;
    std::optional<AsyncSemaphore> _slots;

    mutable std::mutex _mu;
    std::deque<T> _queue;
};

}  // namespace corouv
