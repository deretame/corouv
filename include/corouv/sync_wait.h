#pragma once

#include <uv.h>

#include <async_simple/Try.h>
#include <async_simple/coro/Lazy.h>

#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "corouv/executor.h"

namespace corouv {

// Runs the uv loop until `task` completes, then returns its value (or rethrows).
//
// This is the simplest "driver" to run async_simple::coro::Lazy on top of a
// libuv loop.
//
// It does not call `UvExecutor::shutdown()`; keep the executor reusable.
template <class T>
T run(UvExecutor& ex, async_simple::coro::Lazy<T> task) {
    std::optional<async_simple::Try<T>> out;

    std::move(task).via(&ex).start([&](async_simple::Try<T>&& t) {
        out.emplace(std::move(t));
        uv_stop(ex.loop());
    });

    uv_run(ex.loop(), UV_RUN_DEFAULT);

    if (!out.has_value()) {
        throw std::logic_error("corouv::run: task never completed");
    }

    if constexpr (std::is_void_v<T>) {
        out->value();
        return;
    } else {
        return std::move(*out).value();
    }
}

}  // namespace corouv
