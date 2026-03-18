#include <corouv/runtime.h>
#include <corouv/task_group.h>
#include <corouv/web.h>

#include <iostream>
#include <string>

corouv::Task<void> web_builder_demo(corouv::UvExecutor& ex) {
    auto server = corouv::web::ServerBuilder(ex)
                      .listen_on("127.0.0.1", 0)
                      .get("/hello", [](corouv::web::Request request)
                                           -> corouv::Task<corouv::web::Response> {
                          corouv::web::Response response;
                          response.headers.push_back({"content-type", "text/plain"});
                          response.body = "builder says: " + request.target;
                          co_return response;
                      })
                      .not_found([](corouv::web::Request request)
                                     -> corouv::Task<corouv::web::Response> {
                          corouv::web::Response response;
                          response.status = 404;
                          response.reason = corouv::web::reason_phrase(404);
                          response.body = "no route for " + request.target;
                          response.keep_alive = false;
                          co_return response;
                      })
                      .build();

    co_await server.listen();

    auto group = co_await corouv::make_task_group();
    (void)group.spawn(server.serve());

    auto client = corouv::web::ClientBuilder(ex).build();
    const auto response = co_await client.fetch(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/hello");
    const auto missing = co_await client.fetch(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/missing");

    std::cout << "[web-builder] status=" << response.status << "\n";
    std::cout << "[web-builder] body=" << response.body << "\n";
    std::cout << "[web-builder] missing=" << missing.status << " "
              << missing.body << "\n";

    client.close();
    server.close();
    co_await group.wait();
}

int main() {
    corouv::Runtime rt;
    rt.run(web_builder_demo(rt.executor()));
    return 0;
}
