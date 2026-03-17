#include <corouv/http.h>
#include <corouv/runtime.h>

#include <iostream>
#include <string>

corouv::Task<void> run_http_client(corouv::UvExecutor& ex, std::string url) {
    corouv::http::Request request;
    request.method = "GET";

    const auto response =
        co_await corouv::http::fetch(ex, url, std::move(request));

    std::cout << "[http_client] status=" << response.status << " "
              << response.reason << "\n";
    for (const auto& header : response.headers) {
        std::cout << "[http_client] header " << header.name << ": "
                  << header.value << "\n";
    }
    std::cout << "[http_client] body=" << response.body << "\n";
}

int main(int argc, char** argv) {
    const std::string url =
        argc > 1 ? argv[1] : "http://127.0.0.1:8080/hello?name=corouv";

    corouv::Runtime rt;
    rt.run(run_http_client(rt.executor(), url));
    return 0;
}
