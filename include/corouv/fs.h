#pragma once

#include <uv.h>

#include <async_simple/coro/Lazy.h>

#include <coroutine>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

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

struct DirEntry {
    std::string name;
    uv_dirent_type_t type{UV_DIRENT_UNKNOWN};
};

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

class Stat {
public:
    Stat(uv_loop_t* loop, std::string path)
        : _loop(loop), _path(std::move(path)) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc = uv_fs_stat(_loop, &_req, _path.c_str(), &Stat::on_cb);
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
            throw_uv_error(rc, "uv_fs_stat");
        }
        return st;
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Stat*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    std::string _path;
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Mkdir {
public:
    Mkdir(uv_loop_t* loop, std::string path, int mode)
        : _loop(loop), _path(std::move(path)), _mode(mode) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc =
            uv_fs_mkdir(_loop, &_req, _path.c_str(), _mode, &Mkdir::on_cb);
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
            throw_uv_error(rc, "uv_fs_mkdir");
        }
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Mkdir*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    std::string _path;
    int _mode{};
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Unlink {
public:
    Unlink(uv_loop_t* loop, std::string path)
        : _loop(loop), _path(std::move(path)) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc = uv_fs_unlink(_loop, &_req, _path.c_str(), &Unlink::on_cb);
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
            throw_uv_error(rc, "uv_fs_unlink");
        }
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Unlink*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    std::string _path;
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Rmdir {
public:
    Rmdir(uv_loop_t* loop, std::string path)
        : _loop(loop), _path(std::move(path)) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc = uv_fs_rmdir(_loop, &_req, _path.c_str(), &Rmdir::on_cb);
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
            throw_uv_error(rc, "uv_fs_rmdir");
        }
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Rmdir*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    std::string _path;
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Rename {
public:
    Rename(uv_loop_t* loop, std::string from, std::string to)
        : _loop(loop), _from(std::move(from)), _to(std::move(to)) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc =
            uv_fs_rename(_loop, &_req, _from.c_str(), _to.c_str(), &Rename::on_cb);
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
            throw_uv_error(rc, "uv_fs_rename");
        }
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Rename*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    std::string _from;
    std::string _to;
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Copyfile {
public:
    Copyfile(uv_loop_t* loop, std::string path, std::string new_path, int flags)
        : _loop(loop),
          _path(std::move(path)),
          _new_path(std::move(new_path)),
          _flags(flags) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc = uv_fs_copyfile(_loop, &_req, _path.c_str(),
                                      _new_path.c_str(), _flags,
                                      &Copyfile::on_cb);
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
            throw_uv_error(rc, "uv_fs_copyfile");
        }
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Copyfile*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    std::string _path;
    std::string _new_path;
    int _flags{};
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Symlink {
public:
    Symlink(uv_loop_t* loop, std::string path, std::string new_path, int flags)
        : _loop(loop),
          _path(std::move(path)),
          _new_path(std::move(new_path)),
          _flags(flags) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc = uv_fs_symlink(_loop, &_req, _path.c_str(),
                                     _new_path.c_str(), _flags,
                                     &Symlink::on_cb);
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
            throw_uv_error(rc, "uv_fs_symlink");
        }
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Symlink*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    std::string _path;
    std::string _new_path;
    int _flags{};
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Readlink {
public:
    Readlink(uv_loop_t* loop, std::string path)
        : _loop(loop), _path(std::move(path)) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc =
            uv_fs_readlink(_loop, &_req, _path.c_str(), &Readlink::on_cb);
        if (rc < 0) {
            _start_rc = rc;
            return false;
        }
        _started = true;
        return true;
    }

    std::string await_resume() {
        const int rc = detail::normalize_uv_fs_rc(_start_rc, &_req);
        std::string out;
        if (rc >= 0) {
            const auto* ptr = static_cast<const char*>(uv_fs_get_ptr(&_req));
            if (ptr) {
                out.assign(ptr);
            }
        }
        detail::maybe_cleanup(&_req, _started);
        if (rc < 0) {
            throw_uv_error(rc, "uv_fs_readlink");
        }
        return out;
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Readlink*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    std::string _path;
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Realpath {
public:
    Realpath(uv_loop_t* loop, std::string path)
        : _loop(loop), _path(std::move(path)) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc =
            uv_fs_realpath(_loop, &_req, _path.c_str(), &Realpath::on_cb);
        if (rc < 0) {
            _start_rc = rc;
            return false;
        }
        _started = true;
        return true;
    }

    std::string await_resume() {
        const int rc = detail::normalize_uv_fs_rc(_start_rc, &_req);
        std::string out;
        if (rc >= 0) {
            const auto* ptr = static_cast<const char*>(uv_fs_get_ptr(&_req));
            if (ptr) {
                out.assign(ptr);
            }
        }
        detail::maybe_cleanup(&_req, _started);
        if (rc < 0) {
            throw_uv_error(rc, "uv_fs_realpath");
        }
        return out;
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Realpath*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    std::string _path;
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Access {
public:
    Access(uv_loop_t* loop, std::string path, int mode)
        : _loop(loop), _path(std::move(path)), _mode(mode) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc =
            uv_fs_access(_loop, &_req, _path.c_str(), _mode, &Access::on_cb);
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
            throw_uv_error(rc, "uv_fs_access");
        }
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Access*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    std::string _path;
    int _mode{};
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Opendir {
public:
    Opendir(uv_loop_t* loop, std::string path)
        : _loop(loop), _path(std::move(path)) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc =
            uv_fs_opendir(_loop, &_req, _path.c_str(), &Opendir::on_cb);
        if (rc < 0) {
            _start_rc = rc;
            return false;
        }
        _started = true;
        return true;
    }

    uv_dir_t* await_resume() {
        const int rc = detail::normalize_uv_fs_rc(_start_rc, &_req);
        auto* dir = static_cast<uv_dir_t*>(_req.ptr);
        detail::maybe_cleanup(&_req, _started);
        if (rc < 0) {
            throw_uv_error(rc, "uv_fs_opendir");
        }
        return dir;
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Opendir*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    std::string _path;
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Readdir {
public:
    Readdir(uv_loop_t* loop, uv_dir_t* dir, std::size_t max_entries = 16)
        : _loop(loop),
          _dir(dir),
          _capacity(max_entries == 0 ? 1 : max_entries) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        _raw_entries.resize(_capacity);
        _dir->dirents = _raw_entries.data();
        _dir->nentries = _raw_entries.size();
        const int rc = uv_fs_readdir(_loop, &_req, _dir, &Readdir::on_cb);
        if (rc < 0) {
            _start_rc = rc;
            return false;
        }
        _started = true;
        return true;
    }

    std::vector<DirEntry> await_resume() {
        const int rc = detail::normalize_uv_fs_rc(_start_rc, &_req);
        const ssize_t count = _req.result;
        auto* dir = _dir;
        std::vector<DirEntry> out;
        if (rc >= 0 && count > 0) {
            out.reserve(static_cast<std::size_t>(count));
            for (ssize_t i = 0; i < count; ++i) {
                out.push_back(DirEntry{
                    _raw_entries[static_cast<std::size_t>(i)].name
                        ? _raw_entries[static_cast<std::size_t>(i)].name
                        : "",
                    _raw_entries[static_cast<std::size_t>(i)].type,
                });
            }
        }
        dir->dirents = nullptr;
        dir->nentries = 0;
        detail::maybe_cleanup(&_req, _started);
        if (rc < 0) {
            throw_uv_error(rc, "uv_fs_readdir");
        }
        if (count <= 0) {
            return out;
        }
        return out;
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Readdir*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    uv_dir_t* _dir{};
    std::size_t _capacity{0};
    std::vector<uv_dirent_t> _raw_entries;
    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Closedir {
public:
    Closedir(uv_loop_t* loop, uv_dir_t* dir) : _loop(loop), _dir(dir) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc = uv_fs_closedir(_loop, &_req, _dir, &Closedir::on_cb);
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
            throw_uv_error(rc, "uv_fs_closedir");
        }
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Closedir*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    uv_dir_t* _dir{};
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

class Fsync {
public:
    Fsync(uv_loop_t* loop, uv_file file) : _loop(loop), _file(file) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc = uv_fs_fsync(_loop, &_req, _file, &Fsync::on_cb);
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
            throw_uv_error(rc, "uv_fs_fsync");
        }
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Fsync*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    uv_file _file{};

    uv_fs_t _req{};
    std::coroutine_handle<> _h{};
    int _start_rc{0};
    bool _started{false};
};

class Fdatasync {
public:
    Fdatasync(uv_loop_t* loop, uv_file file) : _loop(loop), _file(file) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        _h = h;
        _req.data = this;
        const int rc =
            uv_fs_fdatasync(_loop, &_req, _file, &Fdatasync::on_cb);
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
            throw_uv_error(rc, "uv_fs_fdatasync");
        }
    }

private:
    static void on_cb(uv_fs_t* req) {
        auto* self = static_cast<Fdatasync*>(req->data);
        self->_h.resume();
    }

    uv_loop_t* _loop{};
    uv_file _file{};

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
