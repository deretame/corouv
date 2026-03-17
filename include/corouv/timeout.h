#pragma once

#include <async_simple/coro/Collect.h>

#include <chrono>
#include <stdexcept>
#include <type_traits>
#include <variant>

#include "corouv/task.h"
#include "corouv/timer.h"

namespace corouv {

class TimeoutError : public std::runtime_error {
public:
    TimeoutError() : std::runtime_error("corouv timeout") {}
};

namespace detail {
struct TimeoutTag {};

template <class Rep, class Period>
Task<TimeoutTag> timeout_after(std::chrono::duration<Rep, Period> dur) {
    co_await corouv::sleep_for(dur);
    co_return TimeoutTag{};
}
}  // namespace detail

template <class T, class Rep, class Period>
Task<T> with_timeout(Task<T> task, std::chrono::duration<Rep, Period> dur) {
    auto timeout_task = detail::timeout_after(dur);

    auto result = co_await async_simple::coro::collectAny<async_simple::Terminate>(
        std::move(task), std::move(timeout_task));

    if (result.index() == 0) {
        auto& t = std::get<0>(result);
        if (t.hasError()) {
            std::rethrow_exception(t.getException());
        }
        if constexpr (std::is_void_v<T>) {
            t.value();
            co_return;
        } else {
            co_return std::move(t).value();
        }
    }

    throw TimeoutError();
}

}  // namespace corouv
