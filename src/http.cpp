#include "corouv/http.h"

#include <async_simple/Executor.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "corouv/task_group.h"
#include "picohttpparser.h"

namespace corouv::http {

namespace {

class BufferCursor {
public:
    BufferCursor(std::string& storage, std::size_t& offset) noexcept
        : _storage(storage), _offset(offset) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return _storage.size() - _offset;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(_storage.data() + _offset, size());
    }

    void append(const char* data, std::size_t len) { _storage.append(data, len); }

    void consume(std::size_t len) {
        _offset += len;
        compact();
    }

    std::string take(std::size_t len) {
        const std::string out(_storage.data() + _offset, len);
        consume(len);
        return out;
    }

    [[nodiscard]] std::size_t find(std::string_view needle) const noexcept {
        return view().find(needle);
    }

private:
    void compact() {
        if (_offset == 0) {
            return;
        }
        if (_offset >= _storage.size()) {
            _storage.clear();
            _offset = 0;
            return;
        }
        if (_offset > 4096 && _offset * 2 >= _storage.size()) {
            _storage.erase(0, _offset);
            _offset = 0;
        }
    }

    std::string& _storage;
    std::size_t& _offset;
};

bool iequals(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(lhs[i]);
        const unsigned char b = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

std::string trim_copy(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();

    while (begin < end &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

std::optional<std::size_t> parse_content_length(const Headers& headers) {
    std::optional<std::size_t> parsed;

    for (const auto& header : headers) {
        if (!iequals(header.name, "Content-Length")) {
            continue;
        }

        const auto value = trim_copy(header.value);
        std::size_t current = 0;
        const auto* begin = value.data();
        const auto* end = value.data() + value.size();
        const auto [ptr, ec] = std::from_chars(begin, end, current, 10);
        if (ec != std::errc{} || ptr != end) {
            throw Error(400, "invalid Content-Length");
        }

        if (parsed.has_value() && *parsed != current) {
            throw Error(400, "conflicting Content-Length headers");
        }
        parsed = current;
    }

    return parsed;
}

bool message_is_chunked(const Headers& headers) {
    return header_contains_token(headers, "Transfer-Encoding", "chunked");
}

bool should_keep_alive(const Headers& headers, int version_minor) {
    if (header_contains_token(headers, "Connection", "close")) {
        return false;
    }
    if (header_contains_token(headers, "Connection", "keep-alive")) {
        return true;
    }
    return version_minor >= 1;
}

bool response_has_body(int status, std::string_view request_method) {
    if (iequals(request_method, "HEAD")) {
        return false;
    }
    if (status >= 100 && status < 200) {
        return false;
    }
    return status != 204 && status != 304;
}

Headers copy_headers(const phr_header* raw, std::size_t count) {
    Headers headers;
    headers.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        if (raw[i].name == nullptr) {
            if (headers.empty()) {
                continue;
            }
            headers.back().value.append(" ");
            headers.back().value.append(raw[i].value, raw[i].value_len);
            continue;
        }

        headers.push_back(Header{
            std::string(raw[i].name, raw[i].name_len),
            std::string(raw[i].value, raw[i].value_len),
        });
    }

    return headers;
}

Task<std::size_t> read_more(net::TcpStream& stream, BufferCursor& buffer,
                            std::size_t max_buffered, int error_status,
                            const char* message) {
    std::array<char, 4096> scratch{};
    const auto n = co_await stream.read_some(
        std::span<char>(scratch.data(), scratch.size()));
    if (n == 0) {
        co_return 0;
    }
    if (buffer.size() + n > max_buffered) {
        throw Error(error_status, message);
    }
    buffer.append(scratch.data(), n);
    co_return n;
}

Task<void> ensure_buffer(net::TcpStream& stream, BufferCursor& buffer,
                         std::size_t wanted, std::size_t max_buffered,
                         int error_status, const char* message,
                         const char* eof_message) {
    while (buffer.size() < wanted) {
        const auto n =
            co_await read_more(stream, buffer, max_buffered, error_status, message);
        if (n == 0) {
            throw Error(400, eof_message);
        }
    }
    co_return;
}

Task<std::string> read_line_crlf(net::TcpStream& stream, BufferCursor& buffer,
                                 std::size_t max_line, int error_status,
                                 const char* too_large_message,
                                 const char* eof_message) {
    for (;;) {
        const auto pos = buffer.find("\r\n");
        if (pos != std::string_view::npos) {
            const std::string out(buffer.view().substr(0, pos));
            buffer.consume(pos + 2);
            co_return out;
        }

        if (buffer.size() >= max_line) {
            throw Error(error_status, too_large_message);
        }

        const auto n =
            co_await read_more(stream, buffer, max_line, error_status,
                               too_large_message);
        if (n == 0) {
            throw Error(400, eof_message);
        }
    }
}

Task<std::string> read_fixed_body(net::TcpStream& stream, BufferCursor& buffer,
                                  const Limits& limits, std::size_t size) {
    if (size > limits.max_body_bytes) {
        throw Error(413, "HTTP body too large");
    }

    co_await ensure_buffer(stream, buffer, size, size, 413, "HTTP body too large",
                           "unexpected eof while reading HTTP body");
    co_return buffer.take(size);
}

Task<std::string> read_chunked_body(net::TcpStream& stream, BufferCursor& buffer,
                                    const Limits& limits) {
    std::string body;

    for (;;) {
        const auto line =
            co_await read_line_crlf(stream, buffer, limits.max_header_bytes, 431,
                                    "chunk header too large",
                                    "unexpected eof while reading chunk header");

        const auto semi = line.find(';');
        const auto chunk_size_text =
            trim_copy(line.substr(0, semi == std::string::npos ? line.size() : semi));

        std::size_t chunk_size = 0;
        const auto [ptr, ec] =
            std::from_chars(chunk_size_text.data(),
                            chunk_size_text.data() + chunk_size_text.size(),
                            chunk_size, 16);
        if (ec != std::errc{} ||
            ptr != chunk_size_text.data() + chunk_size_text.size()) {
            throw Error(400, "invalid chunk size");
        }

        if (chunk_size == 0) {
            for (;;) {
                const auto trailer = co_await read_line_crlf(
                    stream, buffer, limits.max_header_bytes, 431,
                    "trailer headers too large",
                    "unexpected eof while reading trailer headers");
                if (trailer.empty()) {
                    co_return body;
                }
            }
        }

        if (body.size() + chunk_size > limits.max_body_bytes) {
            throw Error(413, "HTTP body too large");
        }

        const std::size_t max_buffered =
            std::min(limits.max_body_bytes - body.size() + 2,
                     limits.max_body_bytes + limits.max_header_bytes);
        co_await ensure_buffer(stream, buffer, chunk_size + 2, max_buffered, 413,
                               "HTTP body too large",
                               "unexpected eof while reading chunk data");

        body.append(buffer.view().data(), chunk_size);
        buffer.consume(chunk_size);

        const auto tail = buffer.view();
        if (tail.size() < 2 || tail[0] != '\r' || tail[1] != '\n') {
            throw Error(400, "invalid chunk terminator");
        }
        buffer.consume(2);
    }
}

Task<std::string> read_until_close_body(net::TcpStream& stream,
                                        BufferCursor& buffer,
                                        const Limits& limits) {
    std::string body(buffer.view());
    buffer.consume(buffer.size());

    std::array<char, 4096> scratch{};
    while (true) {
        const auto n = co_await stream.read_some(
            std::span<char>(scratch.data(), scratch.size()));
        if (n == 0) {
            stream.close();
            break;
        }
        if (body.size() + n > limits.max_body_bytes) {
            throw Error(413, "HTTP body too large");
        }
        body.append(scratch.data(), n);
    }

    co_return body;
}

std::string format_host_header(std::string_view host, std::uint16_t port) {
    const bool bracket = host.find(':') != std::string_view::npos;
    if (port == 80) {
        return bracket ? "[" + std::string(host) + "]" : std::string(host);
    }
    return net::to_string(net::Endpoint{std::string(host), port});
}

void prepare_outgoing_headers(Headers& headers, bool keep_alive, bool chunked,
                              std::size_t body_size) {
    erase_header(headers, "Connection");
    erase_header(headers, "Content-Length");
    erase_header(headers, "Transfer-Encoding");

    set_header(headers, "Connection", keep_alive ? "keep-alive" : "close");

    if (chunked) {
        set_header(headers, "Transfer-Encoding", "chunked");
    } else {
        set_header(headers, "Content-Length", std::to_string(body_size));
    }
}

Task<void> write_chunked_body(net::TcpStream& stream, std::string_view body) {
    if (body.empty()) {
        co_await stream.write_all(std::string_view("0\r\n\r\n"));
        co_return;
    }

    char chunk_header[64] = {0};
    const int n = std::snprintf(chunk_header, sizeof(chunk_header), "%zx\r\n",
                                static_cast<std::size_t>(body.size()));
    co_await stream.write_all(
        std::string_view(chunk_header, static_cast<std::size_t>(n)));
    co_await stream.write_all(body);
    co_await stream.write_all(std::string_view("\r\n0\r\n\r\n"));
}

std::string serialize_headers(const Headers& headers) {
    std::string out;
    for (const auto& header : headers) {
        out.append(header.name);
        out.append(": ");
        out.append(header.value);
        out.append("\r\n");
    }
    return out;
}

}  // namespace

std::optional<std::string_view> find_header(const Headers& headers,
                                            std::string_view name) {
    for (const auto& header : headers) {
        if (iequals(header.name, name)) {
            return header.value;
        }
    }
    return std::nullopt;
}

bool header_contains_token(const Headers& headers, std::string_view name,
                           std::string_view token) {
    for (const auto& header : headers) {
        if (!iequals(header.name, name)) {
            continue;
        }

        std::string_view remaining = header.value;
        while (!remaining.empty()) {
            const auto comma = remaining.find(',');
            const auto part =
                remaining.substr(0, comma == std::string_view::npos
                                        ? remaining.size()
                                        : comma);
            if (iequals(trim_copy(part), token)) {
                return true;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(comma + 1);
        }
    }
    return false;
}

void append_header(Headers& headers, std::string name, std::string value) {
    headers.push_back(Header{std::move(name), std::move(value)});
}

void set_header(Headers& headers, std::string name, std::string value) {
    erase_header(headers, name);
    headers.push_back(Header{std::move(name), std::move(value)});
}

void erase_header(Headers& headers, std::string_view name) {
    std::erase_if(headers, [&](const auto& header) {
        return iequals(header.name, name);
    });
}

std::string reason_phrase(int status) {
    switch (status) {
        case 100:
            return "Continue";
        case 101:
            return "Switching Protocols";
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 202:
            return "Accepted";
        case 204:
            return "No Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 408:
            return "Request Timeout";
        case 409:
            return "Conflict";
        case 411:
            return "Length Required";
        case 413:
            return "Payload Too Large";
        case 414:
            return "URI Too Long";
        case 415:
            return "Unsupported Media Type";
        case 418:
            return "I'm a teapot";
        case 422:
            return "Unprocessable Content";
        case 426:
            return "Upgrade Required";
        case 429:
            return "Too Many Requests";
        case 431:
            return "Request Header Fields Too Large";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 502:
            return "Bad Gateway";
        case 503:
            return "Service Unavailable";
        case 504:
            return "Gateway Timeout";
        default:
            return "Unknown";
    }
}

Connection::Connection(net::TcpStream stream, Limits limits)
    : _stream(std::move(stream)), _limits(limits) {}

bool Connection::open() const noexcept { return _stream.open(); }

void Connection::close() noexcept { _stream.close(); }

Task<std::optional<Request>> Connection::read_request() {
    BufferCursor buffer(_buffer, _buffer_offset);
    std::size_t last_len = 0;

    while (true) {
        auto view = buffer.view();
        if (!view.empty()) {
            std::vector<phr_header> raw(_limits.max_header_count);
            std::size_t num_headers = raw.size();
            const char* method = nullptr;
            const char* path = nullptr;
            std::size_t method_len = 0;
            std::size_t path_len = 0;
            int minor_version = 1;

            const int parsed = phr_parse_request(
                view.data(), view.size(), &method, &method_len, &path, &path_len,
                &minor_version, raw.data(), &num_headers, last_len);
            if (parsed > 0) {
                Request request;
                request.method.assign(method, method_len);
                request.target.assign(path, path_len);
                request.version_minor = minor_version;
                request.headers = copy_headers(raw.data(), num_headers);
                request.keep_alive =
                    should_keep_alive(request.headers, request.version_minor);
                request.chunked = message_is_chunked(request.headers);

                buffer.consume(static_cast<std::size_t>(parsed));

                if (request.chunked) {
                    request.body =
                        co_await read_chunked_body(_stream, buffer, _limits);
                } else if (const auto len =
                               parse_content_length(request.headers);
                           len.has_value()) {
                    request.body =
                        co_await read_fixed_body(_stream, buffer, _limits, *len);
                }

                co_return request;
            }

            if (parsed == -1) {
                throw Error(400, "invalid HTTP request");
            }
        }

        if (buffer.size() >= _limits.max_header_bytes) {
            throw Error(431, "request headers too large");
        }

        last_len = buffer.size();
        const auto n = co_await read_more(_stream, buffer, _limits.max_header_bytes,
                                          431, "request headers too large");
        if (n == 0) {
            if (buffer.empty()) {
                co_return std::nullopt;
            }
            throw Error(400, "unexpected eof while reading request");
        }
    }
}

Task<Response> Connection::read_response(std::string_view request_method) {
    BufferCursor buffer(_buffer, _buffer_offset);
    std::size_t last_len = 0;

    while (true) {
        auto view = buffer.view();
        if (!view.empty()) {
            std::vector<phr_header> raw(_limits.max_header_count);
            std::size_t num_headers = raw.size();
            const char* msg = nullptr;
            std::size_t msg_len = 0;
            int minor_version = 1;
            int status = 0;

            const int parsed = phr_parse_response(
                view.data(), view.size(), &minor_version, &status, &msg, &msg_len,
                raw.data(), &num_headers, last_len);
            if (parsed > 0) {
                Response response;
                response.status = status;
                response.reason.assign(msg, msg_len);
                response.version_minor = minor_version;
                response.headers = copy_headers(raw.data(), num_headers);
                response.keep_alive =
                    should_keep_alive(response.headers, response.version_minor);
                response.chunked = message_is_chunked(response.headers);

                buffer.consume(static_cast<std::size_t>(parsed));

                if (response_has_body(response.status, request_method)) {
                    if (response.chunked) {
                        response.body =
                            co_await read_chunked_body(_stream, buffer, _limits);
                    } else if (const auto len =
                                   parse_content_length(response.headers);
                               len.has_value()) {
                        response.body = co_await read_fixed_body(_stream, buffer,
                                                                 _limits, *len);
                    } else if (!response.keep_alive) {
                        response.body = co_await read_until_close_body(
                            _stream, buffer, _limits);
                    }
                }

                co_return response;
            }

            if (parsed == -1) {
                throw Error(502, "invalid HTTP response");
            }
        }

        if (buffer.size() >= _limits.max_header_bytes) {
            throw Error(502, "response headers too large");
        }

        last_len = buffer.size();
        const auto n = co_await read_more(_stream, buffer, _limits.max_header_bytes,
                                          502, "response headers too large");
        if (n == 0) {
            throw Error(502, "unexpected eof while reading response");
        }
    }
}

Task<void> Connection::write_request(const Request& request,
                                     std::string_view default_host) {
    Request outgoing = request;
    if (outgoing.target.empty()) {
        outgoing.target = "/";
    }
    if (outgoing.version_minor < 0 || outgoing.version_minor > 1) {
        throw std::logic_error("corouv::http::Connection only supports HTTP/1.x");
    }

    if (!default_host.empty() && !find_header(outgoing.headers, "Host")) {
        set_header(outgoing.headers, "Host", std::string(default_host));
    }

    prepare_outgoing_headers(outgoing.headers, outgoing.keep_alive,
                             outgoing.chunked, outgoing.body.size());

    std::string head;
    head.reserve(128 + outgoing.body.size());
    head.append(outgoing.method);
    head.push_back(' ');
    head.append(outgoing.target);
    head.append(" HTTP/1.");
    head.append(std::to_string(outgoing.version_minor));
    head.append("\r\n");
    head.append(serialize_headers(outgoing.headers));
    head.append("\r\n");

    co_await _stream.write_all(std::string_view(head));

    if (outgoing.chunked) {
        co_await write_chunked_body(_stream, outgoing.body);
    } else if (!outgoing.body.empty()) {
        co_await _stream.write_all(std::string_view(outgoing.body));
    }
}

Task<void> Connection::write_response(const Response& response,
                                      std::string_view request_method) {
    Response outgoing = response;
    if (outgoing.reason.empty()) {
        outgoing.reason = reason_phrase(outgoing.status);
    }
    if (outgoing.version_minor < 0 || outgoing.version_minor > 1) {
        throw std::logic_error("corouv::http::Connection only supports HTTP/1.x");
    }

    prepare_outgoing_headers(outgoing.headers, outgoing.keep_alive,
                             outgoing.chunked, outgoing.body.size());

    std::string head;
    head.reserve(128 + outgoing.body.size());
    head.append("HTTP/1.");
    head.append(std::to_string(outgoing.version_minor));
    head.push_back(' ');
    head.append(std::to_string(outgoing.status));
    head.push_back(' ');
    head.append(outgoing.reason);
    head.append("\r\n");
    head.append(serialize_headers(outgoing.headers));
    head.append("\r\n");

    co_await _stream.write_all(std::string_view(head));

    if (!response_has_body(outgoing.status, request_method)) {
        co_return;
    }

    if (outgoing.chunked) {
        co_await write_chunked_body(_stream, outgoing.body);
    } else if (!outgoing.body.empty()) {
        co_await _stream.write_all(std::string_view(outgoing.body));
    }
}

Server::Server(UvExecutor& ex, Handler handler, ServerOptions options)
    : _ex(&ex), _handler(std::move(handler)), _options(std::move(options)) {}

Task<void> Server::listen() {
    if (_listener.has_value() && _listener->open()) {
        co_return;
    }

    _listener =
        co_await net::listen(*_ex, _options.host, _options.port, _options.backlog);
    co_return;
}

Task<void> Server::handle_client(net::TcpStream stream) {
    Connection conn(std::move(stream), _options.limits);

    while (conn.open()) {
        Request request;
        std::optional<Response> request_error_response;
        try {
            auto maybe_request = co_await conn.read_request();
            if (!maybe_request.has_value()) {
                break;
            }
            request = std::move(*maybe_request);
        } catch (const Error& e) {
            Response error_response;
            error_response.status = e.status();
            error_response.reason = reason_phrase(e.status());
            error_response.body = e.what();
            error_response.keep_alive = false;
            request_error_response = std::move(error_response);
        }

        if (request_error_response.has_value()) {
            try {
                co_await conn.write_response(*request_error_response);
            } catch (...) {
            }
            break;
        }

        const std::string request_method = request.method;
        const bool request_keep_alive = request.keep_alive;

        Response response;
        try {
            response = co_await _handler(std::move(request));
        } catch (const async_simple::SignalException&) {
            throw;
        } catch (const Error& e) {
            response.status = e.status();
            response.reason = reason_phrase(e.status());
            response.body = e.what();
            response.keep_alive = false;
        } catch (const std::exception& e) {
            response.status = 500;
            response.reason = reason_phrase(500);
            response.body = e.what();
            response.keep_alive = false;
        } catch (...) {
            response.status = 500;
            response.reason = reason_phrase(500);
            response.body = "internal server error";
            response.keep_alive = false;
        }

        if (!request_keep_alive) {
            response.keep_alive = false;
        }

        try {
            co_await conn.write_response(response, request_method);
        } catch (...) {
            break;
        }

        if (!request_keep_alive || !response.keep_alive) {
            break;
        }
    }

    conn.close();
}

Task<void> Server::serve() {
    if (!_listener.has_value() || !_listener->open()) {
        co_await listen();
    }

    auto connections = co_await corouv::make_task_group();
    std::exception_ptr failure;

    while (_listener.has_value() && _listener->open()) {
        try {
            auto stream = co_await _listener->accept();
            if (!connections.spawn(handle_client(std::move(stream)))) {
                throw std::runtime_error("corouv::http::Server spawn failed");
            }
        } catch (const async_simple::SignalException&) {
            close();
            connections.cancel();
            break;
        } catch (const std::logic_error&) {
            if (!_listener.has_value() || !_listener->open()) {
                break;
            }
            failure = std::current_exception();
            connections.cancel();
            break;
        } catch (...) {
            failure = std::current_exception();
            connections.cancel();
            break;
        }
    }

    try {
        co_await connections.wait();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }

    if (failure) {
        std::rethrow_exception(failure);
    }
}

void Server::close() noexcept {
    if (_listener.has_value()) {
        _listener->close();
    }
}

std::uint16_t Server::port() const noexcept {
    if (_listener.has_value() && _listener->open()) {
        return _listener->local_endpoint().port;
    }
    return _options.port;
}

std::string Server::host() const {
    if (_listener.has_value() && _listener->open()) {
        return _listener->local_endpoint().host;
    }
    return _options.host;
}

Client::Client(UvExecutor& ex, ClientOptions options)
    : _ex(&ex), _options(std::move(options)) {}

Task<void> Client::connect(std::string host, std::uint16_t port) {
    close();
    auto stream = co_await net::connect(*_ex, host, port);
    _host = std::move(host);
    _port = port;
    _connection =
        std::make_unique<Connection>(std::move(stream), _options.limits);
    co_return;
}

Task<Response> Client::request(Request request) {
    if (!_connection || !_connection->open()) {
        throw std::logic_error("corouv::http::Client is not connected");
    }

    request.keep_alive = request.keep_alive && _options.keep_alive;

    const auto host_header = format_host_header(_host, _port);
    co_await _connection->write_request(request, host_header);
    auto response = co_await _connection->read_response(request.method);

    if (!request.keep_alive || !response.keep_alive) {
        close();
    }

    co_return response;
}

bool Client::connected() const noexcept {
    return _connection != nullptr && _connection->open();
}

void Client::close() noexcept {
    if (_connection) {
        _connection->close();
        _connection.reset();
    }
}

Url parse_url(std::string_view url) {
    constexpr std::string_view prefix = "http://";
    if (!url.starts_with(prefix)) {
        throw std::invalid_argument("corouv::http::parse_url only supports http://");
    }

    Url parsed;
    auto remainder = url.substr(prefix.size());

    const auto authority_end = remainder.find_first_of("/?#");
    const auto authority =
        remainder.substr(0, authority_end == std::string_view::npos
                                ? remainder.size()
                                : authority_end);
    remainder = authority_end == std::string_view::npos
                    ? std::string_view{}
                    : remainder.substr(authority_end);

    if (authority.empty()) {
        throw std::invalid_argument("corouv::http::parse_url missing host");
    }

    std::string_view host = authority;
    std::optional<std::uint16_t> port;

    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            throw std::invalid_argument("corouv::http::parse_url invalid IPv6 host");
        }
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                throw std::invalid_argument("corouv::http::parse_url invalid authority");
            }
            const auto port_text = authority.substr(close + 2);
            unsigned int parsed_port = 0;
            const auto [ptr, ec] = std::from_chars(
                port_text.data(), port_text.data() + port_text.size(),
                parsed_port, 10);
            if (ec != std::errc{} ||
                ptr != port_text.data() + port_text.size() ||
                parsed_port > 65535) {
                throw std::invalid_argument("corouv::http::parse_url invalid port");
            }
            port = static_cast<std::uint16_t>(parsed_port);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos &&
            authority.find(':') == authority.rfind(':')) {
            host = authority.substr(0, colon);
            const auto port_text = authority.substr(colon + 1);
            unsigned int parsed_port = 0;
            const auto [ptr, ec] = std::from_chars(
                port_text.data(), port_text.data() + port_text.size(),
                parsed_port, 10);
            if (ec != std::errc{} ||
                ptr != port_text.data() + port_text.size() ||
                parsed_port > 65535) {
                throw std::invalid_argument("corouv::http::parse_url invalid port");
            }
            port = static_cast<std::uint16_t>(parsed_port);
        }
    }

    if (host.empty()) {
        throw std::invalid_argument("corouv::http::parse_url missing host");
    }

    parsed.host.assign(host);
    parsed.port = port.value_or(80);

    if (remainder.empty()) {
        parsed.target = "/";
    } else if (remainder.front() == '#') {
        parsed.target = "/";
    } else {
        const auto hash = remainder.find('#');
        const auto without_fragment =
            remainder.substr(0, hash == std::string_view::npos ? remainder.size()
                                                               : hash);
        if (without_fragment.front() == '?') {
            parsed.target = "/";
            parsed.target.append(without_fragment);
        } else {
            parsed.target.assign(without_fragment);
        }
    }

    return parsed;
}

Task<Response> fetch(UvExecutor& ex, std::string_view url, Request request,
                     ClientOptions options) {
    const auto parsed = parse_url(url);
    if (request.target.empty() || request.target == "/") {
        request.target = parsed.target;
    }

    Client client(ex, std::move(options));
    co_await client.connect(parsed.host, parsed.port);
    co_return co_await client.request(std::move(request));
}

Task<Response> fetch(std::string_view url, Request request,
                     ClientOptions options) {
    auto* ex = co_await async_simple::CurrentExecutor{};
    auto* uvex = dynamic_cast<UvExecutor*>(ex);
    if (!uvex) {
        throw std::logic_error(
            "corouv::http::fetch requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await fetch(*uvex, url, std::move(request),
                             std::move(options));
}

}  // namespace corouv::http
