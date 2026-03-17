#include <corouv/runtime.h>
#include <corouv/timeout.h>

#include <chrono>
#include <stdexcept>

using namespace std::chrono_literals;

corouv::Task<int> timer_task() {
    co_await corouv::sleep_for(20ms);
    co_return 7;
}

int main() {
    corouv::Runtime rt;
    const int v = rt.run(corouv::with_timeout(timer_task(), 1s));
    if (v != 7) {
        throw std::runtime_error("timer_test: unexpected value");
    }
    return 0;
}

