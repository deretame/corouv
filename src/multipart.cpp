#include "corouv/multipart.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace corouv::multipart {

namespace {

bool iequals(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }

    return true;
}

std::string trim_copy(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();

    while (begin < end &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(begin, end - begin));
}

std::string unquote(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        std::string out;
        out.reserve(value.size() - 2);
        for (std::size_t i = 1; i + 1 < value.size(); ++i) {
            if (value[i] == '\\' && i + 2 < value.size()) {
                ++i;
            }
            out.push_back(value[i]);
        }
        return out;
    }
    return trim_copy(value);
}

std::string decode_percent_escaped(std::string_view value) {
    auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return 10 + (ch - 'a');
        }
        if (ch >= 'A' && ch <= 'F') {
            return 10 + (ch - 'A');
        }
        return -1;
    };

    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            out.push_back(value[i]);
            continue;
        }

        if (i + 2 >= value.size()) {
            throw std::runtime_error("corouv::multipart invalid percent-encoding");
        }
        const int hi = hex_value(value[i + 1]);
        const int lo = hex_value(value[i + 2]);
        if (hi < 0 || lo < 0) {
            throw std::runtime_error("corouv::multipart invalid percent-encoding");
        }

        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return out;
}

void parse_content_disposition(Part& part, std::string_view value) {
    std::size_t start = 0;
    bool first = true;
    std::optional<std::string> filename;
    std::optional<std::string> filename_ext;

    while (start < value.size()) {
        const auto semi = value.find(';', start);
        const auto token = trim_copy(value.substr(
            start, semi == std::string_view::npos ? value.size() - start
                                                  : semi - start));
        start = semi == std::string_view::npos ? value.size() : semi + 1;

        if (token.empty()) {
            continue;
        }

        if (first) {
            first = false;
            if (!iequals(token, "form-data")) {
                throw std::runtime_error(
                    "corouv::multipart expected form-data content disposition");
            }
            continue;
        }

        const auto eq = token.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        const auto key = trim_copy(token.substr(0, eq));
        const auto parsed_value = unquote(token.substr(eq + 1));

        if (iequals(key, "name")) {
            part.name = parsed_value;
        } else if (iequals(key, "filename")) {
            filename = parsed_value;
        } else if (iequals(key, "filename*")) {
            const auto first_quote = parsed_value.find('\'');
            const auto second_quote =
                first_quote == std::string::npos
                    ? std::string::npos
                    : parsed_value.find('\'', first_quote + 1);
            const auto encoded =
                second_quote == std::string::npos
                    ? std::string_view(parsed_value)
                    : std::string_view(parsed_value).substr(second_quote + 1);
            filename_ext = decode_percent_escaped(encoded);
        }
    }

    if (filename_ext.has_value()) {
        part.filename = std::move(filename_ext);
    } else if (filename.has_value()) {
        part.filename = std::move(filename);
    }
}

corouv::http::Headers parse_headers_block(std::string_view block) {
    corouv::http::Headers headers;
    std::size_t pos = 0;

    while (pos < block.size()) {
        const auto end = block.find("\r\n", pos);
        const auto line =
            end == std::string_view::npos ? block.substr(pos)
                                          : block.substr(pos, end - pos);
        if (line.empty()) {
            break;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            throw std::runtime_error("corouv::multipart malformed part header");
        }

        headers.push_back(corouv::http::Header{
            trim_copy(line.substr(0, colon)),
            trim_copy(line.substr(colon + 1)),
        });

        if (end == std::string_view::npos) {
            break;
        }
        pos = end + 2;
    }

    return headers;
}

}  // namespace

std::optional<std::string> boundary_from_content_type(
    std::string_view content_type) {
    std::size_t start = 0;
    bool first = true;

    while (start < content_type.size()) {
        const auto semi = content_type.find(';', start);
        const auto token =
            trim_copy(content_type.substr(start, semi == std::string_view::npos
                                                     ? content_type.size() - start
                                                     : semi - start));
        start = semi == std::string_view::npos ? content_type.size() : semi + 1;

        if (token.empty()) {
            continue;
        }

        if (first) {
            first = false;
            if (!iequals(token, "multipart/form-data")) {
                return std::nullopt;
            }
            continue;
        }

        const auto eq = token.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        const auto key = trim_copy(token.substr(0, eq));
        if (!iequals(key, "boundary")) {
            continue;
        }

        return unquote(token.substr(eq + 1));
    }

    return std::nullopt;
}

FormData parse(std::string_view content_type, std::string_view body,
               std::size_t max_parts, std::size_t max_part_bytes) {
    const auto boundary = boundary_from_content_type(content_type);
    if (!boundary.has_value() || boundary->empty()) {
        throw std::runtime_error(
            "corouv::multipart missing boundary in Content-Type");
    }

    const std::string marker = "--" + *boundary;
    const std::string next_marker = "\r\n--" + *boundary;

    if (!body.starts_with(marker)) {
        throw std::runtime_error("corouv::multipart missing opening boundary");
    }

    FormData form;
    std::size_t cursor = marker.size();

    while (true) {
        if (cursor + 2 <= body.size() && body.substr(cursor, 2) == "--") {
            break;
        }
        if (cursor + 2 > body.size() || body.substr(cursor, 2) != "\r\n") {
            throw std::runtime_error("corouv::multipart malformed boundary");
        }
        cursor += 2;

        const auto headers_end = body.find("\r\n\r\n", cursor);
        if (headers_end == std::string_view::npos) {
            throw std::runtime_error("corouv::multipart missing part headers");
        }

        Part part;
        part.headers = parse_headers_block(body.substr(cursor, headers_end - cursor));
        cursor = headers_end + 4;

        for (const auto& header : part.headers) {
            if (iequals(header.name, "Content-Disposition")) {
                parse_content_disposition(part, header.value);
            } else if (iequals(header.name, "Content-Type")) {
                part.content_type = header.value;
            }
        }

        if (part.name.empty()) {
            throw std::runtime_error(
                "corouv::multipart part missing content-disposition name");
        }

        const auto next = body.find(next_marker, cursor);
        if (next == std::string_view::npos) {
            throw std::runtime_error("corouv::multipart missing closing boundary");
        }

        part.body.assign(body.substr(cursor, next - cursor));
        if (part.body.size() > max_part_bytes) {
            throw std::runtime_error("corouv::multipart part body too large");
        }
        form.parts.push_back(std::move(part));

        if (form.parts.size() > max_parts) {
            throw std::runtime_error("corouv::multipart too many parts");
        }

        cursor = next + 2 + marker.size();
        if (cursor + 2 <= body.size() && body.substr(cursor, 2) == "--") {
            break;
        }
    }

    return form;
}

FormData parse_request(const corouv::http::Request& request,
                       std::size_t max_parts, std::size_t max_part_bytes) {
    const auto content_type =
        corouv::http::find_header(request.headers, "Content-Type");
    if (!content_type.has_value()) {
        throw std::runtime_error(
            "corouv::multipart request missing Content-Type header");
    }
    return parse(*content_type, request.body, max_parts, max_part_bytes);
}

std::string build_content_type(std::string_view boundary) {
    return "multipart/form-data; boundary=" + std::string(boundary);
}

std::string serialize(const FormData& form, std::string_view boundary) {
    if (boundary.empty()) {
        throw std::invalid_argument("corouv::multipart boundary must not be empty");
    }

    std::string out;
    for (const auto& part : form.parts) {
        out.append("--");
        out.append(boundary);
        out.append("\r\n");
        out.append("Content-Disposition: form-data; name=\"");
        out.append(part.name);
        out.push_back('"');
        if (part.filename.has_value()) {
            out.append("; filename=\"");
            out.append(*part.filename);
            out.push_back('"');
        }
        out.append("\r\n");
        if (!part.content_type.empty()) {
            out.append("Content-Type: ");
            out.append(part.content_type);
            out.append("\r\n");
        }
        for (const auto& header : part.headers) {
            if (iequals(header.name, "Content-Disposition") ||
                iequals(header.name, "Content-Type")) {
                continue;
            }
            out.append(header.name);
            out.append(": ");
            out.append(header.value);
            out.append("\r\n");
        }
        out.append("\r\n");
        out.append(part.body);
        out.append("\r\n");
    }

    out.append("--");
    out.append(boundary);
    out.append("--\r\n");
    return out;
}

}  // namespace corouv::multipart
