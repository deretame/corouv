#pragma once

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/Sleep.h>

#include <chrono>

namespace corouv {

template <class Rep, class Period>
inline auto sleep_for(std::chrono::duration<Rep, Period> dur)
    -> async_simple::coro::Lazy<void> {
    co_await async_simple::coro::sleep(dur);
}

inline auto yield_now() -> async_simple::coro::Lazy<void> {
    co_await async_simple::coro::Yield{};
}

}  // namespace corouv
