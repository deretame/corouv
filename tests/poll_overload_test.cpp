#include <corouv/poll.h>
#include <corouv/runtime.h>
#include <corouv/timeout.h>

#include <chrono>
#include <stdexcept>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;

// This test intentionally uses raw POSIX fd operations only as a minimal
// harness for corouv::poll. It is not the recommended application-facing I/O
// style for the library; production code should stay on top of corouv/libuv
// wrappers.
corouv::Task<void> poll_with_current_executor(int read_fd, int write_fd) {
    co_await corouv::with_timeout(
        corouv::poll::writable(static_cast<uv_os_sock_t>(write_fd)), 1s);

    std::jthread writer([write_fd]() {
        std::this_thread::sleep_for(20ms);
        const char ch = 'z';
        (void)::write(write_fd, &ch, 1);
    });

    co_await corouv::with_timeout(
        corouv::poll::readable(static_cast<uv_os_sock_t>(read_fd)), 1s);

    char ch = 0;
    const ssize_t n = ::read(read_fd, &ch, 1);
    if (n != 1 || ch != 'z') {
        throw std::runtime_error("poll_overload_test: unexpected read result");
    }
}

int main() {
    int fds[2] = {-1, -1};
    // socketpair + raw read/write/close are test fixtures here, used only to
    // drive readiness transitions for the poll adapter.
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        throw std::runtime_error("poll_overload_test: socketpair failed");
    }

    try {
        corouv::Runtime rt;
        rt.run(poll_with_current_executor(fds[0], fds[1]));
    } catch (...) {
        ::close(fds[0]);
        ::close(fds[1]);
        throw;
    }

    ::close(fds[0]);
    ::close(fds[1]);
    return 0;
}
