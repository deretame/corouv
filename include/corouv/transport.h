#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "corouv/net.h"
#include "corouv/task.h"

namespace corouv::transport {

class MemoryCodec {
public:
    virtual ~MemoryCodec() = default;

    virtual void start_client() = 0;
    virtual void start_server() = 0;

    virtual bool handshake_complete() const noexcept = 0;
    virtual bool wants_input() const noexcept = 0;
    virtual bool has_pending_ciphertext() const noexcept = 0;
    virtual bool plaintext_eof() const noexcept { return false; }
    virtual std::optional<std::chrono::milliseconds> handshake_timeout() const
        noexcept {
        return std::nullopt;
    }

    virtual std::size_t write_plaintext(std::string_view data) = 0;
    virtual std::size_t feed_ciphertext(std::string_view data) = 0;

    virtual std::size_t read_plaintext(std::span<char> out) = 0;
    virtual std::size_t read_ciphertext(std::span<char> out) = 0;
    virtual void start_close() = 0;
};

class PassthroughCodec final : public MemoryCodec {
public:
    void start_client() override {}
    void start_server() override {}

    bool handshake_complete() const noexcept override { return true; }
    bool wants_input() const noexcept override { return false; }
    bool has_pending_ciphertext() const noexcept override;
    bool plaintext_eof() const noexcept override { return false; }

    std::size_t write_plaintext(std::string_view data) override;
    std::size_t feed_ciphertext(std::string_view data) override;

    std::size_t read_plaintext(std::span<char> out) override;
    std::size_t read_ciphertext(std::span<char> out) override;
    void start_close() override {}

private:
    static std::size_t drain(std::string& buffer, std::span<char> out);

    std::string _incoming;
    std::string _outgoing;
};

struct TlsClientConfig {
    std::string server_name;
    std::vector<std::string> trust_anchors_pem;
    std::optional<std::chrono::milliseconds> handshake_timeout;
};

struct TlsServerConfig {
    std::vector<std::string> certificate_chain_pem;
    std::string private_key_pem;
    std::optional<std::chrono::milliseconds> handshake_timeout;
};

std::unique_ptr<MemoryCodec> make_passthrough_codec();
std::unique_ptr<MemoryCodec> make_bearssl_client_codec(TlsClientConfig config);
std::unique_ptr<MemoryCodec> make_bearssl_server_codec(TlsServerConfig config);

class CodecStream {
public:
    CodecStream() = default;
    CodecStream(net::TcpStream stream, std::unique_ptr<MemoryCodec> codec);

    CodecStream(const CodecStream&) = delete;
    CodecStream& operator=(const CodecStream&) = delete;
    CodecStream(CodecStream&&) noexcept = default;
    CodecStream& operator=(CodecStream&&) noexcept = default;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] uv_os_sock_t native_handle() const noexcept;
    [[nodiscard]] const net::Endpoint& local_endpoint() const noexcept;
    [[nodiscard]] const net::Endpoint& peer_endpoint() const noexcept;

    Task<void> handshake_client();
    Task<void> handshake_server();
    Task<void> close_notify();

    Task<std::size_t> read_some(std::span<char> buffer);
    Task<void> write_all(std::span<const char> data);
    Task<void> write_all(std::string_view data);
    Task<std::string> read_until_eof(
        std::size_t max_bytes = 16 * 1024 * 1024);

    void shutdown_write() noexcept;
    void close() noexcept;

private:
    Task<void> run_handshake(bool client);
    Task<void> flush_ciphertext();
    void feed_ciphertext_bytes(std::string_view data);
    void feed_ciphertext_backlog();
    [[nodiscard]] bool has_ciphertext_backlog() const noexcept;
    void compact_ciphertext_backlog() noexcept;

    net::TcpStream _stream;
    std::unique_ptr<MemoryCodec> _codec;
    std::string _ciphertext_backlog;
    std::size_t _ciphertext_backlog_offset{0};
};

CodecStream make_passthrough(net::TcpStream stream);

}  // namespace corouv::transport
