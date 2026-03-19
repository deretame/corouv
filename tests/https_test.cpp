#include <corouv/https.h>
#include <corouv/net.h>
#include <corouv/runtime.h>
#include <corouv/task_group.h>
#include <corouv/timer.h>
#include <corouv/transport.h>

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace std::chrono_literals;

constexpr const char kTestCertPem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIC8zCCAdugAwIBAgIUPnkqEpPfXrR1zjVtuNwMlj8sed4wDQYJKoZIhvcNAQEL\n"
    "BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDMxODA2NDI1NVoXDTM2MDMx\n"
    "NTA2NDI1NVowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF\n"
    "AAOCAQ8AMIIBCgKCAQEAm578yPB/dH2LHT7WdnOpxDHb7/ZSnZ5sFTEKaOSDKBXe\n"
    "KnwyMmG8CBC36NiLvsWnt4y0wbL7kkSnG4lMJWnukV8e5kitC4toUe/PJpUXNIoW\n"
    "rB4TwJolMSqfK1tRzHkGUc+HFf/aVG2TBcZpF9TiTKe1zisDY4Jj+gP6FX596lC0\n"
    "5gV7oyEezhJ8X6rxc7qenkesL3Ez3QF+K4UyJ2lZzY4fyL5I5c+79NbNnWAFqUY0\n"
    "r1RAzLF/iwz3mu6G9M2nHpA8Mo9Econo9dcxLXw6PnYq0p4Fz3eGAnnrUdM+IMpS\n"
    "/izSzWrZB9/HVkxtI8MZrdj/WmTURm051uMe8AQMawIDAQABoz0wOzAaBgNVHREE\n"
    "EzARgglsb2NhbGhvc3SHBH8AAAEwHQYDVR0OBBYEFCPWzh/82JSc2ml0U3/NA4ig\n"
    "QRF2MA0GCSqGSIb3DQEBCwUAA4IBAQCY5kA8UC20CEqFI36OF8f8RkPsIYwN1RKC\n"
    "gCgC+yt8LTi10f7aSfwty/mYT22lPaCYP8R9IjXhVdBHkeNZpsgAUdSDEbS7Qx6P\n"
    "RqXzyjUZllrRVUSc57stAvNIMyFzxTmjp2BfNoVG6i7CHzdF3Us5B/rYIZpRJCc/\n"
    "LJLJk5GHFRL6yHWif4PnrHPCOcu31gNmSqGvq+S2mOD/Wobe//lO1Sgq45mUNN93\n"
    "QeZyHSFmdxq1hSplaV9vX9jxhJFOVE5XMwIkQYqouarCaxJ4UXFWjcWQ1gcAEJDF\n"
    "UJil+z1yo91uNUcRjfF+DDVgBWRs+2fq8ptl47i+IlF9/FqbD+Ni\n"
    "-----END CERTIFICATE-----\n";

constexpr const char kTestKeyPem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCbnvzI8H90fYsd\n"
    "PtZ2c6nEMdvv9lKdnmwVMQpo5IMoFd4qfDIyYbwIELfo2Iu+xae3jLTBsvuSRKcb\n"
    "iUwlae6RXx7mSK0Li2hR788mlRc0ihasHhPAmiUxKp8rW1HMeQZRz4cV/9pUbZMF\n"
    "xmkX1OJMp7XOKwNjgmP6A/oVfn3qULTmBXujIR7OEnxfqvFzup6eR6wvcTPdAX4r\n"
    "hTInaVnNjh/Ivkjlz7v01s2dYAWpRjSvVEDMsX+LDPea7ob0zacekDwyj0Ryiej1\n"
    "1zEtfDo+dirSngXPd4YCeetR0z4gylL+LNLNatkH38dWTG0jwxmt2P9aZNRGbTnW\n"
    "4x7wBAxrAgMBAAECggEAMZhLi2ZJ2EAiU6GxC3L1CZeyNhlUXvMcEuzcGm2o9t9x\n"
    "PCz0emX3XMKnOce5UYUAXFi1Sn3V+tlyQC5TXCEUbLPZzx4eN+2nw2UfqXYePRo8\n"
    "+4FiXiFB9RdWPqUNvpJbVztCG9q8I+f/0PlYiMXJkRBpuliu7cmiPw2BZ9Q2ILQ0\n"
    "a/A4b5jAjIxk5zIHy7x5O3dFuMXQY7a3w0McEPsYPKX34Gtw0+HkUa9fECqqgGQ7\n"
    "Y9L9fvrfEA7O8Op44SMYVxmW2A7RntUDExy2GVg18TccbevatgBfImbTyIzD3hBR\n"
    "xy+GJyk8MTb4zgfGTIf3Mzvj7w3wlpR/MCXZJ7m64QKBgQDSfeIO5n8P2Qg3AZ0B\n"
    "9VvIxuP6RmGHu7phq2eDPrRbm7bW6tLJ3QkXaWPo745+siQAZxAporp/6Gxn7nQN\n"
    "hWCI0M4KUpE/ltCuUzDm5jNz9bQksGXv64SowVAok/6GyT3d/eQGGqqcpZHXU/sy\n"
    "n5OqRDstG65egACUq/9yyUiRaQKBgQC9RCseGRwi8dd8EC+6bHKIYLX5V/4t/JET\n"
    "miOftY53a3JHyJOZICEGBIcCl31QNmt6td0Q/JwAgaIFEyZ95s9T9mp7/DZIWYWU\n"
    "QdJgRFB0yDfLMHVQVWkt+EJFwIuV8Zevt+DkisBkERNALqGt+ULUP6KWUjPzpfRb\n"
    "kYqY1udgswKBgFLe8MNBCEFrBwrw/ampucsgUt1UHE3eIREW9Jf9dfCjK6cNqJOF\n"
    "DHFFMuqgdg93fykRapGZ2adGEHgSr2orWshCvPvfagQJEnuvkQ83DobW82eGc3uz\n"
    "0/TEtxRiv9C9JjhiHwYwNX+ayAJos/tITFC6sDmgLlRSPOhLlzTP/lwRAoGASDiq\n"
    "+2E5i1wdjgnfLJQVYFUHG46oP4QRGBnJXjg5lPg7M4kmSVgpQdKYcjS8bM9XVAvx\n"
    "v3mlTcwptyYHmiNpGfD90TH8xL7kah5z2Kg4y1dbcv2axnzRFemV4GgI277E0xin\n"
    "iI4pvAWIAwXITErBZZyivhnAGUlKZWa0LH5U7GECgYBz0bAfNhoUWwoNCtQzqW+a\n"
    "s4hlmOTIHl7ktG8qzuXOAi0ID5M0GZk58CNsv8Zf/U62XMUoImkZr/xguKJ79LfU\n"
    "c3porAA93jXxbEqPWSwXdNtbKBTFgFAGugEqLvFd7ZVaDxCjOI71GA8X/wV+csGn\n"
    "6Ud/Mk9f+8ybq12EDyjzOg==\n"
    "-----END PRIVATE KEY-----\n";

corouv::transport::TlsServerConfig make_server_tls() {
    corouv::transport::TlsServerConfig tls;
    tls.certificate_chain_pem.emplace_back(kTestCertPem);
    tls.private_key_pem = kTestKeyPem;
    return tls;
}

corouv::transport::TlsClientConfig make_client_tls(std::string server_name) {
    corouv::transport::TlsClientConfig tls;
    tls.server_name = std::move(server_name);
    tls.trust_anchors_pem.emplace_back(kTestCertPem);
    return tls;
}

corouv::Task<void> https_roundtrip_case(corouv::UvExecutor& ex) {
    corouv::https::ServerOptions server_options;
    server_options.host = "127.0.0.1";
    server_options.port = 0;
    server_options.tls = make_server_tls();

    corouv::https::Server server(
        ex,
        [](corouv::https::Request request)
            -> corouv::Task<corouv::https::Response> {
            corouv::https::Response response;
            response.body = request.method + " " + request.target + " " +
                            request.body;
            co_return response;
        },
        std::move(server_options));

    co_await server.listen();

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(server.serve())) {
        throw std::runtime_error("https_test: server spawn failed");
    }

    corouv::https::ClientOptions client_options;
    client_options.tls = make_client_tls("localhost");

    corouv::https::Request request;
    request.method = "POST";
    request.body = "secure";
    const auto response = co_await corouv::https::fetch(
        ex,
        "https://127.0.0.1:" + std::to_string(server.port()) + "/tls",
        request, client_options);

    server.close();
    group.cancel();
    co_await group.wait();

    if (response.status != 200 || response.body != "POST /tls secure") {
        throw std::runtime_error("https_test: https roundtrip mismatch");
    }
}

corouv::Task<void> tls_hostname_mismatch_server_once(
    corouv::net::TcpListener* listener, bool* server_failed) {
    auto raw = co_await listener->accept();
    auto stream = corouv::transport::CodecStream(
        std::move(raw),
        corouv::transport::make_bearssl_server_codec(make_server_tls()));

    bool handshake_failed = false;
    try {
        co_await stream.handshake_server();
    } catch (...) {
        handshake_failed = true;
    }

    stream.close();
    listener->close();
    *server_failed = handshake_failed;
}

corouv::Task<void> tls_hostname_mismatch_case(corouv::UvExecutor& ex) {
    auto listener = co_await corouv::net::listen(ex, "127.0.0.1", 0);
    const auto endpoint = listener.local_endpoint();

    bool server_failed = false;
    auto group = co_await corouv::make_task_group();
    if (!group.spawn(tls_hostname_mismatch_server_once(&listener, &server_failed))) {
        throw std::runtime_error("https_test: mismatch server spawn failed");
    }

    auto raw = co_await corouv::net::connect(ex, endpoint.host, endpoint.port);
    auto stream = corouv::transport::CodecStream(
        std::move(raw), corouv::transport::make_bearssl_client_codec(
                            make_client_tls("wrong-host.invalid")));

    bool client_failed = false;
    try {
        co_await stream.handshake_client();
    } catch (...) {
        client_failed = true;
    }

    stream.close();
    co_await group.wait();

    if (!client_failed) {
        throw std::runtime_error(
            "https_test: expected client handshake failure on hostname mismatch");
    }
}

corouv::Task<void> tls_handshake_timeout_server_once(
    corouv::net::TcpListener* listener) {
    auto raw = co_await listener->accept();
    co_await corouv::sleep_for(200ms);
    raw.close();
    listener->close();
}

corouv::Task<void> tls_handshake_timeout_case(corouv::UvExecutor& ex) {
    auto listener = co_await corouv::net::listen(ex, "127.0.0.1", 0);
    const auto endpoint = listener.local_endpoint();

    auto group = co_await corouv::make_task_group();
    if (!group.spawn(tls_handshake_timeout_server_once(&listener))) {
        throw std::runtime_error("https_test: timeout server spawn failed");
    }

    auto raw = co_await corouv::net::connect(ex, endpoint.host, endpoint.port);
    auto tls = make_client_tls("localhost");
    tls.handshake_timeout = 30ms;
    auto stream = corouv::transport::CodecStream(
        std::move(raw), corouv::transport::make_bearssl_client_codec(std::move(tls)));

    bool timed_out = false;
    try {
        co_await stream.handshake_client();
    } catch (const std::runtime_error& e) {
        timed_out = std::string(e.what()).find("handshake timeout") !=
                    std::string::npos;
    }

    stream.close();
    co_await group.wait();

    if (!timed_out) {
        throw std::runtime_error("https_test: expected handshake timeout");
    }
}

corouv::Task<void> tls_close_notify_server_once(corouv::net::TcpListener* listener,
                                                bool* saw_eof) {
    auto raw = co_await listener->accept();
    auto stream = corouv::transport::CodecStream(
        std::move(raw),
        corouv::transport::make_bearssl_server_codec(make_server_tls()));
    co_await stream.handshake_server();

    std::array<char, 16> scratch{};
    const auto n = co_await stream.read_some(
        std::span<char>(scratch.data(), scratch.size()));
    if (std::string(scratch.data(), n) != "ping") {
        throw std::runtime_error("https_test: close_notify server request mismatch");
    }

    co_await stream.write_all(std::string_view("pong"));
    const auto n2 = co_await stream.read_some(
        std::span<char>(scratch.data(), scratch.size()));
    *saw_eof = (n2 == 0);

    stream.close();
    listener->close();
}

corouv::Task<void> tls_close_notify_case(corouv::UvExecutor& ex) {
    auto listener = co_await corouv::net::listen(ex, "127.0.0.1", 0);
    const auto endpoint = listener.local_endpoint();

    bool saw_eof = false;
    auto group = co_await corouv::make_task_group();
    if (!group.spawn(tls_close_notify_server_once(&listener, &saw_eof))) {
        throw std::runtime_error("https_test: close_notify server spawn failed");
    }

    auto raw = co_await corouv::net::connect(ex, endpoint.host, endpoint.port);
    auto stream = corouv::transport::CodecStream(
        std::move(raw),
        corouv::transport::make_bearssl_client_codec(make_client_tls("localhost")));
    co_await stream.handshake_client();

    co_await stream.write_all(std::string_view("ping"));
    std::array<char, 16> scratch{};
    const auto n = co_await stream.read_some(
        std::span<char>(scratch.data(), scratch.size()));
    if (std::string(scratch.data(), n) != "pong") {
        throw std::runtime_error("https_test: close_notify client response mismatch");
    }

    co_await stream.close_notify();
    co_await group.wait();
    stream.close();

    if (!saw_eof) {
        throw std::runtime_error("https_test: expected eof after close_notify");
    }
}

}  // namespace

int main() {
    corouv::Runtime rt;
    rt.run(https_roundtrip_case(rt.executor()));
    rt.run(tls_hostname_mismatch_case(rt.executor()));
    rt.run(tls_handshake_timeout_case(rt.executor()));
    rt.run(tls_close_notify_case(rt.executor()));
    return 0;
}
