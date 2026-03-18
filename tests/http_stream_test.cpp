#include <corouv/http.h>
#include <corouv/net.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>
#include <corouv/transport.h>

#include <stdexcept>
#include <string>

corouv::Task<void> passthrough_http_server(corouv::net::TcpListener* listener) {
    auto raw = co_await listener->accept();
    auto stream = corouv::transport::make_passthrough(std::move(raw));
    corouv::http::Connection conn(std::move(stream));

    auto request = co_await conn.read_request();
    if (!request.has_value()) {
        throw std::runtime_error("http_stream_test: expected request");
    }
    if (request->method != "POST" || request->target != "/wrapped" ||
        request->body != "payload") {
        throw std::runtime_error("http_stream_test: request mismatch");
    }

    corouv::http::Response response;
    response.chunked = true;
    response.body = "wrapped-response";
    co_await conn.write_response(response, request->method);

    conn.close();
    listener->close();
}

corouv::Task<void> passthrough_http_case(corouv::UvExecutor& ex) {
    auto listener = co_await corouv::net::listen(ex, "127.0.0.1", 0);
    const auto endpoint = listener.local_endpoint();

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(passthrough_http_server(&listener))) {
        throw std::runtime_error("http_stream_test: spawn failed");
    }

    auto raw = co_await corouv::net::connect(ex, endpoint.host, endpoint.port);
    auto stream = corouv::transport::make_passthrough(std::move(raw));
    corouv::http::Connection conn(std::move(stream));

    corouv::http::Request request;
    request.method = "POST";
    request.target = "/wrapped";
    request.body = "payload";
    co_await conn.write_request(request, corouv::net::to_string(endpoint));

    const auto response = co_await conn.read_response(request.method);
    conn.close();

    co_await group.wait();

    if (response.status != 200 || response.body != "wrapped-response" ||
        !response.chunked) {
        throw std::runtime_error("http_stream_test: response mismatch");
    }
}

int main() {
    corouv::Runtime rt;
    rt.run(passthrough_http_case(rt.executor()));
    return 0;
}
