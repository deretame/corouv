#include <corouv/blocking.h>
#include <corouv/runtime.h>

#include <chrono>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

corouv::Task<int> blocking_task() {
    co_return co_await corouv::blocking::run([]() -> int {
        std::this_thread::sleep_for(25ms);
        return 42;
    });
}

int main() {
    corouv::Runtime rt;
    const int v = rt.run(blocking_task());
    if (v != 42) {
        throw std::runtime_error("blocking_test: unexpected value");
    }
    return 0;
}

