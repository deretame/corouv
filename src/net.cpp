#include "corouv/net.h"

#include <async_simple/Executor.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "corouv/poll.h"
#include "corouv/timer.h"
#include "corouv/uv_error.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace corouv::net {

namespace {

#ifdef _WIN32
using SocketLen = int;
constexpr uv_os_sock_t kInvalidSocket = INVALID_SOCKET;
constexpr int kShutdownWrite = SD_SEND;
#else
using SocketLen = socklen_t;
constexpr uv_os_sock_t kInvalidSocket = -1;
constexpr int kShutdownWrite = SHUT_WR;
#endif

struct ResolvedAddress {
    int family = AF_UNSPEC;
    int socktype = SOCK_STREAM;
    int protocol = 0;
    sockaddr_storage storage{};
    SocketLen length = 0;
};

void throw_socket_error(int err, const char* what) {
    throw_uv_error(uv_translate_sys_error(err), what);
}

int last_socket_error() noexcept {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool is_interrupted(int err) noexcept {
#ifdef _WIN32
    return err == WSAEINTR;
#else
    return err == EINTR;
#endif
}

bool would_block(int err) noexcept {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

bool connect_in_progress(int err) noexcept {
#ifdef _WIN32
    return err == WSAEINPROGRESS || err == WSAEWOULDBLOCK ||
           err == WSAEALREADY;
#else
    return err == EINPROGRESS || err == EALREADY || would_block(err);
#endif
}

void close_socket(uv_os_sock_t fd) noexcept {
    if (fd == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    (void)closesocket(fd);
#else
    (void)::close(fd);
#endif
}

void set_nonblocking(uv_os_sock_t fd) {
#ifdef _WIN32
    u_long enabled = 1;
    if (ioctlsocket(fd, FIONBIO, &enabled) != 0) {
        throw_socket_error(last_socket_error(), "ioctlsocket");
    }
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw_socket_error(last_socket_error(), "fcntl(F_GETFL)");
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw_socket_error(last_socket_error(), "fcntl(F_SETFL)");
    }
#endif
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

Endpoint endpoint_from_socket_name(uv_os_sock_t fd, bool peer) {
    sockaddr_storage storage{};
    SocketLen len = sizeof(storage);
    const int rc = peer
                       ? getpeername(fd, reinterpret_cast<sockaddr*>(&storage),
                                     &len)
                       : getsockname(fd, reinterpret_cast<sockaddr*>(&storage),
                                     &len);
    if (rc != 0) {
        throw_socket_error(last_socket_error(),
                           peer ? "getpeername" : "getsockname");
    }
    return endpoint_from_sockaddr(reinterpret_cast<const sockaddr*>(&storage));
}

template <class T>
T socket_get_option(uv_os_sock_t fd, int level, int optname,
                    const char* what) {
    T value{};
    SocketLen len = sizeof(value);
    if (getsockopt(fd, level, optname, reinterpret_cast<char*>(&value), &len) !=
        0) {
        throw_socket_error(last_socket_error(), what);
    }
    return value;
}

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
            if (ai->ai_addr == nullptr) {
                continue;
            }

            ResolvedAddress entry;
            entry.family = ai->ai_family;
            entry.socktype = ai->ai_socktype;
            entry.protocol = ai->ai_protocol;
            entry.length = static_cast<SocketLen>(ai->ai_addrlen);
            std::memcpy(&entry.storage, ai->ai_addr, ai->ai_addrlen);
            out.push_back(entry);
        }

        if (_result != nullptr) {
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

int send_flags() noexcept {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

std::string connect_error_message(std::string_view host, std::uint16_t port,
                                  int err) {
    std::string out = "corouv::net::connect ";
    out += std::string(host);
    out += ":";
    out += std::to_string(port);
    out += " failed: ";
    out += uv_strerror(uv_translate_sys_error(err));
    return out;
}

}  // namespace

std::string to_string(const Endpoint& endpoint) {
    if (endpoint.host.find(':') != std::string::npos) {
        return "[" + endpoint.host + "]:" + std::to_string(endpoint.port);
    }
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

TcpStream::TcpStream(UvExecutor* ex, uv_os_sock_t fd) noexcept
    : _ex(ex), _fd(fd) {}

TcpStream::~TcpStream() { close(); }

TcpStream::TcpStream(TcpStream&& other) noexcept
    : _ex(std::exchange(other._ex, nullptr)),
      _fd(std::exchange(other._fd, kInvalidSocket)),
      _local(std::move(other._local)),
      _peer(std::move(other._peer)) {}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) {
        close();
        _ex = std::exchange(other._ex, nullptr);
        _fd = std::exchange(other._fd, kInvalidSocket);
        _local = std::move(other._local);
        _peer = std::move(other._peer);
    }
    return *this;
}

bool TcpStream::open() const noexcept { return _fd != kInvalidSocket; }

uv_os_sock_t TcpStream::native_handle() const noexcept { return _fd; }

void TcpStream::refresh_endpoints() {
    if (!open()) {
        _local = {};
        _peer = {};
        return;
    }
    _local = endpoint_from_socket_name(_fd, false);
    _peer = endpoint_from_socket_name(_fd, true);
}

Task<std::size_t> TcpStream::read_some(std::span<char> buffer) {
    if (!open()) {
        throw std::logic_error("corouv::net::TcpStream is closed");
    }
    if (!_ex) {
        throw std::logic_error("corouv::net::TcpStream missing executor");
    }
    if (buffer.empty()) {
        co_return 0;
    }

    for (;;) {
#ifdef _WIN32
        const int cap = static_cast<int>(
            std::min<std::size_t>(buffer.size(), static_cast<std::size_t>(INT_MAX)));
        const int rc = recv(_fd, buffer.data(), cap, 0);
        if (rc > 0) {
            co_return static_cast<std::size_t>(rc);
        }
        if (rc == 0) {
            co_return 0;
        }
#else
        const ssize_t rc = ::recv(_fd, buffer.data(), buffer.size(), 0);
        if (rc > 0) {
            co_return static_cast<std::size_t>(rc);
        }
        if (rc == 0) {
            co_return 0;
        }
#endif

        const int err = last_socket_error();
        if (is_interrupted(err)) {
            continue;
        }
        if (would_block(err)) {
            co_await corouv::poll::readable(*_ex, _fd);
            continue;
        }
        throw_socket_error(err, "recv");
    }
}

Task<std::size_t> TcpStream::write_some(std::span<const char> buffer) {
    if (!open()) {
        throw std::logic_error("corouv::net::TcpStream is closed");
    }
    if (!_ex) {
        throw std::logic_error("corouv::net::TcpStream missing executor");
    }
    if (buffer.empty()) {
        co_return 0;
    }

    for (;;) {
#ifdef _WIN32
        const int cap = static_cast<int>(
            std::min<std::size_t>(buffer.size(), static_cast<std::size_t>(INT_MAX)));
        const int rc = send(_fd, buffer.data(), cap, 0);
        if (rc >= 0) {
            co_return static_cast<std::size_t>(rc);
        }
#else
        const ssize_t rc = ::send(_fd, buffer.data(), buffer.size(), send_flags());
        if (rc >= 0) {
            co_return static_cast<std::size_t>(rc);
        }
#endif

        const int err = last_socket_error();
        if (is_interrupted(err)) {
            continue;
        }
        if (would_block(err)) {
            co_await corouv::poll::writable(*_ex, _fd);
            continue;
        }
        throw_socket_error(err, "send");
    }
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
    if (!open()) {
        return;
    }
    const int value = enabled ? 1 : 0;
    if (setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&value),
                   sizeof(value)) != 0) {
        throw_socket_error(last_socket_error(), "setsockopt(TCP_NODELAY)");
    }
}

void TcpStream::set_keepalive(bool enabled, unsigned int delay_seconds) {
    if (!open()) {
        return;
    }
    const int value = enabled ? 1 : 0;
    if (setsockopt(_fd, SOL_SOCKET, SO_KEEPALIVE,
                   reinterpret_cast<const char*>(&value),
                   sizeof(value)) != 0) {
        throw_socket_error(last_socket_error(), "setsockopt(SO_KEEPALIVE)");
    }

#if !defined(_WIN32) && defined(TCP_KEEPIDLE)
    if (enabled) {
        const int delay = static_cast<int>(delay_seconds);
        if (setsockopt(_fd, IPPROTO_TCP, TCP_KEEPIDLE,
                       reinterpret_cast<const char*>(&delay),
                       sizeof(delay)) != 0) {
            throw_socket_error(last_socket_error(), "setsockopt(TCP_KEEPIDLE)");
        }
    }
#else
    (void)delay_seconds;
#endif
}

void TcpStream::shutdown_write() noexcept {
    if (!open()) {
        return;
    }
    if (shutdown(_fd, kShutdownWrite) != 0) {
        const int err = last_socket_error();
        if (!is_interrupted(err) && !would_block(err)) {
            return;
        }
    }
}

void TcpStream::close() noexcept {
    if (!open()) {
        return;
    }
    close_socket(std::exchange(_fd, kInvalidSocket));
    _local = {};
    _peer = {};
}

TcpListener::TcpListener(UvExecutor* ex, uv_os_sock_t fd) noexcept
    : _ex(ex), _fd(fd) {}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : _ex(std::exchange(other._ex, nullptr)),
      _fd(std::exchange(other._fd, kInvalidSocket)),
      _local(std::move(other._local)) {}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        _ex = std::exchange(other._ex, nullptr);
        _fd = std::exchange(other._fd, kInvalidSocket);
        _local = std::move(other._local);
    }
    return *this;
}

bool TcpListener::open() const noexcept { return _fd != kInvalidSocket; }

uv_os_sock_t TcpListener::native_handle() const noexcept { return _fd; }

void TcpListener::refresh_local_endpoint() {
    if (!open()) {
        _local = {};
        return;
    }
    _local = endpoint_from_socket_name(_fd, false);
}

Task<TcpStream> TcpListener::accept() {
    if (!open()) {
        throw std::logic_error("corouv::net::TcpListener is closed");
    }
    if (!_ex) {
        throw std::logic_error("corouv::net::TcpListener missing executor");
    }

    for (;;) {
        sockaddr_storage storage{};
        SocketLen len = sizeof(storage);
        const uv_os_sock_t client =
            ::accept(_fd, reinterpret_cast<sockaddr*>(&storage), &len);
        if (client != kInvalidSocket) {
            set_nonblocking(client);

            TcpStream stream(_ex, client);
            stream._peer =
                endpoint_from_sockaddr(reinterpret_cast<const sockaddr*>(&storage));
            stream._local = endpoint_from_socket_name(client, false);
            try {
                stream.set_nodelay(true);
            } catch (...) {
            }
            co_return std::move(stream);
        }

        const int err = last_socket_error();
        if (is_interrupted(err)) {
            continue;
        }
        if (would_block(err)) {
            // Using a short timer here makes explicit listener shutdowns wake
            // accept loops reliably even when the underlying poll backend does
            // not surface a readable/error event for a closed listening socket.
            co_await corouv::sleep_for(std::chrono::milliseconds(1));
            if (!open()) {
                throw std::logic_error("corouv::net::TcpListener is closed");
            }
            continue;
        }
        throw_socket_error(err, "accept");
    }
}

void TcpListener::close() noexcept {
    if (!open()) {
        return;
    }
    close_socket(std::exchange(_fd, kInvalidSocket));
    _local = {};
}

Task<TcpStream> connect(UvExecutor& ex, std::string host, std::uint16_t port) {
    const auto addresses =
        co_await resolve_tcp(ex, host.empty() ? std::nullopt
                                             : std::optional<std::string>(host),
                             port, false);

    std::optional<std::string> last_error;

    for (const auto& address : addresses) {
        const uv_os_sock_t fd =
            ::socket(address.family, address.socktype, address.protocol);
        if (fd == kInvalidSocket) {
            const int err = last_socket_error();
            last_error = connect_error_message(host, port, err);
            continue;
        }

        try {
            set_nonblocking(fd);
        } catch (...) {
            close_socket(fd);
            throw;
        }

        const int rc =
            ::connect(fd, reinterpret_cast<const sockaddr*>(&address.storage),
                      address.length);
        if (rc == 0) {
            TcpStream stream(&ex, fd);
            stream.refresh_endpoints();
            try {
                stream.set_nodelay(true);
            } catch (...) {
            }
            co_return std::move(stream);
        }

        const int err = last_socket_error();
        if (!connect_in_progress(err)) {
            last_error = connect_error_message(host, port, err);
            close_socket(fd);
            continue;
        }

        try {
            co_await corouv::poll::writable(ex, fd);
        } catch (...) {
            close_socket(fd);
            throw;
        }

        const int socket_err =
            socket_get_option<int>(fd, SOL_SOCKET, SO_ERROR, "getsockopt(SO_ERROR)");
        if (socket_err != 0) {
            last_error = connect_error_message(host, port, socket_err);
            close_socket(fd);
            continue;
        }

        TcpStream stream(&ex, fd);
        stream.refresh_endpoints();
        try {
            stream.set_nodelay(true);
        } catch (...) {
        }
        co_return std::move(stream);
    }

    if (last_error.has_value()) {
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
        const uv_os_sock_t fd =
            ::socket(address.family, address.socktype, address.protocol);
        if (fd == kInvalidSocket) {
            last_error = "socket: ";
            last_error->append(
                uv_strerror(uv_translate_sys_error(last_socket_error())));
            continue;
        }

        try {
            set_nonblocking(fd);
        } catch (...) {
            close_socket(fd);
            throw;
        }

        const int reuse_addr = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<const char*>(&reuse_addr),
                         sizeof(reuse_addr));

        if (::bind(fd, reinterpret_cast<const sockaddr*>(&address.storage),
                   address.length) != 0) {
            const int err = last_socket_error();
            last_error = "bind: ";
            last_error->append(uv_strerror(uv_translate_sys_error(err)));
            close_socket(fd);
            continue;
        }

        if (::listen(fd, backlog) != 0) {
            const int err = last_socket_error();
            last_error = "listen: ";
            last_error->append(uv_strerror(uv_translate_sys_error(err)));
            close_socket(fd);
            continue;
        }

        TcpListener listener(&ex, fd);
        listener.refresh_local_endpoint();
        co_return std::move(listener);
    }

    if (last_error.has_value()) {
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

}  // namespace corouv::net
