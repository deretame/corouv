#include <corouv/http.h>
#include <corouv/multipart.h>

#include <stdexcept>
#include <string>
#include <vector>

void multipart_roundtrip_case() {
    corouv::multipart::FormData form;

    corouv::multipart::Part field;
    field.name = "title";
    field.body = "corouv";
    form.parts.push_back(field);

    corouv::multipart::Part file;
    file.name = "upload";
    file.filename = "hello.txt";
    file.content_type = "text/plain";
    file.body = "hello multipart";
    form.parts.push_back(file);

    const std::string boundary = "corouv-boundary";
    const auto content_type = corouv::multipart::build_content_type(boundary);
    const auto body = corouv::multipart::serialize(form, boundary);

    const auto parsed = corouv::multipart::parse(content_type, body);
    if (parsed.parts.size() != 2) {
        throw std::runtime_error("multipart_test: unexpected part count");
    }
    if (parsed.parts[0].name != "title" || parsed.parts[0].body != "corouv") {
        throw std::runtime_error("multipart_test: field mismatch");
    }
    if (parsed.parts[1].name != "upload" ||
        !parsed.parts[1].filename.has_value() ||
        *parsed.parts[1].filename != "hello.txt" ||
        parsed.parts[1].content_type != "text/plain" ||
        parsed.parts[1].body != "hello multipart") {
        throw std::runtime_error("multipart_test: file part mismatch");
    }

    corouv::http::Request request;
    request.headers.push_back({"content-type", content_type});
    request.body = body;

    const auto parsed_request = corouv::multipart::parse_request(request);
    if (parsed_request.parts.size() != 2) {
        throw std::runtime_error("multipart_test: request parse mismatch");
    }
}

void multipart_filename_star_case() {
    const std::string boundary = "star-boundary";
    const auto content_type = corouv::multipart::build_content_type(boundary);
    const std::string body =
        "--star-boundary\r\n"
        "Content-Disposition: form-data; name=\"upload\"; "
        "filename*=UTF-8''hello%20world.txt\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "payload\r\n"
        "--star-boundary--\r\n";

    const auto parsed = corouv::multipart::parse(content_type, body);
    if (parsed.parts.size() != 1 || !parsed.parts[0].filename.has_value() ||
        *parsed.parts[0].filename != "hello world.txt") {
        throw std::runtime_error("multipart_test: filename* parse mismatch");
    }
}

void multipart_max_part_bytes_case() {
    corouv::multipart::FormData form;
    corouv::multipart::Part field;
    field.name = "note";
    field.body = "0123456789";
    form.parts.push_back(std::move(field));

    const std::string boundary = "limit-boundary";
    const auto content_type = corouv::multipart::build_content_type(boundary);
    const auto body = corouv::multipart::serialize(form, boundary);

    bool threw = false;
    try {
        (void)corouv::multipart::parse(content_type, body, 256, 4);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    if (!threw) {
        throw std::runtime_error("multipart_test: expected part size limit throw");
    }
}

void multipart_parser_robustness_case() {
    {
        const auto boundary = corouv::multipart::boundary_from_content_type(
            "multipart/form-data; charset=utf-8; boundary=\"abc-123\"");
        if (!boundary.has_value() || *boundary != "abc-123") {
            throw std::runtime_error("multipart_test: boundary parse mismatch");
        }
    }

    const std::vector<std::pair<std::string, std::string>> malformed_cases = {
        {
            "multipart/form-data",
            "--b\r\n"
            "Content-Disposition: form-data; name=\"a\"\r\n"
            "\r\n"
            "x\r\n"
            "--b--\r\n",
        },
        {
            "multipart/form-data; boundary=b",
            "garbage\r\n"
            "--b\r\n"
            "Content-Disposition: form-data; name=\"a\"\r\n"
            "\r\n"
            "x\r\n"
            "--b--\r\n",
        },
        {
            "multipart/form-data; boundary=b",
            "--b\r\n"
            "X-Bad-Header\r\n"
            "\r\n"
            "x\r\n"
            "--b--\r\n",
        },
        {
            "multipart/form-data; boundary=b",
            "--b\r\n"
            "Content-Disposition: form-data\r\n"
            "\r\n"
            "x\r\n"
            "--b--\r\n",
        },
        {
            "multipart/form-data; boundary=b",
            "--b\r\n"
            "Content-Disposition: form-data; name=\"a\"; "
            "filename*=UTF-8''bad%ZZ.txt\r\n"
            "\r\n"
            "x\r\n"
            "--b--\r\n",
        },
        {
            "multipart/form-data; boundary=b",
            "--b\r\n"
            "Content-Disposition: form-data; name=\"a\"\r\n"
            "\r\n"
            "x\r\n",
        },
    };

    for (const auto& [content_type, body] : malformed_cases) {
        bool threw = false;
        try {
            (void)corouv::multipart::parse(content_type, body);
        } catch (const std::runtime_error&) {
            threw = true;
        }

        if (!threw) {
            throw std::runtime_error(
                "multipart_test: expected malformed input throw");
        }
    }

    bool max_parts_threw = false;
    try {
        (void)corouv::multipart::parse(
            "multipart/form-data; boundary=b",
            "--b\r\n"
            "Content-Disposition: form-data; name=\"a\"\r\n"
            "\r\n"
            "x\r\n"
            "--b--\r\n",
            0);
    } catch (const std::runtime_error&) {
        max_parts_threw = true;
    }
    if (!max_parts_threw) {
        throw std::runtime_error("multipart_test: expected max_parts throw");
    }
}

int main() {
    multipart_roundtrip_case();
    multipart_filename_star_case();
    multipart_max_part_bytes_case();
    multipart_parser_robustness_case();
    return 0;
}
