#pragma once

#include <uv.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "corouv/executor.h"
#include "corouv/task.h"

namespace corouv::net {

struct Endpoint {
    std::string host;
    std::uint16_t port{0};
};

std::string to_string(const Endpoint& endpoint);

class TcpStream {
public:
    TcpStream() = default;
    ~TcpStream();

    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;

    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] uv_os_sock_t native_handle() const noexcept;

    [[nodiscard]] const Endpoint& local_endpoint() const noexcept {
        return _local;
    }
    [[nodiscard]] const Endpoint& peer_endpoint() const noexcept {
        return _peer;
    }

    Task<std::size_t> read_some(std::span<char> buffer);
    Task<std::size_t> write_some(std::span<const char> buffer);
    Task<void> write_all(std::span<const char> buffer);
    Task<void> write_all(std::string_view data);
    Task<std::string> read_until_eof(
        std::size_t max_bytes = 16 * 1024 * 1024);

    void set_nodelay(bool enabled = true);
    void set_keepalive(bool enabled = true, unsigned int delay_seconds = 60);
    void shutdown_write() noexcept;
    void close() noexcept;

private:
    friend Task<TcpStream> connect(UvExecutor&, std::string, std::uint16_t);
    friend Task<TcpStream> connect(std::string, std::uint16_t);
    friend class TcpListener;

    explicit TcpStream(UvExecutor* ex, uv_os_sock_t fd) noexcept;
    void refresh_endpoints();

    UvExecutor* _ex = nullptr;
    uv_os_sock_t _fd{static_cast<uv_os_sock_t>(-1)};
    Endpoint _local;
    Endpoint _peer;
};

class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] uv_os_sock_t native_handle() const noexcept;
    [[nodiscard]] const Endpoint& local_endpoint() const noexcept {
        return _local;
    }

    Task<TcpStream> accept();
    void close() noexcept;

private:
    friend Task<TcpListener> listen(UvExecutor&, std::string, std::uint16_t,
                                    int);
    friend Task<TcpListener> listen(std::string, std::uint16_t, int);

    explicit TcpListener(UvExecutor* ex, uv_os_sock_t fd) noexcept;
    void refresh_local_endpoint();

    UvExecutor* _ex = nullptr;
    uv_os_sock_t _fd{static_cast<uv_os_sock_t>(-1)};
    Endpoint _local;
};

Task<TcpStream> connect(UvExecutor& ex, std::string host, std::uint16_t port);
Task<TcpStream> connect(std::string host, std::uint16_t port);

Task<TcpListener> listen(UvExecutor& ex, std::string host, std::uint16_t port,
                         int backlog = 128);
Task<TcpListener> listen(std::string host, std::uint16_t port,
                         int backlog = 128);

}  // namespace corouv::net
