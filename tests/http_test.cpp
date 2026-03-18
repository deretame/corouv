#include <corouv/http.h>
#include <corouv/net.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>

#include <atomic>
#include <stdexcept>
#include <string>

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

int main() {
    corouv::Runtime rt;
    rt.run(server_client_case(rt.executor()));
    rt.run(keep_alive_and_chunked_case(rt.executor()));
    rt.run(connection_close_body_case(rt.executor()));
    return 0;
}
