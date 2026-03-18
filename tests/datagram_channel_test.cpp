#include <corouv/datagram_channel.h>
#include <corouv/io.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>

#include <stdexcept>

corouv::Task<void> datagram_channel_case() {
    auto server = co_await corouv::io::bind("127.0.0.1", 0);
    auto client = co_await corouv::io::bind("127.0.0.1", 0);

    corouv::io::DatagramChannel channel(4);
    auto group = co_await corouv::make_task_group();

    if (!group.spawn(channel.pump(server))) {
        throw std::runtime_error("datagram_channel_test: pump spawn failed");
    }

    co_await client.send_to("hello-channel", server.local_endpoint());
    auto packet = co_await channel.recv();

    if (packet.payload != "hello-channel") {
        throw std::runtime_error("datagram_channel_test: payload mismatch");
    }

    server.close();
    client.close();
    co_await group.wait();
}

int main() {
    corouv::Runtime rt;
    rt.run(datagram_channel_case());
    return 0;
}
