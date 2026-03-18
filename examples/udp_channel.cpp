#include <corouv/datagram_channel.h>
#include <corouv/io.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>

#include <iostream>
#include <stdexcept>

corouv::Task<void> udp_channel_demo() {
    auto server = co_await corouv::io::bind("127.0.0.1", 0);
    auto client = co_await corouv::io::bind("127.0.0.1", 0);

    corouv::io::DatagramChannel channel(4);
    auto group = co_await corouv::make_task_group();

    if (!group.spawn(channel.pump(server))) {
        throw std::runtime_error("udp channel example: pump spawn failed");
    }

    co_await client.send_to("hello-channel", server.local_endpoint());
    auto packet = co_await channel.recv();

    std::cout << "[udp-channel] recv=" << packet.payload
              << " from=" << corouv::net::to_string(packet.peer) << "\n";

    server.close();
    client.close();
    co_await group.wait();
}

int main() {
    corouv::Runtime rt;
    rt.run(udp_channel_demo());
    return 0;
}
