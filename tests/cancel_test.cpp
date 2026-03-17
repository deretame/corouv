#include <corouv/cancel.h>
#include <corouv/runtime.h>
#include <corouv/timer.h>

#include <async_simple/Signal.h>

#include <chrono>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

corouv::Task<void> long_running_task() {
    co_await corouv::sleep_for(5s);
}

corouv::Task<void> cancel_after_start() {
    corouv::CancellationSource source;
    auto token = source.token();

    std::jthread canceller([source]() mutable {
        std::this_thread::sleep_for(30ms);
        source.cancel();
    });

    bool canceled = false;
    try {
        co_await corouv::with_cancellation(long_running_task(), token);
    } catch (const async_simple::SignalException&) {
        canceled = true;
    }

    if (!canceled) {
        throw std::runtime_error("cancel_test: expected cancellation");
    }
    if (!source.canceled() || !token.canceled()) {
        throw std::runtime_error("cancel_test: token/source state mismatch");
    }
}

corouv::Task<void> cancel_before_start() {
    corouv::CancellationSource source;
    auto token = source.token();
    (void)source.cancel();

    bool canceled = false;
    try {
        co_await corouv::with_cancellation(long_running_task(), token);
    } catch (const async_simple::SignalException&) {
        canceled = true;
    }

    if (!canceled) {
        throw std::runtime_error(
            "cancel_test: expected immediate cancellation");
    }
}

int main() {
    corouv::Runtime rt;
    rt.run(cancel_after_start());
    rt.run(cancel_before_start());
    return 0;
}
