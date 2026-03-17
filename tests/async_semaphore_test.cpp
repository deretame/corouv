#include <corouv/async_semaphore.h>
#include <corouv/cancel.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>
#include <corouv/timer.h>

#include <async_simple/Signal.h>

#include <chrono>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

corouv::Task<void> semaphore_worker(corouv::AsyncSemaphore* sem, int* active,
                                    int* max_active) {
    co_await sem->acquire();

    ++(*active);
    if (*active > *max_active) {
        *max_active = *active;
    }

    co_await corouv::sleep_for(20ms);

    --(*active);
    sem->release();
}

corouv::Task<void> semaphore_limit_case() {
    corouv::AsyncSemaphore sem(2);
    int active = 0;
    int max_active = 0;

    auto group = co_await corouv::make_task_group();
    for (int i = 0; i < 6; ++i) {
        const bool ok = group.spawn(semaphore_worker(&sem, &active, &max_active));
        if (!ok) {
            throw std::runtime_error("async_semaphore_test: spawn failed");
        }
    }

    co_await group.wait();

    if (max_active > 2) {
        throw std::runtime_error("async_semaphore_test: permit limit violated");
    }
}

corouv::Task<void> semaphore_cancellation_case() {
    corouv::AsyncSemaphore sem(0);
    corouv::CancellationSource source;

    std::jthread canceller([source]() mutable {
        std::this_thread::sleep_for(20ms);
        source.cancel();
    });

    bool canceled = false;
    try {
        co_await corouv::with_cancellation(sem.acquire(), source.token());
    } catch (const async_simple::SignalException&) {
        canceled = true;
    }

    if (!canceled) {
        throw std::runtime_error("async_semaphore_test: expected cancellation");
    }
}

int main() {
    corouv::Runtime rt;
    rt.run(semaphore_limit_case());
    rt.run(semaphore_cancellation_case());
    return 0;
}
