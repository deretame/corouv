#pragma once

#include <async_simple/Signal.h>
#include <async_simple/Try.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "corouv/cancel.h"
#include "corouv/event.h"
#include "corouv/executor.h"
#include "corouv/task.h"

namespace corouv {

namespace detail {

inline bool is_signal_exception(const std::exception_ptr& ep) {
    if (!ep) {
        return false;
    }
    try {
        std::rethrow_exception(ep);
    } catch (const async_simple::SignalException&) {
        return true;
    } catch (...) {
        return false;
    }
}

template <class T>
inline std::exception_ptr try_to_exception(async_simple::Try<T>&& t) {
    if (!t.hasError()) {
        return nullptr;
    }
    return t.getException();
}

template <>
inline std::exception_ptr try_to_exception<void>(async_simple::Try<void>&& t) {
    if (!t.hasError()) {
        try {
            t.value();
            return nullptr;
        } catch (...) {
            return std::current_exception();
        }
    }
    return t.getException();
}

}  // namespace detail

class TaskGroup {
public:
    explicit TaskGroup(UvExecutor& ex) : _state(std::make_shared<State>(&ex)) {}
    ~TaskGroup() { cancel(); }

    TaskGroup(const TaskGroup&) = default;
    TaskGroup& operator=(const TaskGroup&) = default;
    TaskGroup(TaskGroup&&) noexcept = default;
    TaskGroup& operator=(TaskGroup&&) noexcept = default;

    template <class T>
    bool spawn(Task<T> task) {
        auto state = _state;
        if (!state) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(state->mu);
            if (state->cancel_source.canceled()) {
                return false;
            }
            if (state->pending == 0) {
                state->drained.reset();
            }
            ++state->pending;
        }

        auto child =
            with_cancellation(std::move(task), state->cancel_source.token());

        std::move(child).via(state->executor).start(
            [state](async_simple::Try<T>&& t) mutable {
                state->finish(detail::try_to_exception<T>(std::move(t)));
            });
        return true;
    }

    Task<void> wait() {
        auto state = _state;
        if (!state) {
            co_return;
        }

        co_await state->drained.wait();

        std::exception_ptr ep;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            ep = std::exchange(state->first_error, nullptr);
        }

        if (ep) {
            std::rethrow_exception(ep);
        }
        co_return;
    }

    void cancel() const noexcept {
        auto state = _state;
        if (state) {
            state->cancel_source.cancel();
        }
    }

    [[nodiscard]] CancellationToken token() const {
        return _state->cancel_source.token();
    }

    [[nodiscard]] std::size_t pending() const noexcept {
        auto state = _state;
        if (!state) {
            return 0;
        }
        std::lock_guard<std::mutex> lk(state->mu);
        return state->pending;
    }

    [[nodiscard]] bool empty() const noexcept { return pending() == 0; }

private:
    struct State {
        explicit State(UvExecutor* ex) : executor(ex) {}

        void finish(std::exception_ptr ep) {
            bool should_cancel = false;
            bool signal_drained = false;

            {
                std::lock_guard<std::mutex> lk(mu);

                if (ep && !first_error) {
                    if (!(cancel_source.canceled() &&
                          detail::is_signal_exception(ep))) {
                        first_error = ep;
                        should_cancel = true;
                    }
                }

                if (pending > 0) {
                    --pending;
                }
                if (pending == 0) {
                    signal_drained = true;
                }
            }

            if (should_cancel) {
                cancel_source.cancel();
            }
            if (signal_drained) {
                drained.set();
            }
        }

        UvExecutor* executor = nullptr;
        mutable std::mutex mu;
        CancellationSource cancel_source;
        ManualResetEvent drained{true};
        std::size_t pending = 0;
        std::exception_ptr first_error;
    };

    std::shared_ptr<State> _state;
};

inline Task<TaskGroup> make_task_group() {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* corouv_ex = dynamic_cast<UvExecutor*>(ex);
    if (!corouv_ex) {
        throw std::logic_error(
            "corouv::make_task_group requires CurrentExecutor to be UvExecutor");
    }
    co_return TaskGroup(*corouv_ex);
}

}  // namespace corouv
