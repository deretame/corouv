#include <corouv/async_mutex.h>
#include <corouv/cancel.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>
#include <corouv/timer.h>

#include <async_simple/Signal.h>

#include <chrono>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

corouv::Task<void> increment_with_lock(corouv::AsyncMutex* mutex, int* counter,
                                       int loops) {
    for (int i = 0; i < loops; ++i) {
        auto lock = co_await mutex->scoped_lock();
        const int snapshot = *counter;
        co_await corouv::sleep_for(1ms);
        *counter = snapshot + 1;
    }
}

corouv::Task<void> hold_lock(corouv::AsyncMutex* mutex,
                             std::chrono::milliseconds hold_time) {
    auto lock = co_await mutex->scoped_lock();
    co_await corouv::sleep_for(hold_time);
}

corouv::Task<void> mutex_exclusion_case() {
    corouv::AsyncMutex mutex;
    int counter = 0;

    auto group = co_await corouv::make_task_group();
    for (int i = 0; i < 4; ++i) {
        const bool ok = group.spawn(increment_with_lock(&mutex, &counter, 8));
        if (!ok) {
            throw std::runtime_error("async_mutex_test: spawn failed");
        }
    }
    co_await group.wait();

    if (counter != 32) {
        throw std::runtime_error("async_mutex_test: counter mismatch");
    }
}

corouv::Task<void> mutex_cancellation_case() {
    corouv::AsyncMutex mutex;
    auto group = co_await corouv::make_task_group();

    const bool hold_ok = group.spawn(hold_lock(&mutex, 120ms));
    if (!hold_ok) {
        throw std::runtime_error("async_mutex_test: holder spawn failed");
    }

    co_await corouv::sleep_for(20ms);

    corouv::CancellationSource source;
    std::jthread canceller([source]() mutable {
        std::this_thread::sleep_for(20ms);
        source.cancel();
    });

    bool canceled = false;
    try {
        co_await corouv::with_cancellation(mutex.lock(), source.token());
    } catch (const async_simple::SignalException&) {
        canceled = true;
    }

    if (!canceled) {
        throw std::runtime_error("async_mutex_test: expected cancellation");
    }

    co_await group.wait();

    co_await mutex.lock();
    mutex.unlock();
}

int main() {
    corouv::Runtime rt;
    rt.run(mutex_exclusion_case());
    rt.run(mutex_cancellation_case());
    return 0;
}
