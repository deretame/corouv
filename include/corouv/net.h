#pragma once

#include <uv.h>

#include <compare>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "corouv/executor.h"
#include "corouv/task.h"

namespace corouv::net {

struct Endpoint {
    std::string host;
    std::uint16_t port{0};

    auto operator<=>(const Endpoint&) const = default;
};

std::string to_string(const Endpoint& endpoint);

struct Datagram {
    Endpoint peer;
    std::string payload;
    unsigned flags{0};

    [[nodiscard]] bool truncated() const noexcept {
        return (flags & UV_UDP_PARTIAL) != 0;
    }
};

struct DatagramInfo {
    Endpoint peer;
    std::size_t size{0};
    unsigned flags{0};

    [[nodiscard]] bool truncated() const noexcept {
        return (flags & UV_UDP_PARTIAL) != 0;
    }
};

struct UdpBindOptions {
    bool ipv6_only{false};
    bool reuse_address{false};
    bool reuse_port{false};
    bool receive_errors{false};
};

class TcpStream {
public:
    struct State;

    TcpStream() = default;
    ~TcpStream();

    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] uv_os_sock_t native_handle() const noexcept;

    [[nodiscard]] const Endpoint& local_endpoint() const noexcept;
    [[nodiscard]] const Endpoint& peer_endpoint() const noexcept;

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
    explicit TcpStream(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> _state;
};

class TcpListener {
public:
    struct State;

    TcpListener() = default;
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] uv_os_sock_t native_handle() const noexcept;
    [[nodiscard]] const Endpoint& local_endpoint() const noexcept;

    Task<TcpStream> accept();
    void close() noexcept;

private:
    friend Task<TcpListener> listen(UvExecutor&, std::string, std::uint16_t,
                                    int);
    friend Task<TcpListener> listen(std::string, std::uint16_t, int);
    explicit TcpListener(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> _state;
};

Task<TcpStream> connect(UvExecutor& ex, std::string host, std::uint16_t port);
Task<TcpStream> connect(std::string host, std::uint16_t port);

Task<TcpListener> listen(UvExecutor& ex, std::string host, std::uint16_t port,
                         int backlog = 128);
Task<TcpListener> listen(std::string host, std::uint16_t port,
                         int backlog = 128);

class UdpSocket {
public:
    struct State;

    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] uv_os_sock_t native_handle() const noexcept;

    [[nodiscard]] const Endpoint& local_endpoint() const noexcept;
    [[nodiscard]] const Endpoint& peer_endpoint() const noexcept;

    Task<void> connect(std::string host, std::uint16_t port);
    Task<std::size_t> send(std::span<const char> buffer);
    Task<std::size_t> send(std::string_view data);
    Task<std::size_t> send(const char* data) {
        co_return co_await send(std::string_view(data));
    }
    Task<std::size_t> send_to(std::span<const char> buffer, Endpoint endpoint);
    Task<std::size_t> send_to(std::string_view data, Endpoint endpoint);
    Task<std::size_t> send_to(const char* data, Endpoint endpoint) {
        co_return co_await send_to(std::string_view(data), std::move(endpoint));
    }
    Task<Datagram> recv();
    Task<Datagram> recv_from();
    Task<DatagramInfo> recv_some(std::span<char> buffer);
    Task<DatagramInfo> recv_some_from(std::span<char> buffer);

    void set_broadcast(bool enabled = true);
    void set_ttl(int ttl);
    void set_multicast_loop(bool enabled = true);
    void set_multicast_ttl(int ttl);
    void set_multicast_interface(std::string interface_addr);
    void join_multicast(std::string multicast_addr,
                        std::string interface_addr = "0.0.0.0");
    void leave_multicast(std::string multicast_addr,
                         std::string interface_addr = "0.0.0.0");
    void join_multicast_source(std::string multicast_addr, std::string source_addr,
                               std::string interface_addr = "0.0.0.0");
    void leave_multicast_source(std::string multicast_addr,
                                std::string source_addr,
                                std::string interface_addr = "0.0.0.0");

    void close() noexcept;

private:
    friend Task<UdpSocket> bind(UvExecutor&, std::string, std::uint16_t,
                                UdpBindOptions);
    friend Task<UdpSocket> bind(std::string, std::uint16_t, UdpBindOptions);
    explicit UdpSocket(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> _state;
};

Task<UdpSocket> bind(UvExecutor& ex, std::string host, std::uint16_t port,
                     UdpBindOptions options = {});
Task<UdpSocket> bind(std::string host, std::uint16_t port,
                     UdpBindOptions options = {});

}  // namespace corouv::net
