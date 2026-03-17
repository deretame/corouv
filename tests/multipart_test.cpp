#include <corouv/http.h>
#include <corouv/multipart.h>

#include <stdexcept>
#include <string>

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

int main() {
    multipart_roundtrip_case();
    return 0;
}
