#include <corouv/runtime.h>
#include <corouv/task_group.h>
#include <corouv/timer.h>

#include <async_simple/Signal.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string_view>

using namespace std::chrono_literals;

corouv::Task<void> add_with_delay(std::atomic<int>* sum, int value) {
    co_await corouv::sleep_for(10ms);
    sum->fetch_add(value, std::memory_order_relaxed);
}

corouv::Task<void> wait_or_mark_canceled(std::atomic<bool>* canceled) {
    try {
        co_await corouv::sleep_for(5s);
    } catch (const async_simple::SignalException&) {
        canceled->store(true, std::memory_order_release);
        throw;
    }
}

corouv::Task<void> fail_with_delay() {
    co_await corouv::sleep_for(20ms);
    throw std::runtime_error("task_group_boom");
}

corouv::Task<void> run_group_success(corouv::UvExecutor& ex) {
    corouv::TaskGroup group(ex);
    std::atomic<int> sum{0};

    for (int i = 0; i < 5; ++i) {
        const bool ok = group.spawn(add_with_delay(&sum, i));
        if (!ok) {
            throw std::runtime_error("task_group_test: spawn failed");
        }
    }

    co_await group.wait();

    if (sum.load(std::memory_order_relaxed) != 10) {
        throw std::runtime_error("task_group_test: unexpected aggregate value");
    }
}

corouv::Task<void> run_group_failure(corouv::UvExecutor& ex) {
    corouv::TaskGroup group(ex);
    std::atomic<bool> canceled{false};

    const bool slow_ok = group.spawn(wait_or_mark_canceled(&canceled));
    const bool fail_ok = group.spawn(fail_with_delay());

    if (!slow_ok || !fail_ok) {
        throw std::runtime_error("task_group_test: spawn failed");
    }

    bool saw_error = false;
    try {
        co_await group.wait();
    } catch (const std::runtime_error& e) {
        saw_error = std::string_view(e.what()) == "task_group_boom";
    }

    if (!saw_error) {
        throw std::runtime_error("task_group_test: expected child failure");
    }
    if (!canceled.load(std::memory_order_acquire)) {
        throw std::runtime_error("task_group_test: expected sibling cancellation");
    }
}

corouv::Task<void> run_group_factory() {
    auto group = co_await corouv::make_task_group();
    std::atomic<int> value{0};

    const bool ok = group.spawn(add_with_delay(&value, 2));
    if (!ok) {
        throw std::runtime_error("task_group_test: make_task_group spawn failed");
    }

    co_await group.wait();

    if (value.load(std::memory_order_relaxed) != 2) {
        throw std::runtime_error(
            "task_group_test: make_task_group result mismatch");
    }
}

int main() {
    corouv::Runtime rt;
    rt.run(run_group_success(rt.executor()));
    rt.run(run_group_failure(rt.executor()));
    rt.run(run_group_factory());
    return 0;
}
