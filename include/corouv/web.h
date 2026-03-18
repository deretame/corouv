#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "corouv/http.h"
#include "corouv/https.h"
#include "corouv/transport.h"

namespace corouv::web {

namespace detail {

inline bool iequals(std::string_view lhs, std::string_view rhs) noexcept {
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

inline std::string_view request_path(std::string_view target) noexcept {
    const auto q = target.find('?');
    return target.substr(0, q == std::string_view::npos ? target.size() : q);
}

inline bool prefix_matches(std::string_view path, std::string_view prefix) noexcept {
    if (!path.starts_with(prefix)) {
        return false;
    }
    if (path.size() == prefix.size()) {
        return true;
    }
    if (prefix.empty() || prefix.back() == '/') {
        return true;
    }
    return path[prefix.size()] == '/';
}

template <class>
inline constexpr bool dependent_false_v = false;

inline std::vector<std::string_view> split_path_segments(
    std::string_view path) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < path.size() && path[i] == '/') {
        ++i;
    }

    while (i < path.size()) {
        const auto start = i;
        while (i < path.size() && path[i] != '/') {
            ++i;
        }
        if (i > start) {
            out.push_back(path.substr(start, i - start));
        }
        while (i < path.size() && path[i] == '/') {
            ++i;
        }
    }

    return out;
}

struct MatchResult {
    bool matched{false};
    Params params;
    std::size_t matched_segments{0};
};

inline MatchResult match_route(std::string_view path, std::string_view pattern,
                               bool prefix) {
    const auto path_parts = split_path_segments(path);
    const auto pattern_parts = split_path_segments(pattern);

    if (!prefix && path_parts.size() != pattern_parts.size()) {
        return {};
    }
    if (prefix && path_parts.size() < pattern_parts.size()) {
        return {};
    }

    MatchResult out;
    out.params.reserve(pattern_parts.size());
    out.matched_segments = pattern_parts.size();

    for (std::size_t i = 0; i < pattern_parts.size(); ++i) {
        const auto pat = pattern_parts[i];
        const auto seg = path_parts[i];

        if (!pat.empty() && pat.front() == ':' && pat.size() > 1) {
            out.params.push_back(Param{
                .name = std::string(pat.substr(1)),
                .value = std::string(seg),
            });
            continue;
        }

        if (pat == "*") {
            std::string tail;
            for (std::size_t j = i; j < path_parts.size(); ++j) {
                if (!tail.empty()) {
                    tail.push_back('/');
                }
                tail.append(path_parts[j]);
            }
            out.params.push_back(Param{
                .name = "*",
                .value = std::move(tail),
            });
            out.matched = true;
            out.matched_segments = path_parts.size();
            return out;
        }

        if (pat != seg) {
            return {};
        }
    }

    out.matched = true;
    return out;
}

inline std::string strip_matched_prefix(std::string_view target,
                                        std::size_t matched_segments) {
    const auto path = request_path(target);
    const auto suffix = target.substr(path.size());
    const auto parts = split_path_segments(path);

    std::string new_path = "/";
    if (matched_segments < parts.size()) {
        new_path.clear();
        for (std::size_t i = matched_segments; i < parts.size(); ++i) {
            new_path.push_back('/');
            new_path.append(parts[i]);
        }
    }

    new_path.append(suffix);
    return new_path;
}

inline void append_params(Params& dst, Params src) {
    for (auto& entry : src) {
        dst.push_back(std::move(entry));
    }
}

template <class Fn>
Handler to_handler(Fn&& fn) {
    using F = std::decay_t<Fn>;
    if constexpr (std::is_invocable_r_v<Task<Response>, F&, RequestContext>) {
        return Handler(std::forward<Fn>(fn));
    } else if constexpr (std::is_invocable_r_v<Task<Response>, F&, Request>) {
        return Handler(
            [f = F(std::forward<Fn>(fn))](RequestContext ctx) mutable
                -> Task<Response> { co_return co_await f(std::move(ctx.request)); });
    } else {
        static_assert(dependent_false_v<F>,
                      "web handler must accept RequestContext or Request");
    }
}

template <class Fn>
Middleware to_middleware(Fn&& fn) {
    using F = std::decay_t<Fn>;
    if constexpr (std::is_invocable_r_v<Task<Response>, F&, RequestContext, Next>) {
        return Middleware(std::forward<Fn>(fn));
    } else {
        static_assert(dependent_false_v<F>,
                      "web middleware must accept (RequestContext, Next)");
    }
}

}  // namespace detail

using Header = corouv::http::Header;
using Headers = corouv::http::Headers;
using Limits = corouv::http::Limits;
using Error = corouv::http::Error;
using Request = corouv::http::Request;
using Response = corouv::http::Response;
using Url = corouv::http::Url;
using corouv::http::reason_phrase;

struct Param {
    std::string name;
    std::string value;
};

using Params = std::vector<Param>;

struct RequestContext {
    Request request;
    Params params;

    [[nodiscard]] std::optional<std::string_view> param(
        std::string_view name) const noexcept {
        for (const auto& entry : params) {
            if (entry.name == name) {
                return entry.value;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool has_param(std::string_view name) const noexcept {
        return param(name).has_value();
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return detail::request_path(request.target);
    }
};

using RequestHandler = std::function<Task<Response>(Request)>;
using Handler = std::function<Task<Response>(RequestContext)>;
using Next = std::function<Task<Response>(RequestContext)>;
using Middleware = std::function<Task<Response>(RequestContext, Next)>;

struct ClientOptions {
    Limits limits{};
    bool keep_alive{true};
    transport::TlsClientConfig tls;
};

struct ServerOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
    int backlog{128};
    Limits limits{};
    std::optional<transport::TlsServerConfig> tls;
};

inline corouv::http::ClientOptions to_http_options(const ClientOptions& options) {
    return corouv::http::ClientOptions{
        .limits = options.limits,
        .keep_alive = options.keep_alive,
    };
}

inline corouv::https::ClientOptions to_https_options(
    const ClientOptions& options) {
    return corouv::https::ClientOptions{
        .limits = options.limits,
        .tls = options.tls,
        .keep_alive = options.keep_alive,
    };
}

class Client {
public:
    explicit Client(UvExecutor& ex, ClientOptions options = {})
        : _ex(&ex), _options(std::move(options)) {}

    Task<void> connect(std::string_view url) {
        close();

        const auto parsed = corouv::http::parse_url(url);
        _scheme = parsed.scheme;
        _host = parsed.host;
        _port = parsed.port;

        if (parsed.scheme == "http") {
            corouv::http::Client client(*_ex, to_http_options(_options));
            co_await client.connect(parsed.host, parsed.port);
            _impl.template emplace<corouv::http::Client>(std::move(client));
            co_return;
        }

        if (parsed.scheme == "https") {
            corouv::https::Client client(*_ex, to_https_options(_options));
            co_await client.connect(parsed.host, parsed.port);
            _impl.template emplace<corouv::https::Client>(std::move(client));
            co_return;
        }

        throw std::invalid_argument("corouv::web::Client only supports http/https");
    }

    Task<Response> request(Request request) {
        if (auto* client = std::get_if<corouv::http::Client>(&_impl)) {
            co_return co_await client->request(std::move(request));
        }
        if (auto* client = std::get_if<corouv::https::Client>(&_impl)) {
            co_return co_await client->request(std::move(request));
        }
        throw std::logic_error("corouv::web::Client is not connected");
    }

    Task<Response> fetch(std::string_view url, Request request = {}) {
        const auto parsed = corouv::http::parse_url(url);
        if (request.target.empty() || request.target == "/") {
            request.target = parsed.target;
        }

        if (!matches(parsed) || !is_connected()) {
            co_await connect(url);
        }

        co_return co_await this->request(std::move(request));
    }

    [[nodiscard]] bool is_connected() const noexcept {
        if (const auto* client = std::get_if<corouv::http::Client>(&_impl)) {
            return client->is_connected();
        }
        if (const auto* client = std::get_if<corouv::https::Client>(&_impl)) {
            return client->is_connected();
        }
        return false;
    }

    [[nodiscard]] bool is_tls() const noexcept { return _scheme == "https"; }
    [[nodiscard]] const std::string& scheme() const noexcept { return _scheme; }
    [[nodiscard]] const std::string& host() const noexcept { return _host; }
    [[nodiscard]] std::uint16_t port() const noexcept { return _port; }

    void close() noexcept {
        if (auto* client = std::get_if<corouv::http::Client>(&_impl)) {
            client->close();
        } else if (auto* client =
                       std::get_if<corouv::https::Client>(&_impl)) {
            client->close();
        }

        _impl.template emplace<std::monostate>();
        _scheme.clear();
        _host.clear();
        _port = 0;
    }

private:
    [[nodiscard]] bool matches(const Url& parsed) const noexcept {
        return _scheme == parsed.scheme && _host == parsed.host &&
               _port == parsed.port;
    }

    UvExecutor* _ex = nullptr;
    ClientOptions _options;
    std::variant<std::monostate, corouv::http::Client, corouv::https::Client>
        _impl;
    std::string _scheme;
    std::string _host;
    std::uint16_t _port{0};
};

class ClientBuilder {
public:
    explicit ClientBuilder(UvExecutor& ex) : _ex(&ex) {}

    ClientBuilder& limits(Limits value) {
        _options.limits = std::move(value);
        return *this;
    }

    ClientBuilder& keep_alive(bool enabled = true) {
        _options.keep_alive = enabled;
        return *this;
    }

    ClientBuilder& tls(transport::TlsClientConfig config) {
        _options.tls = std::move(config);
        return *this;
    }

    ClientBuilder& server_name(std::string value) {
        _options.tls.server_name = std::move(value);
        return *this;
    }

    ClientBuilder& trust_anchor(std::string pem) {
        _options.tls.trust_anchors_pem.push_back(std::move(pem));
        return *this;
    }

    [[nodiscard]] Client build() const { return Client(*_ex, _options); }

    Task<Response> fetch(std::string_view url, Request request = {}) const {
        auto client = build();
        co_return co_await client.fetch(url, std::move(request));
    }

private:
    UvExecutor* _ex = nullptr;
    ClientOptions _options;
};

class Server {
public:
    using Handler = std::function<Task<Response>(Request)>;

    explicit Server(corouv::http::Server server)
        : _impl(std::move(server)) {}
    explicit Server(corouv::https::Server server)
        : _impl(std::move(server)) {}

    Task<void> listen() {
        if (auto* server = std::get_if<corouv::http::Server>(&_impl)) {
            co_await server->listen();
            co_return;
        }
        co_await std::get<corouv::https::Server>(_impl).listen();
    }

    Task<void> serve() {
        if (auto* server = std::get_if<corouv::http::Server>(&_impl)) {
            co_await server->serve();
            co_return;
        }
        co_await std::get<corouv::https::Server>(_impl).serve();
    }

    void close() noexcept {
        if (auto* server = std::get_if<corouv::http::Server>(&_impl)) {
            server->close();
            return;
        }
        std::get<corouv::https::Server>(_impl).close();
    }

    [[nodiscard]] bool is_tls() const noexcept {
        return std::holds_alternative<corouv::https::Server>(_impl);
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        if (const auto* server = std::get_if<corouv::http::Server>(&_impl)) {
            return server->port();
        }
        return std::get<corouv::https::Server>(_impl).port();
    }

    [[nodiscard]] std::string host() const {
        if (const auto* server = std::get_if<corouv::http::Server>(&_impl)) {
            return server->host();
        }
        return std::get<corouv::https::Server>(_impl).host();
    }

private:
    std::variant<corouv::http::Server, corouv::https::Server> _impl;
};

class Router {
public:
    using Handler = Server::Handler;

    Router() : _state(std::make_shared<State>()) {}

    Router& route(std::optional<std::string> method, std::string path,
                  Handler handler, bool prefix = false) {
        _state->routes.push_back(Route{
            .method = std::move(method),
            .path = std::move(path),
            .handler = std::move(handler),
            .prefix = prefix,
        });
        return *this;
    }

    Router& any(std::string path, Handler handler) {
        return route(std::nullopt, std::move(path), std::move(handler), false);
    }

    Router& get(std::string path, Handler handler) {
        return route(std::string("GET"), std::move(path), std::move(handler), false);
    }

    Router& post(std::string path, Handler handler) {
        return route(std::string("POST"), std::move(path), std::move(handler), false);
    }

    Router& put(std::string path, Handler handler) {
        return route(std::string("PUT"), std::move(path), std::move(handler), false);
    }

    Router& patch(std::string path, Handler handler) {
        return route(std::string("PATCH"), std::move(path), std::move(handler), false);
    }

    Router& delete_(std::string path, Handler handler) {
        return route(std::string("DELETE"), std::move(path), std::move(handler),
                     false);
    }

    Router& prefix(std::string path, Handler handler) {
        return route(std::nullopt, std::move(path), std::move(handler), true);
    }

    Router& not_found(Handler handler) {
        _state->not_found = std::move(handler);
        return *this;
    }

    Task<Response> handle(Request request) const {
        const auto path = detail::request_path(request.target);

        for (const auto& route : _state->routes) {
            if (route.method.has_value() &&
                !detail::iequals(request.method, *route.method)) {
                continue;
            }

            const bool matched =
                route.prefix ? detail::prefix_matches(path, route.path)
                             : path == route.path;
            if (!matched) {
                continue;
            }

            co_return co_await route.handler(std::move(request));
        }

        if (_state->not_found.has_value()) {
            co_return co_await (*_state->not_found)(std::move(request));
        }

        Response response;
        response.status = 404;
        response.reason = corouv::http::reason_phrase(404);
        response.body = "not found";
        response.keep_alive = false;
        co_return response;
    }

    [[nodiscard]] Handler handler() const {
        auto state = _state;
        return [state](Request request) -> Task<Response> {
            Router router;
            router._state = state;
            co_return co_await router.handle(std::move(request));
        };
    }

private:
    struct Route {
        std::optional<std::string> method;
        std::string path;
        Handler handler;
        bool prefix{false};
    };

    struct State {
        std::vector<Route> routes;
        std::optional<Handler> not_found;
    };

    std::shared_ptr<State> _state;
};

using App = Router;

class ServerBuilder {
public:
    using Handler = Server::Handler;

    explicit ServerBuilder(UvExecutor& ex) : _ex(&ex) {}

    ServerBuilder& host(std::string value) {
        _options.host = std::move(value);
        return *this;
    }

    ServerBuilder& port(std::uint16_t value) {
        _options.port = value;
        return *this;
    }

    ServerBuilder& listen_on(std::string host, std::uint16_t port) {
        _options.host = std::move(host);
        _options.port = port;
        return *this;
    }

    ServerBuilder& backlog(int value) {
        _options.backlog = value;
        return *this;
    }

    ServerBuilder& limits(Limits value) {
        _options.limits = std::move(value);
        return *this;
    }

    ServerBuilder& handle(Handler handler) {
        _handler = std::move(handler);
        return *this;
    }

    ServerBuilder& routes(Router router) {
        _router = std::move(router);
        return *this;
    }

    ServerBuilder& app(App app) {
        _router = std::move(app);
        return *this;
    }

    ServerBuilder& route(std::optional<std::string> method, std::string path,
                         Handler handler, bool prefix = false) {
        ensure_router().route(std::move(method), std::move(path),
                              std::move(handler), prefix);
        return *this;
    }

    ServerBuilder& any(std::string path, Handler handler) {
        ensure_router().any(std::move(path), std::move(handler));
        return *this;
    }

    ServerBuilder& get(std::string path, Handler handler) {
        ensure_router().get(std::move(path), std::move(handler));
        return *this;
    }

    ServerBuilder& post(std::string path, Handler handler) {
        ensure_router().post(std::move(path), std::move(handler));
        return *this;
    }

    ServerBuilder& put(std::string path, Handler handler) {
        ensure_router().put(std::move(path), std::move(handler));
        return *this;
    }

    ServerBuilder& patch(std::string path, Handler handler) {
        ensure_router().patch(std::move(path), std::move(handler));
        return *this;
    }

    ServerBuilder& delete_(std::string path, Handler handler) {
        ensure_router().delete_(std::move(path), std::move(handler));
        return *this;
    }

    ServerBuilder& prefix(std::string path, Handler handler) {
        ensure_router().prefix(std::move(path), std::move(handler));
        return *this;
    }

    ServerBuilder& not_found(Handler handler) {
        ensure_router().not_found(std::move(handler));
        return *this;
    }

    ServerBuilder& tls(transport::TlsServerConfig config) {
        _options.tls = std::move(config);
        return *this;
    }

    ServerBuilder& clear_tls() {
        _options.tls.reset();
        return *this;
    }

    [[nodiscard]] Server build() const {
        auto handler = _handler;
        if (!handler && _router.has_value()) {
            handler = _router->handler();
        }

        if (!handler) {
            throw std::logic_error("corouv::web::ServerBuilder requires a handler");
        }

        if (_options.tls.has_value()) {
            return Server(corouv::https::Server(
                *_ex, *handler,
                corouv::https::ServerOptions{
                    .host = _options.host,
                    .port = _options.port,
                    .backlog = _options.backlog,
                    .limits = _options.limits,
                    .tls = *_options.tls,
                }));
        }

        return Server(corouv::http::Server(
            *_ex, *handler,
            corouv::http::ServerOptions{
                .host = _options.host,
                .port = _options.port,
                .backlog = _options.backlog,
                .limits = _options.limits,
            }));
    }

private:
    Router& ensure_router() {
        if (!_router.has_value()) {
            _router.emplace();
        }
        return *_router;
    }

    UvExecutor* _ex = nullptr;
    ServerOptions _options;
    std::optional<Handler> _handler;
    std::optional<Router> _router;
};

}  // namespace corouv::web
