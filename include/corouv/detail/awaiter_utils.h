#pragma once

#include <async_simple/Executor.h>
#include <async_simple/coro/Lazy.h>

#include <coroutine>
#include <exception>
#include <utility>

namespace corouv::detail {

inline async_simple::Executor* current_executor_from_handle(
    std::coroutine_handle<> h) noexcept {
    auto promise =
        std::coroutine_handle<async_simple::coro::detail::LazyPromiseBase>::
            from_address(h.address())
                .promise();
    return promise._executor;
}

inline void resume_handle(async_simple::Executor* executor,
                          std::coroutine_handle<> h) noexcept {
    if (!h) {
        return;
    }

    if (executor) {
        const bool scheduled =
            executor->schedule([h]() mutable noexcept { h.resume(); });
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

}  // namespace corouv::detail
