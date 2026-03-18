#include <corouv/net.h>
#include <corouv/runtime.h>

#include <stdexcept>

corouv::Task<void> udp_options_case() {
    corouv::net::UdpBindOptions options;
    options.reuse_address = true;

    auto a = co_await corouv::net::bind("0.0.0.0", 0, options);
    auto b = co_await corouv::net::bind("0.0.0.0", a.local_endpoint().port, options);

    a.set_broadcast(true);
    a.set_ttl(16);
    a.set_multicast_loop(true);
    a.set_multicast_ttl(8);
    a.set_multicast_interface("0.0.0.0");
    a.join_multicast("239.255.0.1");
    a.leave_multicast("239.255.0.1");
    a.join_multicast_source("239.255.0.2", "127.0.0.1");
    a.leave_multicast_source("239.255.0.2", "127.0.0.1");

    if (a.local_endpoint().port == 0 || b.local_endpoint().port == 0) {
        throw std::runtime_error("udp_options_test: bind did not produce a port");
    }

    a.close();
    b.close();
}

int main() {
    corouv::Runtime rt;
    rt.run(udp_options_case());
    return 0;
}
