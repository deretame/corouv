#include "corouv/https.h"

#include <async_simple/Executor.h>
#include <async_simple/Signal.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "corouv/task_group.h"

namespace corouv::https {

namespace {

std::string format_host_header(std::string_view host, std::uint16_t port) {
    const bool bracket = host.find(':') != std::string_view::npos;
    if (port == 443) {
        return bracket ? "[" + std::string(host) + "]" : std::string(host);
    }
    return net::to_string(net::Endpoint{std::string(host), port});
}

}  // namespace

Client::Client(UvExecutor& ex, ClientOptions options)
    : _ex(&ex), _options(std::move(options)) {}

Task<void> Client::connect(std::string host, std::uint16_t port) {
    close();

    auto tls = _options.tls;
    if (tls.server_name.empty()) {
        tls.server_name = host;
    }

    auto raw = co_await net::connect(*_ex, host, port);
    auto stream = transport::CodecStream(
        std::move(raw), transport::make_bearssl_client_codec(std::move(tls)));
    co_await stream.handshake_client();

    _host = std::move(host);
    _port = port;
    _connection = std::make_unique<Connection>(std::move(stream), _options.limits);
}

Task<Response> Client::request(Request request) {
    if (!_connection || !_connection->is_open()) {
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

bool Client::is_connected() const noexcept {
    return _connection != nullptr && _connection->is_open();
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
    if (_listener.has_value() && _listener->is_open()) {
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

    while (conn.is_open()) {
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
        } catch (const async_simple::SignalException&) {
            throw;
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
    if (!_listener.has_value() || !_listener->is_open()) {
        co_await listen();
    }

    if (auto* slot = co_await async_simple::coro::CurrentSlot{}; slot != nullptr) {
        (void)async_simple::signalHelper{async_simple::Terminate}.tryEmplace(
            slot, [this](async_simple::SignalType, async_simple::Signal*) {
                this->close();
            });
    }

    auto connections = co_await corouv::make_task_group();
    std::exception_ptr failure;

    while (_listener.has_value() && _listener->is_open()) {
        try {
            auto stream = co_await _listener->accept();
            if (!connections.spawn(handle_client(std::move(stream)))) {
                throw std::runtime_error("corouv::https::Server spawn failed");
            }
        } catch (const async_simple::SignalException&) {
            close();
            connections.cancel();
            break;
        } catch (const std::logic_error&) {
            if (!_listener.has_value() || !_listener->is_open()) {
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
    if (_listener.has_value() && _listener->is_open()) {
        return _listener->local_endpoint().port;
    }
    return _options.port;
}

std::string Server::host() const {
    if (_listener.has_value() && _listener->is_open()) {
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
