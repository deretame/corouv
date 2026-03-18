#include <corouv/net.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>

#include <array>
#include <stdexcept>

corouv::Task<void> udp_recv_some_server(corouv::net::UdpSocket* server) {
    std::array<char, 4> small{};
    const auto info = co_await server->recv_some_from(
        std::span<char>(small.data(), small.size()));

    if (std::string_view(small.data(), info.size) != "0123") {
        throw std::runtime_error("udp_recv_some_test: payload prefix mismatch");
    }
    if (!info.truncated()) {
        throw std::runtime_error("udp_recv_some_test: expected truncation");
    }

    co_await server->send_to("ok", info.peer);
    server->close();
}

corouv::Task<void> udp_recv_some_case() {
    auto server = co_await corouv::net::bind("127.0.0.1", 0);
    auto client = co_await corouv::net::bind("127.0.0.1", 0);

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(udp_recv_some_server(&server))) {
        throw std::runtime_error("udp_recv_some_test: spawn failed");
    }

    co_await client.send_to("0123456789", server.local_endpoint());

    std::array<char, 8> reply{};
    const auto info =
        co_await client.recv_some(std::span<char>(reply.data(), reply.size()));

    if (std::string_view(reply.data(), info.size) != "ok") {
        throw std::runtime_error("udp_recv_some_test: reply mismatch");
    }

    client.close();
    co_await group.wait();
}

int main() {
    corouv::Runtime rt;
    rt.run(udp_recv_some_case());
    return 0;
}

