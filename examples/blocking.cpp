#include <corouv/corouv.h>
#include <corouv/loop.h>
#include <corouv/sync_wait.h>

#include <async_simple/coro/Lazy.h>

#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

async_simple::coro::Lazy<void> demo_blocking() {
    std::cout << "[blocking] start on loop thread\n";

    const int x = co_await corouv::blocking::run([]() -> int {
        // Simulate a blocking DB connect / query.
        std::this_thread::sleep_for(200ms);
        return 7;
    });

    std::cout << "[blocking] result=" << x << "\n";
}

int main() {
    corouv::Loop loop;
    corouv::UvExecutor ex(loop.raw());

    corouv::run(ex, demo_blocking());

    ex.shutdown();
    loop.run(UV_RUN_DEFAULT);
    return 0;
}
