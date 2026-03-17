#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "corouv/http.h"

namespace corouv::multipart {

struct Part {
    std::string name;
    std::optional<std::string> filename;
    std::string content_type;
    corouv::http::Headers headers;
    std::string body;
};

struct FormData {
    std::vector<Part> parts;
};

std::optional<std::string> boundary_from_content_type(
    std::string_view content_type);

FormData parse(std::string_view content_type, std::string_view body,
               std::size_t max_parts = 256);
FormData parse_request(const corouv::http::Request& request,
                       std::size_t max_parts = 256);

std::string build_content_type(std::string_view boundary);
std::string serialize(const FormData& form, std::string_view boundary);

}  // namespace corouv::multipart
