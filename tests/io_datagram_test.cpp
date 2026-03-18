#include <corouv/io.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>

#include <stdexcept>

corouv::Task<void> io_datagram_server(corouv::io::DatagramSocket* server) {
    server->set_broadcast(false);
    server->set_ttl(8);

    auto request = co_await server->recv_from();
    if (request.payload != "ping-io") {
        throw std::runtime_error("io_datagram_test: server payload mismatch");
    }

    co_await server->send_to("pong-io", request.peer);
    server->close();
}

corouv::Task<void> io_datagram_case() {
    auto server = co_await corouv::io::bind("127.0.0.1", 0);
    auto client = co_await corouv::io::bind("127.0.0.1", 0);

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(io_datagram_server(&server))) {
        throw std::runtime_error("io_datagram_test: server spawn failed");
    }

    co_await client.connect("127.0.0.1", server.local_endpoint().port);
    if (client.peer_endpoint().port != server.local_endpoint().port) {
        throw std::runtime_error("io_datagram_test: peer endpoint mismatch");
    }

    const auto sent = co_await client.send("ping-io");
    if (sent != 7) {
        throw std::runtime_error("io_datagram_test: unexpected sent size");
    }

    const auto response = co_await client.recv();
    client.close();

    co_await group.wait();

    if (response.payload != "pong-io") {
        throw std::runtime_error("io_datagram_test: client payload mismatch");
    }
}

int main() {
    corouv::Runtime rt;
    rt.run(io_datagram_case());
    return 0;
}
