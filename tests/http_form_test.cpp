#include <corouv/http.h>

#include <stdexcept>
#include <string>
#include <vector>

void form_urlencoded_roundtrip_case() {
    corouv::http::FormFields fields;
    fields.push_back({"name", "Alice Bob"});
    fields.push_back({"note", "a+b&c=d%"});
    fields.push_back({"path", "/v1/resource?id=1"});

    const auto body = corouv::http::serialize_form_urlencoded(fields);
    if (body !=
        "name=Alice+Bob&note=a%2Bb%26c%3Dd%25&path=%2Fv1%2Fresource%3Fid%3D1") {
        throw std::runtime_error("http_form_test: serialized body mismatch");
    }

    const auto parsed = corouv::http::parse_form_urlencoded(body);
    if (parsed.size() != 3 || parsed[0].name != "name" ||
        parsed[0].value != "Alice Bob" || parsed[1].name != "note" ||
        parsed[1].value != "a+b&c=d%" || parsed[2].name != "path" ||
        parsed[2].value != "/v1/resource?id=1") {
        throw std::runtime_error("http_form_test: parse roundtrip mismatch");
    }

    const auto name = corouv::http::find_form_value(parsed, "name");
    if (!name.has_value() || *name != "Alice Bob") {
        throw std::runtime_error("http_form_test: find_form_value mismatch");
    }
    if (corouv::http::find_form_value(parsed, "missing").has_value()) {
        throw std::runtime_error("http_form_test: missing form value mismatch");
    }
}

void form_urlencoded_request_case() {
    corouv::http::Request request;
    request.method = "POST";
    request.target = "/submit";
    request.headers.push_back(
        {"Content-Type", "application/x-www-form-urlencoded; charset=UTF-8"});
    request.body = "title=hello+world&empty=&count=42";

    const auto fields = corouv::http::parse_form_urlencoded_request(request);
    if (fields.size() != 3 || fields[0].name != "title" ||
        fields[0].value != "hello world" || fields[1].name != "empty" ||
        fields[1].value != "" || fields[2].name != "count" ||
        fields[2].value != "42") {
        throw std::runtime_error("http_form_test: request parse mismatch");
    }
}

void form_urlencoded_invalid_percent_case() {
    bool threw = false;
    try {
        (void)corouv::http::parse_form_urlencoded("bad=%GG");
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    if (!threw) {
        throw std::runtime_error("http_form_test: expected invalid percent throw");
    }
}

void form_urlencoded_wrong_content_type_case() {
    corouv::http::Request request;
    request.method = "POST";
    request.headers.push_back({"Content-Type", "text/plain"});
    request.body = "a=1";

    bool threw = false;
    try {
        (void)corouv::http::parse_form_urlencoded_request(request);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    if (!threw) {
        throw std::runtime_error(
            "http_form_test: expected wrong content-type throw");
    }
}

void form_urlencoded_multi_value_case() {
    const auto fields =
        corouv::http::parse_form_urlencoded("tag=one&tag=two&tag=three");
    const auto tags = corouv::http::find_form_values(fields, "tag");
    if (tags.size() != 3 || tags[0] != "one" || tags[1] != "two" ||
        tags[2] != "three") {
        throw std::runtime_error("http_form_test: multi-value mismatch");
    }
}

void form_urlencoded_edge_case() {
    const auto fields = corouv::http::parse_form_urlencoded(
        "&&=value&&name=alice&&novalue&encoded=%2B+%252F&");
    if (fields.size() != 4 || fields[0].name != "" || fields[0].value != "value" ||
        fields[1].name != "name" || fields[1].value != "alice" ||
        fields[2].name != "novalue" || fields[2].value != "" ||
        fields[3].name != "encoded" || fields[3].value != "+ %2F") {
        throw std::runtime_error("http_form_test: edge-case parse mismatch");
    }
}

void form_urlencoded_limits_and_missing_header_case() {
    bool too_many_fields = false;
    try {
        (void)corouv::http::parse_form_urlencoded("a=1&b=2", 1);
    } catch (const std::runtime_error&) {
        too_many_fields = true;
    }
    if (!too_many_fields) {
        throw std::runtime_error(
            "http_form_test: expected max_fields limit throw");
    }

    corouv::http::Request request;
    request.method = "POST";
    request.body = "a=1";

    bool missing_header = false;
    try {
        (void)corouv::http::parse_form_urlencoded_request(request);
    } catch (const std::runtime_error&) {
        missing_header = true;
    }
    if (!missing_header) {
        throw std::runtime_error(
            "http_form_test: expected missing Content-Type throw");
    }
}

void parse_url_robustness_case() {
    {
        const auto parsed = corouv::http::parse_url("http://example.com");
        if (parsed.scheme != "http" || parsed.host != "example.com" ||
            parsed.port != 80 || parsed.target != "/") {
            throw std::runtime_error("http_form_test: parse_url basic mismatch");
        }
    }

    {
        const auto parsed =
            corouv::http::parse_url("https://example.com?x=1#fragment");
        if (parsed.scheme != "https" || parsed.host != "example.com" ||
            parsed.port != 443 || parsed.target != "/?x=1") {
            throw std::runtime_error(
                "http_form_test: parse_url query/fragment mismatch");
        }
    }

    {
        const auto parsed = corouv::http::parse_url("http://[::1]:8080/p?q=1");
        if (parsed.host != "::1" || parsed.port != 8080 ||
            parsed.target != "/p?q=1") {
            throw std::runtime_error("http_form_test: parse_url ipv6 mismatch");
        }
    }

    const std::vector<std::string> invalid_urls = {
        "ftp://example.com/",
        "http:///missing-host",
        "http://[::1/path",
        "http://[::1]extra",
        "http://example.com:99999/",
        "http://example.com:/",
        "http://:80/",
    };
    for (const auto& url : invalid_urls) {
        bool threw = false;
        try {
            (void)corouv::http::parse_url(url);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        if (!threw) {
            throw std::runtime_error("http_form_test: expected invalid url throw");
        }
    }
}

int main() {
    form_urlencoded_roundtrip_case();
    form_urlencoded_request_case();
    form_urlencoded_invalid_percent_case();
    form_urlencoded_wrong_content_type_case();
    form_urlencoded_multi_value_case();
    form_urlencoded_edge_case();
    form_urlencoded_limits_and_missing_header_case();
    parse_url_robustness_case();
    return 0;
}
