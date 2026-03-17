#include <corouv/runtime.h>
#include <corouv/task_group.h>
#include <corouv/timer.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;

corouv::Task<void> add_to_sum(std::atomic<int>* sum, int value) {
    co_await corouv::sleep_for(80ms);
    sum->fetch_add(value, std::memory_order_relaxed);
}

corouv::Task<void> demo_task_group() {
    auto group = co_await corouv::make_task_group();
    std::atomic<int> sum{0};

    for (int i = 1; i <= 4; ++i) {
        const bool ok = group.spawn(add_to_sum(&sum, i));
        if (!ok) {
            throw std::runtime_error("task_group spawn failed");
        }
    }

    co_await group.wait();

    std::cout << "[task_group] sum=" << sum.load(std::memory_order_relaxed)
              << "\n";
}

int main() {
    corouv::Runtime rt;
    rt.run(demo_task_group());
    return 0;
}
