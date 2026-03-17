#include <corouv/cancel.h>
#include <corouv/event.h>
#include <corouv/runtime.h>
#include <corouv/timer.h>

#include <async_simple/Signal.h>

#include <chrono>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

corouv::Task<void> wait_event(corouv::ManualResetEvent* event) {
    co_await event->wait();
}

corouv::Task<void> event_set_reset_case() {
    corouv::ManualResetEvent event(false);

    std::jthread setter1([&event]() {
        std::this_thread::sleep_for(20ms);
        event.set();
    });
    co_await wait_event(&event);

    event.reset();

    std::jthread setter2([&event]() {
        std::this_thread::sleep_for(20ms);
        event.set();
    });
    co_await wait_event(&event);

    if (!event.is_set()) {
        throw std::runtime_error("event_test: expected signaled state");
    }
}

corouv::Task<void> event_cancellation_case() {
    corouv::ManualResetEvent event(false);
    corouv::CancellationSource source;

    std::jthread canceller([source]() mutable {
        std::this_thread::sleep_for(20ms);
        source.cancel();
    });

    bool canceled = false;
    try {
        co_await corouv::with_cancellation(wait_event(&event), source.token());
    } catch (const async_simple::SignalException&) {
        canceled = true;
    }

    if (!canceled) {
        throw std::runtime_error("event_test: expected cancellation");
    }
}

int main() {
    corouv::Runtime rt;
    rt.run(event_set_reset_case());
    rt.run(event_cancellation_case());
    return 0;
}
