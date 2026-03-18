#include <corouv/net.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>
#include <corouv/timeout.h>

#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;

corouv::Task<void> udp_multicast_demo() {
    corouv::net::UdpBindOptions options;
    options.reuse_address = true;

    auto receiver = co_await corouv::net::bind("0.0.0.0", 0, options);
    auto sender = co_await corouv::net::bind("0.0.0.0", 0);

    receiver.set_multicast_loop(true);
    receiver.join_multicast("239.255.0.42", "127.0.0.1");

    sender.set_multicast_interface("127.0.0.1");
    sender.set_multicast_loop(true);

    co_await sender.send_to("mcast",
                            {"239.255.0.42", receiver.local_endpoint().port});
    auto packet = co_await corouv::with_timeout(receiver.recv_from(), 2s);

    std::cout << "[udp-mcast] recv=" << packet.payload
              << " from=" << corouv::net::to_string(packet.peer) << "\n";

    receiver.leave_multicast("239.255.0.42", "127.0.0.1");
    receiver.close();
    sender.close();
}

int main() {
    corouv::Runtime rt;
    rt.run(udp_multicast_demo());
    return 0;
}
