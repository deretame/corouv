#include <corouv/http.h>
#include <corouv/runtime.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>

corouv::Task<void> run_http_server(corouv::Runtime* rt, std::string host,
                                   std::uint16_t port, int max_requests) {
    std::atomic<int> handled{0};
    corouv::http::Server* server_ptr = nullptr;

    corouv::http::Server server(
        rt->executor(),
        [&](corouv::http::Request request) -> corouv::Task<corouv::http::Response> {
            corouv::http::Response response;
            response.headers.push_back({"content-type", "text/plain"});
            response.body = "method=" + request.method + "\ntarget=" +
                            request.target + "\nbody=" + request.body + "\n";

            if (handled.fetch_add(1, std::memory_order_relaxed) + 1 >=
                max_requests) {
                if (server_ptr) {
                    server_ptr->close();
                }
            }

            co_return response;
        },
        corouv::http::ServerOptions{
            .host = std::move(host),
            .port = port,
        });
    server_ptr = &server;

    co_await server.listen();
    std::cout << "[http_server] listening on " << server.host() << ":"
              << server.port() << " (max_requests=" << max_requests << ")\n";
    co_await server.serve();
}

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const auto port =
        static_cast<std::uint16_t>(argc > 2 ? std::atoi(argv[2]) : 8080);
    const int max_requests = argc > 3 ? std::atoi(argv[3]) : 1;

    corouv::Runtime rt;
    rt.run(run_http_server(&rt, host, port, max_requests));
    return 0;
}
