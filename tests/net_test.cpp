#include <corouv/net.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>

#include <array>
#include <stdexcept>
#include <string>

corouv::Task<void> tcp_server_task(corouv::net::TcpListener* listener,
                                   std::string* server_received) {
    auto peer = co_await listener->accept();

    std::array<char, 16> scratch{};
    const auto n =
        co_await peer.read_some(std::span<char>(scratch.data(), scratch.size()));
    server_received->assign(scratch.data(), n);

    co_await peer.write_all(std::string_view("pong"));
    peer.close();
    listener->close();
}

corouv::Task<void> tcp_roundtrip_case() {
    auto listener = co_await corouv::net::listen("127.0.0.1", 0);
    const auto port = listener.local_endpoint().port;

    std::string server_received;
    auto group = co_await corouv::make_task_group();
    const bool server_ok = group.spawn(tcp_server_task(&listener, &server_received));
    if (!server_ok) {
        throw std::runtime_error("net_test: server spawn failed");
    }

    auto client = co_await corouv::net::connect("127.0.0.1", port);
    co_await client.write_all(std::string_view("ping"));

    std::array<char, 16> reply{};
    const auto n =
        co_await client.read_some(std::span<char>(reply.data(), reply.size()));
    client.close();

    co_await group.wait();

    if (server_received != "ping") {
        throw std::runtime_error("net_test: server payload mismatch");
    }
    if (std::string(reply.data(), n) != "pong") {
        throw std::runtime_error("net_test: client payload mismatch");
    }
}

int main() {
    corouv::Runtime rt;
    rt.run(tcp_roundtrip_case());
    return 0;
}
