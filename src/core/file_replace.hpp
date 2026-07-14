#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace axiom::core {

// Unique sibling temporaries avoid collisions between concurrent operations and
// stay on the destination volume so the final replacement can be atomic.
inline std::filesystem::path unique_sibling_path(
    const std::filesystem::path& destination, std::wstring_view purpose) {
    static std::atomic<std::uint64_t> sequence{0};
    std::filesystem::path result = destination;
    result += L".";
    result += purpose;
    result += L".";
#ifdef _WIN32
    result += std::to_wstring(GetCurrentProcessId());
#else
    result += std::to_wstring(static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
#endif
    result += L".";
    result += std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
    result += L".tmp";
    return result;
}

// Install a completed sibling temporary without deleting the valid destination
// first. On failure both the old destination and the temporary remain available
// to the caller/guard for recovery or cleanup.
inline void replace_file(const std::filesystem::path& temporary,
                         const std::filesystem::path& destination,
                         std::string_view description = "file") {
#ifdef _WIN32
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(destination, exists_error);
    if (exists_error) {
        throw std::runtime_error("cannot inspect destination " +
                                 std::string(description) + ": " +
                                 exists_error.message());
    }

    if (exists && ReplaceFileW(destination.c_str(), temporary.c_str(), nullptr,
                               REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        return;
    }
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return;
    }
    const DWORD native_error = GetLastError();
    throw std::runtime_error("failed to install " + std::string(description) + ": " +
                             std::system_category().message(
                                 static_cast<int>(native_error)));
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        throw std::runtime_error("failed to install " + std::string(description) + ": " +
                                 error.message());
    }
#endif
}

}  // namespace axiom::core
