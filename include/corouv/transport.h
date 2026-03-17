#pragma once

#include <cstdint>
#include <memory>
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

    virtual std::size_t write_plaintext(std::string_view data) = 0;
    virtual std::size_t feed_ciphertext(std::string_view data) = 0;

    virtual std::size_t read_plaintext(std::span<char> out) = 0;
    virtual std::size_t read_ciphertext(std::span<char> out) = 0;
};

class PassthroughCodec final : public MemoryCodec {
public:
    void start_client() override {}
    void start_server() override {}

    bool handshake_complete() const noexcept override { return true; }
    bool wants_input() const noexcept override { return false; }
    bool has_pending_ciphertext() const noexcept override;

    std::size_t write_plaintext(std::string_view data) override;
    std::size_t feed_ciphertext(std::string_view data) override;

    std::size_t read_plaintext(std::span<char> out) override;
    std::size_t read_ciphertext(std::span<char> out) override;

private:
    static std::size_t drain(std::string& buffer, std::span<char> out);

    std::string _incoming;
    std::string _outgoing;
};

struct TlsClientConfig {
    std::string server_name;
    std::vector<std::string> trust_anchors_pem;
};

struct TlsServerConfig {
    std::vector<std::string> certificate_chain_pem;
    std::string private_key_pem;
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

    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] uv_os_sock_t native_handle() const noexcept;

    Task<void> handshake_client();
    Task<void> handshake_server();

    Task<std::size_t> read_some(std::span<char> buffer);
    Task<void> write_all(std::string_view data);
    Task<std::string> read_until_eof(
        std::size_t max_bytes = 16 * 1024 * 1024);

    void close() noexcept;

private:
    Task<void> run_handshake(bool client);
    Task<void> flush_ciphertext();

    net::TcpStream _stream;
    std::unique_ptr<MemoryCodec> _codec;
};

CodecStream make_passthrough(net::TcpStream stream);

}  // namespace corouv::transport
