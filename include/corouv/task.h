#pragma once

#include <async_simple/coro/Lazy.h>

namespace corouv {

template <class T = void>
using Task = async_simple::coro::Lazy<T>;

}  // namespace corouv
