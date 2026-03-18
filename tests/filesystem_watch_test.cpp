#include <corouv/file.h>
#include <corouv/filesystem.h>
#include <corouv/runtime.h>
#include <corouv/timeout.h>

#include <chrono>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

corouv::Task<void> filesystem_watch_case(corouv::UvExecutor& ex,
                                         const std::string& root,
                                         corouv::io::WatchOptions options) {
    const std::string ignored_path = root + "/ignore.txt";
    const std::string file_path = root + "/watch.txt";

    co_await corouv::io::create_directories(ex, root);
    auto watcher = co_await corouv::io::watch(ex, root, options);

    auto ignored = co_await corouv::io::open(
        ex, ignored_path, UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_RDWR, 0644);
    co_await ignored.write_all("ignore");
    co_await ignored.close();

    auto file = co_await corouv::io::open(
        ex, file_path, UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_RDWR, 0644);
    co_await file.write_all("watch-event");
    co_await file.close();

    std::optional<corouv::io::WatchEvent> event;
    if (options.backend == corouv::io::WatchBackend::poll) {
        event = co_await corouv::with_timeout(watcher.next_change(), 2s);
    } else {
        corouv::io::WatchFilter filter;
        filter.filename = "watch.txt";
        event = co_await corouv::with_timeout(watcher.next(std::move(filter)), 2s);
    }
    if (!event.has_value()) {
        throw std::runtime_error("filesystem_watch_test: expected watch event");
    }
    if (!event->ok()) {
        throw std::runtime_error("filesystem_watch_test: watch status error");
    }
    if (!(event->is_change() || event->is_rename())) {
        throw std::runtime_error("filesystem_watch_test: unexpected event mask");
    }
    if (options.backend == corouv::io::WatchBackend::event &&
        event->filename.has_value() && *event->filename != "watch.txt") {
        throw std::runtime_error("filesystem_watch_test: filename mismatch");
    }

    watcher.close();
    if (watcher.is_open()) {
        throw std::runtime_error("filesystem_watch_test: watcher should stop reporting open after close");
    }
    const auto closed = co_await corouv::with_timeout(watcher.next(), 2s);
    if (closed.has_value()) {
        throw std::runtime_error("filesystem_watch_test: expected end-of-stream after close");
    }
    co_await corouv::io::remove_all(ex, root);
}

int main() {
    const std::string event_root = "/tmp/corouv_filesystem_watch_test_event";
    const std::string poll_root = "/tmp/corouv_filesystem_watch_test_poll";

    try {
        corouv::Runtime cleanup_rt;
        cleanup_rt.run(corouv::io::remove_all(cleanup_rt.executor(), event_root));
        cleanup_rt.run(corouv::io::remove_all(cleanup_rt.executor(), poll_root));
    } catch (...) {
    }

    try {
        corouv::Runtime rt;
        rt.run(filesystem_watch_case(rt.executor(), event_root, {}));

        corouv::io::WatchOptions poll_options;
        poll_options.backend = corouv::io::WatchBackend::poll;
        poll_options.poll_interval_ms = 25;
        rt.run(filesystem_watch_case(rt.executor(), poll_root, poll_options));
    } catch (...) {
        try {
            corouv::Runtime cleanup_rt;
            cleanup_rt.run(corouv::io::remove_all(cleanup_rt.executor(), event_root));
            cleanup_rt.run(corouv::io::remove_all(cleanup_rt.executor(), poll_root));
        } catch (...) {
        }
        throw;
    }

    try {
        corouv::Runtime cleanup_rt;
        cleanup_rt.run(corouv::io::remove_all(cleanup_rt.executor(), event_root));
        cleanup_rt.run(corouv::io::remove_all(cleanup_rt.executor(), poll_root));
    } catch (...) {
    }
    return 0;
}
