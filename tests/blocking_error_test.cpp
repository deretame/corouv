#include <corouv/blocking.h>
#include <corouv/runtime.h>

#include <stdexcept>
#include <string_view>

corouv::Task<void> blocking_error_task() {
    bool saw_expected = false;
    try {
        (void)co_await corouv::blocking::run([]() -> int {
            throw std::runtime_error("blocking_boom");
        });
    } catch (const std::runtime_error& e) {
        saw_expected = std::string_view(e.what()) == "blocking_boom";
    }

    if (!saw_expected) {
        throw std::runtime_error("blocking_error_test: expected worker exception");
    }
}

int main() {
    corouv::Runtime rt;
    rt.run(blocking_error_task());
    return 0;
}
