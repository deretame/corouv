#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace corouv::sql {

struct ColumnMeta {
    std::string_view member;
    std::string_view column;
    bool primary_key{false};
};

template <class T>
struct ModelMeta {
    static constexpr std::string_view table{};
    static constexpr std::array<ColumnMeta, 0> columns{};
};

template <class T>
inline constexpr bool has_model_meta_v =
    !ModelMeta<T>::table.empty() || (ModelMeta<T>::columns.size() > 0);

}  // namespace corouv::sql
