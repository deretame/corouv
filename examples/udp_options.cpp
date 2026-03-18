#include <corouv/net.h>
#include <corouv/runtime.h>

#include <iostream>

corouv::Task<void> udp_options_demo() {
    corouv::net::UdpBindOptions options;
    options.reuse_address = true;

    auto socket = co_await corouv::net::bind("0.0.0.0", 0, options);

    socket.set_broadcast(true);
    socket.set_ttl(16);
    socket.set_multicast_loop(true);
    socket.set_multicast_ttl(8);
    socket.set_multicast_interface("0.0.0.0");
    socket.join_multicast("239.255.0.1");
    socket.leave_multicast("239.255.0.1");

    std::cout << "[udp-options] local="
              << corouv::net::to_string(socket.local_endpoint()) << "\n";
    socket.close();
}

int main() {
    corouv::Runtime rt;
    rt.run(udp_options_demo());
    return 0;
}

