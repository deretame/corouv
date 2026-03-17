#include <corouv/cancel.h>
#include <corouv/runtime.h>
#include <corouv/timeout.h>

#include <async_simple/Signal.h>

#include <chrono>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

corouv::Task<void> slow_task() {
    co_await corouv::sleep_for(300ms);
}

corouv::Task<void> cancellation_task() {
    co_await corouv::sleep_for(5s);
}

corouv::Task<void> run_timeout_case() {
    bool timed_out = false;
    try {
        co_await corouv::with_timeout(slow_task(), 30ms);
    } catch (const corouv::TimeoutError&) {
        timed_out = true;
    }

    if (!timed_out) {
        throw std::runtime_error("timeout_test: expected timeout");
    }
}

corouv::Task<void> run_cancel_case() {
    corouv::CancellationSource source;

    std::jthread canceller([source]() mutable {
        std::this_thread::sleep_for(30ms);
        source.cancel();
    });

    bool canceled = false;
    try {
        co_await corouv::with_cancellation(cancellation_task(), source.token());
    } catch (const async_simple::SignalException&) {
        canceled = true;
    }

    if (!canceled) {
        throw std::runtime_error("timeout_test: expected cancellation");
    }
}

int main() {
    corouv::Runtime rt;
    rt.run(run_timeout_case());
    rt.run(run_cancel_case());
    return 0;
}

