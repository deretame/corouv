#include <corouv/async_mutex.h>
#include <corouv/async_queue.h>
#include <corouv/async_semaphore.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>
#include <corouv/timer.h>

#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;

corouv::Task<void> produce_numbers(corouv::AsyncQueue<int>* queue, int count,
                                   int workers) {
    for (int i = 1; i <= count; ++i) {
        co_await queue->push(i);
    }
    for (int i = 0; i < workers; ++i) {
        co_await queue->push(-1);  // end marker for each worker
    }
}

corouv::Task<void> worker_loop(int worker_id, corouv::AsyncQueue<int>* queue,
                               corouv::AsyncSemaphore* sem,
                               corouv::AsyncMutex* sum_mutex, int* sum) {
    while (true) {
        const int value = co_await queue->pop();
        if (value < 0) {
            break;
        }

        co_await sem->acquire();  // limit concurrent processing
        co_await corouv::sleep_for(40ms);

        co_await sum_mutex->with_lock([&]() {
            *sum += value;
            std::cout << "[primitives] worker " << worker_id << " value=" << value
                      << " sum=" << *sum << "\n";
        });

        sem->release();
    }
}

corouv::Task<void> demo_primitives() {
    constexpr int kWorkers = 2;

    corouv::AsyncQueue<int> queue(4);
    corouv::AsyncSemaphore sem(2);
    corouv::AsyncMutex sum_mutex;
    int sum = 0;

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(produce_numbers(&queue, 8, kWorkers))) {
        throw std::runtime_error("primitives example: producer spawn failed");
    }
    if (!group.spawn(worker_loop(1, &queue, &sem, &sum_mutex, &sum))) {
        throw std::runtime_error("primitives example: worker 1 spawn failed");
    }
    if (!group.spawn(worker_loop(2, &queue, &sem, &sum_mutex, &sum))) {
        throw std::runtime_error("primitives example: worker 2 spawn failed");
    }

    co_await group.wait();
    std::cout << "[primitives] final sum=" << sum << "\n";
}

int main() {
    corouv::Runtime rt;
    rt.run(demo_primitives());
    return 0;
}
