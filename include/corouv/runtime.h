#pragma once

#include <async_simple/Try.h>

#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "corouv/executor.h"
#include "corouv/loop.h"
#include "corouv/sync_wait.h"
#include "corouv/task.h"

namespace corouv {

class Runtime final {
public:
    Runtime() : _executor(_loop.raw()) {}
    ~Runtime() { shutdown(); }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    Loop& loop() noexcept { return _loop; }
    const Loop& loop() const noexcept { return _loop; }

    UvExecutor& executor() noexcept { return _executor; }
    const UvExecutor& executor() const noexcept { return _executor; }

    template <class T>
    T run(Task<T> task) {
        ensure_running();
        return corouv::run(_executor, std::move(task));
    }

    template <class T, class F>
    void start(Task<T> task, F&& cb) {
        ensure_running();
        std::move(task).via(&_executor).start(std::forward<F>(cb));
    }

    template <class T>
    void detach(Task<T> task) {
        ensure_running();
        std::move(task).via(&_executor).start([](async_simple::Try<T>&& t) {
            if (t.hasError()) {
                std::rethrow_exception(t.getException());
            }
            if constexpr (!std::is_void_v<T>) {
                (void)t.value();
            } else {
                t.value();
            }
        });
    }

    void post(std::function<void()> fn) {
        ensure_running();
        if (!_executor.schedule(std::move(fn))) {
            throw std::runtime_error("corouv::Runtime::post failed");
        }
    }

    void poll_once() { _loop.run(UV_RUN_NOWAIT); }
    void run_loop() { _loop.run(UV_RUN_DEFAULT); }
    void stop() { _loop.stop(); }

    void shutdown() {
        if (_shutdown) {
            return;
        }
        _shutdown = true;
        _executor.shutdown();
        _loop.run(UV_RUN_DEFAULT);
    }

private:
    void ensure_running() const {
        if (_shutdown) {
            throw std::logic_error("corouv::Runtime is shut down");
        }
    }

    Loop _loop;
    UvExecutor _executor;
    bool _shutdown{false};
};

}  // namespace corouv
