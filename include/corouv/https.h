#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "corouv/http.h"
#include "corouv/transport.h"

namespace corouv::https {

using Header = corouv::http::Header;
using Headers = corouv::http::Headers;
using Limits = corouv::http::Limits;
using Error = corouv::http::Error;
using Request = corouv::http::Request;
using Response = corouv::http::Response;
using Url = corouv::http::Url;

class Connection {
public:
    explicit Connection(transport::CodecStream stream, Limits limits = {});

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) noexcept = default;
    Connection& operator=(Connection&&) noexcept = default;

    Task<std::optional<Request>> read_request();
    Task<Response> read_response(std::string_view request_method = {});
    Task<void> write_request(const Request& request,
                             std::string_view default_host = {});
    Task<void> write_response(const Response& response,
                              std::string_view request_method = {});

    [[nodiscard]] bool open() const noexcept;
    transport::CodecStream& stream() noexcept { return _stream; }
    const transport::CodecStream& stream() const noexcept { return _stream; }
    void close() noexcept;

private:
    transport::CodecStream _stream;
    Limits _limits;
    std::string _buffer;
    std::size_t _buffer_offset{0};
};

struct ClientOptions {
    Limits limits{};
    transport::TlsClientConfig tls;
    bool keep_alive{true};
};

class Client {
public:
    explicit Client(UvExecutor& ex, ClientOptions options = {});

    Task<void> connect(std::string host, std::uint16_t port);
    Task<Response> request(Request request);

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] const std::string& host() const noexcept { return _host; }
    [[nodiscard]] std::uint16_t port() const noexcept { return _port; }

    void close() noexcept;

private:
    UvExecutor* _ex = nullptr;
    ClientOptions _options;
    std::string _host;
    std::uint16_t _port{0};
    std::unique_ptr<Connection> _connection;
};

struct ServerOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
    int backlog{128};
    Limits limits{};
    transport::TlsServerConfig tls;
};

class Server {
public:
    using Handler = std::function<Task<Response>(Request)>;

    Server(UvExecutor& ex, Handler handler, ServerOptions options = {});

    Task<void> listen();
    Task<void> serve();
    void close() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::string host() const;

private:
    Task<void> handle_client(net::TcpStream stream);

    UvExecutor* _ex = nullptr;
    Handler _handler;
    ServerOptions _options;
    std::optional<net::TcpListener> _listener;
};

Task<Response> fetch(UvExecutor& ex, std::string_view url,
                     Request request = {}, ClientOptions options = {});
Task<Response> fetch(std::string_view url, Request request = {},
                     ClientOptions options = {});

}  // namespace corouv::https
