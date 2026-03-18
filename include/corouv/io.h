#pragma once

#include <uv.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "corouv/net.h"
#include "corouv/task.h"

namespace corouv::io {

class ByteStream;
class ByteListener;
class DatagramSocket;

using Endpoint = net::Endpoint;
using Datagram = net::Datagram;
using DatagramInfo = net::DatagramInfo;
using UdpBindOptions = net::UdpBindOptions;

namespace detail {

template <class Stream>
concept ReadableStream =
    requires(Stream& stream, std::span<char> buffer, std::size_t max_bytes) {
        { stream.native_handle() } -> std::same_as<uv_os_sock_t>;
        { stream.local_endpoint() } -> std::same_as<const net::Endpoint&>;
        { stream.peer_endpoint() } -> std::same_as<const net::Endpoint&>;
        { stream.close() } -> std::same_as<void>;
    } && (requires(Stream& stream) {
              { stream.is_open() } -> std::convertible_to<bool>;
          } || requires(Stream& stream) {
              { stream.open() } -> std::convertible_to<bool>;
          }) &&
    (requires(Stream& stream, std::span<char> buffer) {
        { stream.recv_some(buffer) } -> std::same_as<Task<std::size_t>>;
    } || requires(Stream& stream, std::span<char> buffer) {
        { stream.read_some(buffer) } -> std::same_as<Task<std::size_t>>;
    }) &&
    (requires(Stream& stream, std::size_t max_bytes) {
        { stream.recv_until_eof(max_bytes) } -> std::same_as<Task<std::string>>;
    } || requires(Stream& stream, std::size_t max_bytes) {
        { stream.read_until_eof(max_bytes) } -> std::same_as<Task<std::string>>;
    });

template <class Stream>
concept WritableStream =
    (requires(Stream& stream, std::span<const char> buffer,
              std::string_view data) {
        { stream.send_all(buffer) } -> std::same_as<Task<void>>;
        { stream.send_all(data) } -> std::same_as<Task<void>>;
    } || requires(Stream& stream, std::span<const char> buffer,
                   std::string_view data) {
        { stream.write_all(buffer) } -> std::same_as<Task<void>>;
        { stream.write_all(data) } -> std::same_as<Task<void>>;
    });

template <class Stream>
concept StreamSource =
    ReadableStream<Stream> && WritableStream<Stream> &&
    (!std::same_as<std::remove_cvref_t<Stream>, ByteStream>);

template <class Listener>
concept ListenerSource =
    requires(Listener& listener) {
        { listener.native_handle() } -> std::same_as<uv_os_sock_t>;
        { listener.local_endpoint() } -> std::same_as<const net::Endpoint&>;
        { listener.close() } -> std::same_as<void>;
    } && (requires(Listener& listener) {
              { listener.is_open() } -> std::convertible_to<bool>;
          } || requires(Listener& listener) {
              { listener.open() } -> std::convertible_to<bool>;
          }) &&
    requires(Listener& listener) {
        { listener.accept() };
    } &&
    (!std::same_as<std::remove_cvref_t<Listener>, ByteListener>);

template <class Socket>
concept DatagramSource =
    requires(Socket& socket, std::string host, std::uint16_t port,
             std::span<const char> out, Endpoint endpoint, std::span<char> in) {
        { socket.native_handle() } -> std::same_as<uv_os_sock_t>;
        { socket.local_endpoint() } -> std::same_as<const Endpoint&>;
        { socket.peer_endpoint() } -> std::same_as<const Endpoint&>;
        { socket.close() } -> std::same_as<void>;
        { socket.connect(std::move(host), port) } -> std::same_as<Task<void>>;
        { socket.send(out) } -> std::same_as<Task<std::size_t>>;
        { socket.send_to(out, endpoint) } -> std::same_as<Task<std::size_t>>;
        { socket.recv() } -> std::same_as<Task<Datagram>>;
        { socket.recv_from() } -> std::same_as<Task<Datagram>>;
        { socket.recv_some(in) } -> std::same_as<Task<DatagramInfo>>;
        { socket.recv_some_from(in) } -> std::same_as<Task<DatagramInfo>>;
    } && (requires(Socket& socket) {
              { socket.is_open() } -> std::convertible_to<bool>;
          } || requires(Socket& socket) {
              { socket.open() } -> std::convertible_to<bool>;
          }) &&
    (!std::same_as<std::remove_cvref_t<Socket>, DatagramSocket>);

template <class Resource>
bool is_open(Resource& resource) noexcept {
    if constexpr (requires(Resource& r) { r.is_open(); }) {
        return resource.is_open();
    } else {
        return resource.open();
    }
}

template <class Stream>
Task<std::size_t> recv_some(Stream& stream, std::span<char> buffer) {
    if constexpr (requires(Stream& s, std::span<char> b) {
                      s.recv_some(b);
                  }) {
        co_return co_await stream.recv_some(buffer);
    } else {
        co_return co_await stream.read_some(buffer);
    }
}

template <class Stream>
Task<void> send_all(Stream& stream, std::span<const char> buffer) {
    if constexpr (requires(Stream& s, std::span<const char> b) {
                      s.send_all(b);
                  }) {
        co_await stream.send_all(buffer);
    } else {
        co_await stream.write_all(buffer);
    }
}

template <class Stream>
Task<void> send_all(Stream& stream, std::string_view data) {
    if constexpr (requires(Stream& s, std::string_view d) {
                      s.send_all(d);
                  }) {
        co_await stream.send_all(data);
    } else {
        co_await stream.write_all(data);
    }
}

template <class Stream>
Task<std::string> recv_until_eof(Stream& stream, std::size_t max_bytes) {
    if constexpr (requires(Stream& s, std::size_t limit) {
                      s.recv_until_eof(limit);
                  }) {
        co_return co_await stream.recv_until_eof(max_bytes);
    } else {
        co_return co_await stream.read_until_eof(max_bytes);
    }
}

template <class Stream>
void shutdown_send(Stream& stream) noexcept {
    if constexpr (requires(Stream& s) { s.shutdown_send(); }) {
        stream.shutdown_send();
    } else if constexpr (requires(Stream& s) { s.shutdown_write(); }) {
        stream.shutdown_write();
    }
}

template <class Listener>
Task<ByteStream> accept_as_stream(Listener& listener) {
    auto stream = co_await listener.accept();
    co_return ByteStream(std::move(stream));
}

template <class Socket>
Task<void> connect_datagram(Socket& socket, std::string host,
                            std::uint16_t port) {
    co_await socket.connect(std::move(host), port);
}

template <class Socket>
Task<std::size_t> send_datagram(Socket& socket, std::span<const char> buffer) {
    co_return co_await socket.send(buffer);
}

template <class Socket>
Task<std::size_t> send_datagram_to(Socket& socket, std::span<const char> buffer,
                                   Endpoint endpoint) {
    co_return co_await socket.send_to(buffer, std::move(endpoint));
}

template <class Socket>
Task<Datagram> recv_datagram(Socket& socket) {
    co_return co_await socket.recv();
}

template <class Socket>
Task<Datagram> recv_datagram_from(Socket& socket) {
    co_return co_await socket.recv_from();
}

template <class Socket>
Task<DatagramInfo> recv_datagram_some(Socket& socket, std::span<char> buffer) {
    co_return co_await socket.recv_some(buffer);
}

template <class Socket>
Task<DatagramInfo> recv_datagram_some_from(Socket& socket,
                                           std::span<char> buffer) {
    co_return co_await socket.recv_some_from(buffer);
}

template <class Socket>
void require_datagram_feature(bool supported, const char* feature) {
    if (!supported) {
        throw std::logic_error(std::string("corouv::io::DatagramSocket backend does not support ") +
                               feature);
    }
}

template <class Socket>
void set_broadcast(Socket& socket, bool enabled) {
    if constexpr (requires(Socket& s) { s.set_broadcast(enabled); }) {
        socket.set_broadcast(enabled);
    } else {
        require_datagram_feature<Socket>(false, "set_broadcast");
    }
}

template <class Socket>
void set_ttl(Socket& socket, int ttl) {
    if constexpr (requires(Socket& s) { s.set_ttl(ttl); }) {
        socket.set_ttl(ttl);
    } else {
        require_datagram_feature<Socket>(false, "set_ttl");
    }
}

template <class Socket>
void set_multicast_loop(Socket& socket, bool enabled) {
    if constexpr (requires(Socket& s) { s.set_multicast_loop(enabled); }) {
        socket.set_multicast_loop(enabled);
    } else {
        require_datagram_feature<Socket>(false, "set_multicast_loop");
    }
}

template <class Socket>
void set_multicast_ttl(Socket& socket, int ttl) {
    if constexpr (requires(Socket& s) { s.set_multicast_ttl(ttl); }) {
        socket.set_multicast_ttl(ttl);
    } else {
        require_datagram_feature<Socket>(false, "set_multicast_ttl");
    }
}

template <class Socket>
void set_multicast_interface(Socket& socket, std::string interface_addr) {
    if constexpr (requires(Socket& s) {
                      s.set_multicast_interface(std::move(interface_addr));
                  }) {
        socket.set_multicast_interface(std::move(interface_addr));
    } else {
        require_datagram_feature<Socket>(false, "set_multicast_interface");
    }
}

template <class Socket>
void join_multicast(Socket& socket, std::string multicast_addr,
                    std::string interface_addr) {
    if constexpr (requires(Socket& s) {
                      s.join_multicast(std::move(multicast_addr),
                                       std::move(interface_addr));
                  }) {
        socket.join_multicast(std::move(multicast_addr),
                              std::move(interface_addr));
    } else {
        require_datagram_feature<Socket>(false, "join_multicast");
    }
}

template <class Socket>
void leave_multicast(Socket& socket, std::string multicast_addr,
                     std::string interface_addr) {
    if constexpr (requires(Socket& s) {
                      s.leave_multicast(std::move(multicast_addr),
                                        std::move(interface_addr));
                  }) {
        socket.leave_multicast(std::move(multicast_addr),
                               std::move(interface_addr));
    } else {
        require_datagram_feature<Socket>(false, "leave_multicast");
    }
}

template <class Socket>
void join_multicast_source(Socket& socket, std::string multicast_addr,
                           std::string source_addr,
                           std::string interface_addr) {
    if constexpr (requires(Socket& s) {
                      s.join_multicast_source(std::move(multicast_addr),
                                              std::move(source_addr),
                                              std::move(interface_addr));
                  }) {
        socket.join_multicast_source(std::move(multicast_addr),
                                     std::move(source_addr),
                                     std::move(interface_addr));
    } else {
        require_datagram_feature<Socket>(false, "join_multicast_source");
    }
}

template <class Socket>
void leave_multicast_source(Socket& socket, std::string multicast_addr,
                            std::string source_addr,
                            std::string interface_addr) {
    if constexpr (requires(Socket& s) {
                      s.leave_multicast_source(std::move(multicast_addr),
                                               std::move(source_addr),
                                               std::move(interface_addr));
                  }) {
        socket.leave_multicast_source(std::move(multicast_addr),
                                      std::move(source_addr),
                                      std::move(interface_addr));
    } else {
        require_datagram_feature<Socket>(false, "leave_multicast_source");
    }
}

}  // namespace detail

class ByteStream {
public:
    ByteStream() = default;

    template <detail::StreamSource Stream>
    ByteStream(Stream&& stream)
        : _self(std::make_unique<Model<std::remove_cvref_t<Stream>>>(
              std::forward<Stream>(stream))) {}

    ByteStream(const ByteStream&) = delete;
    ByteStream& operator=(const ByteStream&) = delete;
    ByteStream(ByteStream&&) noexcept = default;
    ByteStream& operator=(ByteStream&&) noexcept = default;

    [[nodiscard]] bool is_open() const noexcept {
        return _self != nullptr && _self->is_open();
    }

    [[nodiscard]] uv_os_sock_t native_handle() const noexcept {
        return _self != nullptr ? _self->native_handle()
                                : static_cast<uv_os_sock_t>(-1);
    }

    [[nodiscard]] const net::Endpoint& local_endpoint() const {
        ensure("local_endpoint");
        return _self->local_endpoint();
    }

    [[nodiscard]] const net::Endpoint& peer_endpoint() const {
        ensure("peer_endpoint");
        return _self->peer_endpoint();
    }

    Task<std::size_t> recv_some(std::span<char> buffer) {
        ensure("recv_some");
        co_return co_await _self->recv_some(buffer);
    }

    Task<void> send_all(std::span<const char> buffer) {
        ensure("send_all");
        co_await _self->send_all(buffer);
    }

    Task<void> send_all(std::string_view data) {
        ensure("send_all");
        co_await _self->send_all(data);
    }

    Task<std::string> recv_until_eof(std::size_t max_bytes = 16 * 1024 * 1024) {
        ensure("recv_until_eof");
        co_return co_await _self->recv_until_eof(max_bytes);
    }

    void shutdown_send() noexcept {
        if (_self != nullptr) {
            _self->shutdown_send();
        }
    }

    void close() noexcept {
        if (_self != nullptr) {
            _self->close();
        }
    }

private:
    struct Concept {
        virtual ~Concept() = default;

        virtual bool is_open() const noexcept = 0;
        virtual uv_os_sock_t native_handle() const noexcept = 0;
        virtual const net::Endpoint& local_endpoint() const = 0;
        virtual const net::Endpoint& peer_endpoint() const = 0;
        virtual Task<std::size_t> recv_some(std::span<char> buffer) = 0;
        virtual Task<void> send_all(std::span<const char> buffer) = 0;
        virtual Task<void> send_all(std::string_view data) = 0;
        virtual Task<std::string> recv_until_eof(std::size_t max_bytes) = 0;
        virtual void shutdown_send() noexcept = 0;
        virtual void close() noexcept = 0;
    };

    template <class Stream>
    struct Model final : Concept {
        explicit Model(Stream stream) : stream(std::move(stream)) {}

        bool is_open() const noexcept override {
            return detail::is_open(stream);
        }

        uv_os_sock_t native_handle() const noexcept override {
            return stream.native_handle();
        }

        const net::Endpoint& local_endpoint() const override {
            return stream.local_endpoint();
        }

        const net::Endpoint& peer_endpoint() const override {
            return stream.peer_endpoint();
        }

        Task<std::size_t> recv_some(std::span<char> buffer) override {
            co_return co_await detail::recv_some(stream, buffer);
        }

        Task<void> send_all(std::span<const char> buffer) override {
            co_await detail::send_all(stream, buffer);
        }

        Task<void> send_all(std::string_view data) override {
            co_await detail::send_all(stream, data);
        }

        Task<std::string> recv_until_eof(std::size_t max_bytes) override {
            co_return co_await detail::recv_until_eof(stream, max_bytes);
        }

        void shutdown_send() noexcept override {
            detail::shutdown_send(stream);
        }

        void close() noexcept override { stream.close(); }

        Stream stream;
    };

    void ensure(const char* operation) const {
        if (_self == nullptr) {
            throw std::logic_error(std::string("corouv::io::ByteStream cannot ") +
                                   operation + " on an empty stream");
        }
    }

    std::unique_ptr<Concept> _self;
};

class ByteListener {
public:
    ByteListener() = default;

    template <detail::ListenerSource Listener>
    ByteListener(Listener&& listener)
        : _self(std::make_unique<Model<std::remove_cvref_t<Listener>>>(
              std::forward<Listener>(listener))) {}

    ByteListener(const ByteListener&) = delete;
    ByteListener& operator=(const ByteListener&) = delete;
    ByteListener(ByteListener&&) noexcept = default;
    ByteListener& operator=(ByteListener&&) noexcept = default;

    [[nodiscard]] bool is_open() const noexcept {
        return _self != nullptr && _self->is_open();
    }

    [[nodiscard]] uv_os_sock_t native_handle() const noexcept {
        return _self != nullptr ? _self->native_handle()
                                : static_cast<uv_os_sock_t>(-1);
    }

    [[nodiscard]] const net::Endpoint& local_endpoint() const {
        ensure("local_endpoint");
        return _self->local_endpoint();
    }

    Task<ByteStream> accept() {
        ensure("accept");
        co_return co_await _self->accept();
    }

    void close() noexcept {
        if (_self != nullptr) {
            _self->close();
        }
    }

private:
    struct Concept {
        virtual ~Concept() = default;

        virtual bool is_open() const noexcept = 0;
        virtual uv_os_sock_t native_handle() const noexcept = 0;
        virtual const net::Endpoint& local_endpoint() const = 0;
        virtual Task<ByteStream> accept() = 0;
        virtual void close() noexcept = 0;
    };

    template <class Listener>
    struct Model final : Concept {
        explicit Model(Listener listener) : listener(std::move(listener)) {}

        bool is_open() const noexcept override {
            return detail::is_open(listener);
        }

        uv_os_sock_t native_handle() const noexcept override {
            return listener.native_handle();
        }

        const net::Endpoint& local_endpoint() const override {
            return listener.local_endpoint();
        }

        Task<ByteStream> accept() override {
            co_return co_await detail::accept_as_stream(listener);
        }

        void close() noexcept override { listener.close(); }

        Listener listener;
    };

    void ensure(const char* operation) const {
        if (_self == nullptr) {
            throw std::logic_error(std::string("corouv::io::ByteListener cannot ") +
                                   operation + " on an empty listener");
        }
    }

    std::unique_ptr<Concept> _self;
};

class DatagramSocket {
public:
    DatagramSocket() = default;

    template <detail::DatagramSource Socket>
    DatagramSocket(Socket&& socket)
        : _self(std::make_unique<Model<std::remove_cvref_t<Socket>>>(
              std::forward<Socket>(socket))) {}

    DatagramSocket(const DatagramSocket&) = delete;
    DatagramSocket& operator=(const DatagramSocket&) = delete;
    DatagramSocket(DatagramSocket&&) noexcept = default;
    DatagramSocket& operator=(DatagramSocket&&) noexcept = default;

    [[nodiscard]] bool is_open() const noexcept {
        return _self != nullptr && _self->is_open();
    }

    [[nodiscard]] uv_os_sock_t native_handle() const noexcept {
        return _self != nullptr ? _self->native_handle()
                                : static_cast<uv_os_sock_t>(-1);
    }

    [[nodiscard]] const Endpoint& local_endpoint() const {
        ensure("local_endpoint");
        return _self->local_endpoint();
    }

    [[nodiscard]] const Endpoint& peer_endpoint() const {
        ensure("peer_endpoint");
        return _self->peer_endpoint();
    }

    Task<void> connect(std::string host, std::uint16_t port) {
        ensure("connect");
        co_await _self->connect(std::move(host), port);
    }

    Task<std::size_t> send(std::span<const char> buffer) {
        ensure("send");
        co_return co_await _self->send(buffer);
    }

    Task<std::size_t> send(std::string_view data) {
        co_return co_await send(std::span<const char>(data.data(), data.size()));
    }

    Task<std::size_t> send(const char* data) {
        co_return co_await send(std::string_view(data));
    }

    Task<std::size_t> send_to(std::span<const char> buffer, Endpoint endpoint) {
        ensure("send_to");
        co_return co_await _self->send_to(buffer, std::move(endpoint));
    }

    Task<std::size_t> send_to(std::string_view data, Endpoint endpoint) {
        co_return co_await send_to(
            std::span<const char>(data.data(), data.size()), std::move(endpoint));
    }

    Task<std::size_t> send_to(const char* data, Endpoint endpoint) {
        co_return co_await send_to(std::string_view(data), std::move(endpoint));
    }

    Task<Datagram> recv() {
        ensure("recv");
        co_return co_await _self->recv();
    }

    Task<Datagram> recv_from() {
        ensure("recv_from");
        co_return co_await _self->recv_from();
    }

    Task<DatagramInfo> recv_some(std::span<char> buffer) {
        ensure("recv_some");
        co_return co_await _self->recv_some(buffer);
    }

    Task<DatagramInfo> recv_some_from(std::span<char> buffer) {
        ensure("recv_some_from");
        co_return co_await _self->recv_some_from(buffer);
    }

    void set_broadcast(bool enabled = true) {
        ensure("set_broadcast");
        _self->set_broadcast(enabled);
    }

    void set_ttl(int ttl) {
        ensure("set_ttl");
        _self->set_ttl(ttl);
    }

    void set_multicast_loop(bool enabled = true) {
        ensure("set_multicast_loop");
        _self->set_multicast_loop(enabled);
    }

    void set_multicast_ttl(int ttl) {
        ensure("set_multicast_ttl");
        _self->set_multicast_ttl(ttl);
    }

    void set_multicast_interface(std::string interface_addr) {
        ensure("set_multicast_interface");
        _self->set_multicast_interface(std::move(interface_addr));
    }

    void join_multicast(std::string multicast_addr,
                        std::string interface_addr = "0.0.0.0") {
        ensure("join_multicast");
        _self->join_multicast(std::move(multicast_addr),
                              std::move(interface_addr));
    }

    void leave_multicast(std::string multicast_addr,
                         std::string interface_addr = "0.0.0.0") {
        ensure("leave_multicast");
        _self->leave_multicast(std::move(multicast_addr),
                               std::move(interface_addr));
    }

    void join_multicast_source(std::string multicast_addr,
                               std::string source_addr,
                               std::string interface_addr = "0.0.0.0") {
        ensure("join_multicast_source");
        _self->join_multicast_source(std::move(multicast_addr),
                                     std::move(source_addr),
                                     std::move(interface_addr));
    }

    void leave_multicast_source(std::string multicast_addr,
                                std::string source_addr,
                                std::string interface_addr = "0.0.0.0") {
        ensure("leave_multicast_source");
        _self->leave_multicast_source(std::move(multicast_addr),
                                      std::move(source_addr),
                                      std::move(interface_addr));
    }

    void close() noexcept {
        if (_self != nullptr) {
            _self->close();
        }
    }

private:
    struct Concept {
        virtual ~Concept() = default;

        virtual bool is_open() const noexcept = 0;
        virtual uv_os_sock_t native_handle() const noexcept = 0;
        virtual const Endpoint& local_endpoint() const = 0;
        virtual const Endpoint& peer_endpoint() const = 0;
        virtual Task<void> connect(std::string host, std::uint16_t port) = 0;
        virtual Task<std::size_t> send(std::span<const char> buffer) = 0;
        virtual Task<std::size_t> send_to(std::span<const char> buffer,
                                          Endpoint endpoint) = 0;
        virtual Task<Datagram> recv() = 0;
        virtual Task<Datagram> recv_from() = 0;
        virtual Task<DatagramInfo> recv_some(std::span<char> buffer) = 0;
        virtual Task<DatagramInfo> recv_some_from(std::span<char> buffer) = 0;
        virtual void set_broadcast(bool enabled) = 0;
        virtual void set_ttl(int ttl) = 0;
        virtual void set_multicast_loop(bool enabled) = 0;
        virtual void set_multicast_ttl(int ttl) = 0;
        virtual void set_multicast_interface(std::string interface_addr) = 0;
        virtual void join_multicast(std::string multicast_addr,
                                    std::string interface_addr) = 0;
        virtual void leave_multicast(std::string multicast_addr,
                                     std::string interface_addr) = 0;
        virtual void join_multicast_source(std::string multicast_addr,
                                           std::string source_addr,
                                           std::string interface_addr) = 0;
        virtual void leave_multicast_source(std::string multicast_addr,
                                            std::string source_addr,
                                            std::string interface_addr) = 0;
        virtual void close() noexcept = 0;
    };

    template <class Socket>
    struct Model final : Concept {
        explicit Model(Socket socket) : socket(std::move(socket)) {}

        bool is_open() const noexcept override {
            return detail::is_open(socket);
        }

        uv_os_sock_t native_handle() const noexcept override {
            return socket.native_handle();
        }

        const Endpoint& local_endpoint() const override {
            return socket.local_endpoint();
        }

        const Endpoint& peer_endpoint() const override {
            return socket.peer_endpoint();
        }

        Task<void> connect(std::string host, std::uint16_t port) override {
            co_await detail::connect_datagram(socket, std::move(host), port);
        }

        Task<std::size_t> send(std::span<const char> buffer) override {
            co_return co_await detail::send_datagram(socket, buffer);
        }

        Task<std::size_t> send_to(std::span<const char> buffer,
                                  Endpoint endpoint) override {
            co_return co_await detail::send_datagram_to(socket, buffer,
                                                        std::move(endpoint));
        }

        Task<Datagram> recv() override {
            co_return co_await detail::recv_datagram(socket);
        }

        Task<Datagram> recv_from() override {
            co_return co_await detail::recv_datagram_from(socket);
        }

        Task<DatagramInfo> recv_some(std::span<char> buffer) override {
            co_return co_await detail::recv_datagram_some(socket, buffer);
        }

        Task<DatagramInfo> recv_some_from(std::span<char> buffer) override {
            co_return co_await detail::recv_datagram_some_from(socket, buffer);
        }

        void set_broadcast(bool enabled) override {
            detail::set_broadcast(socket, enabled);
        }

        void set_ttl(int ttl) override { detail::set_ttl(socket, ttl); }

        void set_multicast_loop(bool enabled) override {
            detail::set_multicast_loop(socket, enabled);
        }

        void set_multicast_ttl(int ttl) override {
            detail::set_multicast_ttl(socket, ttl);
        }

        void set_multicast_interface(std::string interface_addr) override {
            detail::set_multicast_interface(socket, std::move(interface_addr));
        }

        void join_multicast(std::string multicast_addr,
                            std::string interface_addr) override {
            detail::join_multicast(socket, std::move(multicast_addr),
                                   std::move(interface_addr));
        }

        void leave_multicast(std::string multicast_addr,
                             std::string interface_addr) override {
            detail::leave_multicast(socket, std::move(multicast_addr),
                                    std::move(interface_addr));
        }

        void join_multicast_source(std::string multicast_addr,
                                   std::string source_addr,
                                   std::string interface_addr) override {
            detail::join_multicast_source(socket, std::move(multicast_addr),
                                          std::move(source_addr),
                                          std::move(interface_addr));
        }

        void leave_multicast_source(std::string multicast_addr,
                                    std::string source_addr,
                                    std::string interface_addr) override {
            detail::leave_multicast_source(socket, std::move(multicast_addr),
                                           std::move(source_addr),
                                           std::move(interface_addr));
        }

        void close() noexcept override { socket.close(); }

        Socket socket;
    };

    void ensure(const char* operation) const {
        if (_self == nullptr) {
            throw std::logic_error(
                std::string("corouv::io::DatagramSocket cannot ") + operation +
                " on an empty socket");
        }
    }

    std::unique_ptr<Concept> _self;
};

inline Task<ByteStream> connect(UvExecutor& ex, std::string host,
                                std::uint16_t port) {
    co_return ByteStream(co_await net::connect(ex, std::move(host), port));
}

inline Task<ByteStream> connect(std::string host, std::uint16_t port) {
    co_return ByteStream(co_await net::connect(std::move(host), port));
}

inline Task<ByteListener> listen(UvExecutor& ex, std::string host,
                                 std::uint16_t port, int backlog = 128) {
    co_return ByteListener(
        co_await net::listen(ex, std::move(host), port, backlog));
}

inline Task<ByteListener> listen(std::string host, std::uint16_t port,
                                 int backlog = 128) {
    co_return ByteListener(
        co_await net::listen(std::move(host), port, backlog));
}

inline Task<DatagramSocket> bind(UvExecutor& ex, std::string host,
                                 std::uint16_t port,
                                 UdpBindOptions options = {}) {
    co_return DatagramSocket(
        co_await net::bind(ex, std::move(host), port, std::move(options)));
}

inline Task<DatagramSocket> bind(std::string host, std::uint16_t port,
                                 UdpBindOptions options = {}) {
    co_return DatagramSocket(
        co_await net::bind(std::move(host), port, std::move(options)));
}

}  // namespace corouv::io
