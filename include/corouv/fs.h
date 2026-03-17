#pragma once

#include <uv.h>

#include <async_simple/coro/Lazy.h>

#include <coroutine>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "corouv/executor.h"
#include "corouv/uv_error.h"

namespace corouv::fs {

namespace detail {

inline void maybe_cleanup(uv_fs_t* req, bool started) noexcept {
    if (started) {
        uv_fs_req_cleanup(req);
    }
}

inline int normalize_uv_fs_rc(int start_rc, const uv_fs_t* req) noexcept {
    if (start_rc < 0) {
        return start_rc;
    }
    return static_cast<int>(req->result);
}

}  // namespace detail

class Open {
public:
    Open(uv_loop_t* loop, std::string path, int flags, int mode)
        : _loop(loop), _path(std::move(path)), _flags(flags), _mode(mode) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc =
            uv_fs_open(_loop, &_req, _path.c_str(), _flags, _mode, &Open::on_cb);
        if (rc < 0) {
            _start_rc = rc;
            return false;
        }
        _started = true;
        return true;
    }

    uv_file await_resume() {
        const int rc = detail::normalize_uv_fs_rc(_start_rc, &_req);
        detail::maybe_cleanup(&_req, _started);
        if (rc < 0) {
            throw_uv_error(rc, "uv_fs_open");
        }
        return static_cast<uv_file>(_req.result);
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Open*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    std::string _path;
    int _flags{};
    int _mode{};

    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Fstat {
public:
    Fstat(uv_loop_t* loop, uv_file file) : _loop(loop), _file(file) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc = uv_fs_fstat(_loop, &_req, _file, &Fstat::on_cb);
        if (rc < 0) {
            _start_rc = rc;
            return false;
        }
        _started = true;
        return true;
    }

    uv_stat_t await_resume() {
        const int rc = detail::normalize_uv_fs_rc(_start_rc, &_req);
        const uv_stat_t st = _req.statbuf;
        detail::maybe_cleanup(&_req, _started);
        if (rc < 0) {
            throw_uv_error(rc, "uv_fs_fstat");
        }
        return st;
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Fstat*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    uv_file _file{};
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Read {
public:
    Read(uv_loop_t* loop, uv_file file, std::span<char> buf, int64_t offset)
        : _loop(loop), _file(file), _buf(buf), _offset(offset) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;

        const size_t cap = _buf.size();
        const size_t max_u32 = std::numeric_limits<unsigned int>::max();
        const unsigned int len =
            static_cast<unsigned int>(cap > max_u32 ? max_u32 : cap);
        _uvbuf = uv_buf_init(_buf.data(), len);

        const int rc = uv_fs_read(_loop, &_req, _file, &_uvbuf, 1, _offset,
                                  &Read::on_cb);
        if (rc < 0) {
            _start_rc = rc;
            return false;
        }
        _started = true;
        return true;
    }

    ssize_t await_resume() {
        const int rc = detail::normalize_uv_fs_rc(_start_rc, &_req);
        const ssize_t n = _req.result;
        detail::maybe_cleanup(&_req, _started);
        if (rc < 0) {
            throw_uv_error(rc, "uv_fs_read");
        }
        return n;
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Read*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    uv_file _file{};
    std::span<char> _buf;
    int64_t _offset{};

    uv_buf_t _uvbuf{};
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Write {
public:
    Write(uv_loop_t* loop, uv_file file, std::span<const char> buf,
          int64_t offset)
        : _loop(loop), _file(file), _buf(buf), _offset(offset) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;

        const size_t cap = _buf.size();
        const size_t max_u32 = std::numeric_limits<unsigned int>::max();
        const unsigned int len =
            static_cast<unsigned int>(cap > max_u32 ? max_u32 : cap);
        _uvbuf = uv_buf_init(const_cast<char*>(_buf.data()), len);

        const int rc = uv_fs_write(_loop, &_req, _file, &_uvbuf, 1, _offset,
                                   &Write::on_cb);
        if (rc < 0) {
            _start_rc = rc;
            return false;
        }
        _started = true;
        return true;
    }

    ssize_t await_resume() {
        const int rc = detail::normalize_uv_fs_rc(_start_rc, &_req);
        const ssize_t n = _req.result;
        detail::maybe_cleanup(&_req, _started);
        if (rc < 0) {
            throw_uv_error(rc, "uv_fs_write");
        }
        return n;
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Write*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    uv_file _file{};
    std::span<const char> _buf;
    int64_t _offset{};

    uv_buf_t _uvbuf{};
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Close {
public:
    Close(uv_loop_t* loop, uv_file file) : _loop(loop), _file(file) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc = uv_fs_close(_loop, &_req, _file, &Close::on_cb);
        if (rc < 0) {
            _start_rc = rc;
            return false;
        }
        _started = true;
        return true;
    }

    void await_resume() {
        const int rc = detail::normalize_uv_fs_rc(_start_rc, &_req);
        detail::maybe_cleanup(&_req, _started);
        if (rc < 0) {
            throw_uv_error(rc, "uv_fs_close");
        }
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Close*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    uv_file _file{};

    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

inline async_simple::coro::Lazy<std::string> read_file(UvExecutor& ex,
                                                       std::string_view path) {
    uv_loop_t* loop = ex.loop();

    // O_RDONLY is a POSIX flag; keep this utility Linux-first for now.
    // If you need portability, route flags/mode from the caller.
    constexpr int kReadOnly = 0;  // O_RDONLY

    uv_file f = co_await Open(loop, std::string(path), kReadOnly, 0);

    // C++ forbids `co_await` in catch handlers. For cleanup on failure we fall
    // back to libuv's synchronous close (cb == nullptr).
    try {
        const uv_stat_t st = co_await Fstat(loop, f);
        const auto size64 = static_cast<int64_t>(st.st_size);
        const size_t size = size64 > 0 ? static_cast<size_t>(size64) : 0;

        std::string out;
        out.resize(size);

        size_t off = 0;
        while (off < out.size()) {
            const ssize_t n = co_await Read(
                loop, f,
                std::span<char>(out.data() + off, out.size() - off),
                static_cast<int64_t>(off));
            if (n == 0) {  // EOF
                break;
            }
            off += static_cast<size_t>(n);
        }

        out.resize(off);
        co_await Close(loop, f);
        co_return out;
    } catch (...) {
        uv_fs_t req{};
        (void)uv_fs_close(loop, &req, f, nullptr);
        uv_fs_req_cleanup(&req);
        throw;
    }
}

}  // namespace corouv::fs
