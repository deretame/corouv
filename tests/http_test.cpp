#include <corouv/http.h>
#include <corouv/multipart.h>
#include <corouv/net.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>
#include <corouv/timer.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace http_test_detail {

class ScriptedStream {
public:
    std::vector<std::string> incoming;

    [[nodiscard]] bool is_open() const noexcept { return _open; }

    [[nodiscard]] uv_os_sock_t native_handle() const noexcept {
        return static_cast<uv_os_sock_t>(-1);
    }

    [[nodiscard]] const corouv::net::Endpoint& local_endpoint() const noexcept {
        return _local;
    }

    [[nodiscard]] const corouv::net::Endpoint& peer_endpoint() const noexcept {
        return _peer;
    }

    corouv::Task<std::size_t> recv_some(std::span<char> buffer) {
        if (!_open || buffer.empty()) {
            co_return 0;
        }

        if (_chunk_index >= incoming.size()) {
            _open = false;
            co_return 0;
        }

        const auto& chunk = incoming[_chunk_index];
        const auto available = chunk.size() - _chunk_offset;
        const auto n = std::min<std::size_t>(available, buffer.size());
        std::memcpy(buffer.data(), chunk.data() + _chunk_offset, n);

        _chunk_offset += n;
        if (_chunk_offset >= chunk.size()) {
            ++_chunk_index;
            _chunk_offset = 0;
        }

        co_return n;
    }

    corouv::Task<void> send_all(std::span<const char>) { co_return; }

    corouv::Task<void> send_all(std::string_view) { co_return; }

    corouv::Task<std::string> recv_until_eof(std::size_t max_bytes) {
        std::string out;
        std::array<char, 1024> scratch{};

        while (true) {
            const auto n = co_await recv_some(
                std::span<char>(scratch.data(), scratch.size()));
            if (n == 0) {
                break;
            }
            if (out.size() + n > max_bytes) {
                throw std::runtime_error("scripted stream: recv_until_eof limit");
            }
            out.append(scratch.data(), n);
        }

        co_return out;
    }

    void shutdown_send() noexcept {}

    void close() noexcept { _open = false; }

private:
    bool _open{true};
    corouv::net::Endpoint _local{"127.0.0.1", 10000};
    corouv::net::Endpoint _peer{"127.0.0.1", 10001};
    std::size_t _chunk_index{0};
    std::size_t _chunk_offset{0};
};

class TimedScriptedStream {
public:
    std::vector<std::string> incoming;
    std::vector<std::chrono::milliseconds> recv_delays;
    std::chrono::milliseconds send_delay{0ms};

    [[nodiscard]] bool is_open() const noexcept { return _open; }

    [[nodiscard]] uv_os_sock_t native_handle() const noexcept {
        return static_cast<uv_os_sock_t>(-1);
    }

    [[nodiscard]] const corouv::net::Endpoint& local_endpoint() const noexcept {
        return _local;
    }

    [[nodiscard]] const corouv::net::Endpoint& peer_endpoint() const noexcept {
        return _peer;
    }

    corouv::Task<std::size_t> recv_some(std::span<char> buffer) {
        if (!_open || buffer.empty()) {
            co_return 0;
        }

        if (_chunk_index >= incoming.size()) {
            _open = false;
            co_return 0;
        }

        if (_chunk_offset == 0 && _chunk_index < recv_delays.size() &&
            recv_delays[_chunk_index] > 0ms) {
            co_await corouv::sleep_for(recv_delays[_chunk_index]);
        }

        const auto& chunk = incoming[_chunk_index];
        const auto available = chunk.size() - _chunk_offset;
        const auto n = std::min<std::size_t>(available, buffer.size());
        std::memcpy(buffer.data(), chunk.data() + _chunk_offset, n);

        _chunk_offset += n;
        if (_chunk_offset >= chunk.size()) {
            ++_chunk_index;
            _chunk_offset = 0;
        }

        co_return n;
    }

    corouv::Task<void> send_all(std::span<const char> data) {
        if (send_delay > 0ms) {
            co_await corouv::sleep_for(send_delay);
        }
        written.append(data.data(), data.size());
        co_return;
    }

    corouv::Task<void> send_all(std::string_view data) {
        if (send_delay > 0ms) {
            co_await corouv::sleep_for(send_delay);
        }
        written.append(data.data(), data.size());
        co_return;
    }

    corouv::Task<std::string> recv_until_eof(std::size_t max_bytes) {
        std::string out;
        std::array<char, 1024> scratch{};

        while (true) {
            const auto n = co_await recv_some(
                std::span<char>(scratch.data(), scratch.size()));
            if (n == 0) {
                break;
            }
            if (out.size() + n > max_bytes) {
                throw std::runtime_error(
                    "timed scripted stream: recv_until_eof limit");
            }
            out.append(scratch.data(), n);
        }

        co_return out;
    }

    void shutdown_send() noexcept {}

    void close() noexcept { _open = false; }

    std::string written;

private:
    bool _open{true};
    corouv::net::Endpoint _local{"127.0.0.1", 10010};
    corouv::net::Endpoint _peer{"127.0.0.1", 10011};
    std::size_t _chunk_index{0};
    std::size_t _chunk_offset{0};
};

struct RawHeaderRead {
    std::string header_block;
    std::string extra_bytes;
};

corouv::Task<RawHeaderRead> read_raw_header_block(corouv::net::TcpStream& stream,
                                                  std::size_t max_bytes = 8192) {
    std::string acc;
    std::array<char, 512> scratch{};

    while (acc.size() < max_bytes) {
        const auto n = co_await stream.read_some(
            std::span<char>(scratch.data(), scratch.size()));
        if (n == 0) {
            throw std::runtime_error(
                "http_test: eof before raw header block completed");
        }
        acc.append(scratch.data(), n);
        const auto end = acc.find("\r\n\r\n");
        if (end != std::string::npos) {
            RawHeaderRead out;
            out.header_block = acc.substr(0, end + 4);
            out.extra_bytes = acc.substr(end + 4);
            co_return out;
        }
    }

    throw std::runtime_error("http_test: raw header block too large");
}

corouv::Task<std::string> read_exact_raw(corouv::net::TcpStream& stream,
                                         std::size_t wanted) {
    std::string out;
    out.reserve(wanted);
    std::array<char, 512> scratch{};

    while (out.size() < wanted) {
        const auto need = std::min<std::size_t>(scratch.size(), wanted - out.size());
        const auto n = co_await stream.read_some(
            std::span<char>(scratch.data(), need));
        if (n == 0) {
            throw std::runtime_error("http_test: eof while reading exact raw bytes");
        }
        out.append(scratch.data(), n);
    }

    co_return out;
}

}  // namespace http_test_detail

corouv::Task<void> keep_alive_server_task(corouv::net::TcpListener* listener,
                                          std::atomic<int>* accepts) {
    auto stream = co_await listener->accept();
    accepts->fetch_add(1, std::memory_order_relaxed);

    corouv::http::Connection conn(std::move(stream));

    auto first = co_await conn.read_request();
    if (!first.has_value() || first->body != "alpha") {
        throw std::runtime_error("http_test: first request mismatch");
    }

    corouv::http::Response response1;
    response1.body = "first-response";
    co_await conn.write_response(response1, first->method);

    auto second = co_await conn.read_request();
    if (!second.has_value() || !second->chunked || second->body != "beta") {
        throw std::runtime_error("http_test: second request mismatch");
    }

    corouv::http::Response response2;
    response2.chunked = true;
    response2.body = "second-response";
    co_await conn.write_response(response2, second->method);

    auto third = co_await conn.read_request();
    if (third.has_value()) {
        throw std::runtime_error("http_test: expected eof after close");
    }

    conn.close();
    listener->close();
}

corouv::Task<void> close_body_server_task(corouv::net::TcpListener* listener) {
    auto stream = co_await listener->accept();
    corouv::http::Connection conn(std::move(stream));

    auto request = co_await conn.read_request();
    if (!request.has_value() || request->target != "/close") {
        throw std::runtime_error("http_test: close request mismatch");
    }

    co_await conn.stream().send_all(
        std::string_view("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nclose-body"));
    conn.close();
    listener->close();
}

corouv::Task<void> interim_response_server_task(corouv::net::TcpListener* listener) {
    auto stream = co_await listener->accept();
    corouv::http::Connection conn(std::move(stream));

    const auto request = co_await conn.read_request();
    if (!request.has_value() || request->target != "/interim") {
        throw std::runtime_error("http_test: interim request mismatch");
    }

    co_await conn.stream().send_all(
        std::string_view("HTTP/1.1 100 Continue\r\n\r\n"
                         "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfinal"));
    conn.close();
    listener->close();
}

corouv::Task<void> early_hints_server_task(corouv::net::TcpListener* listener) {
    auto stream = co_await listener->accept();
    corouv::http::Connection conn(std::move(stream));

    const auto request = co_await conn.read_request();
    if (!request.has_value() || request->target != "/early-hints") {
        throw std::runtime_error("http_test: early hints request mismatch");
    }

    co_await conn.stream().send_all(
        std::string_view(
            "HTTP/1.1 103 Early Hints\r\n"
            "Link: </app.css>; rel=preload; as=style\r\n"
            "\r\n"
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "final"));
    conn.close();
    listener->close();
}

corouv::Task<void> streaming_server_task(corouv::net::TcpListener* listener) {
    auto stream = co_await listener->accept();
    corouv::http::Connection conn(std::move(stream));

    std::string request_body;
    const auto request = co_await conn.read_request_stream(
        [&request_body](std::string_view chunk) -> corouv::Task<void> {
            request_body.append(chunk);
            co_return;
        });
    if (!request.has_value() || request->method != "POST" ||
        request->target != "/stream" || request_body != "alphabeta") {
        throw std::runtime_error("http_test: streaming request mismatch");
    }

    corouv::http::Response response;
    response.status = 200;

    corouv::http::BodyChunkSource source =
        [i = 0]() mutable -> corouv::Task<std::optional<std::string>> {
        if (i == 0) {
            ++i;
            co_return std::string("hello ");
        }
        if (i == 1) {
            ++i;
            co_return std::string("stream");
        }
        co_return std::nullopt;
    };

    co_await conn.write_response_stream(response, std::move(source),
                                        request->method);
    conn.close();
    listener->close();
}

corouv::Task<void> response_timeout_server_task(corouv::net::TcpListener* listener) {
    auto stream = co_await listener->accept();
    corouv::http::Connection conn(std::move(stream));
    const auto request = co_await conn.read_request();
    if (!request.has_value() || request->target != "/timeout") {
        throw std::runtime_error("http_test: timeout request mismatch");
    }

    co_await corouv::sleep_for(200ms);
    try {
        corouv::http::Response response;
        response.body = "late";
        co_await conn.write_response(response, request->method);
    } catch (...) {
    }

    conn.close();
    listener->close();
}

corouv::Task<void> expect_continue_client_server_task(
    corouv::net::TcpListener* listener) {
    auto stream = co_await listener->accept();

    auto head = co_await http_test_detail::read_raw_header_block(stream);
    if (!head.extra_bytes.empty()) {
        throw std::runtime_error(
            "http_test: client sent body before 100 continue");
    }
    if (head.header_block.find("POST /expect-client HTTP/1.1\r\n") != 0 ||
        head.header_block.find("Expect: 100-continue\r\n") == std::string::npos) {
        throw std::runtime_error("http_test: expect-client header mismatch");
    }

    co_await stream.write_all(std::string_view("HTTP/1.1 100 Continue\r\n\r\n"));

    const auto body = co_await http_test_detail::read_exact_raw(stream, 5);
    if (body != "hello") {
        throw std::runtime_error("http_test: expect-client body mismatch");
    }

    co_await stream.write_all(
        std::string_view("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"));
    stream.close();
    listener->close();
}

corouv::Task<void> expect_continue_timeout_server_task(
    corouv::net::TcpListener* listener) {
    auto stream = co_await listener->accept();
    auto head = co_await http_test_detail::read_raw_header_block(stream);
    if (head.header_block.find("Expect: 100-continue\r\n") == std::string::npos) {
        throw std::runtime_error("http_test: expect-timeout header mismatch");
    }
    if (!head.extra_bytes.empty()) {
        throw std::runtime_error(
            "http_test: expect-timeout got unexpected body bytes");
    }

    co_await corouv::sleep_for(200ms);
    stream.close();
    listener->close();
}

corouv::Task<void> trailers_server_task(corouv::net::TcpListener* listener) {
    auto stream = co_await listener->accept();
    corouv::http::Connection conn(std::move(stream));

    const auto request = co_await conn.read_request();
    if (!request.has_value() || request->target != "/trailers" ||
        request->body != "payload") {
        throw std::runtime_error("http_test: trailer request mismatch");
    }

    const auto req_trailer =
        corouv::http::find_header(request->trailers, "X-Req-Trailer");
    if (!req_trailer.has_value() || *req_trailer != "done") {
        throw std::runtime_error("http_test: request trailer mismatch");
    }

    corouv::http::Response response;
    response.status = 200;
    response.trailers.push_back({"X-Resp-Trailer", "sent"});

    corouv::http::BodyChunkSource source =
        [sent = false]() mutable -> corouv::Task<std::optional<std::string>> {
        if (sent) {
            co_return std::nullopt;
        }
        sent = true;
        co_return std::string("ok");
    };

    co_await conn.write_response_stream(response, std::move(source),
                                        request->method);
    conn.close();
    listener->close();
}

corouv::Task<void> server_client_case(corouv::UvExecutor& ex) {
    corouv::http::Server server(
        ex,
        [](corouv::http::Request request)
            -> corouv::Task<corouv::http::Response> {
            corouv::http::Response response;
            response.body =
                request.method + " " + request.target + " " + request.body;
            co_return response;
        },
        corouv::http::ServerOptions{.host = "127.0.0.1", .port = 0});

    co_await server.listen();

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(server.serve())) {
        throw std::runtime_error("http_test: server spawn failed");
    }

    corouv::http::Request request;
    request.method = "POST";
    request.body = "hello";

    const auto response = co_await corouv::http::fetch(
        ex,
        "http://127.0.0.1:" + std::to_string(server.port()) + "/echo?x=1",
        request);

    group.cancel();
    co_await group.wait();

    if (response.status != 200) {
        throw std::runtime_error("http_test: unexpected status");
    }
    if (response.body != "POST /echo?x=1 hello") {
        throw std::runtime_error("http_test: unexpected response body");
    }
}

corouv::Task<void> keep_alive_and_chunked_case(corouv::UvExecutor& ex) {
    auto listener = co_await corouv::net::listen(ex, "127.0.0.1", 0);
    const auto port = listener.local_endpoint().port;

    std::atomic<int> accepts{0};
    auto group = co_await corouv::make_task_group();
    if (!group.spawn(keep_alive_server_task(&listener, &accepts))) {
        throw std::runtime_error("http_test: connection server spawn failed");
    }

    corouv::http::Client client(ex);
    co_await client.connect("127.0.0.1", port);

    corouv::http::Request request1;
    request1.method = "POST";
    request1.target = "/first";
    request1.body = "alpha";
    const auto response1 = co_await client.request(request1);

    corouv::http::Request request2;
    request2.method = "POST";
    request2.target = "/second";
    request2.body = "beta";
    request2.chunked = true;
    const auto response2 = co_await client.request(request2);
    client.close();

    co_await group.wait();

    if (response1.body != "first-response") {
        throw std::runtime_error("http_test: first response mismatch");
    }
    if (response2.body != "second-response") {
        throw std::runtime_error("http_test: second response mismatch");
    }
    if (accepts.load(std::memory_order_relaxed) != 1) {
        throw std::runtime_error("http_test: expected keep-alive reuse");
    }
}

corouv::Task<void> connection_close_body_case(corouv::UvExecutor& ex) {
    auto listener = co_await corouv::net::listen(ex, "127.0.0.1", 0);
    const auto port = listener.local_endpoint().port;

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(close_body_server_task(&listener))) {
        throw std::runtime_error("http_test: close server spawn failed");
    }

    corouv::http::Client client(ex);
    co_await client.connect("127.0.0.1", port);

    corouv::http::Request request;
    request.target = "/close";
    const auto response = co_await client.request(request);

    co_await group.wait();

    if (response.body != "close-body") {
        throw std::runtime_error("http_test: close-delimited body mismatch");
    }
    if (client.is_connected()) {
        throw std::runtime_error("http_test: client should close after response");
    }
}

corouv::Task<void> fixed_body_pipeline_overread_case() {
    http_test_detail::ScriptedStream stream;
    stream.incoming.push_back(
        "POST /first HTTP/1.1\r\nHost: example\r\nContent-Length: 1\r\n\r\n");
    stream.incoming.push_back(
        "AGET /second HTTP/1.1\r\nHost: example\r\nContent-Length: 0\r\n\r\n");

    corouv::http::Connection conn(corouv::io::ByteStream(std::move(stream)));

    const auto first = co_await conn.read_request();
    if (!first.has_value() || first->target != "/first" || first->body != "A") {
        throw std::runtime_error("http_test: pipelined first request mismatch");
    }

    const auto second = co_await conn.read_request();
    if (!second.has_value() || second->target != "/second" ||
        !second->body.empty()) {
        throw std::runtime_error("http_test: pipelined second request mismatch");
    }

    const auto eof = co_await conn.read_request();
    if (eof.has_value()) {
        throw std::runtime_error("http_test: pipelined eof mismatch");
    }
}

corouv::Task<void> multipart_formdata_post_case(corouv::UvExecutor& ex) {
    corouv::multipart::FormData form;

    corouv::multipart::Part title;
    title.name = "title";
    title.body = "corouv";
    form.parts.push_back(std::move(title));

    corouv::multipart::Part upload;
    upload.name = "upload";
    upload.filename = "hello.txt";
    upload.content_type = "text/plain";
    upload.body = "hello multipart";
    form.parts.push_back(std::move(upload));

    const std::string boundary = "corouv-http-boundary";
    const auto content_type = corouv::multipart::build_content_type(boundary);
    const auto body = corouv::multipart::serialize(form, boundary);

    corouv::http::Server server(
        ex,
        [](corouv::http::Request request)
            -> corouv::Task<corouv::http::Response> {
            if (request.method != "POST" || request.target != "/upload") {
                throw std::runtime_error("http_test: multipart route mismatch");
            }

            const auto parsed = corouv::multipart::parse_request(request);
            if (parsed.parts.size() != 2 ||
                parsed.parts[0].name != "title" ||
                parsed.parts[0].body != "corouv" ||
                parsed.parts[1].name != "upload" ||
                !parsed.parts[1].filename.has_value() ||
                *parsed.parts[1].filename != "hello.txt" ||
                parsed.parts[1].body != "hello multipart") {
                throw std::runtime_error("http_test: multipart parse mismatch");
            }

            corouv::http::Response response;
            response.body = "multipart ok " + parsed.parts[0].body + " " +
                            parsed.parts[1].body;
            co_return response;
        },
        corouv::http::ServerOptions{.host = "127.0.0.1", .port = 0});

    co_await server.listen();

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(server.serve())) {
        throw std::runtime_error("http_test: multipart server spawn failed");
    }

    corouv::http::Request request;
    request.method = "POST";
    request.target = "/upload";
    request.headers.push_back({"content-type", content_type});
    request.body = body;

    const auto response = co_await corouv::http::fetch(
        ex, "http://127.0.0.1:" + std::to_string(server.port()) + "/upload",
        std::move(request));

    group.cancel();
    co_await group.wait();

    if (response.status != 200 ||
        response.body != "multipart ok corouv hello multipart") {
        throw std::runtime_error(
            "http_test: multipart response mismatch status=" +
            std::to_string(response.status) + " body=" + response.body);
    }
}

corouv::Task<void> interim_response_case(corouv::UvExecutor& ex) {
    auto listener = co_await corouv::net::listen(ex, "127.0.0.1", 0);
    const auto endpoint = listener.local_endpoint();

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(interim_response_server_task(&listener))) {
        throw std::runtime_error("http_test: interim server spawn failed");
    }

    auto stream = co_await corouv::net::connect(ex, endpoint.host, endpoint.port);
    corouv::http::Connection conn(std::move(stream));

    corouv::http::Request request;
    request.method = "POST";
    request.target = "/interim";
    request.body = "ok";
    co_await conn.write_request(request, corouv::net::to_string(endpoint));
    const auto response = co_await conn.read_response(request.method);
    conn.close();

    co_await group.wait();

    if (response.status != 200 || response.body != "final") {
        throw std::runtime_error("http_test: interim response handling mismatch");
    }
}

corouv::Task<void> streaming_body_case(corouv::UvExecutor& ex) {
    auto listener = co_await corouv::net::listen(ex, "127.0.0.1", 0);
    const auto endpoint = listener.local_endpoint();

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(streaming_server_task(&listener))) {
        throw std::runtime_error("http_test: streaming server spawn failed");
    }

    auto stream = co_await corouv::net::connect(ex, endpoint.host, endpoint.port);
    corouv::http::Connection conn(std::move(stream));

    corouv::http::Request request;
    request.method = "POST";
    request.target = "/stream";

    corouv::http::BodyChunkSource request_source =
        [i = 0]() mutable -> corouv::Task<std::optional<std::string>> {
        if (i == 0) {
            ++i;
            co_return std::string("alpha");
        }
        if (i == 1) {
            ++i;
            co_return std::string("beta");
        }
        co_return std::nullopt;
    };

    co_await conn.write_request_stream(request, std::move(request_source),
                                       corouv::net::to_string(endpoint));

    std::string response_body;
    const auto response = co_await conn.read_response_stream(
        request.method, [&response_body](std::string_view chunk) -> corouv::Task<void> {
            response_body.append(chunk);
            co_return;
        });
    conn.close();

    co_await group.wait();

    if (response.status != 200 || !response.body.empty() ||
        response_body != "hello stream") {
        throw std::runtime_error("http_test: streaming response mismatch");
    }
}

corouv::Task<void> response_timeout_case(corouv::UvExecutor& ex) {
    auto listener = co_await corouv::net::listen(ex, "127.0.0.1", 0);
    const auto port = listener.local_endpoint().port;

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(response_timeout_server_task(&listener))) {
        throw std::runtime_error("http_test: timeout server spawn failed");
    }

    corouv::http::ClientOptions options;
    options.timeouts.read_headers = std::chrono::milliseconds(30);

    corouv::http::Client client(ex, options);
    co_await client.connect("127.0.0.1", port);

    corouv::http::Request request;
    request.method = "GET";
    request.target = "/timeout";

    bool timed_out = false;
    try {
        (void)co_await client.request(std::move(request));
    } catch (const corouv::http::Error& e) {
        timed_out = (e.status() == 504);
    }

    client.close();
    co_await group.wait();

    if (!timed_out) {
        throw std::runtime_error("http_test: expected response header timeout");
    }
}

corouv::Task<void> transfer_encoding_content_length_conflict_request_case() {
    http_test_detail::ScriptedStream stream;
    stream.incoming.push_back(
        "POST /smuggle HTTP/1.1\r\n"
        "Host: example\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
        "0\r\n\r\n");

    corouv::http::Connection conn(corouv::io::ByteStream(std::move(stream)));
    bool rejected = false;
    try {
        (void)co_await conn.read_request();
    } catch (const corouv::http::Error& e) {
        rejected = (e.status() == 400);
    }

    if (!rejected) {
        throw std::runtime_error(
            "http_test: expected TE+CL request rejection");
    }
}

corouv::Task<void> transfer_encoding_content_length_conflict_response_case() {
    http_test_detail::ScriptedStream stream;
    stream.incoming.push_back(
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "2\r\nok\r\n0\r\n\r\n");

    corouv::http::Connection conn(corouv::io::ByteStream(std::move(stream)));
    bool rejected = false;
    try {
        (void)co_await conn.read_response("GET");
    } catch (const corouv::http::Error& e) {
        rejected = (e.status() == 502);
    }

    if (!rejected) {
        throw std::runtime_error(
            "http_test: expected TE+CL response rejection");
    }
}

corouv::Task<void> transfer_encoding_unsupported_request_case() {
    http_test_detail::ScriptedStream stream;
    stream.incoming.push_back(
        "POST /te HTTP/1.1\r\n"
        "Host: example\r\n"
        "Transfer-Encoding: gzip\r\n"
        "\r\n");

    corouv::http::Connection conn(corouv::io::ByteStream(std::move(stream)));
    bool rejected = false;
    try {
        (void)co_await conn.read_request();
    } catch (const corouv::http::Error& e) {
        rejected = (e.status() == 400);
    }

    if (!rejected) {
        throw std::runtime_error(
            "http_test: expected unsupported request Transfer-Encoding rejection");
    }
}

corouv::Task<void> transfer_encoding_unsupported_response_case() {
    http_test_detail::ScriptedStream stream;
    stream.incoming.push_back(
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: gzip\r\n"
        "\r\n");

    corouv::http::Connection conn(corouv::io::ByteStream(std::move(stream)));
    bool rejected = false;
    try {
        (void)co_await conn.read_response("GET");
    } catch (const corouv::http::Error& e) {
        rejected = (e.status() == 502);
    }

    if (!rejected) {
        throw std::runtime_error(
            "http_test: expected unsupported response Transfer-Encoding rejection");
    }
}

corouv::Task<void> expect_header_unsupported_case() {
    http_test_detail::ScriptedStream stream;
    stream.incoming.push_back(
        "POST /expect-unsupported HTTP/1.1\r\n"
        "Host: example\r\n"
        "Expect: fancy-feature\r\n"
        "Content-Length: 0\r\n"
        "\r\n");

    corouv::http::Connection conn(corouv::io::ByteStream(std::move(stream)));
    bool rejected = false;
    try {
        (void)co_await conn.read_request();
    } catch (const corouv::http::Error& e) {
        rejected = (e.status() == 417);
    }

    if (!rejected) {
        throw std::runtime_error(
            "http_test: expected unsupported Expect header rejection");
    }
}

corouv::Task<void> expect_continue_server_case(corouv::UvExecutor& ex) {
    corouv::http::Server server(
        ex,
        [](corouv::http::Request request)
            -> corouv::Task<corouv::http::Response> {
            if (request.method != "POST" || request.target != "/expect-server" ||
                request.body != "hello") {
                throw std::runtime_error("http_test: expect-server request mismatch");
            }

            corouv::http::Response response;
            response.body = "ok";
            co_return response;
        },
        corouv::http::ServerOptions{.host = "127.0.0.1", .port = 0});

    co_await server.listen();
    auto group = co_await corouv::make_task_group();
    if (!group.spawn(server.serve())) {
        throw std::runtime_error("http_test: expect-server spawn failed");
    }

    auto stream =
        co_await corouv::net::connect(ex, "127.0.0.1", server.port());
    co_await stream.write_all(
        std::string_view("POST /expect-server HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\n"
                         "Expect: 100-continue\r\n"
                         "Content-Length: 5\r\n"
                         "\r\n"));

    const auto interim = co_await http_test_detail::read_raw_header_block(stream);
    if (interim.header_block.find("HTTP/1.1 100 Continue\r\n") != 0) {
        throw std::runtime_error("http_test: expect-server missing 100 continue");
    }
    if (!interim.extra_bytes.empty()) {
        throw std::runtime_error(
            "http_test: expect-server unexpected extra interim bytes");
    }

    co_await stream.write_all(std::string_view("hello"));
    corouv::http::Connection conn(std::move(stream));
    const auto response = co_await conn.read_response("POST");
    conn.close();

    group.cancel();
    co_await group.wait();

    if (response.status != 200 || response.body != "ok") {
        throw std::runtime_error("http_test: expect-server final response mismatch");
    }
}

corouv::Task<void> expect_continue_client_case(corouv::UvExecutor& ex) {
    auto listener = co_await corouv::net::listen(ex, "127.0.0.1", 0);
    const auto port = listener.local_endpoint().port;

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(expect_continue_client_server_task(&listener))) {
        throw std::runtime_error("http_test: expect-client server spawn failed");
    }

    corouv::http::Client client(ex);
    co_await client.connect("127.0.0.1", port);

    corouv::http::Request request;
    request.method = "POST";
    request.target = "/expect-client";
    request.body = "hello";
    request.headers.push_back({"Expect", "100-continue"});

    const auto response = co_await client.request(std::move(request));
    client.close();
    co_await group.wait();

    if (response.status != 200 || response.body != "ok") {
        throw std::runtime_error("http_test: expect-client response mismatch");
    }
}

corouv::Task<void> expect_continue_timeout_case(corouv::UvExecutor& ex) {
    auto listener = co_await corouv::net::listen(ex, "127.0.0.1", 0);
    const auto port = listener.local_endpoint().port;

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(expect_continue_timeout_server_task(&listener))) {
        throw std::runtime_error("http_test: expect-timeout server spawn failed");
    }

    corouv::http::ClientOptions options;
    options.timeouts.read_headers = 30ms;
    corouv::http::Client client(ex, options);
    co_await client.connect("127.0.0.1", port);

    corouv::http::Request request;
    request.method = "POST";
    request.target = "/expect-timeout";
    request.body = "hello";
    request.headers.push_back({"Expect", "100-continue"});

    bool timed_out = false;
    try {
        (void)co_await client.request(std::move(request));
    } catch (const corouv::http::Error& e) {
        timed_out = (e.status() == 504);
    }

    client.close();
    co_await group.wait();

    if (!timed_out) {
        throw std::runtime_error("http_test: expected 100-continue timeout");
    }
}

corouv::Task<void> chunked_trailers_roundtrip_case(corouv::UvExecutor& ex) {
    auto listener = co_await corouv::net::listen(ex, "127.0.0.1", 0);
    const auto endpoint = listener.local_endpoint();

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(trailers_server_task(&listener))) {
        throw std::runtime_error("http_test: trailers server spawn failed");
    }

    auto stream = co_await corouv::net::connect(ex, endpoint.host, endpoint.port);
    corouv::http::Connection conn(std::move(stream));

    corouv::http::Request request;
    request.method = "POST";
    request.target = "/trailers";
    request.trailers.push_back({"X-Req-Trailer", "done"});

    corouv::http::BodyChunkSource source =
        [sent = false]() mutable -> corouv::Task<std::optional<std::string>> {
        if (sent) {
            co_return std::nullopt;
        }
        sent = true;
        co_return std::string("payload");
    };
    co_await conn.write_request_stream(request, std::move(source),
                                       corouv::net::to_string(endpoint));

    const auto response = co_await conn.read_response(request.method);
    conn.close();
    co_await group.wait();

    const auto resp_trailer =
        corouv::http::find_header(response.trailers, "X-Resp-Trailer");
    if (response.status != 200 || response.body != "ok" ||
        !resp_trailer.has_value() || *resp_trailer != "sent") {
        throw std::runtime_error("http_test: trailer response mismatch");
    }
}

corouv::Task<void> early_hints_response_case(corouv::UvExecutor& ex) {
    auto listener = co_await corouv::net::listen(ex, "127.0.0.1", 0);
    const auto endpoint = listener.local_endpoint();

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(early_hints_server_task(&listener))) {
        throw std::runtime_error("http_test: early hints server spawn failed");
    }

    auto stream = co_await corouv::net::connect(ex, endpoint.host, endpoint.port);
    corouv::http::Connection conn(std::move(stream));

    corouv::http::Request request;
    request.method = "GET";
    request.target = "/early-hints";
    co_await conn.write_request(request, corouv::net::to_string(endpoint));

    const auto response = co_await conn.read_response(request.method);
    conn.close();

    co_await group.wait();

    if (response.status != 200 || response.body != "final") {
        throw std::runtime_error("http_test: early hints response mismatch");
    }
}

corouv::Task<void> switching_protocols_case() {
    http_test_detail::ScriptedStream stream;
    stream.incoming.push_back(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "\r\n"
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "ok");

    corouv::http::Connection conn(corouv::io::ByteStream(std::move(stream)));

    const auto upgraded = co_await conn.read_response("GET");
    if (upgraded.status != 101 || upgraded.reason != "Switching Protocols" ||
        !upgraded.body.empty()) {
        throw std::runtime_error(
            "http_test: switching protocols response mismatch");
    }

    const auto following = co_await conn.read_response("GET");
    if (following.status != 200 || following.body != "ok") {
        throw std::runtime_error(
            "http_test: response after switching protocols mismatch");
    }
}

corouv::Task<void> request_body_timeout_case() {
    http_test_detail::TimedScriptedStream stream;
    stream.incoming.push_back(
        "POST /slow-body HTTP/1.1\r\n"
        "Host: example\r\n"
        "Content-Length: 5\r\n"
        "\r\n");
    stream.incoming.push_back("hello");
    stream.recv_delays.push_back(0ms);
    stream.recv_delays.push_back(200ms);

    corouv::http::IoTimeouts timeouts;
    timeouts.read_body = 30ms;
    corouv::http::Connection conn(
        corouv::io::ByteStream(std::move(stream)), corouv::http::Limits{},
        timeouts);

    bool timed_out = false;
    try {
        (void)co_await conn.read_request();
    } catch (const corouv::http::Error& e) {
        timed_out = (e.status() == 408);
    }

    if (!timed_out) {
        throw std::runtime_error("http_test: expected request body timeout");
    }
}

corouv::Task<void> write_timeout_case() {
    http_test_detail::TimedScriptedStream stream;
    stream.send_delay = 200ms;

    corouv::http::IoTimeouts timeouts;
    timeouts.write = 30ms;
    corouv::http::Connection conn(
        corouv::io::ByteStream(std::move(stream)), corouv::http::Limits{},
        timeouts);

    corouv::http::Request request;
    request.method = "POST";
    request.target = "/write-timeout";
    request.body = "abc";

    bool timed_out = false;
    try {
        co_await conn.write_request(request, "example.test");
    } catch (const corouv::http::Error& e) {
        timed_out = (e.status() == 504);
    }

    if (!timed_out) {
        throw std::runtime_error("http_test: expected write timeout");
    }
}

int main() {
    corouv::Runtime rt;
    rt.run(server_client_case(rt.executor()));
    rt.run(keep_alive_and_chunked_case(rt.executor()));
    rt.run(connection_close_body_case(rt.executor()));
    rt.run(fixed_body_pipeline_overread_case());
    rt.run(transfer_encoding_content_length_conflict_request_case());
    rt.run(transfer_encoding_content_length_conflict_response_case());
    rt.run(transfer_encoding_unsupported_request_case());
    rt.run(transfer_encoding_unsupported_response_case());
    rt.run(multipart_formdata_post_case(rt.executor()));
    rt.run(interim_response_case(rt.executor()));
    rt.run(expect_continue_server_case(rt.executor()));
    rt.run(expect_continue_client_case(rt.executor()));
    rt.run(expect_continue_timeout_case(rt.executor()));
    rt.run(expect_header_unsupported_case());
    rt.run(chunked_trailers_roundtrip_case(rt.executor()));
    rt.run(early_hints_response_case(rt.executor()));
    rt.run(switching_protocols_case());
    rt.run(streaming_body_case(rt.executor()));
    rt.run(response_timeout_case(rt.executor()));
    rt.run(request_body_timeout_case());
    rt.run(write_timeout_case());
    return 0;
}
