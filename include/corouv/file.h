#pragma once

#include <uv.h>

#include <async_simple/Executor.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "corouv/executor.h"
#include "corouv/fs.h"
#include "corouv/task.h"

namespace corouv::io {

class File {
public:
    File() = default;
    ~File() = default;

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&&) noexcept = default;
    File& operator=(File&&) noexcept = default;

    [[nodiscard]] bool is_open() const noexcept {
        auto state = _state;
        if (!state) {
            return false;
        }
        std::lock_guard<std::mutex> lk(state->mu);
        return state->is_open_unlocked();
    }

    [[nodiscard]] uv_file native_handle() const noexcept {
        auto state = _state;
        if (!state) {
            return File::invalid_file();
        }
        std::lock_guard<std::mutex> lk(state->mu);
        return state->file;
    }

    [[nodiscard]] const std::string& path() const noexcept {
        static const std::string empty;
        auto state = _state;
        return state ? state->path : empty;
    }

    [[nodiscard]] std::uint64_t position() const {
        auto state = require_state("position");
        std::lock_guard<std::mutex> lk(state->mu);
        state->ensure_open_unlocked("position");
        return static_cast<std::uint64_t>(state->offset);
    }

    void seek(std::uint64_t offset) {
        auto state = require_state("seek");
        const auto value = to_offset(offset, "seek");

        std::lock_guard<std::mutex> lk(state->mu);
        state->ensure_open_unlocked("seek");
        if (state->closing || state->sequential_busy) {
            throw std::logic_error(
                "corouv::io::File::seek requires no sequential operation in flight");
        }
        state->offset = value;
    }

    void rewind() { seek(0); }

    Task<uv_stat_t> stat() const {
        auto state = require_state("stat");
        const auto lease = state->acquire(false, "stat");

        try {
            auto st = co_await fs::Fstat(lease.loop, lease.file);
            state->release(false, false, 0);
            co_return st;
        } catch (...) {
            state->release(false, false, 0);
            throw;
        }
    }

    Task<std::uint64_t> size() const {
        const auto st = co_await stat();
        co_return st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
    }

    Task<std::size_t> read_at(std::uint64_t offset, std::span<char> buffer) const {
        auto state = require_state("read_at");
        const auto lease = state->acquire(false, "read_at");
        const auto pos = to_offset(offset, "read_at");

        try {
            const auto n = co_await fs::Read(lease.loop, lease.file, buffer, pos);
            state->release(false, false, 0);
            co_return n > 0 ? static_cast<std::size_t>(n) : 0;
        } catch (...) {
            state->release(false, false, 0);
            throw;
        }
    }

    Task<std::size_t> write_at(std::uint64_t offset,
                               std::span<const char> buffer) const {
        auto state = require_state("write_at");
        const auto lease = state->acquire(false, "write_at");
        const auto pos = to_offset(offset, "write_at");

        try {
            const auto n = co_await fs::Write(lease.loop, lease.file, buffer, pos);
            state->release(false, false, 0);
            co_return n > 0 ? static_cast<std::size_t>(n) : 0;
        } catch (...) {
            state->release(false, false, 0);
            throw;
        }
    }

    Task<std::size_t> write_at(std::uint64_t offset, std::string_view data) const {
        co_return co_await write_at(
            offset, std::span<const char>(data.data(), data.size()));
    }

    Task<std::size_t> write_at(std::uint64_t offset, const char* data) const {
        co_return co_await write_at(offset, std::string_view(data));
    }

    Task<std::size_t> write_at(std::uint64_t offset,
                               const std::string& data) const {
        co_return co_await write_at(offset, std::string_view(data));
    }

    Task<std::size_t> read_some(std::span<char> buffer) {
        auto state = require_state("read_some");
        const auto lease = state->acquire(true, "read_some");

        try {
            const auto n =
                co_await fs::Read(lease.loop, lease.file, buffer, lease.offset);
            const auto advanced =
                n > 0 ? lease.offset + static_cast<std::int64_t>(n) : lease.offset;
            state->release(true, true, advanced);
            co_return n > 0 ? static_cast<std::size_t>(n) : 0;
        } catch (...) {
            state->release(true, false, 0);
            throw;
        }
    }

    Task<std::size_t> write_some(std::span<const char> buffer) {
        auto state = require_state("write_some");
        const auto lease = state->acquire(true, "write_some");

        try {
            const auto n =
                co_await fs::Write(lease.loop, lease.file, buffer, lease.offset);
            const auto advanced =
                n > 0 ? lease.offset + static_cast<std::int64_t>(n) : lease.offset;
            state->release(true, true, advanced);
            co_return n > 0 ? static_cast<std::size_t>(n) : 0;
        } catch (...) {
            state->release(true, false, 0);
            throw;
        }
    }

    Task<std::size_t> write_some(std::string_view data) {
        co_return co_await write_some(
            std::span<const char>(data.data(), data.size()));
    }

    Task<std::size_t> write_some(const char* data) {
        co_return co_await write_some(std::string_view(data));
    }

    Task<std::size_t> write_some(const std::string& data) {
        co_return co_await write_some(std::string_view(data));
    }

    Task<void> write_all(std::span<const char> buffer) {
        auto state = require_state("write_all");
        const auto lease = state->acquire(true, "write_all");
        auto offset = lease.offset;
        std::size_t written = 0;

        try {
            while (written < buffer.size()) {
                const auto n = co_await fs::Write(
                    lease.loop, lease.file,
                    buffer.subspan(written, buffer.size() - written), offset);
                if (n <= 0) {
                    throw std::runtime_error("corouv::io::File write stalled");
                }
                written += static_cast<std::size_t>(n);
                offset += n;
            }
            state->release(true, true, offset);
            co_return;
        } catch (...) {
            state->release(true, false, 0);
            throw;
        }
    }

    Task<void> write_all(std::string_view data) {
        co_await write_all(std::span<const char>(data.data(), data.size()));
    }

    Task<void> write_all(const char* data) {
        co_await write_all(std::string_view(data));
    }

    Task<void> write_all(const std::string& data) {
        co_await write_all(std::string_view(data));
    }

    Task<std::string> read_until_eof(
        std::size_t max_bytes = 16 * 1024 * 1024) {
        auto state = require_state("read_until_eof");
        const auto lease = state->acquire(true, "read_until_eof");
        auto offset = lease.offset;
        std::string out;
        std::array<char, 4096> scratch{};

        try {
            while (true) {
                const auto n = co_await fs::Read(
                    lease.loop, lease.file,
                    std::span<char>(scratch.data(), scratch.size()), offset);
                if (n == 0) {
                    break;
                }
                if (n < 0) {
                    throw std::runtime_error("corouv::io::File read failed");
                }
                const auto count = static_cast<std::size_t>(n);
                if (out.size() + count > max_bytes) {
                    throw std::runtime_error(
                        "corouv::io::File read_until_eof exceeded max_bytes");
                }
                out.append(scratch.data(), count);
                offset += n;
            }

            state->release(true, true, offset);
            co_return out;
        } catch (...) {
            state->release(true, false, 0);
            throw;
        }
    }

    Task<std::string> read_all(std::size_t max_bytes = 16 * 1024 * 1024) {
        co_return co_await read_until_eof(max_bytes);
    }

    Task<void> sync() const {
        auto state = require_state("sync");
        const auto lease = state->acquire(false, "sync");

        try {
            co_await fs::Fsync(lease.loop, lease.file);
            state->release(false, false, 0);
            co_return;
        } catch (...) {
            state->release(false, false, 0);
            throw;
        }
    }

    Task<void> datasync() const {
        auto state = require_state("datasync");
        const auto lease = state->acquire(false, "datasync");

        try {
            co_await fs::Fdatasync(lease.loop, lease.file);
            state->release(false, false, 0);
            co_return;
        } catch (...) {
            state->release(false, false, 0);
            throw;
        }
    }

    Task<void> close() {
        auto state = _state;
        if (!state) {
            co_return;
        }

        uv_loop_t* loop = nullptr;
        uv_file file = File::invalid_file();
        {
            std::lock_guard<std::mutex> lk(state->mu);
            if (state->file == File::invalid_file()) {
                co_return;
            }
            if (state->closing) {
                throw std::logic_error("corouv::io::File::close already in progress");
            }
            if (state->inflight != 0 || state->sequential_busy) {
                throw std::logic_error(
                    "corouv::io::File::close requires no operations in flight");
            }
            state->closing = true;
            loop = state->loop;
            file = state->file;
        }

        try {
            co_await fs::Close(loop, file);
        } catch (...) {
            std::lock_guard<std::mutex> lk(state->mu);
            state->closing = false;
            throw;
        }

        std::lock_guard<std::mutex> lk(state->mu);
        state->file = File::invalid_file();
        state->offset = 0;
        state->closing = false;
    }

private:
    static constexpr uv_file invalid_file() noexcept {
        return static_cast<uv_file>(-1);
    }

    static std::int64_t to_offset(std::uint64_t value, const char* what) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::out_of_range(std::string("corouv::io::File::") + what +
                                    " offset exceeds int64_t");
        }
        return static_cast<std::int64_t>(value);
    }

    struct Lease {
        uv_loop_t* loop = nullptr;
        uv_file file = File::invalid_file();
        std::int64_t offset = 0;
    };

    struct State {
        State(uv_loop_t* loop, uv_file file, std::string path,
              std::int64_t offset)
            : loop(loop), file(file), path(std::move(path)), offset(offset) {}

        ~State() { close_sync(); }

        [[nodiscard]] bool is_open_unlocked() const noexcept {
            return file != File::invalid_file() && !closing;
        }

        void ensure_open_unlocked(const char* op) const {
            if (file == File::invalid_file() || closing) {
                throw std::logic_error(std::string("corouv::io::File cannot ") +
                                       op + " on a closed file");
            }
        }

        Lease acquire(bool sequential, const char* op) {
            std::lock_guard<std::mutex> lk(mu);
            ensure_open_unlocked(op);
            if (sequential && sequential_busy) {
                throw std::logic_error(
                    std::string("corouv::io::File does not support concurrent ") +
                    op + " operations");
            }
            if (sequential) {
                sequential_busy = true;
            }
            ++inflight;
            return Lease{loop, file, offset};
        }

        void release(bool sequential, bool update_offset,
                     std::int64_t new_offset) noexcept {
            std::lock_guard<std::mutex> lk(mu);
            if (update_offset) {
                offset = new_offset;
            }
            if (sequential) {
                sequential_busy = false;
            }
            if (inflight > 0) {
                --inflight;
            }
        }

        void close_sync() noexcept {
            uv_file to_close = File::invalid_file();
            uv_loop_t* current_loop = nullptr;

            {
                std::lock_guard<std::mutex> lk(mu);
                if (file == File::invalid_file()) {
                    return;
                }
                to_close = file;
                current_loop = loop;
                file = File::invalid_file();
                closing = false;
                sequential_busy = false;
                inflight = 0;
            }

            if (!current_loop) {
                return;
            }

            uv_fs_t req{};
            (void)uv_fs_close(current_loop, &req, to_close, nullptr);
            uv_fs_req_cleanup(&req);
        }

        mutable std::mutex mu;
        uv_loop_t* loop = nullptr;
        uv_file file = File::invalid_file();
        std::string path;
        std::int64_t offset = 0;
        std::size_t inflight = 0;
        bool sequential_busy = false;
        bool closing = false;
    };

    explicit File(std::shared_ptr<State> state) : _state(std::move(state)) {}

    std::shared_ptr<State> require_state(const char* op) const {
        auto state = _state;
        if (!state) {
            throw std::logic_error(std::string("corouv::io::File cannot ") + op +
                                   " on an empty file");
        }
        return state;
    }

    std::shared_ptr<State> _state;

    friend Task<File> open(UvExecutor&, std::string, int, int, std::uint64_t);
    friend Task<File> open(std::string, int, int, std::uint64_t);
};

inline Task<File> open(UvExecutor& ex, std::string path, int flags, int mode = 0644,
                       std::uint64_t offset = 0) {
    auto file = co_await fs::Open(ex.loop(), path, flags, mode);
    co_return File(std::make_shared<File::State>(
        ex.loop(), file, std::move(path), File::to_offset(offset, "open")));
}

inline Task<File> open(std::string path, int flags, int mode = 0644,
                       std::uint64_t offset = 0) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::io::open requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await open(*uvex, std::move(path), flags, mode, offset);
}

}  // namespace corouv::io
