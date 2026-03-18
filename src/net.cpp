#include "corouv/net.h"

#include <async_simple/Executor.h>
#include <async_simple/Signal.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "corouv/detail/awaiter_utils.h"
#include "corouv/uv_error.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#endif

namespace corouv::net {

namespace {

using NameLen = int;
constexpr uv_os_sock_t kInvalidSocket = static_cast<uv_os_sock_t>(-1);

bool write_would_block(int rc) noexcept {
    return rc == UV_EAGAIN || rc == UV_ENOSYS;
}

Endpoint endpoint_from_sockaddr(const sockaddr* addr) {
    char host[INET6_ADDRSTRLEN] = {0};
    std::uint16_t port = 0;

    if (addr->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
        if (uv_inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host)) != 0) {
            std::strcpy(host, "0.0.0.0");
        }
        port = ntohs(in->sin_port);
    } else if (addr->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(addr);
        if (uv_inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host)) != 0) {
            std::strcpy(host, "::");
        }
        port = ntohs(in6->sin6_port);
    } else {
        std::strcpy(host, "<unknown>");
    }

    return Endpoint{host, port};
}

template <class Handle, class Getter>
Endpoint endpoint_from_uv_handle(const Handle* handle, Getter getter) {
    sockaddr_storage storage{};
    NameLen len = sizeof(storage);
    const int rc = getter(handle, reinterpret_cast<sockaddr*>(&storage), &len);
    if (rc != 0) {
        throw_uv_error(rc, "uv_tcp_getsockname/uv_tcp_getpeername");
    }
    return endpoint_from_sockaddr(reinterpret_cast<const sockaddr*>(&storage));
}

struct ResolvedAddress {
    int family = AF_UNSPEC;
    int socktype = SOCK_STREAM;
    int protocol = 0;
    sockaddr_storage storage{};
    int length = 0;
};

class GetAddrInfoAwaiter {
public:
    GetAddrInfoAwaiter(uv_loop_t* loop, std::optional<std::string> node,
                       std::string service, addrinfo hints)
        : _loop(loop),
          _node(std::move(node)),
          _service(std::move(service)),
          _hints(hints) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
        _h = h;
        _req.data = this;

        const int rc = uv_getaddrinfo(_loop, &_req, &GetAddrInfoAwaiter::on_cb,
                                      _node ? _node->c_str() : nullptr,
                                      _service.c_str(), &_hints);
        if (rc != 0) {
            _start_rc = rc;
            return false;
        }

        _started = true;
        return true;
    }

    std::vector<ResolvedAddress> await_resume() {
        if (_start_rc != 0) {
            throw_uv_error(_start_rc, "uv_getaddrinfo");
        }
        if (!_started) {
            throw std::logic_error("corouv::net resolve missing state");
        }
        if (_status != 0) {
            throw_uv_error(_status, "uv_getaddrinfo");
        }

        std::vector<ResolvedAddress> out;
        for (auto* ai = _result; ai != nullptr; ai = ai->ai_next) {
            if (!ai->ai_addr) {
                continue;
            }

            ResolvedAddress entry;
            entry.family = ai->ai_family;
            entry.socktype = ai->ai_socktype;
            entry.protocol = ai->ai_protocol;
            entry.length = static_cast<int>(ai->ai_addrlen);
            std::memcpy(&entry.storage, ai->ai_addr, ai->ai_addrlen);
            out.push_back(entry);
        }

        if (_result) {
            uv_freeaddrinfo(_result);
            _result = nullptr;
        }

        if (out.empty()) {
            throw std::runtime_error("corouv::net resolve returned no results");
        }
        return out;
    }

private:
    static void on_cb(uv_getaddrinfo_t* req, int status, addrinfo* res) {
        auto* self = static_cast<GetAddrInfoAwaiter*>(req->data);
        self->_status = status;
        self->_result = res;
        self->_h.resume();
    }

    uv_loop_t* _loop = nullptr;
    std::optional<std::string> _node;
    std::string _service;
    addrinfo _hints{};
    uv_getaddrinfo_t _req{};
    std::coroutine_handle<> _h{};
    addrinfo* _result = nullptr;
    int _start_rc = 0;
    int _status = 0;
    bool _started = false;
};

Task<std::vector<ResolvedAddress>> resolve_tcp(UvExecutor& ex,
                                               std::optional<std::string> host,
                                               std::uint16_t port,
                                               bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = passive ? AI_PASSIVE : 0;

    co_return co_await GetAddrInfoAwaiter(ex.loop(), std::move(host),
                                          std::to_string(port), hints);
}

Task<std::vector<ResolvedAddress>> resolve_udp(UvExecutor& ex,
                                               std::optional<std::string> host,
                                               std::uint16_t port,
                                               bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = passive ? AI_PASSIVE : 0;

    co_return co_await GetAddrInfoAwaiter(ex.loop(), std::move(host),
                                          std::to_string(port), hints);
}

unsigned udp_bind_flags(const UdpBindOptions& options) noexcept {
    unsigned flags = 0;
    if (options.ipv6_only) {
        flags |= UV_UDP_IPV6ONLY;
    }
    if (options.reuse_address) {
        flags |= UV_UDP_REUSEADDR;
    }
    if (options.reuse_port) {
        flags |= UV_UDP_REUSEPORT;
    }
    if (options.receive_errors) {
        flags |= UV_UDP_LINUX_RECVERR;
    }
    return flags;
}

std::string connect_error_message(std::string_view host, std::uint16_t port,
                                  int err) {
    std::string out = "corouv::net::connect ";
    out += std::string(host);
    out += ":";
    out += std::to_string(port);
    out += " failed: ";
    out += uv_strerror(err);
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
            detail::resume_handle(_ex, h);
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

}  // namespace

struct TcpStream::State : std::enable_shared_from_this<TcpStream::State> {
    explicit State(UvExecutor* executor) : ex(executor) {}

    ~State() = default;

    int init_on_loop() {
        if (initialized) {
            return 0;
        }

        const int rc = uv_tcp_init(ex->loop(), &handle);
        if (rc != 0) {
            return rc;
        }

        holder = new std::shared_ptr<State>(shared_from_this());
        handle.data = holder;
        initialized = true;
        return 0;
    }

    bool open() const noexcept {
        return initialized && !closing.load(std::memory_order_acquire) &&
               !closed.load(std::memory_order_acquire);
    }

    uv_stream_t* stream() noexcept {
        return reinterpret_cast<uv_stream_t*>(&handle);
    }

    const uv_stream_t* stream() const noexcept {
        return reinterpret_cast<const uv_stream_t*>(&handle);
    }

    void refresh_endpoints() {
        local = endpoint_from_uv_handle(
            &handle, [](const uv_tcp_t* h, sockaddr* a, int* l) {
                return uv_tcp_getsockname(h, a, l);
            });
        peer = endpoint_from_uv_handle(
            &handle, [](const uv_tcp_t* h, sockaddr* a, int* l) {
                return uv_tcp_getpeername(h, a, l);
            });
    }

    int start_reading_on_loop() {
        std::lock_guard<std::mutex> lk(mu);
        if (!open() || read_started || eof || read_error != 0) {
            return 0;
        }

        const int rc = uv_read_start(stream(), &State::alloc_cb, &State::read_cb);
        if (rc == 0) {
            read_started = true;
        }
        return rc;
    }

    void request_close() noexcept {
        auto self = shared_from_this();
        if (ex && !ex->currentThreadInExecutor()) {
            if (ex->schedule([self]() noexcept { self->close_on_loop(); })) {
                return;
            }
        }
        close_on_loop();
    }

    void close_on_loop() noexcept {
        if (!initialized) {
            closed.store(true, std::memory_order_release);
            return;
        }
        if (closing.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        std::coroutine_handle<> waiter;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (read_started) {
                (void)uv_read_stop(stream());
                read_started = false;
            }
            if (!eof && read_error == 0) {
                read_error = UV_ECANCELED;
            }
            waiter = std::exchange(read_waiter, std::coroutine_handle<>{});
        }

        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&handle))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&handle), &State::on_close);
        }

        detail::resume_handle(ex, waiter);
    }

    static void alloc_cb(uv_handle_t*, size_t suggested_size, uv_buf_t* buf) {
        auto* data = new char[suggested_size > 0 ? suggested_size : 4096];
        *buf = uv_buf_init(data,
                           static_cast<unsigned int>(suggested_size > 0
                                                         ? suggested_size
                                                         : 4096));
    }

    static void read_cb(uv_stream_t* stream, ssize_t nread,
                        const uv_buf_t* buf) {
        std::unique_ptr<char[]> guard(buf && buf->base ? buf->base : nullptr);

        auto* holder = static_cast<std::shared_ptr<State>*>(stream->data);
        if (!holder) {
            return;
        }
        auto self = *holder;

        std::coroutine_handle<> waiter;
        {
            std::lock_guard<std::mutex> lk(self->mu);

            if (nread > 0) {
                self->read_buffer.append(buf->base, static_cast<std::size_t>(nread));
            } else if (nread == UV_EOF) {
                self->eof = true;
                self->read_started = false;
                (void)uv_read_stop(stream);
            } else if (nread < 0) {
                self->read_error = static_cast<int>(nread);
                self->read_started = false;
                (void)uv_read_stop(stream);
            }

            if (nread != 0) {
                waiter = std::exchange(self->read_waiter, std::coroutine_handle<>{});
            }
        }

        detail::resume_handle(self->ex, waiter);
    }

    static void on_close(uv_handle_t* handle) noexcept {
        auto* holder = static_cast<std::shared_ptr<State>*>(handle->data);
        if (!holder) {
            return;
        }

        auto self = *holder;
        self->closed.store(true, std::memory_order_release);
        handle->data = nullptr;
        delete holder;
    }

    UvExecutor* ex = nullptr;
    uv_tcp_t handle{};
    std::shared_ptr<State>* holder = nullptr;
    mutable std::mutex mu;
    std::string read_buffer;
    std::coroutine_handle<> read_waiter{};
    int read_error = 0;
    bool read_started = false;
    bool eof = false;
    bool initialized = false;
    Endpoint local;
    Endpoint peer;
    std::atomic<bool> closing{false};
    std::atomic<bool> closed{false};
};

struct TcpListener::State : std::enable_shared_from_this<TcpListener::State> {
    explicit State(UvExecutor* executor) : ex(executor) {}

    int init_on_loop() {
        if (initialized) {
            return 0;
        }

        const int rc = uv_tcp_init(ex->loop(), &handle);
        if (rc != 0) {
            return rc;
        }

        holder = new std::shared_ptr<State>(shared_from_this());
        handle.data = holder;
        initialized = true;
        return 0;
    }

    bool open() const noexcept {
        return initialized && !closing.load(std::memory_order_acquire) &&
               !closed.load(std::memory_order_acquire);
    }

    void refresh_local_endpoint() {
        local = endpoint_from_uv_handle(
            &handle, [](const uv_tcp_t* h, sockaddr* a, int* l) {
                return uv_tcp_getsockname(h, a, l);
            });
    }

    void request_close() noexcept {
        auto self = shared_from_this();
        if (ex && !ex->currentThreadInExecutor()) {
            if (ex->schedule([self]() noexcept { self->close_on_loop(); })) {
                return;
            }
        }
        close_on_loop();
    }

    void close_on_loop() noexcept {
        if (!initialized) {
            closed.store(true, std::memory_order_release);
            return;
        }
        if (closing.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        std::coroutine_handle<> waiter;
        {
            std::lock_guard<std::mutex> lk(mu);
            accept_error = UV_ECANCELED;
            waiter = std::exchange(accept_waiter, std::coroutine_handle<>{});
        }

        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&handle))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&handle), &State::on_close);
        }

        detail::resume_handle(ex, waiter);
    }

    static void on_connection(uv_stream_t* server, int status) {
        auto* holder = static_cast<std::shared_ptr<State>*>(server->data);
        if (!holder) {
            return;
        }
        auto self = *holder;

        std::coroutine_handle<> waiter;
        if (status < 0) {
            {
                std::lock_guard<std::mutex> lk(self->mu);
                self->accept_error = status;
                waiter =
                    std::exchange(self->accept_waiter, std::coroutine_handle<>{});
            }
            detail::resume_handle(self->ex, waiter);
            return;
        }

        auto child = std::make_shared<TcpStream::State>(self->ex);
        int rc = child->init_on_loop();
        if (rc == 0) {
            rc = uv_accept(server, child->stream());
        }

        if (rc == 0) {
            child->refresh_endpoints();
            (void)uv_tcp_nodelay(&child->handle, 1);

            {
                std::lock_guard<std::mutex> lk(self->mu);
                self->pending.emplace_back(std::move(child));
                waiter =
                    std::exchange(self->accept_waiter, std::coroutine_handle<>{});
            }
            detail::resume_handle(self->ex, waiter);
            return;
        }

        if (child->initialized) {
            child->request_close();
        }

        {
            std::lock_guard<std::mutex> lk(self->mu);
            self->accept_error = rc;
            waiter = std::exchange(self->accept_waiter, std::coroutine_handle<>{});
        }
        detail::resume_handle(self->ex, waiter);
    }

    static void on_close(uv_handle_t* handle) noexcept {
        auto* holder = static_cast<std::shared_ptr<State>*>(handle->data);
        if (!holder) {
            return;
        }

        auto self = *holder;
        self->closed.store(true, std::memory_order_release);
        handle->data = nullptr;
        delete holder;
    }

    UvExecutor* ex = nullptr;
    uv_tcp_t handle{};
    std::shared_ptr<State>* holder = nullptr;
    mutable std::mutex mu;
    std::deque<std::shared_ptr<TcpStream::State>> pending;
    std::coroutine_handle<> accept_waiter{};
    int accept_error = 0;
    bool initialized = false;
    Endpoint local;
    std::atomic<bool> closing{false};
    std::atomic<bool> closed{false};
};

namespace {

class ReadAwaiter {
public:
    ReadAwaiter(std::shared_ptr<TcpStream::State> state, std::span<char> buffer)
        : _state(std::move(state)), _buffer(buffer) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
        if (!_state) {
            _setup_error = UV_EBADF;
            return false;
        }

        auto register_waiter = [this, h]() -> bool {
            {
                std::lock_guard<std::mutex> lk(_state->mu);
                if (!_state->open() || !_state->read_buffer.empty() || _state->eof ||
                    _state->read_error != 0) {
                    return false;
                }
                if (_state->read_waiter) {
                    _concurrent_read = true;
                    return false;
                }
                _state->read_waiter = h;
            }

            const int rc = _state->start_reading_on_loop();
            if (rc != 0) {
                {
                    std::lock_guard<std::mutex> lk(_state->mu);
                    _state->read_waiter = std::coroutine_handle<>{};
                    _state->read_error = rc;
                }
                _setup_error = rc;
                return false;
            }

            return true;
        };

        if (_state->ex->currentThreadInExecutor()) {
            return register_waiter();
        }

        const bool scheduled = _state->ex->schedule([this, h, register_waiter]() {
            if (!register_waiter()) {
                detail::resume_handle(_state->ex, h);
            }
        });
        if (!scheduled) {
            _setup_error = UV_ECANCELED;
            return false;
        }
        return true;
    }

    std::size_t await_resume() {
        if (_concurrent_read) {
            throw std::logic_error(
                "corouv::net::TcpStream does not support concurrent reads");
        }
        if (_setup_error != 0) {
            throw_uv_error(_setup_error, "uv_read_start");
        }
        if (!_state) {
            throw std::logic_error("corouv::net::TcpStream missing state");
        }

        std::lock_guard<std::mutex> lk(_state->mu);
        if (!_state->read_buffer.empty()) {
            const auto n = std::min(_buffer.size(), _state->read_buffer.size());
            std::memcpy(_buffer.data(), _state->read_buffer.data(), n);
            _state->read_buffer.erase(0, n);
            return n;
        }
        if (_state->read_error != 0 && _state->read_error != UV_EOF) {
            throw_uv_error(_state->read_error, "uv_read");
        }
        return 0;
    }

private:
    std::shared_ptr<TcpStream::State> _state;
    std::span<char> _buffer;
    int _setup_error = 0;
    bool _concurrent_read = false;
};

class WriteAwaiter {
public:
    WriteAwaiter(std::shared_ptr<TcpStream::State> state,
                 std::span<const char> buffer)
        : _state(std::move(state)),
          _buffer(buffer.data(),
                  buffer.data() +
                      std::min<std::size_t>(buffer.size(),
                                            static_cast<std::size_t>(
                                                std::numeric_limits<unsigned int>::max()))) {}

    bool await_ready() const noexcept { return _buffer.empty(); }

    bool await_suspend(std::coroutine_handle<> h) {
        if (!_state) {
            _start_rc = UV_EBADF;
            return false;
        }

        _op = std::make_shared<WriteOp>();
        _op->state = _state;
        _op->buffer = _buffer;
        _op->continuation = h;

        if (_state->ex->currentThreadInExecutor()) {
            return _op->start_on_loop();
        }

        const bool scheduled = _state->ex->schedule([op = _op]() {
            if (!op->start_on_loop()) {
                detail::resume_handle(op->state->ex, op->continuation);
            }
        });
        if (!scheduled) {
            _start_rc = UV_ECANCELED;
            return false;
        }
        return true;
    }

    std::size_t await_resume() {
        if (_start_rc != 0) {
            throw_uv_error(_start_rc, "uv_write");
        }
        if (!_op) {
            return _buffer.size();
        }
        if (_op->start_rc != 0) {
            throw_uv_error(_op->start_rc, "uv_write");
        }
        if (_op->status != 0) {
            throw_uv_error(_op->status, "uv_write");
        }
        return _op->result;
    }

private:
    struct WriteOp : std::enable_shared_from_this<WriteOp> {
        bool start_on_loop() {
            if (!state || !state->open()) {
                start_rc = UV_ECANCELED;
                return false;
            }

            uv_buf_t buf = uv_buf_init(
                buffer.data(), static_cast<unsigned int>(buffer.size()));

            const int rc = uv_try_write(state->stream(), &buf, 1);
            if (rc > 0) {
                result = static_cast<std::size_t>(rc);
                return false;
            }
            if (rc < 0 && !write_would_block(rc)) {
                start_rc = rc;
                return false;
            }

            req.data = new std::shared_ptr<WriteOp>(shared_from_this());
            const int write_rc = uv_write(&req, state->stream(), &buf, 1,
                                          &WriteOp::on_write);
            if (write_rc != 0) {
                delete static_cast<std::shared_ptr<WriteOp>*>(req.data);
                req.data = nullptr;
                start_rc = write_rc;
                return false;
            }
            return true;
        }

        static void on_write(uv_write_t* req, int status) noexcept {
            auto* holder = static_cast<std::shared_ptr<WriteOp>*>(req->data);
            auto self = *holder;
            delete holder;
            req->data = nullptr;

            self->status = status;
            if (status == 0) {
                self->result = self->buffer.size();
            }
            detail::resume_handle(self->state ? self->state->ex : nullptr,
                                  self->continuation);
        }

        std::shared_ptr<TcpStream::State> state;
        uv_write_t req{};
        std::string buffer;
        std::coroutine_handle<> continuation{};
        int start_rc = 0;
        int status = 0;
        std::size_t result = 0;
    };

    std::shared_ptr<TcpStream::State> _state;
    std::string _buffer;
    std::shared_ptr<WriteOp> _op;
    int _start_rc = 0;
};

class ConnectAwaiter {
public:
    ConnectAwaiter(std::shared_ptr<TcpStream::State> state,
                   ResolvedAddress address)
        : _state(std::move(state)), _address(address) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
        _continuation = h;

        auto start = [this]() -> bool {
            const int init_rc = _state->init_on_loop();
            if (init_rc != 0) {
                _start_rc = init_rc;
                return false;
            }

            _req.data = this;
            const int rc = uv_tcp_connect(
                &_req, &_state->handle,
                reinterpret_cast<const sockaddr*>(&_address.storage), &ConnectAwaiter::on_connect);
            if (rc != 0) {
                _start_rc = rc;
                return false;
            }
            return true;
        };

        if (_state->ex->currentThreadInExecutor()) {
            return start();
        }

        const bool scheduled = _state->ex->schedule([this, start]() {
            if (!start()) {
                detail::resume_handle(_state->ex, _continuation);
            }
        });
        if (!scheduled) {
            _start_rc = UV_ECANCELED;
            return false;
        }
        return true;
    }

    int await_resume() const noexcept {
        if (_start_rc != 0) {
            return _start_rc;
        }
        return _status;
    }

private:
    static void on_connect(uv_connect_t* req, int status) {
        auto* self = static_cast<ConnectAwaiter*>(req->data);
        self->_status = status;
        detail::resume_handle(self->_state->ex, self->_continuation);
    }

    std::shared_ptr<TcpStream::State> _state;
    ResolvedAddress _address;
    uv_connect_t _req{};
    std::coroutine_handle<> _continuation{};
    int _start_rc = 0;
    int _status = 0;
};

class AcceptAwaiter {
public:
    explicit AcceptAwaiter(std::shared_ptr<TcpListener::State> state)
        : _state(std::move(state)) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
        if (!_state) {
            _start_rc = UV_EBADF;
            return false;
        }

        auto register_waiter = [this, h]() -> bool {
            std::lock_guard<std::mutex> lk(_state->mu);
            if (!_state->pending.empty() || !_state->open() ||
                _state->accept_error != 0) {
                return false;
            }
            if (_state->accept_waiter) {
                _concurrent_accept = true;
                return false;
            }
            _state->accept_waiter = h;
            return true;
        };

        if (_state->ex->currentThreadInExecutor()) {
            return register_waiter();
        }

        const bool scheduled = _state->ex->schedule([this, h, register_waiter]() {
            if (!register_waiter()) {
                detail::resume_handle(_state->ex, h);
            }
        });
        if (!scheduled) {
            _start_rc = UV_ECANCELED;
            return false;
        }
        return true;
    }

    std::shared_ptr<TcpStream::State> await_resume() {
        if (_concurrent_accept) {
            throw std::logic_error(
                "corouv::net::TcpListener does not support concurrent accept calls");
        }
        if (_start_rc != 0) {
            throw_uv_error(_start_rc, "uv_listen/uv_accept");
        }
        if (!_state) {
            throw std::logic_error("corouv::net::TcpListener missing state");
        }

        std::lock_guard<std::mutex> lk(_state->mu);
        if (!_state->pending.empty()) {
            auto stream = std::move(_state->pending.front());
            _state->pending.pop_front();
            return stream;
        }
        if (_state->accept_error != 0 && _state->accept_error != UV_ECANCELED) {
            throw_uv_error(_state->accept_error, "uv_listen/uv_accept");
        }
        throw std::logic_error("corouv::net::TcpListener is closed");
    }

private:
    std::shared_ptr<TcpListener::State> _state;
    int _start_rc = 0;
    bool _concurrent_accept = false;
};

}  // namespace

std::string to_string(const Endpoint& endpoint) {
    if (endpoint.host.find(':') != std::string::npos) {
        return "[" + endpoint.host + "]:" + std::to_string(endpoint.port);
    }
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

TcpStream::TcpStream(std::shared_ptr<State> state) noexcept
    : _state(std::move(state)) {}

TcpStream::~TcpStream() { close(); }

TcpStream::TcpStream(TcpStream&& other) noexcept = default;
TcpStream& TcpStream::operator=(TcpStream&& other) noexcept = default;

bool TcpStream::is_open() const noexcept { return _state && _state->open(); }

uv_os_sock_t TcpStream::native_handle() const noexcept {
    if (!_state || !_state->open()) {
        return kInvalidSocket;
    }

    uv_os_fd_t fd = -1;
    if (uv_fileno(reinterpret_cast<const uv_handle_t*>(&_state->handle), &fd) != 0) {
        return kInvalidSocket;
    }
    return static_cast<uv_os_sock_t>(fd);
}

const Endpoint& TcpStream::local_endpoint() const noexcept {
    static const Endpoint empty{};
    return _state ? _state->local : empty;
}

const Endpoint& TcpStream::peer_endpoint() const noexcept {
    static const Endpoint empty{};
    return _state ? _state->peer : empty;
}

Task<std::size_t> TcpStream::read_some(std::span<char> buffer) {
    if (!is_open()) {
        throw std::logic_error("corouv::net::TcpStream is closed");
    }
    if (buffer.empty()) {
        co_return 0;
    }
    co_return co_await ReadAwaiter(_state, buffer);
}

Task<std::size_t> TcpStream::write_some(std::span<const char> buffer) {
    if (!is_open()) {
        throw std::logic_error("corouv::net::TcpStream is closed");
    }
    if (buffer.empty()) {
        co_return 0;
    }
    co_return co_await WriteAwaiter(_state, buffer);
}

Task<void> TcpStream::write_all(std::span<const char> buffer) {
    std::size_t written = 0;
    while (written < buffer.size()) {
        const auto chunk =
            co_await write_some(buffer.subspan(written, buffer.size() - written));
        if (chunk == 0) {
            throw std::runtime_error("corouv::net::TcpStream write stalled");
        }
        written += chunk;
    }
    co_return;
}

Task<void> TcpStream::write_all(std::string_view data) {
    co_await write_all(std::span<const char>(data.data(), data.size()));
}

Task<std::string> TcpStream::read_until_eof(std::size_t max_bytes) {
    std::string out;
    std::array<char, 4096> scratch{};

    for (;;) {
        const auto n = co_await read_some(
            std::span<char>(scratch.data(), scratch.size()));
        if (n == 0) {
            break;
        }
        if (out.size() + n > max_bytes) {
            throw std::runtime_error(
                "corouv::net::TcpStream read_until_eof exceeded max_bytes");
        }
        out.append(scratch.data(), n);
    }

    co_return out;
}

void TcpStream::set_nodelay(bool enabled) {
    if (!_state || !_state->open()) {
        return;
    }
    if (!_state->ex->currentThreadInExecutor()) {
        throw std::logic_error(
            "corouv::net::TcpStream::set_nodelay must run on the loop thread");
    }
    const int rc = uv_tcp_nodelay(&_state->handle, enabled ? 1 : 0);
    if (rc != 0) {
        throw_uv_error(rc, "uv_tcp_nodelay");
    }
}

void TcpStream::set_keepalive(bool enabled, unsigned int delay_seconds) {
    if (!_state || !_state->open()) {
        return;
    }
    if (!_state->ex->currentThreadInExecutor()) {
        throw std::logic_error(
            "corouv::net::TcpStream::set_keepalive must run on the loop thread");
    }
    const int rc =
        uv_tcp_keepalive(&_state->handle, enabled ? 1 : 0, delay_seconds);
    if (rc != 0) {
        throw_uv_error(rc, "uv_tcp_keepalive");
    }
}

void TcpStream::shutdown_write() noexcept {
    if (!_state || !_state->open()) {
        return;
    }

    struct ShutdownOp {
        uv_shutdown_t req{};

        static void on_shutdown(uv_shutdown_t* req, int) noexcept {
            delete static_cast<ShutdownOp*>(req->data);
        }
    };

    auto state = _state;
    auto start = [state]() noexcept {
        if (!state->open()) {
            return;
        }
        auto* op = new ShutdownOp();
        op->req.data = op;
        const int rc = uv_shutdown(&op->req, state->stream(), &ShutdownOp::on_shutdown);
        if (rc != 0) {
            delete op;
        }
    };

    if (_state->ex->currentThreadInExecutor()) {
        start();
    } else {
        (void)_state->ex->schedule(start);
    }
}

void TcpStream::close() noexcept {
    if (_state) {
        _state->request_close();
        _state.reset();
    }
}

TcpListener::TcpListener(std::shared_ptr<State> state) noexcept
    : _state(std::move(state)) {}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept = default;
TcpListener& TcpListener::operator=(TcpListener&& other) noexcept = default;

bool TcpListener::is_open() const noexcept { return _state && _state->open(); }

uv_os_sock_t TcpListener::native_handle() const noexcept {
    if (!_state || !_state->open()) {
        return kInvalidSocket;
    }

    uv_os_fd_t fd = -1;
    if (uv_fileno(reinterpret_cast<const uv_handle_t*>(&_state->handle), &fd) != 0) {
        return kInvalidSocket;
    }
    return static_cast<uv_os_sock_t>(fd);
}

const Endpoint& TcpListener::local_endpoint() const noexcept {
    static const Endpoint empty{};
    return _state ? _state->local : empty;
}

Task<TcpStream> TcpListener::accept() {
    if (!is_open()) {
        throw std::logic_error("corouv::net::TcpListener is closed");
    }
    auto state = co_await AcceptAwaiter(_state);
    co_return TcpStream(std::move(state));
}

void TcpListener::close() noexcept {
    if (_state) {
        _state->request_close();
        _state.reset();
    }
}

Task<TcpStream> connect(UvExecutor& ex, std::string host, std::uint16_t port) {
    const auto addresses =
        co_await resolve_tcp(ex, host.empty() ? std::nullopt
                                             : std::optional<std::string>(host),
                             port, false);

    std::optional<std::string> last_error;

    for (const auto& address : addresses) {
        auto state = std::make_shared<TcpStream::State>(&ex);
        const int status = co_await ConnectAwaiter(state, address);
        if (status == 0) {
            state->refresh_endpoints();
            (void)uv_tcp_nodelay(&state->handle, 1);
            co_return TcpStream(std::move(state));
        }

        last_error = connect_error_message(host, port, status);
        state->request_close();
    }

    if (last_error) {
        throw std::runtime_error(*last_error);
    }
    throw std::runtime_error("corouv::net::connect failed");
}

Task<TcpStream> connect(std::string host, std::uint16_t port) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::net::connect requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await connect(*uvex, std::move(host), port);
}

Task<TcpListener> listen(UvExecutor& ex, std::string host, std::uint16_t port,
                         int backlog) {
    const auto addresses =
        co_await resolve_tcp(ex, host.empty() ? std::nullopt
                                             : std::optional<std::string>(host),
                             port, true);

    std::optional<std::string> last_error;

    for (const auto& address : addresses) {
        auto state = std::make_shared<TcpListener::State>(&ex);
        const int init_rc = state->init_on_loop();
        if (init_rc != 0) {
            last_error = "uv_tcp_init: ";
            last_error->append(uv_strerror(init_rc));
            continue;
        }

        const int bind_rc = uv_tcp_bind(
            &state->handle, reinterpret_cast<const sockaddr*>(&address.storage), 0);
        if (bind_rc != 0) {
            last_error = "uv_tcp_bind: ";
            last_error->append(uv_strerror(bind_rc));
            state->request_close();
            continue;
        }

        const int listen_rc =
            uv_listen(reinterpret_cast<uv_stream_t*>(&state->handle), backlog,
                      &TcpListener::State::on_connection);
        if (listen_rc != 0) {
            last_error = "uv_listen: ";
            last_error->append(uv_strerror(listen_rc));
            state->request_close();
            continue;
        }

        state->refresh_local_endpoint();
        co_return TcpListener(std::move(state));
    }

    if (last_error) {
        throw std::runtime_error("corouv::net::listen failed: " + *last_error);
    }
    throw std::runtime_error("corouv::net::listen failed");
}

Task<TcpListener> listen(std::string host, std::uint16_t port, int backlog) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::net::listen requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await listen(*uvex, std::move(host), port, backlog);
}

struct UdpSocket::State : std::enable_shared_from_this<UdpSocket::State> {
    explicit State(UvExecutor* executor) : ex(executor) {}

    int init_on_loop() {
        if (initialized) {
            return 0;
        }

        const int rc = uv_udp_init(ex->loop(), &handle);
        if (rc != 0) {
            return rc;
        }

        holder = new std::shared_ptr<State>(shared_from_this());
        handle.data = holder;
        initialized = true;
        return 0;
    }

    bool open() const noexcept {
        return initialized && !closing.load(std::memory_order_acquire) &&
               !closed.load(std::memory_order_acquire);
    }

    void refresh_local_endpoint() {
        local = endpoint_from_uv_handle(
            &handle, [](const uv_udp_t* h, sockaddr* a, int* l) {
                return uv_udp_getsockname(h, a, l);
            });
    }

    void refresh_peer_endpoint() {
        sockaddr_storage storage{};
        NameLen len = sizeof(storage);
        const int rc =
            uv_udp_getpeername(&handle, reinterpret_cast<sockaddr*>(&storage), &len);
        if (rc == 0) {
            peer = endpoint_from_sockaddr(reinterpret_cast<const sockaddr*>(&storage));
        } else {
            peer = {};
        }
    }

    int start_receiving_on_loop() {
        std::lock_guard<std::mutex> lk(mu);
        if (!open() || recv_started || recv_error != 0) {
            return 0;
        }

        const int rc = uv_udp_recv_start(&handle, &State::alloc_cb, &State::recv_cb);
        if (rc == 0) {
            recv_started = true;
        }
        return rc;
    }

    void request_close() noexcept {
        auto self = shared_from_this();
        if (ex && !ex->currentThreadInExecutor()) {
            if (ex->schedule([self]() noexcept { self->close_on_loop(); })) {
                return;
            }
        }
        close_on_loop();
    }

    void close_on_loop() noexcept {
        if (!initialized) {
            closed.store(true, std::memory_order_release);
            return;
        }
        if (closing.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        std::coroutine_handle<> waiter;
        std::coroutine_handle<> direct_waiter;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (recv_started) {
                (void)uv_udp_recv_stop(&handle);
                recv_started = false;
            }
            if (pending.empty() && recv_error == 0) {
                recv_error = UV_ECANCELED;
            }
            waiter = std::exchange(recv_waiter, std::coroutine_handle<>{});
            direct_waiter =
                std::exchange(recv_some_waiter, std::coroutine_handle<>{});
            recv_some_data = nullptr;
            recv_some_capacity = 0;
        }

        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&handle))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&handle), &State::on_close);
        }

        detail::resume_handle(ex, waiter);
        detail::resume_handle(ex, direct_waiter);
    }

    void cancel_recv_waiter(std::coroutine_handle<> h) noexcept {
        std::coroutine_handle<> waiter;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (recv_waiter == h) {
                waiter = std::exchange(recv_waiter, std::coroutine_handle<>{});
            }
        }
        detail::resume_handle(ex, waiter);
    }

    void cancel_recv_some_waiter(std::coroutine_handle<> h) noexcept {
        std::coroutine_handle<> waiter;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (recv_some_waiter == h) {
                waiter =
                    std::exchange(recv_some_waiter, std::coroutine_handle<>{});
                recv_some_data = nullptr;
                recv_some_capacity = 0;
            }
        }
        detail::resume_handle(ex, waiter);
    }

    static void alloc_cb(uv_handle_t*, size_t suggested_size, uv_buf_t* buf) {
        auto* data = new char[suggested_size > 0 ? suggested_size : 65536];
        *buf = uv_buf_init(data,
                           static_cast<unsigned int>(suggested_size > 0
                                                         ? suggested_size
                                                         : 65536));
    }

    static void recv_cb(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
                        const sockaddr* addr, unsigned flags) {
        std::unique_ptr<char[]> guard(buf && buf->base ? buf->base : nullptr);

        auto* holder = static_cast<std::shared_ptr<State>*>(handle->data);
        if (!holder) {
            return;
        }
        auto self = *holder;

        std::coroutine_handle<> waiter;
        std::coroutine_handle<> direct_waiter;
        {
            std::lock_guard<std::mutex> lk(self->mu);
            if (nread > 0) {
                if (self->recv_some_waiter) {
                    const auto copy =
                        std::min<std::size_t>(static_cast<std::size_t>(nread),
                                              self->recv_some_capacity);
                    if (copy > 0 && self->recv_some_data != nullptr) {
                        std::memcpy(self->recv_some_data, buf->base, copy);
                    }

                    self->recv_some_result.size = copy;
                    self->recv_some_result.flags = flags;
                    if (static_cast<std::size_t>(nread) > self->recv_some_capacity) {
                        self->recv_some_result.flags |= UV_UDP_PARTIAL;
                    }
                    if (addr) {
                        self->recv_some_result.peer = endpoint_from_sockaddr(addr);
                    } else {
                        self->recv_some_result.peer = self->peer;
                    }

                    direct_waiter =
                        std::exchange(self->recv_some_waiter, std::coroutine_handle<>{});
                    self->recv_some_data = nullptr;
                    self->recv_some_capacity = 0;
                } else {
                    Datagram datagram;
                    datagram.payload.assign(buf->base, static_cast<std::size_t>(nread));
                    datagram.flags = flags;
                    if (addr) {
                        datagram.peer = endpoint_from_sockaddr(addr);
                    } else {
                        datagram.peer = self->peer;
                    }
                    self->pending.push_back(std::move(datagram));
                }
            } else if (nread < 0) {
                self->recv_error = static_cast<int>(nread);
                self->recv_started = false;
                (void)uv_udp_recv_stop(handle);
                direct_waiter =
                    std::exchange(self->recv_some_waiter, std::coroutine_handle<>{});
                self->recv_some_data = nullptr;
                self->recv_some_capacity = 0;
            }

            if (nread != 0) {
                waiter = std::exchange(self->recv_waiter, std::coroutine_handle<>{});
            }
        }

        detail::resume_handle(self->ex, waiter);
        detail::resume_handle(self->ex, direct_waiter);
    }

    static void on_close(uv_handle_t* handle) noexcept {
        auto* holder = static_cast<std::shared_ptr<State>*>(handle->data);
        if (!holder) {
            return;
        }

        auto self = *holder;
        self->closed.store(true, std::memory_order_release);
        handle->data = nullptr;
        delete holder;
    }

    UvExecutor* ex = nullptr;
    uv_udp_t handle{};
    std::shared_ptr<State>* holder = nullptr;
    mutable std::mutex mu;
    std::deque<Datagram> pending;
    std::coroutine_handle<> recv_waiter{};
    std::coroutine_handle<> recv_some_waiter{};
    char* recv_some_data = nullptr;
    std::size_t recv_some_capacity = 0;
    DatagramInfo recv_some_result{};
    int recv_error = 0;
    bool recv_started = false;
    bool initialized = false;
    Endpoint local;
    Endpoint peer;
    std::atomic<bool> closing{false};
    std::atomic<bool> closed{false};
};

namespace {

class UdpRecvAwaiter {
public:
    UdpRecvAwaiter(std::shared_ptr<UdpSocket::State> state, async_simple::Slot* slot)
        : _state(std::move(state)), _slot(slot) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
        if (!_state) {
            _start_rc = UV_EBADF;
            return false;
        }

        auto register_cancel = [this, h]() -> bool {
            if (!_slot) {
                return true;
            }
            return async_simple::signalHelper{async_simple::Terminate}.tryEmplace(
                _slot,
                [state = _state, h](async_simple::SignalType, async_simple::Signal*) {
                    if (state && state->ex) {
                        (void)state->ex->schedule(
                            [state, h]() noexcept { state->cancel_recv_waiter(h); });
                    }
                });
        };

        auto register_waiter = [this, h]() -> bool {
            {
                std::lock_guard<std::mutex> lk(_state->mu);
                if (!_state->pending.empty() || !_state->open() ||
                    _state->recv_error != 0) {
                    return false;
                }
                if (_state->recv_waiter) {
                    _concurrent_recv = true;
                    return false;
                }
                _state->recv_waiter = h;
            }

            const int rc = _state->start_receiving_on_loop();
            if (rc != 0) {
                {
                    std::lock_guard<std::mutex> lk(_state->mu);
                    _state->recv_waiter = std::coroutine_handle<>{};
                    _state->recv_error = rc;
                }
                _start_rc = rc;
                return false;
            }
            return true;
        };

        if (!register_cancel()) {
            return false;
        }

        if (_state->ex->currentThreadInExecutor()) {
            return register_waiter();
        }

        const bool scheduled = _state->ex->schedule([this, h, register_waiter]() {
            if (!register_waiter()) {
                detail::resume_handle(_state->ex, h);
            }
        });
        if (!scheduled) {
            _start_rc = UV_ECANCELED;
            return false;
        }
        return true;
    }

    Datagram await_resume() {
        if (_concurrent_recv) {
            throw std::logic_error(
                "corouv::net::UdpSocket does not support concurrent recv calls");
        }
        async_simple::signalHelper{async_simple::Terminate}.checkHasCanceled(
            _slot, "corouv::net::UdpSocket recv canceled");
        if (_start_rc != 0) {
            throw_uv_error(_start_rc, "uv_udp_recv_start");
        }
        if (!_state) {
            throw std::logic_error("corouv::net::UdpSocket missing state");
        }

        std::lock_guard<std::mutex> lk(_state->mu);
        if (!_state->pending.empty()) {
            Datagram datagram = std::move(_state->pending.front());
            _state->pending.pop_front();
            return datagram;
        }
        if (_state->recv_error != 0 && _state->recv_error != UV_ECANCELED) {
            throw_uv_error(_state->recv_error, "uv_udp_recv");
        }
        throw std::logic_error("corouv::net::UdpSocket is closed");
    }

private:
    std::shared_ptr<UdpSocket::State> _state;
    async_simple::Slot* _slot = nullptr;
    int _start_rc = 0;
    bool _concurrent_recv = false;
};

class UdpRecvSomeAwaiter {
public:
    UdpRecvSomeAwaiter(std::shared_ptr<UdpSocket::State> state,
                       std::span<char> buffer, async_simple::Slot* slot)
        : _state(std::move(state)), _buffer(buffer), _slot(slot) {}

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
        if (!_state) {
            _start_rc = UV_EBADF;
            return false;
        }

        auto register_cancel = [this, h]() -> bool {
            if (!_slot) {
                return true;
            }
            return async_simple::signalHelper{async_simple::Terminate}.tryEmplace(
                _slot,
                [state = _state, h](async_simple::SignalType, async_simple::Signal*) {
                    if (state && state->ex) {
                        (void)state->ex->schedule([state, h]() noexcept {
                            state->cancel_recv_some_waiter(h);
                        });
                    }
                });
        };

        auto register_waiter = [this, h]() -> bool {
            {
                std::lock_guard<std::mutex> lk(_state->mu);
                if (!_state->pending.empty() || !_state->open() ||
                    _state->recv_error != 0) {
                    return false;
                }
                if (_state->recv_waiter || _state->recv_some_waiter) {
                    _concurrent_recv = true;
                    return false;
                }
                _state->recv_some_waiter = h;
                _state->recv_some_data = _buffer.data();
                _state->recv_some_capacity = _buffer.size();
            }

            const int rc = _state->start_receiving_on_loop();
            if (rc != 0) {
                {
                    std::lock_guard<std::mutex> lk(_state->mu);
                    _state->recv_some_waiter = std::coroutine_handle<>{};
                    _state->recv_some_data = nullptr;
                    _state->recv_some_capacity = 0;
                    _state->recv_error = rc;
                }
                _start_rc = rc;
                return false;
            }
            return true;
        };

        if (!register_cancel()) {
            return false;
        }

        if (_state->ex->currentThreadInExecutor()) {
            return register_waiter();
        }

        const bool scheduled = _state->ex->schedule([this, h, register_waiter]() {
            if (!register_waiter()) {
                detail::resume_handle(_state->ex, h);
            }
        });
        if (!scheduled) {
            _start_rc = UV_ECANCELED;
            return false;
        }
        return true;
    }

    DatagramInfo await_resume() {
        if (_concurrent_recv) {
            throw std::logic_error(
                "corouv::net::UdpSocket does not support concurrent recv calls");
        }
        async_simple::signalHelper{async_simple::Terminate}.checkHasCanceled(
            _slot, "corouv::net::UdpSocket recv_some canceled");
        if (_start_rc != 0) {
            throw_uv_error(_start_rc, "uv_udp_recv_start");
        }
        if (!_state) {
            throw std::logic_error("corouv::net::UdpSocket missing state");
        }

        std::lock_guard<std::mutex> lk(_state->mu);
        if (!_state->pending.empty()) {
            auto datagram = std::move(_state->pending.front());
            _state->pending.pop_front();

            DatagramInfo info;
            info.peer = std::move(datagram.peer);
            info.flags = datagram.flags;
            info.size = std::min<std::size_t>(_buffer.size(), datagram.payload.size());
            if (info.size > 0) {
                std::memcpy(_buffer.data(), datagram.payload.data(), info.size);
            }
            if (datagram.payload.size() > _buffer.size()) {
                info.flags |= UV_UDP_PARTIAL;
            }
            return info;
        }
        if (_state->recv_some_result.size > 0 || _state->recv_some_result.flags != 0 ||
            !_state->recv_some_result.peer.host.empty() ||
            _state->recv_some_result.peer.port != 0) {
            auto info = _state->recv_some_result;
            _state->recv_some_result = {};
            return info;
        }
        if (_state->recv_error != 0 && _state->recv_error != UV_ECANCELED) {
            throw_uv_error(_state->recv_error, "uv_udp_recv");
        }
        throw std::logic_error("corouv::net::UdpSocket is closed");
    }

private:
    std::shared_ptr<UdpSocket::State> _state;
    std::span<char> _buffer;
    async_simple::Slot* _slot = nullptr;
    int _start_rc = 0;
    bool _concurrent_recv = false;
};

class UdpSendAwaiter {
public:
    UdpSendAwaiter(std::shared_ptr<UdpSocket::State> state, std::string buffer,
                   std::optional<ResolvedAddress> address)
        : _state(std::move(state)),
          _buffer(std::move(buffer)),
          _address(std::move(address)) {}

    bool await_ready() const noexcept { return _buffer.empty(); }

    bool await_suspend(std::coroutine_handle<> h) {
        if (!_state) {
            _start_rc = UV_EBADF;
            return false;
        }

        _op = std::make_shared<SendOp>();
        _op->state = _state;
        _op->buffer = _buffer;
        _op->address = _address;
        _op->continuation = h;

        if (_state->ex->currentThreadInExecutor()) {
            return _op->start_on_loop();
        }

        const bool scheduled = _state->ex->schedule([op = _op]() {
            if (!op->start_on_loop()) {
                detail::resume_handle(op->state->ex, op->continuation);
            }
        });
        if (!scheduled) {
            _start_rc = UV_ECANCELED;
            return false;
        }
        return true;
    }

    std::size_t await_resume() {
        if (_start_rc != 0) {
            throw_uv_error(_start_rc, "uv_udp_send");
        }
        if (!_op) {
            return _buffer.size();
        }
        if (_op->start_rc != 0) {
            throw_uv_error(_op->start_rc, "uv_udp_send");
        }
        if (_op->status != 0) {
            throw_uv_error(_op->status, "uv_udp_send");
        }
        return _op->result;
    }

private:
    struct SendOp : std::enable_shared_from_this<SendOp> {
        bool start_on_loop() {
            if (!state || !state->open()) {
                start_rc = UV_ECANCELED;
                return false;
            }

            uv_buf_t buf = uv_buf_init(
                buffer.data(), static_cast<unsigned int>(buffer.size()));
            const sockaddr* addr =
                address ? reinterpret_cast<const sockaddr*>(&address->storage)
                        : nullptr;

            const int rc = uv_udp_try_send(&state->handle, &buf, 1, addr);
            if (rc >= 0) {
                result = static_cast<std::size_t>(rc);
                return false;
            }
            if (!write_would_block(rc)) {
                start_rc = rc;
                return false;
            }

            req.data = new std::shared_ptr<SendOp>(shared_from_this());
            const int send_rc =
                uv_udp_send(&req, &state->handle, &buf, 1, addr, &SendOp::on_send);
            if (send_rc != 0) {
                delete static_cast<std::shared_ptr<SendOp>*>(req.data);
                req.data = nullptr;
                start_rc = send_rc;
                return false;
            }
            return true;
        }

        static void on_send(uv_udp_send_t* req, int status) noexcept {
            auto* holder = static_cast<std::shared_ptr<SendOp>*>(req->data);
            auto self = *holder;
            delete holder;
            req->data = nullptr;

            self->status = status;
            if (status == 0) {
                self->result = self->buffer.size();
            }
            detail::resume_handle(self->state ? self->state->ex : nullptr,
                                  self->continuation);
        }

        std::shared_ptr<UdpSocket::State> state;
        uv_udp_send_t req{};
        std::string buffer;
        std::optional<ResolvedAddress> address;
        std::coroutine_handle<> continuation{};
        int start_rc = 0;
        int status = 0;
        std::size_t result = 0;
    };

    std::shared_ptr<UdpSocket::State> _state;
    std::string _buffer;
    std::optional<ResolvedAddress> _address;
    std::shared_ptr<SendOp> _op;
    int _start_rc = 0;
};

}  // namespace

UdpSocket::UdpSocket(std::shared_ptr<State> state) noexcept
    : _state(std::move(state)) {}

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept = default;
UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept = default;

bool UdpSocket::is_open() const noexcept { return _state && _state->open(); }

uv_os_sock_t UdpSocket::native_handle() const noexcept {
    if (!_state || !_state->open()) {
        return kInvalidSocket;
    }

    uv_os_fd_t fd = -1;
    if (uv_fileno(reinterpret_cast<const uv_handle_t*>(&_state->handle), &fd) != 0) {
        return kInvalidSocket;
    }
    return static_cast<uv_os_sock_t>(fd);
}

const Endpoint& UdpSocket::local_endpoint() const noexcept {
    static const Endpoint empty{};
    return _state ? _state->local : empty;
}

const Endpoint& UdpSocket::peer_endpoint() const noexcept {
    static const Endpoint empty{};
    return _state ? _state->peer : empty;
}

Task<void> UdpSocket::connect(std::string host, std::uint16_t port) {
    if (!is_open()) {
        throw std::logic_error("corouv::net::UdpSocket is closed");
    }

    const auto addresses =
        co_await resolve_udp(*_state->ex, host.empty() ? std::nullopt
                                                       : std::optional<std::string>(host),
                             port, false);

    std::optional<int> last_error;
    for (const auto& address : addresses) {
        const int rc = co_await LoopCallAwaiter(
            _state->ex, [state = _state, address]() {
                return uv_udp_connect(
                    &state->handle,
                    reinterpret_cast<const sockaddr*>(&address.storage));
            });
        if (rc == 0) {
            _state->refresh_local_endpoint();
            _state->refresh_peer_endpoint();
            co_return;
        }
        last_error = rc;
    }

    throw_uv_error(last_error.value_or(UV_EINVAL), "uv_udp_connect");
}

Task<std::size_t> UdpSocket::send(std::span<const char> buffer) {
    if (!is_open()) {
        throw std::logic_error("corouv::net::UdpSocket is closed");
    }
    if (_state->peer.host.empty()) {
        throw std::logic_error(
            "corouv::net::UdpSocket::send requires a connected socket");
    }
    co_return co_await UdpSendAwaiter(
        _state,
        std::string(buffer.data(), buffer.data() + buffer.size()),
        std::nullopt);
}

Task<std::size_t> UdpSocket::send(std::string_view data) {
    co_return co_await send(std::span<const char>(data.data(), data.size()));
}

Task<std::size_t> UdpSocket::send_to(std::span<const char> buffer,
                                     Endpoint endpoint) {
    if (!is_open()) {
        throw std::logic_error("corouv::net::UdpSocket is closed");
    }

    const auto addresses =
        co_await resolve_udp(*_state->ex, endpoint.host.empty() ? std::nullopt
                                                                : std::optional<std::string>(endpoint.host),
                             endpoint.port, false);
    co_return co_await UdpSendAwaiter(
        _state,
        std::string(buffer.data(), buffer.data() + buffer.size()),
        addresses.front());
}

Task<std::size_t> UdpSocket::send_to(std::string_view data, Endpoint endpoint) {
    co_return co_await send_to(std::span<const char>(data.data(), data.size()),
                               std::move(endpoint));
}

Task<Datagram> UdpSocket::recv() {
    if (!is_open()) {
        throw std::logic_error("corouv::net::UdpSocket is closed");
    }
    auto* slot = co_await async_simple::coro::CurrentSlot{};
    co_return co_await UdpRecvAwaiter(_state, slot);
}

Task<Datagram> UdpSocket::recv_from() { co_return co_await recv(); }

Task<DatagramInfo> UdpSocket::recv_some(std::span<char> buffer) {
    if (!is_open()) {
        throw std::logic_error("corouv::net::UdpSocket is closed");
    }
    auto* slot = co_await async_simple::coro::CurrentSlot{};
    co_return co_await UdpRecvSomeAwaiter(_state, buffer, slot);
}

Task<DatagramInfo> UdpSocket::recv_some_from(std::span<char> buffer) {
    co_return co_await recv_some(buffer);
}

void UdpSocket::set_broadcast(bool enabled) {
    if (!_state || !_state->open()) {
        return;
    }
    if (!_state->ex->currentThreadInExecutor()) {
        throw std::logic_error(
            "corouv::net::UdpSocket::set_broadcast must run on the loop thread");
    }
    const int rc = uv_udp_set_broadcast(&_state->handle, enabled ? 1 : 0);
    if (rc != 0) {
        throw_uv_error(rc, "uv_udp_set_broadcast");
    }
}

void UdpSocket::set_ttl(int ttl) {
    if (!_state || !_state->open()) {
        return;
    }
    if (!_state->ex->currentThreadInExecutor()) {
        throw std::logic_error(
            "corouv::net::UdpSocket::set_ttl must run on the loop thread");
    }
    const int rc = uv_udp_set_ttl(&_state->handle, ttl);
    if (rc != 0) {
        throw_uv_error(rc, "uv_udp_set_ttl");
    }
}

void UdpSocket::set_multicast_loop(bool enabled) {
    if (!_state || !_state->open()) {
        return;
    }
    if (!_state->ex->currentThreadInExecutor()) {
        throw std::logic_error(
            "corouv::net::UdpSocket::set_multicast_loop must run on the loop thread");
    }
    const int rc = uv_udp_set_multicast_loop(&_state->handle, enabled ? 1 : 0);
    if (rc != 0) {
        throw_uv_error(rc, "uv_udp_set_multicast_loop");
    }
}

void UdpSocket::set_multicast_ttl(int ttl) {
    if (!_state || !_state->open()) {
        return;
    }
    if (!_state->ex->currentThreadInExecutor()) {
        throw std::logic_error(
            "corouv::net::UdpSocket::set_multicast_ttl must run on the loop thread");
    }
    const int rc = uv_udp_set_multicast_ttl(&_state->handle, ttl);
    if (rc != 0) {
        throw_uv_error(rc, "uv_udp_set_multicast_ttl");
    }
}

void UdpSocket::set_multicast_interface(std::string interface_addr) {
    if (!_state || !_state->open()) {
        return;
    }
    if (!_state->ex->currentThreadInExecutor()) {
        throw std::logic_error(
            "corouv::net::UdpSocket::set_multicast_interface must run on the loop thread");
    }
    const int rc =
        uv_udp_set_multicast_interface(&_state->handle, interface_addr.c_str());
    if (rc != 0) {
        throw_uv_error(rc, "uv_udp_set_multicast_interface");
    }
}

void UdpSocket::join_multicast(std::string multicast_addr,
                               std::string interface_addr) {
    if (!_state || !_state->open()) {
        return;
    }
    if (!_state->ex->currentThreadInExecutor()) {
        throw std::logic_error(
            "corouv::net::UdpSocket::join_multicast must run on the loop thread");
    }
    const int rc =
        uv_udp_set_membership(&_state->handle, multicast_addr.c_str(),
                              interface_addr.c_str(), UV_JOIN_GROUP);
    if (rc != 0) {
        throw_uv_error(rc, "uv_udp_set_membership(join)");
    }
}

void UdpSocket::leave_multicast(std::string multicast_addr,
                                std::string interface_addr) {
    if (!_state || !_state->open()) {
        return;
    }
    if (!_state->ex->currentThreadInExecutor()) {
        throw std::logic_error(
            "corouv::net::UdpSocket::leave_multicast must run on the loop thread");
    }
    const int rc =
        uv_udp_set_membership(&_state->handle, multicast_addr.c_str(),
                              interface_addr.c_str(), UV_LEAVE_GROUP);
    if (rc != 0) {
        throw_uv_error(rc, "uv_udp_set_membership(leave)");
    }
}

void UdpSocket::join_multicast_source(std::string multicast_addr,
                                      std::string source_addr,
                                      std::string interface_addr) {
    if (!_state || !_state->open()) {
        return;
    }
    if (!_state->ex->currentThreadInExecutor()) {
        throw std::logic_error(
            "corouv::net::UdpSocket::join_multicast_source must run on the loop thread");
    }
    const int rc = uv_udp_set_source_membership(
        &_state->handle, multicast_addr.c_str(), interface_addr.c_str(),
        source_addr.c_str(), UV_JOIN_GROUP);
    if (rc != 0) {
        throw_uv_error(rc, "uv_udp_set_source_membership(join)");
    }
}

void UdpSocket::leave_multicast_source(std::string multicast_addr,
                                       std::string source_addr,
                                       std::string interface_addr) {
    if (!_state || !_state->open()) {
        return;
    }
    if (!_state->ex->currentThreadInExecutor()) {
        throw std::logic_error(
            "corouv::net::UdpSocket::leave_multicast_source must run on the loop thread");
    }
    const int rc = uv_udp_set_source_membership(
        &_state->handle, multicast_addr.c_str(), interface_addr.c_str(),
        source_addr.c_str(), UV_LEAVE_GROUP);
    if (rc != 0) {
        throw_uv_error(rc, "uv_udp_set_source_membership(leave)");
    }
}

void UdpSocket::close() noexcept {
    if (_state) {
        _state->request_close();
        _state.reset();
    }
}

Task<UdpSocket> bind(UvExecutor& ex, std::string host, std::uint16_t port,
                     UdpBindOptions options) {
    const auto addresses =
        co_await resolve_udp(ex, host.empty() ? std::nullopt
                                             : std::optional<std::string>(host),
                             port, true);

    std::optional<int> last_error;
    for (const auto& address : addresses) {
        auto state = std::make_shared<UdpSocket::State>(&ex);
        const int init_rc = co_await LoopCallAwaiter(
            &ex, [state]() { return state->init_on_loop(); });
        if (init_rc != 0) {
            last_error = init_rc;
            continue;
        }

        const int bind_rc = co_await LoopCallAwaiter(
            &ex, [state, address, options]() {
                return uv_udp_bind(
                    &state->handle,
                    reinterpret_cast<const sockaddr*>(&address.storage),
                    udp_bind_flags(options));
            });
        if (bind_rc == 0) {
            state->refresh_local_endpoint();
            state->refresh_peer_endpoint();
            co_return UdpSocket(std::move(state));
        }

        last_error = bind_rc;
        state->request_close();
    }

    throw_uv_error(last_error.value_or(UV_EINVAL), "uv_udp_bind");
}

Task<UdpSocket> bind(std::string host, std::uint16_t port,
                     UdpBindOptions options) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::net::bind requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await bind(*uvex, std::move(host), port, options);
}

}  // namespace corouv::net
