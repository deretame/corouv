#include <corouv/net.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>

#include <stdexcept>

corouv::Task<void> udp_server_task(corouv::net::UdpSocket* server) {
    auto request = co_await server->recv_from();
    if (request.payload != "ping") {
        throw std::runtime_error("udp_test: server payload mismatch");
    }

    co_await server->send_to("pong", request.peer);
    server->close();
}

corouv::Task<void> udp_roundtrip_case() {
    auto server = co_await corouv::net::bind("127.0.0.1", 0);
    auto client = co_await corouv::net::bind("127.0.0.1", 0);

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(udp_server_task(&server))) {
        throw std::runtime_error("udp_test: server spawn failed");
    }

    co_await client.connect("127.0.0.1", server.local_endpoint().port);
    if (client.peer_endpoint().port != server.local_endpoint().port) {
        throw std::runtime_error("udp_test: peer endpoint not updated");
    }

    const auto sent = co_await client.send("ping");
    if (sent != 4) {
        throw std::runtime_error("udp_test: unexpected sent size");
    }

    const auto response = co_await client.recv();
    client.close();

    co_await group.wait();

    if (response.payload != "pong") {
        throw std::runtime_error("udp_test: client payload mismatch");
    }
}

int main() {
    corouv::Runtime rt;
    rt.run(udp_roundtrip_case());
    return 0;
}

