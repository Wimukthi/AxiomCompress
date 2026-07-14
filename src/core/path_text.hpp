#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace axiom::core {

// std::filesystem::path::string() and generic_string() use the active narrow
// code page on Windows. Archive names are UTF-8, so convert through char8_t
// explicitly and never lose or reject a valid Unicode filename.
inline std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

inline std::string generic_path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

inline std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

}  // namespace axiom::core
