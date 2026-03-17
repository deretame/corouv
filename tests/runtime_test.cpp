#include <corouv/runtime.h>
#include <corouv/timer.h>

#include <async_simple/Try.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>

using namespace std::chrono_literals;

corouv::Task<int> compute_value() {
    co_await corouv::sleep_for(10ms);
    co_return 3;
}

corouv::Task<void> detached_worker(corouv::Runtime* rt, std::atomic<bool>* done) {
    co_await corouv::sleep_for(10ms);
    done->store(true, std::memory_order_release);
    rt->stop();
}

int main() {
    corouv::Runtime rt;

    std::atomic<int> post_hits{0};
    rt.post([&]() {
        post_hits.fetch_add(1, std::memory_order_relaxed);
        rt.stop();
    });
    rt.run_loop();
    if (post_hits.load(std::memory_order_relaxed) != 1) {
        throw std::runtime_error("runtime_test: post() did not run");
    }

    std::optional<int> start_value;
    rt.start(compute_value(), [&](async_simple::Try<int>&& t) {
        start_value.emplace(t.value());
        rt.stop();
    });
    rt.run_loop();
    if (!start_value.has_value() || *start_value != 3) {
        throw std::runtime_error("runtime_test: start() callback mismatch");
    }

    std::atomic<bool> detached_done{false};
    rt.detach(detached_worker(&rt, &detached_done));
    rt.run_loop();
    if (!detached_done.load(std::memory_order_acquire)) {
        throw std::runtime_error("runtime_test: detach() did not complete");
    }

    return 0;
}
