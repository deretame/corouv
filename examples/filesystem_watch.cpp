#include <corouv/file.h>
#include <corouv/filesystem.h>
#include <corouv/runtime.h>
#include <corouv/timeout.h>

#include <chrono>
#include <iostream>
#include <string>

using namespace std::chrono_literals;

corouv::Task<void> filesystem_watch_demo(corouv::UvExecutor& ex,
                                         std::string root,
                                         corouv::io::WatchOptions options) {
    const std::string ignored_path = root + "/ignore.txt";
    const std::string file_path = root + "/watch.txt";

    co_await corouv::io::create_directories(ex, root);
    auto watcher = co_await corouv::io::watch(ex, root, options);

    auto ignored = co_await corouv::io::open(
        ex, ignored_path, UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_RDWR, 0644);
    co_await ignored.write_all("ignore\n");
    co_await ignored.close();

    auto file = co_await corouv::io::open(
        ex, file_path, UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_RDWR, 0644);
    co_await file.write_all("watch me\n");
    co_await file.close();

    std::optional<corouv::io::WatchEvent> event;
    if (options.backend == corouv::io::WatchBackend::poll) {
        event = co_await corouv::with_timeout(watcher.next_change(), 2s);
    } else {
        corouv::io::WatchFilter filter;
        filter.filename = "watch.txt";
        event = co_await corouv::with_timeout(watcher.next(std::move(filter)), 2s);
    }
    if (event.has_value()) {
        std::cout << "[filesystem-watch] backend="
                  << (watcher.backend() == corouv::io::WatchBackend::poll ? "poll"
                                                                          : "event")
                  << " path=" << event->path;
        if (event->filename.has_value()) {
            std::cout << " file=" << *event->filename;
        }
        std::cout << " rename=" << event->is_rename()
                  << " change=" << event->is_change() << "\n";
    }

    watcher.close();
    const auto closed = co_await corouv::with_timeout(watcher.next(), 2s);
    std::cout << "[filesystem-watch] closed=" << (!closed.has_value()) << "\n";
    co_await corouv::io::remove_all(ex, root);
}

int main(int argc, char** argv) {
    std::string root =
        argc > 1 ? argv[1] : "/tmp/corouv-filesystem-watch-example";
    corouv::io::WatchOptions options;
    if (argc > 2 && std::string_view(argv[2]) == "poll") {
        options.backend = corouv::io::WatchBackend::poll;
        options.poll_interval_ms = 25;
    }

    corouv::Runtime rt;
    try {
        rt.run(corouv::io::remove_all(rt.executor(), root));
    } catch (...) {
    }
    rt.run(filesystem_watch_demo(rt.executor(), root, options));
    try {
        rt.run(corouv::io::remove_all(rt.executor(), root));
    } catch (...) {
    }
    return 0;
}
