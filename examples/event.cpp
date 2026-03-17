#include <corouv/event.h>
#include <corouv/runtime.h>
#include <corouv/timer.h>

#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

corouv::Task<void> wait_once(corouv::ManualResetEvent* event) {
    std::cout << "[event] waiting...\n";
    co_await event->wait();
    std::cout << "[event] resumed\n";
}

corouv::Task<void> demo_event() {
    corouv::ManualResetEvent event(false);

    std::jthread setter([&event]() {
        std::this_thread::sleep_for(150ms);
        std::cout << "[event] set\n";
        event.set();
    });

    co_await wait_once(&event);
}

int main() {
    corouv::Runtime rt;
    rt.run(demo_event());
    return 0;
}
