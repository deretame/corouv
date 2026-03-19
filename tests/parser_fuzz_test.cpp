#include <corouv/http.h>
#include <corouv/multipart.h>

#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string random_string(std::mt19937& rng, std::size_t min_len,
                          std::size_t max_len, std::string_view alphabet) {
    std::uniform_int_distribution<std::size_t> len_dist(min_len, max_len);
    std::uniform_int_distribution<std::size_t> ch_dist(0, alphabet.size() - 1);

    std::string out;
    const auto n = len_dist(rng);
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(alphabet[ch_dist(rng)]);
    }
    return out;
}

void parse_url_property_case() {
    std::mt19937 rng(123456);
    const std::string alpha = "abcdefghijklmnopqrstuvwxyz";

    for (int i = 0; i < 400; ++i) {
        const bool https = (i % 2) == 0;
        const std::string scheme = https ? "https" : "http";
        const std::uint16_t default_port = https ? 443 : 80;

        const std::string host = random_string(rng, 3, 10, alpha) + ".test";
        const bool with_port = (i % 3) == 0;
        const std::uint16_t port = static_cast<std::uint16_t>(
            1 + (rng() % 65535));

        std::string path = "/";
        if ((i % 5) != 0) {
            path += random_string(rng, 1, 8, alpha);
            if ((i % 7) == 0) {
                path += "/" + random_string(rng, 1, 8, alpha);
            }
        }

        std::string target = path;
        if ((i % 4) == 0) {
            target += "?k=" + random_string(rng, 1, 6, alpha);
        }

        std::string url = scheme + "://" + host;
        if (with_port) {
            url += ":" + std::to_string(port);
        }
        url += target;

        const auto parsed = corouv::http::parse_url(url);
        if (parsed.scheme != scheme || parsed.host != host ||
            parsed.port != (with_port ? port : default_port) ||
            parsed.target != target) {
            throw std::runtime_error("parser_fuzz_test: parse_url property mismatch");
        }
    }

    const std::string fuzz_alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:/?#[]@!$&'()*+,;=%-_.";
    for (int i = 0; i < 1200; ++i) {
        const auto fuzz = random_string(rng, 0, 80, fuzz_alphabet);
        try {
            (void)corouv::http::parse_url(fuzz);
        } catch (const std::invalid_argument&) {
        }
    }
}

void form_property_case() {
    std::mt19937 rng(424242);
    const std::string key_chars = "abcdefghijklmnopqrstuvwxyz0123456789_-";
    const std::string value_chars =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-./?&=+%";

    for (int i = 0; i < 300; ++i) {
        corouv::http::FormFields fields;
        const int count = static_cast<int>(rng() % 6);
        fields.reserve(static_cast<std::size_t>(count));

        for (int j = 0; j < count; ++j) {
            fields.push_back(corouv::http::FormField{
                .name = random_string(rng, 0, 8, key_chars),
                .value = random_string(rng, 0, 16, value_chars),
            });
        }

        const auto encoded = corouv::http::serialize_form_urlencoded(fields);
        const auto decoded = corouv::http::parse_form_urlencoded(encoded,
                                                                 fields.size() + 1);
        if (decoded.size() != fields.size()) {
            throw std::runtime_error("parser_fuzz_test: form roundtrip size mismatch");
        }
        for (std::size_t k = 0; k < fields.size(); ++k) {
            if (decoded[k].name != fields[k].name ||
                decoded[k].value != fields[k].value) {
                throw std::runtime_error(
                    "parser_fuzz_test: form roundtrip content mismatch");
            }
        }
    }

    const std::string fuzz_alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789%&=+_-.~";
    for (int i = 0; i < 1200; ++i) {
        const auto fuzz = random_string(rng, 0, 120, fuzz_alphabet);
        try {
            (void)corouv::http::parse_form_urlencoded(fuzz, 1024);
        } catch (const std::invalid_argument&) {
        } catch (const std::runtime_error&) {
        }
    }
}

void multipart_property_case() {
    std::mt19937 rng(98765);
    const std::string alpha = "abcdefghijklmnopqrstuvwxyz0123456789";
    const std::string body_chars =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-./";

    for (int i = 0; i < 180; ++i) {
        corouv::multipart::FormData form;
        const int count = 1 + static_cast<int>(rng() % 4);
        form.parts.reserve(static_cast<std::size_t>(count));

        for (int j = 0; j < count; ++j) {
            corouv::multipart::Part part;
            part.name = "field" + std::to_string(j);
            part.body = random_string(rng, 0, 24, body_chars);
            if ((rng() % 2) == 0) {
                part.filename = random_string(rng, 3, 10, alpha) + ".txt";
                part.content_type = "text/plain";
            }
            form.parts.push_back(std::move(part));
        }

        const std::string boundary = "b" + random_string(rng, 8, 20, alpha);
        const auto content_type = corouv::multipart::build_content_type(boundary);
        const auto body = corouv::multipart::serialize(form, boundary);
        const auto parsed = corouv::multipart::parse(content_type, body,
                                                     256, 8 * 1024 * 1024);

        if (parsed.parts.size() != form.parts.size()) {
            throw std::runtime_error(
                "parser_fuzz_test: multipart roundtrip size mismatch");
        }
        for (std::size_t k = 0; k < form.parts.size(); ++k) {
            if (parsed.parts[k].name != form.parts[k].name ||
                parsed.parts[k].body != form.parts[k].body ||
                parsed.parts[k].filename != form.parts[k].filename) {
                throw std::runtime_error(
                    "parser_fuzz_test: multipart roundtrip content mismatch");
            }
        }
    }

    const std::string fuzz_alphabet =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-./;:=\"'&%?";
    for (int i = 0; i < 900; ++i) {
        const auto fuzz_content_type = random_string(rng, 0, 80, fuzz_alphabet);
        const auto fuzz_body = random_string(rng, 0, 200, fuzz_alphabet);
        try {
            (void)corouv::multipart::parse(fuzz_content_type, fuzz_body, 32, 1024);
        } catch (const std::runtime_error&) {
        }
    }
}

}  // namespace

int main() {
    parse_url_property_case();
    form_property_case();
    multipart_property_case();
    return 0;
}
