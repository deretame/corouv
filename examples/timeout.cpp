#include <corouv/runtime.h>
#include <corouv/timeout.h>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

corouv::Task<int> slow() {
    co_await corouv::sleep_for(500ms);
    co_return 1;
}

int main() {
    corouv::Runtime rt;

    try {
        const int v = rt.run(corouv::with_timeout(slow(), 100ms));
        std::cout << "[timeout] value=" << v << "\n";
    } catch (const corouv::TimeoutError& e) {
        std::cout << "[timeout] " << e.what() << "\n";
    }

    return 0;
}
