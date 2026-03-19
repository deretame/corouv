#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "corouv/executor.h"
#include "corouv/io.h"
#include "corouv/net.h"
#include "corouv/task.h"

namespace corouv::http {

struct Header {
    std::string name;
    std::string value;
};

using Headers = std::vector<Header>;

std::optional<std::string_view> find_header(const Headers& headers,
                                            std::string_view name);
bool header_contains_token(const Headers& headers, std::string_view name,
                           std::string_view token);
void append_header(Headers& headers, std::string name, std::string value);
void set_header(Headers& headers, std::string name, std::string value);
void erase_header(Headers& headers, std::string_view name);

std::string reason_phrase(int status);

struct Limits {
    std::size_t max_header_bytes{64 * 1024};
    std::size_t max_body_bytes{8 * 1024 * 1024};
    std::size_t max_header_count{100};
};

class Error : public std::runtime_error {
public:
    Error(int status, std::string message)
        : std::runtime_error(std::move(message)), _status(status) {}

    [[nodiscard]] int status() const noexcept { return _status; }

private:
    int _status = 500;
};

struct Request {
    std::string method{"GET"};
    std::string target{"/"};
    Headers headers;
    std::string body;
    Headers trailers;
    int version_minor{1};
    bool keep_alive{true};
    bool chunked{false};
};

struct Response {
    int status{200};
    std::string reason;
    Headers headers;
    std::string body;
    Headers trailers;
    int version_minor{1};
    bool keep_alive{true};
    bool chunked{false};
};

struct FormField {
    std::string name;
    std::string value;
};

using FormFields = std::vector<FormField>;
// Streaming body callback used by read_*_stream APIs.
// Called with each decoded body chunk.
using BodySink = std::function<Task<void>(std::string_view)>;
// Streaming body producer used by write_*_stream APIs.
// Return std::nullopt to signal end-of-body.
using BodyChunkSource = std::function<Task<std::optional<std::string>>()>;

// Optional per-operation timeouts.
// - read_headers: request/response head parse stage.
// - read_body: body read stage (fixed/chunked/close-delimited).
// - write: header/body writes.
struct IoTimeouts {
    std::optional<std::chrono::milliseconds> read_headers;
    std::optional<std::chrono::milliseconds> read_body;
    std::optional<std::chrono::milliseconds> write;
};

// Form helpers for application/x-www-form-urlencoded.
bool is_form_urlencoded_content_type(std::string_view content_type);
std::string form_urlencode(std::string_view value);
std::string form_urldecode(std::string_view value);
FormFields parse_form_urlencoded(std::string_view body,
                                 std::size_t max_fields = 1024);
FormFields parse_form_urlencoded_request(const Request& request,
                                         std::size_t max_fields = 1024);
std::string serialize_form_urlencoded(const FormFields& fields);
std::optional<std::string_view> find_form_value(const FormFields& fields,
                                                std::string_view name) noexcept;
std::vector<std::string_view> find_form_values(const FormFields& fields,
                                               std::string_view name) noexcept;

class Connection {
public:
    explicit Connection(io::ByteStream stream, Limits limits = {},
                        IoTimeouts timeouts = {});

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) noexcept = default;
    Connection& operator=(Connection&&) noexcept = default;

    // Read and buffer the full request body into Request::body.
    Task<std::optional<Request>> read_request();
    // Stream request body through on_chunk, leaving Request::body empty.
    Task<std::optional<Request>> read_request_stream(BodySink on_chunk);
    // Read and buffer the full response body into Response::body.
    Task<Response> read_response(std::string_view request_method = {});
    // Stream response body through on_chunk, leaving Response::body empty.
    Task<Response> read_response_stream(std::string_view request_method,
                                        BodySink on_chunk);
    // Write request with Request::body as fixed payload (or chunked when
    // request.chunked=true).
    Task<void> write_request(const Request& request,
                             std::string_view default_host = {});
    // Write request body from body_source as chunked transfer-encoding.
    Task<void> write_request_stream(const Request& request,
                                    BodyChunkSource body_source,
                                    std::string_view default_host = {});
    // Write response with Response::body as fixed payload (or chunked when
    // response.chunked=true).
    Task<void> write_response(const Response& response,
                              std::string_view request_method = {});
    // Write response body from body_source as chunked transfer-encoding when
    // the status/method permits a response body.
    Task<void> write_response_stream(const Response& response,
                                     BodyChunkSource body_source,
                                     std::string_view request_method = {});

    [[nodiscard]] bool is_open() const noexcept;
    io::ByteStream& stream() noexcept { return _stream; }
    const io::ByteStream& stream() const noexcept { return _stream; }
    void close() noexcept;

private:
    Task<Response> read_response_stream_impl(std::string_view request_method,
                                             BodySink on_chunk,
                                             bool skip_informational);

    io::ByteStream _stream;
    Limits _limits;
    IoTimeouts _timeouts;
    std::string _buffer;
    std::size_t _buffer_offset{0};
    std::optional<Response> _prefetched_response;
};

struct ServerOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
    int backlog{128};
    Limits limits{};
    IoTimeouts timeouts{};
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
    Task<void> handle_client(io::ByteStream stream);

    UvExecutor* _ex = nullptr;
    Handler _handler;
    ServerOptions _options;
    std::optional<io::ByteListener> _listener;
};

struct ClientOptions {
    Limits limits{};
    bool keep_alive{true};
    IoTimeouts timeouts{};
};

class Client {
public:
    explicit Client(UvExecutor& ex, ClientOptions options = {});

    Task<void> connect(std::string host, std::uint16_t port);
    Task<Response> request(Request request);

    [[nodiscard]] bool is_connected() const noexcept;
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

struct Url {
    std::string scheme{"http"};
    std::string host;
    std::uint16_t port{80};
    std::string target{"/"};
};

Url parse_url(std::string_view url);

Task<Response> fetch(UvExecutor& ex, std::string_view url,
                     Request request = {}, ClientOptions options = {});
Task<Response> fetch(std::string_view url, Request request = {},
                     ClientOptions options = {});

}  // namespace corouv::http
