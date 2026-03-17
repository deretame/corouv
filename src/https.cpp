#include "corouv/https.h"

#include <async_simple/Executor.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "picohttpparser.h"

namespace corouv::https {

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
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
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

        std::size_t value = 0;
        const auto text = trim_copy(header.value);
        const auto [ptr, ec] =
            std::from_chars(text.data(), text.data() + text.size(), value, 10);
        if (ec != std::errc{} || ptr != text.data() + text.size()) {
            throw Error(400, "invalid Content-Length");
        }
        if (parsed.has_value() && *parsed != value) {
            throw Error(400, "conflicting Content-Length headers");
        }
        parsed = value;
    }
    return parsed;
}

bool message_is_chunked(const Headers& headers) {
    return corouv::http::header_contains_token(headers, "Transfer-Encoding",
                                               "chunked");
}

bool should_keep_alive(const Headers& headers, int version_minor) {
    if (corouv::http::header_contains_token(headers, "Connection", "close")) {
        return false;
    }
    if (corouv::http::header_contains_token(headers, "Connection", "keep-alive")) {
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
            if (!headers.empty()) {
                headers.back().value.push_back(' ');
                headers.back().value.append(raw[i].value, raw[i].value_len);
            }
            continue;
        }

        headers.push_back(Header{
            std::string(raw[i].name, raw[i].name_len),
            std::string(raw[i].value, raw[i].value_len),
        });
    }

    return headers;
}

Task<std::size_t> read_more(transport::CodecStream& stream, BufferCursor& buffer,
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

Task<void> ensure_buffer(transport::CodecStream& stream, BufferCursor& buffer,
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
}

Task<std::string> read_line_crlf(transport::CodecStream& stream,
                                 BufferCursor& buffer, std::size_t max_line,
                                 int error_status,
                                 const char* too_large_message,
                                 const char* eof_message) {
    while (true) {
        const auto pos = buffer.find("\r\n");
        if (pos != std::string_view::npos) {
            const std::string out(buffer.view().substr(0, pos));
            buffer.consume(pos + 2);
            co_return out;
        }

        if (buffer.size() >= max_line) {
            throw Error(error_status, too_large_message);
        }

        const auto n = co_await read_more(stream, buffer, max_line, error_status,
                                          too_large_message);
        if (n == 0) {
            throw Error(400, eof_message);
        }
    }
}

Task<std::string> read_fixed_body(transport::CodecStream& stream,
                                  BufferCursor& buffer, const Limits& limits,
                                  std::size_t size) {
    if (size > limits.max_body_bytes) {
        throw Error(413, "HTTPS body too large");
    }

    co_await ensure_buffer(stream, buffer, size, size, 413, "HTTPS body too large",
                           "unexpected eof while reading HTTPS body");
    co_return buffer.take(size);
}

Task<std::string> read_chunked_body(transport::CodecStream& stream,
                                    BufferCursor& buffer, const Limits& limits) {
    std::string body;

    while (true) {
        const auto line = co_await read_line_crlf(
            stream, buffer, limits.max_header_bytes, 431, "chunk header too large",
            "unexpected eof while reading chunk header");

        const auto semi = line.find(';');
        const auto size_text =
            trim_copy(line.substr(0, semi == std::string_view::npos ? line.size()
                                                                    : semi));

        std::size_t chunk_size = 0;
        const auto [ptr, ec] = std::from_chars(size_text.data(),
                                               size_text.data() + size_text.size(),
                                               chunk_size, 16);
        if (ec != std::errc{} || ptr != size_text.data() + size_text.size()) {
            throw Error(400, "invalid chunk size");
        }

        if (chunk_size == 0) {
            while (true) {
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
            throw Error(413, "HTTPS body too large");
        }

        co_await ensure_buffer(
            stream, buffer, chunk_size + 2,
            std::min(limits.max_body_bytes - body.size() + 2,
                     limits.max_body_bytes + limits.max_header_bytes),
            413, "HTTPS body too large",
            "unexpected eof while reading chunk data");

        body.append(buffer.view().data(), chunk_size);
        buffer.consume(chunk_size);
        if (buffer.view().substr(0, 2) != "\r\n") {
            throw Error(400, "invalid chunk terminator");
        }
        buffer.consume(2);
    }
}

Task<std::string> read_until_close_body(transport::CodecStream& stream,
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
            throw Error(413, "HTTPS body too large");
        }
        body.append(scratch.data(), n);
    }

    co_return body;
}

std::string format_host_header(std::string_view host, std::uint16_t port) {
    const bool bracket = host.find(':') != std::string_view::npos;
    if (port == 443) {
        return bracket ? "[" + std::string(host) + "]" : std::string(host);
    }
    return net::to_string(net::Endpoint{std::string(host), port});
}

void prepare_outgoing_headers(Headers& headers, bool keep_alive, bool chunked,
                              std::size_t body_size) {
    corouv::http::erase_header(headers, "Connection");
    corouv::http::erase_header(headers, "Content-Length");
    corouv::http::erase_header(headers, "Transfer-Encoding");

    corouv::http::set_header(headers, "Connection",
                             keep_alive ? "keep-alive" : "close");

    if (chunked) {
        corouv::http::set_header(headers, "Transfer-Encoding", "chunked");
    } else {
        corouv::http::set_header(headers, "Content-Length",
                                 std::to_string(body_size));
    }
}

Task<void> write_chunked_body(transport::CodecStream& stream,
                              std::string_view body) {
    if (body.empty()) {
        co_await stream.write_all("0\r\n\r\n");
        co_return;
    }

    char chunk_header[64] = {0};
    const int n = std::snprintf(chunk_header, sizeof(chunk_header), "%zx\r\n",
                                static_cast<std::size_t>(body.size()));
    co_await stream.write_all(
        std::string_view(chunk_header, static_cast<std::size_t>(n)));
    co_await stream.write_all(body);
    co_await stream.write_all("\r\n0\r\n\r\n");
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

Connection::Connection(transport::CodecStream stream, Limits limits)
    : _stream(std::move(stream)), _limits(limits) {}

bool Connection::open() const noexcept { return _stream.open(); }

void Connection::close() noexcept { _stream.close(); }

Task<std::optional<Request>> Connection::read_request() {
    BufferCursor buffer(_buffer, _buffer_offset);
    std::size_t last_len = 0;

    while (true) {
        const auto view = buffer.view();
        if (!view.empty()) {
            std::vector<phr_header> raw(_limits.max_header_count);
            std::size_t num_headers = raw.size();
            const char* method = nullptr;
            const char* path = nullptr;
            std::size_t method_len = 0;
            std::size_t path_len = 0;
            int minor_version = 1;

            const int parsed =
                phr_parse_request(view.data(), view.size(), &method, &method_len,
                                  &path, &path_len, &minor_version, raw.data(),
                                  &num_headers, last_len);
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
                throw Error(400, "invalid HTTPS request");
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
        const auto view = buffer.view();
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
                throw Error(502, "invalid HTTPS response");
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

    if (!default_host.empty() &&
        !corouv::http::find_header(outgoing.headers, "Host")) {
        corouv::http::set_header(outgoing.headers, "Host",
                                 std::string(default_host));
    }

    prepare_outgoing_headers(outgoing.headers, outgoing.keep_alive,
                             outgoing.chunked, outgoing.body.size());

    std::string head;
    head.append(outgoing.method);
    head.push_back(' ');
    head.append(outgoing.target);
    head.append(" HTTP/1.");
    head.append(std::to_string(outgoing.version_minor));
    head.append("\r\n");
    head.append(serialize_headers(outgoing.headers));
    head.append("\r\n");

    co_await _stream.write_all(head);
    if (outgoing.chunked) {
        co_await write_chunked_body(_stream, outgoing.body);
    } else if (!outgoing.body.empty()) {
        co_await _stream.write_all(outgoing.body);
    }
}

Task<void> Connection::write_response(const Response& response,
                                      std::string_view request_method) {
    Response outgoing = response;
    if (outgoing.reason.empty()) {
        outgoing.reason = corouv::http::reason_phrase(outgoing.status);
    }

    prepare_outgoing_headers(outgoing.headers, outgoing.keep_alive,
                             outgoing.chunked, outgoing.body.size());

    std::string head;
    head.append("HTTP/1.");
    head.append(std::to_string(outgoing.version_minor));
    head.push_back(' ');
    head.append(std::to_string(outgoing.status));
    head.push_back(' ');
    head.append(outgoing.reason);
    head.append("\r\n");
    head.append(serialize_headers(outgoing.headers));
    head.append("\r\n");

    co_await _stream.write_all(head);
    if (!response_has_body(outgoing.status, request_method)) {
        co_return;
    }
    if (outgoing.chunked) {
        co_await write_chunked_body(_stream, outgoing.body);
    } else if (!outgoing.body.empty()) {
        co_await _stream.write_all(outgoing.body);
    }
}

Client::Client(UvExecutor& ex, ClientOptions options)
    : _ex(&ex), _options(std::move(options)) {}

Task<void> Client::connect(std::string host, std::uint16_t port) {
    close();

    auto tls = _options.tls;
    if (tls.server_name.empty()) {
        tls.server_name = host;
    }

    auto raw = co_await net::connect(*_ex, host, port);
    auto stream =
        transport::CodecStream(std::move(raw), transport::make_bearssl_client_codec(std::move(tls)));
    co_await stream.handshake_client();

    _host = std::move(host);
    _port = port;
    _connection = std::make_unique<Connection>(std::move(stream), _options.limits);
}

Task<Response> Client::request(Request request) {
    if (!_connection || !_connection->open()) {
        throw std::logic_error("corouv::https::Client is not connected");
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

Server::Server(UvExecutor& ex, Handler handler, ServerOptions options)
    : _ex(&ex), _handler(std::move(handler)), _options(std::move(options)) {}

Task<void> Server::listen() {
    if (_listener.has_value() && _listener->open()) {
        co_return;
    }

    _listener =
        co_await net::listen(*_ex, _options.host, _options.port, _options.backlog);
}

Task<void> Server::handle_client(net::TcpStream stream) {
    auto tls_stream = transport::CodecStream(
        std::move(stream), transport::make_bearssl_server_codec(_options.tls));
    co_await tls_stream.handshake_server();

    Connection conn(std::move(tls_stream), _options.limits);

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
            error_response.reason = corouv::http::reason_phrase(e.status());
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

        const auto request_method = request.method;
        const bool request_keep_alive = request.keep_alive;

        Response response;
        try {
            response = co_await _handler(std::move(request));
        } catch (const Error& e) {
            response.status = e.status();
            response.reason = corouv::http::reason_phrase(e.status());
            response.body = e.what();
            response.keep_alive = false;
        } catch (const std::exception& e) {
            response.status = 500;
            response.reason = corouv::http::reason_phrase(500);
            response.body = e.what();
            response.keep_alive = false;
        } catch (...) {
            response.status = 500;
            response.reason = corouv::http::reason_phrase(500);
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
                throw std::runtime_error("corouv::https::Server spawn failed");
            }
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

Task<Response> fetch(UvExecutor& ex, std::string_view url, Request request,
                     ClientOptions options) {
    const auto parsed = corouv::http::parse_url(url);
    if (parsed.scheme != "https") {
        throw std::invalid_argument("corouv::https::fetch requires https:// URL");
    }

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
            "corouv::https::fetch requires CurrentExecutor to be UvExecutor");
    }
    co_return co_await fetch(*uvex, url, std::move(request),
                             std::move(options));
}

}  // namespace corouv::https
