#include <corouv/loop.h>
#include <corouv/sync_wait.h>

#include <async_simple/coro/Sleep.h>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

async_simple::coro::Lazy<void> demo_timer() {
    std::cout << "[timer] t0\n";
    co_await async_simple::coro::sleep(200ms);
    std::cout << "[timer] t1 (+200ms)\n";
    co_await async_simple::coro::sleep(300ms);
    std::cout << "[timer] t2 (+300ms)\n";
}

int main() {
    corouv::Loop loop;
    corouv::UvExecutor ex(loop.raw());

    corouv::run(ex, demo_timer());

    // Close executor's internal uv_async_t handle, then drain its close callback.
    ex.shutdown();
    loop.run(UV_RUN_DEFAULT);

    return 0;
}
