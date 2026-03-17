#include <corouv/poll.h>
#include <corouv/runtime.h>
#include <corouv/timeout.h>

#include <chrono>
#include <stdexcept>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;

corouv::Task<void> poll_task(corouv::UvExecutor& ex, int read_fd, int write_fd) {
    std::jthread writer([write_fd]() {
        std::this_thread::sleep_for(30ms);
        const char ch = 'x';
        (void)::write(write_fd, &ch, 1);
    });

    co_await corouv::with_timeout(corouv::poll::readable(
                                      ex, static_cast<uv_os_sock_t>(read_fd)),
                                  1s);

    char ch = 0;
    const ssize_t n = ::read(read_fd, &ch, 1);
    if (n != 1 || ch != 'x') {
        throw std::runtime_error("poll_test: unexpected read result");
    }
}

int main() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        throw std::runtime_error("poll_test: socketpair failed");
    }

    try {
        corouv::Runtime rt;
        rt.run(poll_task(rt.executor(), fds[0], fds[1]));
    } catch (...) {
        ::close(fds[0]);
        ::close(fds[1]);
        throw;
    }

    ::close(fds[0]);
    ::close(fds[1]);
    return 0;
}

