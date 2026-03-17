#pragma once

#include <uv.h>

#include <stdexcept>
#include <string>

namespace corouv {

inline void throw_uv_error(int code, const char* what) {
    std::string msg;
    msg.reserve(128);
    msg.append(what);
    msg.append(": ");
    msg.append(uv_strerror(code));
    throw std::runtime_error(std::move(msg));
}

}  // namespace corouv
