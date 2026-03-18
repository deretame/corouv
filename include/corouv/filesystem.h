#pragma once

#include <uv.h>

#include <async_simple/Executor.h>

#include <atomic>
#include <coroutine>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <utility>
#include <vector>

#include "corouv/executor.h"
#include "corouv/async_queue.h"
#include "corouv/fs.h"
#include "corouv/task.h"

namespace corouv::io {

namespace detail {

inline bool is_path_separator(char ch) noexcept {
    return ch == '/' || ch == '\\';
}

struct NormalizedPath {
    std::string prefix;
    bool absolute{false};
    std::vector<std::string> parts;
};

inline NormalizedPath normalize_path(std::string_view path) {
    NormalizedPath out;
    std::size_t i = 0;

    if (path.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(path[0])) != 0 &&
        path[1] == ':') {
        out.prefix.assign(path.substr(0, 2));
        i = 2;
    }

    while (i < path.size() && is_path_separator(path[i])) {
        out.absolute = true;
        ++i;
    }

    while (i <= path.size()) {
        const std::size_t start = i;
        while (i < path.size() && !is_path_separator(path[i])) {
            ++i;
        }

        const auto part = path.substr(start, i - start);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (!out.parts.empty() && out.parts.back() != "..") {
                    out.parts.pop_back();
                } else if (!out.absolute) {
                    out.parts.emplace_back(part);
                }
            } else {
                out.parts.emplace_back(part);
            }
        }

        while (i < path.size() && is_path_separator(path[i])) {
            ++i;
        }
        if (i >= path.size()) {
            break;
        }
    }

    return out;
}

inline std::string normalized_to_string(const NormalizedPath& path) {
    std::string out = path.prefix;
    if (path.absolute) {
        out.push_back('/');
    }

    bool first = true;
    for (const auto& part : path.parts) {
        if (!first && !out.empty() && out.back() != '/') {
            out.push_back('/');
        }
        out.append(part);
        first = false;
    }

    if (out.empty()) {
        return path.absolute ? "/" : ".";
    }
    return out;
}

inline std::string join_path(std::string_view base, std::string_view name) {
    if (base.empty() || base == ".") {
        return std::string(name);
    }
    std::string out(base);
    if (!out.empty() && out.back() != '/') {
        out.push_back('/');
    }
    out.append(name);
    return out;
}

template <class Fn>
class LoopCallAwaiter {
public:
    LoopCallAwaiter(UvExecutor* ex, Fn fn) : _ex(ex), _fn(std::move(fn)) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
        if (_ex == nullptr) {
            _rc = UV_EBADF;
            return false;
        }

        if (_ex->currentThreadInExecutor()) {
            _rc = _fn();
            return false;
        }

        const bool scheduled = _ex->schedule([this, h]() mutable {
            _rc = _fn();
            h.resume();
        });
        if (!scheduled) {
            _rc = UV_ECANCELED;
            return false;
        }
        return true;
    }

    int await_resume() const noexcept { return _rc; }

private:
    UvExecutor* _ex = nullptr;
    Fn _fn;
    int _rc = 0;
};

}  // namespace detail

struct DirectoryEntry {
    std::string name;
    uv_dirent_type_t type{UV_DIRENT_UNKNOWN};

    [[nodiscard]] bool is_file() const noexcept {
        return type == UV_DIRENT_FILE;
    }

    [[nodiscard]] bool is_directory() const noexcept {
        return type == UV_DIRENT_DIR;
    }

    [[nodiscard]] bool is_symlink() const noexcept {
        return type == UV_DIRENT_LINK;
    }
};

inline DirectoryEntry to_directory_entry(fs::DirEntry entry) {
    return DirectoryEntry{std::move(entry.name), entry.type};
}

struct WatchEvent {
    std::string path;
    std::optional<std::string> filename;
    unsigned events{0};
    int status{0};

    [[nodiscard]] bool ok() const noexcept { return status == 0; }
    [[nodiscard]] bool is_rename() const noexcept {
        return (events & UV_RENAME) != 0;
    }
    [[nodiscard]] bool is_change() const noexcept {
        return (events & UV_CHANGE) != 0;
    }
};

enum class WatchBackend {
    event,
    poll,
};

struct WatchOptions {
    bool watch_entry{false};
    bool stat_fallback{false};
    bool recursive{false};
    WatchBackend backend{WatchBackend::event};
    unsigned poll_interval_ms{250};
};

struct WatchFilter {
    std::optional<std::string> filename;
    bool rename{true};
    bool change{true};
    bool errors{true};
};

inline bool is_directory(const uv_stat_t& st) noexcept {
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

inline bool is_regular_file(const uv_stat_t& st) noexcept {
    return (st.st_mode & S_IFMT) == S_IFREG;
}

inline unsigned watch_flags(const WatchOptions& options) noexcept {
    unsigned flags = 0;
    if (options.watch_entry) {
        flags |= UV_FS_EVENT_WATCH_ENTRY;
    }
    if (options.stat_fallback) {
        flags |= UV_FS_EVENT_STAT;
    }
    if (options.recursive) {
        flags |= UV_FS_EVENT_RECURSIVE;
    }
    return flags;
}

inline bool matches(const WatchEvent& event, const WatchFilter& filter) {
    if (filter.filename.has_value()) {
        if (!event.filename.has_value() || *event.filename != *filter.filename) {
            return false;
        }
    }

    if (event.status != 0) {
        return filter.errors;
    }

    const bool matched_rename = filter.rename && event.is_rename();
    const bool matched_change = filter.change && event.is_change();
    return matched_rename || matched_change;
}

class Directory {
public:
    Directory() = default;
    ~Directory() = default;

    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
    Directory(Directory&&) noexcept = default;
    Directory& operator=(Directory&&) noexcept = default;

    [[nodiscard]] bool is_open() const noexcept {
        auto state = _state;
        if (!state) {
            return false;
        }
        std::lock_guard<std::mutex> lk(state->mu);
        return state->is_open_unlocked();
    }

    [[nodiscard]] const std::string& path() const noexcept {
        static const std::string empty;
        auto state = _state;
        return state ? state->path : empty;
    }

    Task<std::optional<DirectoryEntry>> read_one() {
        auto batch = co_await read_some(1);
        if (batch.empty()) {
            co_return std::nullopt;
        }
        co_return std::move(batch.front());
    }

    Task<std::vector<DirectoryEntry>> read_some(std::size_t max_entries = 16) {
        auto state = require_state("read_some");
        auto lease = state->acquire("read_some");

        try {
            auto entries = co_await fs::Readdir(lease.loop, lease.dir, max_entries);
            state->release();

            std::vector<DirectoryEntry> out;
            out.reserve(entries.size());
            for (auto& entry : entries) {
                out.push_back(to_directory_entry(std::move(entry)));
            }
            co_return out;
        } catch (...) {
            state->release();
            throw;
        }
    }

    Task<std::vector<DirectoryEntry>> read_all(std::size_t batch_size = 32) {
        std::vector<DirectoryEntry> out;
        while (true) {
            auto batch = co_await read_some(batch_size);
            if (batch.empty()) {
                break;
            }
            for (auto& entry : batch) {
                out.push_back(std::move(entry));
            }
        }
        co_return out;
    }

    Task<void> close() {
        auto state = _state;
        if (!state) {
            co_return;
        }

        uv_loop_t* loop = nullptr;
        uv_dir_t* dir = nullptr;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            if (!state->is_open_unlocked()) {
                co_return;
            }
            if (state->closing) {
                throw std::logic_error(
                    "corouv::io::Directory::close already in progress");
            }
            if (state->read_inflight) {
                throw std::logic_error(
                    "corouv::io::Directory::close requires no read in flight");
            }
            state->closing = true;
            loop = state->loop;
            dir = state->dir;
        }

        try {
            co_await fs::Closedir(loop, dir);
        } catch (...) {
            std::lock_guard<std::mutex> lk(state->mu);
            state->closing = false;
            throw;
        }

        std::lock_guard<std::mutex> lk(state->mu);
        state->dir = nullptr;
        state->closing = false;
    }

private:
    struct Lease {
        uv_loop_t* loop = nullptr;
        uv_dir_t* dir = nullptr;
    };

    struct State {
        State(uv_loop_t* loop, uv_dir_t* dir, std::string path)
            : loop(loop), dir(dir), path(std::move(path)) {}

        ~State() { close_sync(); }

        [[nodiscard]] bool is_open_unlocked() const noexcept {
            return dir != nullptr && !closing;
        }

        Lease acquire(const char* op) {
            std::lock_guard<std::mutex> lk(mu);
            if (!is_open_unlocked()) {
                throw std::logic_error(std::string("corouv::io::Directory cannot ") +
                                       op + " on a closed directory");
            }
            if (read_inflight) {
                throw std::logic_error(
                    std::string("corouv::io::Directory does not support concurrent ") +
                    op + " operations");
            }
            read_inflight = true;
            return Lease{loop, dir};
        }

        void release() noexcept {
            std::lock_guard<std::mutex> lk(mu);
            read_inflight = false;
        }

        void close_sync() noexcept {
            uv_dir_t* current_dir = nullptr;
            uv_loop_t* current_loop = nullptr;

            {
                std::lock_guard<std::mutex> lk(mu);
                if (dir == nullptr) {
                    return;
                }
                current_dir = dir;
                current_loop = loop;
                dir = nullptr;
                read_inflight = false;
                closing = false;
            }

            if (!current_loop || !current_dir) {
                return;
            }

            uv_fs_t req{};
            (void)uv_fs_closedir(current_loop, &req, current_dir, nullptr);
            uv_fs_req_cleanup(&req);
        }

        mutable std::mutex mu;
        uv_loop_t* loop = nullptr;
        uv_dir_t* dir = nullptr;
        std::string path;
        bool read_inflight = false;
        bool closing = false;
    };

    explicit Directory(std::shared_ptr<State> state) : _state(std::move(state)) {}

    std::shared_ptr<State> require_state(const char* op) const {
        auto state = _state;
        if (!state) {
            throw std::logic_error(std::string("corouv::io::Directory cannot ") +
                                   op + " on an empty directory");
        }
        return state;
    }

    std::shared_ptr<State> _state;

    friend Task<Directory> open_directory(UvExecutor&, std::string);
    friend Task<Directory> open_directory(std::string);
};

class FileWatcher {
public:
    FileWatcher() = default;
    ~FileWatcher() { close(); }

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;
    FileWatcher(FileWatcher&&) noexcept = default;
    FileWatcher& operator=(FileWatcher&&) noexcept = default;

    [[nodiscard]] bool is_open() const noexcept {
        auto state = _state;
        return state != nullptr && state->is_open();
    }

    [[nodiscard]] bool is_closed() const noexcept {
        auto state = _state;
        return state == nullptr || state->closed.load(std::memory_order_acquire);
    }

    [[nodiscard]] WatchBackend backend() const noexcept {
        auto state = _state;
        return state ? state->backend : WatchBackend::event;
    }

    [[nodiscard]] const std::string& path() const noexcept {
        static const std::string empty;
        auto state = _state;
        return state ? state->path : empty;
    }

    Task<std::optional<WatchEvent>> next() {
        auto state = require_state("next");

        if (auto item = state->queue.try_pop(); item.has_value()) {
            co_return std::move(*item);
        }
        if (!state->is_open() && state->closed.load(std::memory_order_acquire)) {
            co_return std::nullopt;
        }

        co_return co_await state->queue.pop();
    }

    Task<std::optional<WatchEvent>> next(WatchFilter filter) {
        for (;;) {
            auto event = co_await next();
            if (!event.has_value()) {
                co_return std::nullopt;
            }
            if (matches(*event, filter)) {
                co_return event;
            }
        }
    }

    Task<std::optional<WatchEvent>> next_change(
        std::optional<std::string> filename = std::nullopt) {
        WatchFilter filter;
        filter.filename = std::move(filename);
        filter.rename = false;
        filter.change = true;
        filter.errors = false;
        co_return co_await next(std::move(filter));
    }

    Task<std::optional<WatchEvent>> next_rename(
        std::optional<std::string> filename = std::nullopt) {
        WatchFilter filter;
        filter.filename = std::move(filename);
        filter.rename = true;
        filter.change = false;
        filter.errors = false;
        co_return co_await next(std::move(filter));
    }

    std::optional<WatchEvent> try_next() {
        auto state = require_state("try_next");
        auto item = state->queue.try_pop();
        if (!item.has_value()) {
            return std::nullopt;
        }
        return std::move(*item);
    }

    void close() noexcept {
        auto state = _state;
        if (!state) {
            return;
        }
        state->request_close();
    }

private:
    struct State : std::enable_shared_from_this<State> {
        State(UvExecutor* ex, std::string path, WatchOptions options)
            : ex(ex), path(std::move(path)), backend(options.backend),
              poll_interval_ms(options.poll_interval_ms) {}

        ~State() {
            if (!closed.load(std::memory_order_acquire)) {
                request_close();
            }
        }

        [[nodiscard]] bool is_open() const noexcept {
            if (!initialized.load(std::memory_order_acquire)) {
                return false;
            }
            return started.load(std::memory_order_acquire) &&
                   !closing.load(std::memory_order_acquire) &&
                   !closed.load(std::memory_order_acquire);
        }

        void request_close() noexcept {
            bool expected = false;
            if (!closing.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel)) {
                return;
            }

            while (queue.try_pop().has_value()) {
            }
            try {
                (void)queue.try_push(std::nullopt);
            } catch (...) {
            }

            if (!initialized.load(std::memory_order_acquire)) {
                closed.store(true, std::memory_order_release);
                return;
            }

            auto self = shared_from_this();
            auto close_on_loop = [self]() noexcept {
                if (self->closed.load(std::memory_order_acquire)) {
                    return;
                }
                if (self->started.load(std::memory_order_acquire)) {
                    if (self->backend == WatchBackend::event) {
                        (void)uv_fs_event_stop(&self->event_handle);
                    } else {
                        (void)uv_fs_poll_stop(&self->poll_handle);
                    }
                }
                auto* handle = self->raw_handle();
                if (!uv_is_closing(handle)) {
                    uv_close(handle, &State::on_close);
                }
            };

            if (ex && ex->currentThreadInExecutor()) {
                close_on_loop();
                return;
            }

            if (!ex || !ex->schedule(std::move(close_on_loop))) {
                closed.store(true, std::memory_order_release);
            }
        }

        static void on_event(uv_fs_event_t* handle, const char* filename,
                             int events, int status) {
            auto* holder =
                static_cast<std::shared_ptr<State>*>(handle->data);
            if (!holder) {
                return;
            }

            auto self = *holder;
            if (self->closed.load(std::memory_order_acquire) ||
                self->closing.load(std::memory_order_acquire)) {
                return;
            }

            WatchEvent event;
            event.path = self->path;
            if (filename) {
                event.filename = std::string(filename);
            }
            event.events = static_cast<unsigned>(events);
            event.status = status;
            try {
                (void)self->queue.try_push(std::move(event));
            } catch (...) {
            }
        }

        static bool same_timespec(const uv_timespec_t& a,
                                  const uv_timespec_t& b) noexcept {
            return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
        }

        static unsigned classify_poll_events(const uv_stat_t& prev,
                                             const uv_stat_t& curr) noexcept {
            unsigned events = 0;

            const bool identity_changed =
                prev.st_dev != curr.st_dev || prev.st_ino != curr.st_ino ||
                ((prev.st_mode & S_IFMT) != (curr.st_mode & S_IFMT));
            if (identity_changed) {
                events |= UV_RENAME;
            }

            const bool content_changed =
                prev.st_size != curr.st_size || prev.st_mode != curr.st_mode ||
                prev.st_nlink != curr.st_nlink ||
                !same_timespec(prev.st_mtim, curr.st_mtim) ||
                !same_timespec(prev.st_ctim, curr.st_ctim) ||
                !same_timespec(prev.st_birthtim, curr.st_birthtim);
            if (content_changed) {
                events |= UV_CHANGE;
            }

            if (events == 0) {
                events = UV_CHANGE;
            }
            return events;
        }

        static void on_poll(uv_fs_poll_t* handle, int status,
                            const uv_stat_t* prev, const uv_stat_t* curr) {
            auto* holder = static_cast<std::shared_ptr<State>*>(handle->data);
            if (!holder) {
                return;
            }

            auto self = *holder;
            if (self->closed.load(std::memory_order_acquire) ||
                self->closing.load(std::memory_order_acquire)) {
                return;
            }

            WatchEvent event;
            event.path = self->path;
            event.status = status;
            if (status == 0 && prev != nullptr && curr != nullptr) {
                event.events = classify_poll_events(*prev, *curr);
            }

            try {
                (void)self->queue.try_push(std::move(event));
            } catch (...) {
            }
        }

        static void on_close(uv_handle_t* handle) noexcept {
            auto* holder =
                static_cast<std::shared_ptr<State>*>(handle->data);
            if (!holder) {
                return;
            }

            auto self = *holder;
            handle->data = nullptr;
            self->closed.store(true, std::memory_order_release);
            try {
                (void)self->queue.try_push(std::nullopt);
            } catch (...) {
            }
            delete holder;
        }

        uv_handle_t* raw_handle() noexcept {
            if (backend == WatchBackend::event) {
                return reinterpret_cast<uv_handle_t*>(&event_handle);
            }
            return reinterpret_cast<uv_handle_t*>(&poll_handle);
        }

        UvExecutor* ex = nullptr;
        uv_fs_event_t event_handle{};
        uv_fs_poll_t poll_handle{};
        std::shared_ptr<State>* holder = nullptr;
        std::string path;
        WatchBackend backend{WatchBackend::event};
        unsigned poll_interval_ms{250};
        AsyncQueue<std::optional<WatchEvent>> queue;
        std::atomic<bool> initialized{false};
        std::atomic<bool> started{false};
        std::atomic<bool> closing{false};
        std::atomic<bool> closed{false};
    };

    explicit FileWatcher(std::shared_ptr<State> state)
        : _state(std::move(state)) {}

    std::shared_ptr<State> require_state(const char* op) const {
        auto state = _state;
        if (!state) {
            throw std::logic_error(std::string("corouv::io::FileWatcher cannot ") +
                                   op + " on an empty watcher");
        }
        return state;
    }

    std::shared_ptr<State> _state;

    friend Task<FileWatcher> watch(UvExecutor&, std::string, WatchOptions);
    friend Task<FileWatcher> watch(std::string, WatchOptions);
};

Task<Directory> open_directory(UvExecutor& ex, std::string path);
Task<Directory> open_directory(std::string path);
Task<std::vector<DirectoryEntry>> read_directory(UvExecutor& ex, std::string path,
                                                 std::size_t batch_size);
Task<std::vector<DirectoryEntry>> read_directory(std::string path,
                                                 std::size_t batch_size);
Task<FileWatcher> watch(UvExecutor& ex, std::string path,
                        WatchOptions options = {});
Task<FileWatcher> watch(std::string path, WatchOptions options = {});

inline Task<uv_stat_t> stat(UvExecutor& ex, std::string path) {
    co_return co_await fs::Stat(ex.loop(), std::move(path));
}

inline Task<uv_stat_t> stat(std::string path) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::stat requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await stat(*uvex, std::move(path));
}

inline Task<bool> exists(UvExecutor& ex, std::string path) {
    try {
        co_await fs::Access(ex.loop(), std::move(path), 0);
        co_return true;
    } catch (...) {
        co_return false;
    }
}

inline Task<bool> exists(std::string path) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::exists requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await exists(*uvex, std::move(path));
}

inline Task<void> mkdir(UvExecutor& ex, std::string path, int mode = 0755) {
    co_await fs::Mkdir(ex.loop(), std::move(path), mode);
}

inline Task<void> mkdir(std::string path, int mode = 0755) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::mkdir requires CurrentExecutor to be UvExecutor");
    }
    co_await mkdir(*uvex, std::move(path), mode);
}

inline Task<void> create_directories(UvExecutor& ex, std::string path,
                                     int mode = 0755) {
    const auto normalized = detail::normalize_path(path);
    if (normalized.parts.empty()) {
        co_return;
    }

    std::string current = normalized.prefix;
    if (normalized.absolute) {
        current.push_back('/');
    }

    bool first = true;
    for (const auto& part : normalized.parts) {
        if (!first && !current.empty() && current.back() != '/') {
            current.push_back('/');
        }
        current.append(part);
        first = false;

        if (co_await exists(ex, current)) {
            continue;
        }

        bool created = false;
        std::exception_ptr failure;
        try {
            co_await mkdir(ex, current, mode);
            created = true;
        } catch (...) {
            failure = std::current_exception();
        }

        if (!created && !(co_await exists(ex, current))) {
            std::rethrow_exception(failure);
        }
    }
}

inline Task<void> create_directories(std::string path, int mode = 0755) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::create_directories requires CurrentExecutor to be UvExecutor");
    }
    co_await create_directories(*uvex, std::move(path), mode);
}

inline Task<void> remove_file(UvExecutor& ex, std::string path) {
    co_await fs::Unlink(ex.loop(), std::move(path));
}

inline Task<void> remove_file(std::string path) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::remove_file requires CurrentExecutor to be UvExecutor");
    }
    co_await remove_file(*uvex, std::move(path));
}

inline Task<void> remove_directory(UvExecutor& ex, std::string path) {
    co_await fs::Rmdir(ex.loop(), std::move(path));
}

inline Task<void> remove_directory(std::string path) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::remove_directory requires CurrentExecutor to be UvExecutor");
    }
    co_await remove_directory(*uvex, std::move(path));
}

inline Task<void> rename(UvExecutor& ex, std::string from, std::string to) {
    co_await fs::Rename(ex.loop(), std::move(from), std::move(to));
}

inline Task<void> rename(std::string from, std::string to) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::rename requires CurrentExecutor to be UvExecutor");
    }
    co_await rename(*uvex, std::move(from), std::move(to));
}

inline Task<void> copy_file(UvExecutor& ex, std::string from, std::string to,
                            int flags = 0) {
    co_await fs::Copyfile(ex.loop(), std::move(from), std::move(to), flags);
}

inline Task<void> copy_file(std::string from, std::string to, int flags = 0) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::copy_file requires CurrentExecutor to be UvExecutor");
    }
    co_await copy_file(*uvex, std::move(from), std::move(to), flags);
}

inline Task<std::string> readlink(UvExecutor& ex, std::string path) {
    co_return co_await fs::Readlink(ex.loop(), std::move(path));
}

inline Task<std::string> readlink(std::string path) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::readlink requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await readlink(*uvex, std::move(path));
}

inline Task<std::string> realpath(UvExecutor& ex, std::string path) {
    co_return co_await fs::Realpath(ex.loop(), std::move(path));
}

inline Task<std::string> realpath(std::string path) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::realpath requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await realpath(*uvex, std::move(path));
}

inline Task<void> symlink(UvExecutor& ex, std::string target,
                          std::string link_path, int flags = 0) {
    co_await fs::Symlink(ex.loop(), std::move(target), std::move(link_path),
                         flags);
}

inline Task<void> symlink(std::string target, std::string link_path,
                          int flags = 0) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::symlink requires CurrentExecutor to be UvExecutor");
    }
    co_await symlink(*uvex, std::move(target), std::move(link_path), flags);
}

inline Task<void> remove_all(UvExecutor& ex, std::string path) {
    if (!(co_await exists(ex, path))) {
        co_return;
    }

    const auto st = co_await stat(ex, path);
    if (!is_directory(st)) {
        co_await remove_file(ex, std::move(path));
        co_return;
    }

    auto dir = co_await open_directory(ex, path);
    auto entries = co_await dir.read_all();
    co_await dir.close();
    for (auto& entry : entries) {
        co_await remove_all(ex, detail::join_path(path, entry.name));
    }
    co_await remove_directory(ex, std::move(path));
}

inline Task<void> remove_all(std::string path) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::remove_all requires CurrentExecutor to be UvExecutor");
    }
    co_await remove_all(*uvex, std::move(path));
}

inline Task<Directory> open_directory(UvExecutor& ex, std::string path) {
    auto* dir = co_await fs::Opendir(ex.loop(), path);
    co_return Directory(
        std::make_shared<Directory::State>(ex.loop(), dir, std::move(path)));
}

inline Task<Directory> open_directory(std::string path) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::open_directory requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await open_directory(*uvex, std::move(path));
}

inline Task<FileWatcher> watch(UvExecutor& ex, std::string path,
                               WatchOptions options) {
    auto state =
        std::make_shared<FileWatcher::State>(&ex, std::move(path), options);
    const auto flags = watch_flags(options);

    const int rc = co_await detail::LoopCallAwaiter(
        &ex, [state, flags]() {
            if (state->backend == WatchBackend::event) {
                const int init_rc =
                    uv_fs_event_init(state->ex->loop(), &state->event_handle);
                if (init_rc != 0) {
                    return init_rc;
                }
                state->initialized.store(true, std::memory_order_release);

                state->holder = new std::shared_ptr<FileWatcher::State>(state);
                state->event_handle.data = state->holder;

                const int start_rc = uv_fs_event_start(
                    &state->event_handle, &FileWatcher::State::on_event,
                    state->path.c_str(), flags);
                if (start_rc != 0) {
                    uv_close(reinterpret_cast<uv_handle_t*>(&state->event_handle),
                             &FileWatcher::State::on_close);
                    return start_rc;
                }
            } else {
                const int init_rc =
                    uv_fs_poll_init(state->ex->loop(), &state->poll_handle);
                if (init_rc != 0) {
                    return init_rc;
                }
                state->initialized.store(true, std::memory_order_release);

                state->holder = new std::shared_ptr<FileWatcher::State>(state);
                state->poll_handle.data = state->holder;

                const int start_rc = uv_fs_poll_start(
                    &state->poll_handle, &FileWatcher::State::on_poll,
                    state->path.c_str(), state->poll_interval_ms);
                if (start_rc != 0) {
                    uv_close(reinterpret_cast<uv_handle_t*>(&state->poll_handle),
                             &FileWatcher::State::on_close);
                    return start_rc;
                }
            }

            state->started.store(true, std::memory_order_release);
            return 0;
        });

    if (rc != 0) {
        throw_uv_error(rc, options.backend == WatchBackend::event
                               ? "uv_fs_event_start"
                               : "uv_fs_poll_start");
    }

    co_return FileWatcher(std::move(state));
}

inline Task<FileWatcher> watch(std::string path, WatchOptions options) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::watch requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await watch(*uvex, std::move(path), options);
}

inline Task<std::vector<DirectoryEntry>> read_directory(UvExecutor& ex,
                                                        std::string path,
                                                        std::size_t batch_size = 32) {
    auto dir = co_await open_directory(ex, std::move(path));
    auto entries = co_await dir.read_all(batch_size);
    co_await dir.close();
    co_return entries;
}

inline Task<std::vector<DirectoryEntry>> read_directory(std::string path,
                                                        std::size_t batch_size = 32) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::read_directory requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await read_directory(*uvex, std::move(path), batch_size);
}

}  // namespace corouv::io
