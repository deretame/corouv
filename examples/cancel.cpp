#include <corouv/cancel.h>
#include <corouv/runtime.h>
#include <corouv/timer.h>

#include <async_simple/Signal.h>

#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

corouv::Task<void> long_work() {
    for (int i = 1; i <= 10; ++i) {
        co_await corouv::sleep_for(200ms);
        std::cout << "[cancel] tick " << i << "\n";
    }
}

int main() {
    corouv::Runtime rt;
    corouv::CancellationSource source;

    std::jthread canceller([source]() mutable {
        std::this_thread::sleep_for(550ms);
        std::cout << "[cancel] trigger cancellation\n";
        source.cancel();
    });

    try {
        rt.run(corouv::with_cancellation(long_work(), source.token()));
        std::cout << "[cancel] completed without cancellation\n";
    } catch (const async_simple::SignalException&) {
        std::cout << "[cancel] canceled\n";
    }

    return 0;
}
