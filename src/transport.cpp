#include "corouv/transport.h"

#include <bearssl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace corouv::transport {

namespace {

[[noreturn]] void throw_tls_error(const char* what, int err = 0) {
    std::string msg = what;
    if (err != 0) {
        msg.append(": BearSSL error ");
        msg.append(std::to_string(err));
    }
    throw std::runtime_error(std::move(msg));
}

std::size_t copy_and_erase(std::string& buffer, std::span<char> out) {
    const auto count = std::min(buffer.size(), out.size());
    if (count == 0) {
        return 0;
    }

    std::memcpy(out.data(), buffer.data(), count);
    buffer.erase(0, count);
    return count;
}

struct PemObject {
    std::string name;
    std::vector<unsigned char> data;
};

void pem_append(void* ctx, const void* data, std::size_t len) {
    auto* out = static_cast<std::vector<unsigned char>*>(ctx);
    const auto* bytes = static_cast<const unsigned char*>(data);
    out->insert(out->end(), bytes, bytes + len);
}

std::vector<PemObject> decode_pem_objects(std::string_view input) {
    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);

    std::vector<PemObject> out;
    std::vector<unsigned char> current;
    std::string source(input);
    if (source.empty() || source.back() != '\n') {
        source.push_back('\n');
    }

    const auto* buf = reinterpret_cast<const unsigned char*>(source.data());
    std::size_t len = source.size();

    while (len > 0) {
        const auto used = br_pem_decoder_push(&pc, buf, len);
        buf += used;
        len -= used;

        switch (br_pem_decoder_event(&pc)) {
            case BR_PEM_BEGIN_OBJ:
                current.clear();
                br_pem_decoder_setdest(&pc, &pem_append, &current);
                break;
            case BR_PEM_END_OBJ:
                out.push_back(PemObject{
                    std::string(br_pem_decoder_name(&pc)),
                    std::move(current),
                });
                current.clear();
                break;
            case BR_PEM_ERROR:
                throw_tls_error("invalid PEM data");
            default:
                break;
        }
    }

    return out;
}

struct OwnedCertificateChain {
    std::vector<std::vector<unsigned char>> blobs;
    std::vector<br_x509_certificate> certs;

    void refresh() {
        certs.clear();
        certs.reserve(blobs.size());
        for (auto& blob : blobs) {
            certs.push_back(
                br_x509_certificate{blob.data(), static_cast<size_t>(blob.size())});
        }
    }
};

OwnedCertificateChain load_certificates(const std::vector<std::string>& pems) {
    OwnedCertificateChain chain;
    for (const auto& pem : pems) {
        for (auto& obj : decode_pem_objects(pem)) {
            if (obj.name == "CERTIFICATE" || obj.name == "X509 CERTIFICATE") {
                chain.blobs.push_back(std::move(obj.data));
            }
        }
    }
    if (chain.blobs.empty()) {
        throw_tls_error("no certificate found in PEM input");
    }
    chain.refresh();
    return chain;
}

struct OwnedPrivateKey {
    enum class Type { None, RSA, EC };

    Type type{Type::None};
    br_rsa_private_key rsa{};
    br_ec_private_key ec{};
    std::vector<unsigned char> p;
    std::vector<unsigned char> q;
    std::vector<unsigned char> dp;
    std::vector<unsigned char> dq;
    std::vector<unsigned char> iq;
    std::vector<unsigned char> x;
};

OwnedPrivateKey load_private_key(std::string_view pem) {
    const auto objects = decode_pem_objects(pem);
    for (const auto& obj : objects) {
        if (obj.name != "RSA PRIVATE KEY" && obj.name != "EC PRIVATE KEY" &&
            obj.name != "PRIVATE KEY") {
            continue;
        }

        br_skey_decoder_context dc;
        br_skey_decoder_init(&dc);
        br_skey_decoder_push(&dc, obj.data.data(), obj.data.size());
        const int err = br_skey_decoder_last_error(&dc);
        if (err != 0) {
            throw_tls_error("failed to decode private key", err);
        }

        OwnedPrivateKey key;
        switch (br_skey_decoder_key_type(&dc)) {
            case BR_KEYTYPE_RSA: {
                key.type = OwnedPrivateKey::Type::RSA;
                const auto* rk = br_skey_decoder_get_rsa(&dc);
                key.p.assign(rk->p, rk->p + rk->plen);
                key.q.assign(rk->q, rk->q + rk->qlen);
                key.dp.assign(rk->dp, rk->dp + rk->dplen);
                key.dq.assign(rk->dq, rk->dq + rk->dqlen);
                key.iq.assign(rk->iq, rk->iq + rk->iqlen);
                key.rsa = br_rsa_private_key{
                    rk->n_bitlen,
                    key.p.data(),
                    key.p.size(),
                    key.q.data(),
                    key.q.size(),
                    key.dp.data(),
                    key.dp.size(),
                    key.dq.data(),
                    key.dq.size(),
                    key.iq.data(),
                    key.iq.size(),
                };
                return key;
            }
            case BR_KEYTYPE_EC: {
                key.type = OwnedPrivateKey::Type::EC;
                const auto* ek = br_skey_decoder_get_ec(&dc);
                key.x.assign(ek->x, ek->x + ek->xlen);
                key.ec = br_ec_private_key{
                    ek->curve,
                    key.x.data(),
                    key.x.size(),
                };
                return key;
            }
            default:
                throw_tls_error("unsupported private key type");
        }
    }

    throw_tls_error("no private key found in PEM input");
}

void dn_append(void* ctx, const void* buf, std::size_t len) {
    auto* out = static_cast<std::vector<unsigned char>*>(ctx);
    const auto* bytes = static_cast<const unsigned char*>(buf);
    out->insert(out->end(), bytes, bytes + len);
}

struct OwnedTrustAnchor {
    std::vector<unsigned char> dn;
    std::vector<unsigned char> rsa_n;
    std::vector<unsigned char> rsa_e;
    std::vector<unsigned char> ec_q;
    br_x509_trust_anchor ta{};
};

OwnedTrustAnchor certificate_to_trust_anchor(const br_x509_certificate& cert) {
    br_x509_decoder_context dc;
    std::vector<unsigned char> dn;
    br_x509_decoder_init(&dc, &dn_append, &dn);
    br_x509_decoder_push(&dc, cert.data, cert.data_len);
    auto* pk = br_x509_decoder_get_pkey(&dc);
    if (pk == nullptr) {
        throw_tls_error("failed to decode trust anchor certificate",
                        br_x509_decoder_last_error(&dc));
    }

    OwnedTrustAnchor out;
    out.dn = std::move(dn);
    out.ta.dn = br_x500_name{out.dn.data(), out.dn.size()};
    out.ta.flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;

    switch (pk->key_type) {
        case BR_KEYTYPE_RSA:
            out.rsa_n.assign(pk->key.rsa.n, pk->key.rsa.n + pk->key.rsa.nlen);
            out.rsa_e.assign(pk->key.rsa.e, pk->key.rsa.e + pk->key.rsa.elen);
            out.ta.pkey.key_type = BR_KEYTYPE_RSA;
            out.ta.pkey.key.rsa = br_rsa_public_key{
                out.rsa_n.data(),
                out.rsa_n.size(),
                out.rsa_e.data(),
                out.rsa_e.size(),
            };
            break;
        case BR_KEYTYPE_EC:
            out.ec_q.assign(pk->key.ec.q, pk->key.ec.q + pk->key.ec.qlen);
            out.ta.pkey.key_type = BR_KEYTYPE_EC;
            out.ta.pkey.key.ec = br_ec_public_key{
                pk->key.ec.curve,
                out.ec_q.data(),
                out.ec_q.size(),
            };
            break;
        default:
            throw_tls_error("unsupported trust anchor key type");
    }

    return out;
}

int certificate_signer_key_type(const br_x509_certificate& cert) {
    br_x509_decoder_context dc;
    br_x509_decoder_init(&dc, nullptr, nullptr);
    br_x509_decoder_push(&dc, cert.data, cert.data_len);
    const auto err = br_x509_decoder_last_error(&dc);
    if (err != 0) {
        throw_tls_error("failed to decode certificate signer", err);
    }
    return br_x509_decoder_get_signer_key_type(&dc);
}

class BearSslCodecBase : public MemoryCodec {
public:
    bool handshake_complete() const noexcept override {
        const auto st = br_ssl_engine_current_state(engine_const());
        return (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) != 0;
    }

    bool wants_input() const noexcept override {
        const auto st = br_ssl_engine_current_state(engine_const());
        return (st & BR_SSL_RECVREC) != 0;
    }

    bool has_pending_ciphertext() const noexcept override {
        const auto st = br_ssl_engine_current_state(engine_const());
        return (st & BR_SSL_SENDREC) != 0;
    }

    std::size_t write_plaintext(std::string_view data) override {
        ensure_ok();
        const auto st = br_ssl_engine_current_state(engine_const());
        if ((st & BR_SSL_SENDAPP) == 0) {
            return 0;
        }

        std::size_t len = 0;
        auto* buf = br_ssl_engine_sendapp_buf(engine(), &len);
        if (buf == nullptr || len == 0) {
            return 0;
        }

        const auto copied = std::min<std::size_t>(data.size(), len);
        std::memcpy(buf, data.data(), copied);
        br_ssl_engine_sendapp_ack(engine(), copied);
        br_ssl_engine_flush(engine(), 0);
        return copied;
    }

    std::size_t feed_ciphertext(std::string_view data) override {
        ensure_ok();
        const auto st = br_ssl_engine_current_state(engine_const());
        if ((st & BR_SSL_RECVREC) == 0) {
            return 0;
        }

        std::size_t len = 0;
        auto* buf = br_ssl_engine_recvrec_buf(engine(), &len);
        if (buf == nullptr || len == 0) {
            return 0;
        }

        const auto copied = std::min<std::size_t>(data.size(), len);
        std::memcpy(buf, data.data(), copied);
        br_ssl_engine_recvrec_ack(engine(), copied);
        ensure_ok();
        return copied;
    }

    std::size_t read_plaintext(std::span<char> out) override {
        ensure_ok();
        const auto st = br_ssl_engine_current_state(engine_const());
        if ((st & BR_SSL_RECVAPP) == 0) {
            return 0;
        }

        std::size_t len = 0;
        auto* buf = br_ssl_engine_recvapp_buf(engine(), &len);
        if (buf == nullptr || len == 0) {
            return 0;
        }

        const auto copied = std::min<std::size_t>(out.size(), len);
        std::memcpy(out.data(), buf, copied);
        br_ssl_engine_recvapp_ack(engine(), copied);
        return copied;
    }

    std::size_t read_ciphertext(std::span<char> out) override {
        ensure_ok();
        const auto st = br_ssl_engine_current_state(engine_const());
        if ((st & BR_SSL_SENDREC) == 0) {
            return 0;
        }

        std::size_t len = 0;
        auto* buf = br_ssl_engine_sendrec_buf(engine(), &len);
        if (buf == nullptr || len == 0) {
            return 0;
        }

        const auto copied = std::min<std::size_t>(out.size(), len);
        std::memcpy(out.data(), buf, copied);
        br_ssl_engine_sendrec_ack(engine(), copied);
        ensure_ok();
        return copied;
    }

protected:
    BearSslCodecBase() = default;

    void ensure_ok() const {
        const int err = br_ssl_engine_last_error(engine_const());
        if (err != BR_ERR_OK) {
            throw_tls_error("BearSSL engine failed", err);
        }
    }

    virtual br_ssl_engine_context* engine() noexcept = 0;
    virtual const br_ssl_engine_context* engine_const() const noexcept = 0;
};

class BearSslClientCodec final : public BearSslCodecBase {
public:
    explicit BearSslClientCodec(TlsClientConfig config) : _config(std::move(config)) {
        if (_config.server_name.empty()) {
            throw_tls_error("TLS client config requires server_name");
        }

        auto trust_certs = load_certificates(_config.trust_anchors_pem);
        _trust_anchors.reserve(trust_certs.certs.size());
        for (const auto& cert : trust_certs.certs) {
            _trust_anchors.push_back(certificate_to_trust_anchor(cert));
        }
        _trust_anchor_refs.reserve(_trust_anchors.size());
        for (auto& anchor : _trust_anchors) {
            _trust_anchor_refs.push_back(anchor.ta);
        }

        br_ssl_client_init_full(&_client, &_x509, _trust_anchor_refs.data(),
                                _trust_anchor_refs.size());
        br_ssl_engine_set_buffers_bidi(&_client.eng, _ibuf.data(), _ibuf.size(),
                                       _obuf.data(), _obuf.size());
    }

    void start_client() override {
        if (!br_ssl_client_reset(&_client, _config.server_name.c_str(), 0)) {
            throw_tls_error("br_ssl_client_reset failed");
        }
    }

    void start_server() override {
        throw std::logic_error("client TLS codec cannot start as server");
    }

protected:
    br_ssl_engine_context* engine() noexcept override { return &_client.eng; }
    const br_ssl_engine_context* engine_const() const noexcept override {
        return &_client.eng;
    }

private:
    TlsClientConfig _config;
    std::vector<OwnedTrustAnchor> _trust_anchors;
    std::vector<br_x509_trust_anchor> _trust_anchor_refs;
    br_x509_minimal_context _x509{};
    br_ssl_client_context _client{};
    std::array<unsigned char, BR_SSL_BUFSIZE_INPUT> _ibuf{};
    std::array<unsigned char, BR_SSL_BUFSIZE_OUTPUT> _obuf{};
};

class BearSslServerCodec final : public BearSslCodecBase {
public:
    explicit BearSslServerCodec(TlsServerConfig config)
        : _config(std::move(config)),
          _chain(load_certificates(_config.certificate_chain_pem)),
          _key(load_private_key(_config.private_key_pem)) {
        if (_key.type == OwnedPrivateKey::Type::RSA) {
            br_ssl_server_init_full_rsa(&_server, _chain.certs.data(),
                                        _chain.certs.size(), &_key.rsa);
        } else if (_key.type == OwnedPrivateKey::Type::EC) {
            const int issuer_key_type = certificate_signer_key_type(_chain.certs[0]);
            br_ssl_server_init_full_ec(&_server, _chain.certs.data(),
                                       _chain.certs.size(), issuer_key_type,
                                       &_key.ec);
        } else {
            throw_tls_error("unsupported server private key");
        }

        br_ssl_engine_set_buffers_bidi(&_server.eng, _ibuf.data(), _ibuf.size(),
                                       _obuf.data(), _obuf.size());
    }

    void start_client() override {
        throw std::logic_error("server TLS codec cannot start as client");
    }

    void start_server() override {
        if (!br_ssl_server_reset(&_server)) {
            throw_tls_error("br_ssl_server_reset failed");
        }
    }

protected:
    br_ssl_engine_context* engine() noexcept override { return &_server.eng; }
    const br_ssl_engine_context* engine_const() const noexcept override {
        return &_server.eng;
    }

private:
    TlsServerConfig _config;
    OwnedCertificateChain _chain;
    OwnedPrivateKey _key;
    br_ssl_server_context _server{};
    std::array<unsigned char, BR_SSL_BUFSIZE_INPUT> _ibuf{};
    std::array<unsigned char, BR_SSL_BUFSIZE_OUTPUT> _obuf{};
};

}  // namespace

bool PassthroughCodec::has_pending_ciphertext() const noexcept {
    return !_outgoing.empty();
}

std::size_t PassthroughCodec::write_plaintext(std::string_view data) {
    _outgoing.append(data.data(), data.size());
    return data.size();
}

std::size_t PassthroughCodec::feed_ciphertext(std::string_view data) {
    _incoming.append(data.data(), data.size());
    return data.size();
}

std::size_t PassthroughCodec::read_plaintext(std::span<char> out) {
    return drain(_incoming, out);
}

std::size_t PassthroughCodec::read_ciphertext(std::span<char> out) {
    return drain(_outgoing, out);
}

std::size_t PassthroughCodec::drain(std::string& buffer, std::span<char> out) {
    return copy_and_erase(buffer, out);
}

std::unique_ptr<MemoryCodec> make_passthrough_codec() {
    return std::make_unique<PassthroughCodec>();
}

std::unique_ptr<MemoryCodec> make_bearssl_client_codec(TlsClientConfig config) {
    return std::make_unique<BearSslClientCodec>(std::move(config));
}

std::unique_ptr<MemoryCodec> make_bearssl_server_codec(TlsServerConfig config) {
    return std::make_unique<BearSslServerCodec>(std::move(config));
}

CodecStream::CodecStream(net::TcpStream stream, std::unique_ptr<MemoryCodec> codec)
    : _stream(std::move(stream)),
      _codec(codec ? std::move(codec) : make_passthrough_codec()) {}

bool CodecStream::is_open() const noexcept { return _stream.is_open(); }

uv_os_sock_t CodecStream::native_handle() const noexcept {
    return _stream.native_handle();
}

const net::Endpoint& CodecStream::local_endpoint() const noexcept {
    return _stream.local_endpoint();
}

const net::Endpoint& CodecStream::peer_endpoint() const noexcept {
    return _stream.peer_endpoint();
}

Task<void> CodecStream::handshake_client() { co_await run_handshake(true); }

Task<void> CodecStream::handshake_server() { co_await run_handshake(false); }

Task<void> CodecStream::run_handshake(bool client) {
    if (!_codec) {
        _codec = make_passthrough_codec();
    }

    if (client) {
        _codec->start_client();
    } else {
        _codec->start_server();
    }

    std::array<char, 4096> scratch{};
    while (!_codec->handshake_complete()) {
        co_await flush_ciphertext();
        if (_codec->handshake_complete()) {
            break;
        }

        if (!_codec->wants_input()) {
            if (!_codec->has_pending_ciphertext()) {
                throw std::runtime_error(
                    "corouv::transport handshake stalled");
            }
            continue;
        }

        const auto n = co_await _stream.read_some(
            std::span<char>(scratch.data(), scratch.size()));
        if (n == 0) {
            throw std::runtime_error(
                "corouv::transport handshake hit eof");
        }

        std::size_t offset = 0;
        while (offset < n) {
            const auto consumed =
                _codec->feed_ciphertext(std::string_view(scratch.data() + offset,
                                                         n - offset));
            if (consumed == 0) {
                break;
            }
            offset += consumed;
        }
    }

    co_await flush_ciphertext();
}

Task<void> CodecStream::flush_ciphertext() {
    if (!_codec) {
        co_return;
    }

    std::array<char, 4096> scratch{};
    while (_codec->has_pending_ciphertext()) {
        const auto n =
            _codec->read_ciphertext(std::span<char>(scratch.data(), scratch.size()));
        if (n == 0) {
            break;
        }
        co_await _stream.write_all(std::string_view(scratch.data(), n));
    }
}

Task<std::size_t> CodecStream::read_some(std::span<char> buffer) {
    if (!is_open()) {
        throw std::logic_error("corouv::transport::CodecStream is closed");
    }
    if (buffer.empty()) {
        co_return 0;
    }

    if (const auto ready = _codec->read_plaintext(buffer); ready > 0) {
        co_return ready;
    }

    std::array<char, 4096> scratch{};
    while (true) {
        co_await flush_ciphertext();
        if (const auto ready = _codec->read_plaintext(buffer); ready > 0) {
            co_return ready;
        }

        const auto n = co_await _stream.read_some(
            std::span<char>(scratch.data(), scratch.size()));
        if (n == 0) {
            co_return 0;
        }

        std::size_t offset = 0;
        while (offset < n) {
            const auto consumed =
                _codec->feed_ciphertext(std::string_view(scratch.data() + offset,
                                                         n - offset));
            if (consumed == 0) {
                break;
            }
            offset += consumed;
        }

        if (const auto ready = _codec->read_plaintext(buffer); ready > 0) {
            co_return ready;
        }
    }
}

Task<void> CodecStream::write_all(std::span<const char> data) {
    co_await write_all(std::string_view(data.data(), data.size()));
}

Task<void> CodecStream::write_all(std::string_view data) {
    if (!is_open()) {
        throw std::logic_error("corouv::transport::CodecStream is closed");
    }

    std::size_t offset = 0;
    std::array<char, 4096> scratch{};
    while (offset < data.size()) {
        const auto consumed = _codec->write_plaintext(data.substr(offset));
        if (consumed > 0) {
            offset += consumed;
        }

        co_await flush_ciphertext();
        if (offset >= data.size()) {
            break;
        }

        const auto n = co_await _stream.read_some(
            std::span<char>(scratch.data(), scratch.size()));
        if (n == 0) {
            throw std::runtime_error(
                "corouv::transport::CodecStream write hit eof");
        }

        std::size_t feed_offset = 0;
        while (feed_offset < n) {
            const auto feed = _codec->feed_ciphertext(
                std::string_view(scratch.data() + feed_offset, n - feed_offset));
            if (feed == 0) {
                break;
            }
            feed_offset += feed;
        }
    }

    co_await flush_ciphertext();
}

Task<std::string> CodecStream::read_until_eof(std::size_t max_bytes) {
    std::string out;
    std::array<char, 4096> scratch{};

    while (true) {
        const auto n = co_await read_some(
            std::span<char>(scratch.data(), scratch.size()));
        if (n == 0) {
            break;
        }
        if (out.size() + n > max_bytes) {
            throw std::runtime_error(
                "corouv::transport::CodecStream read_until_eof exceeded max_bytes");
        }
        out.append(scratch.data(), n);
    }

    co_return out;
}

void CodecStream::shutdown_write() noexcept { _stream.shutdown_write(); }

void CodecStream::close() noexcept { _stream.close(); }

CodecStream make_passthrough(net::TcpStream stream) {
    return CodecStream(std::move(stream), make_passthrough_codec());
}

}  // namespace corouv::transport
