#pragma once

#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

#include "corouv/async_queue.h"
#include "corouv/io.h"
#include "corouv/net.h"

namespace corouv::net {

class DatagramChannel {
public:
    explicit DatagramChannel(
        std::size_t capacity = AsyncQueue<Datagram>::kUnbounded)
        : _queue(capacity) {}

    Task<void> push(Datagram datagram) {
        co_await _queue.push(std::move(datagram));
        co_return;
    }

    bool try_push(Datagram datagram) {
        return _queue.try_push(std::move(datagram));
    }

    Task<Datagram> recv() { co_return co_await _queue.pop(); }

    std::optional<Datagram> try_recv() { return _queue.try_pop(); }

    [[nodiscard]] bool empty() const noexcept { return _queue.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return _queue.size(); }
    [[nodiscard]] bool bounded() const noexcept { return _queue.bounded(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return _queue.capacity(); }

    template <class Socket>
    Task<void> pump(Socket& socket) {
        while (socket.is_open()) {
            try {
                co_await push(co_await socket.recv_from());
            } catch (const std::logic_error&) {
                if (!socket.is_open()) {
                    break;
                }
                throw;
            }
        }
        co_return;
    }

private:
    AsyncQueue<Datagram> _queue;
};

}  // namespace corouv::net

namespace corouv::io {

using DatagramChannel = corouv::net::DatagramChannel;

}  // namespace corouv::io
