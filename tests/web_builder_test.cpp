#include <corouv/runtime.h>
#include <corouv/task_group.h>
#include <corouv/web.h>

#include <stdexcept>
#include <string>

corouv::Task<void> web_builder_case(corouv::UvExecutor& ex) {
    auto server = corouv::web::ServerBuilder(ex)
                      .listen_on("127.0.0.1", 0)
                      .post("/builder", [](corouv::web::Request request)
                                              -> corouv::Task<corouv::web::Response> {
                          corouv::web::Response response;
                          response.body = request.method + " " + request.target +
                                          " " + request.body;
                          co_return response;
                      })
                      .not_found([](corouv::web::Request)
                                     -> corouv::Task<corouv::web::Response> {
                          corouv::web::Response response;
                          response.status = 404;
                          response.reason = corouv::web::reason_phrase(404);
                          response.body = "missing";
                          response.keep_alive = false;
                          co_return response;
                      })
                      .build();

    co_await server.listen();

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(server.serve())) {
        throw std::runtime_error("web_builder_test: server spawn failed");
    }

    auto client = corouv::web::ClientBuilder(ex).keep_alive(true).build();

    corouv::web::Request request;
    request.method = "POST";
    request.body = "builder";

    const auto response = co_await client.fetch(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/builder",
        request);

    if (!client.is_connected()) {
        throw std::runtime_error("web_builder_test: expected connected client");
    }
    if (response.status != 200 || response.body != "POST /builder builder") {
        throw std::runtime_error("web_builder_test: response mismatch");
    }

    const auto missing = co_await client.fetch(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/missing");
    if (missing.status != 404 || missing.body != "missing") {
        throw std::runtime_error("web_builder_test: not_found mismatch");
    }

    client.close();
    server.close();
    co_await group.wait();
}

int main() {
    corouv::Runtime rt;
    rt.run(web_builder_case(rt.executor()));
    return 0;
}
