#include <corouv/io.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>

#include <iostream>
#include <stdexcept>

corouv::Task<void> udp_server(corouv::io::DatagramSocket* server) {
    auto request = co_await server->recv_from();
    std::cout << "[udp] server recv=" << request.payload
              << " from=" << corouv::net::to_string(request.peer) << "\n";

    co_await server->send_to("pong", request.peer);
    server->close();
}

corouv::Task<void> udp_demo() {
    auto server = co_await corouv::io::bind("127.0.0.1", 0);
    auto client = co_await corouv::io::bind("127.0.0.1", 0);

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(udp_server(&server))) {
        throw std::runtime_error("udp example: server spawn failed");
    }

    co_await client.connect("127.0.0.1", server.local_endpoint().port);
    co_await client.send("ping");

    const auto response = co_await client.recv();
    std::cout << "[udp] client recv=" << response.payload << "\n";

    client.close();
    co_await group.wait();
}

int main() {
    corouv::Runtime rt;
    rt.run(udp_demo());
    return 0;
}
