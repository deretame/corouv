#include <corouv/async_queue.h>
#include <corouv/cancel.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>

#include <async_simple/Signal.h>

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

corouv::Task<void> produce_sequence(corouv::AsyncQueue<int>* queue, int n) {
    for (int i = 0; i < n; ++i) {
        co_await queue->push(i);
    }
    co_await queue->push(-1);  // end marker
}

corouv::Task<void> consume_sequence(corouv::AsyncQueue<int>* queue,
                                    std::vector<int>* out) {
    while (true) {
        const int v = co_await queue->pop();
        if (v < 0) {
            break;
        }
        out->push_back(v);
    }
}

corouv::Task<void> queue_roundtrip_case() {
    corouv::AsyncQueue<int> queue(2);
    std::vector<int> out;

    auto group = co_await corouv::make_task_group();
    const bool produce_ok = group.spawn(produce_sequence(&queue, 20));
    const bool consume_ok = group.spawn(consume_sequence(&queue, &out));

    if (!produce_ok || !consume_ok) {
        throw std::runtime_error("async_queue_test: spawn failed");
    }

    co_await group.wait();

    if (out.size() != 20) {
        throw std::runtime_error("async_queue_test: size mismatch");
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] != static_cast<int>(i)) {
            throw std::runtime_error("async_queue_test: order mismatch");
        }
    }
}

void queue_try_case() {
    corouv::AsyncQueue<int> queue(1);

    if (!queue.try_push(7)) {
        throw std::runtime_error("async_queue_test: try_push(7) failed");
    }
    if (queue.try_push(8)) {
        throw std::runtime_error("async_queue_test: expected full queue");
    }

    auto first = queue.try_pop();
    if (!first || *first != 7) {
        throw std::runtime_error("async_queue_test: try_pop mismatch");
    }

    if (queue.try_pop().has_value()) {
        throw std::runtime_error("async_queue_test: expected empty queue");
    }
}

corouv::Task<void> queue_pop_cancellation_case() {
    corouv::AsyncQueue<int> queue(1);
    corouv::CancellationSource source;

    std::jthread canceller([source]() mutable {
        std::this_thread::sleep_for(20ms);
        source.cancel();
    });

    bool canceled = false;
    try {
        (void)co_await corouv::with_cancellation(queue.pop(), source.token());
    } catch (const async_simple::SignalException&) {
        canceled = true;
    }

    if (!canceled) {
        throw std::runtime_error("async_queue_test: expected cancellation");
    }
}

int main() {
    queue_try_case();

    corouv::Runtime rt;
    rt.run(queue_roundtrip_case());
    rt.run(queue_pop_cancellation_case());
    return 0;
}
