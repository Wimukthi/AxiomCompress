#pragma once

#include "core/crypto.hpp"

#include <array>
#include <cerrno>
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
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace axiom::core {

// Unique sibling temporaries avoid collisions between concurrent operations and
// stay on the destination volume so the final replacement can be atomic. The name
// carries 64 random bits: anyone else who can write to the directory cannot plant a
// link or file at a name they cannot guess, which a predictable name (a clock, a
// process id, a counter) would let them do.
inline std::filesystem::path unique_sibling_path(
    const std::filesystem::path& destination, std::wstring_view purpose) {
    std::array<std::uint8_t, 8> random{};
    random_bytes(random);
    constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::filesystem::path result = destination;
    result += L".";
    result += purpose;
    result += L".";
    for (const auto byte : random) {
        result += kHex[byte >> 4];
        result += kHex[byte & 0x0F];
    }
    result += L".tmp";
    return result;
}

// Forces the contents of `path` to stable storage. Filesystems that cannot sync
// (reported as "not supported" or "invalid") are not an error; a failed write-back
// is.
inline std::error_code sync_file(const std::filesystem::path& path) {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::error_code(static_cast<int>(GetLastError()), std::system_category());
    }
    std::error_code result;
    if (!FlushFileBuffers(handle)) {
        const DWORD native = GetLastError();
        if (native != ERROR_INVALID_FUNCTION && native != ERROR_NOT_SUPPORTED) {
            result = std::error_code(static_cast<int>(native), std::system_category());
        }
    }
    CloseHandle(handle);
    return result;
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) return std::error_code(errno, std::generic_category());
    int status = -1;
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    // fsync() alone leaves the data in the drive's cache on macOS.
    status = ::fcntl(fd, F_FULLFSYNC);
#endif
    if (status != 0) status = ::fsync(fd);
    const int native = status == 0 ? 0 : errno;
    ::close(fd);
    if (status == 0 || native == EINVAL || native == ENOTSUP || native == EROFS) return {};
    return std::error_code(native, std::generic_category());
#endif
}

// Makes a rename or creation inside `directory` durable. Best effort, and a
// no-op on Windows, where the replacement is requested with write-through.
inline void sync_directory(const std::filesystem::path& directory) noexcept {
#ifndef _WIN32
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = ::open(directory.empty() ? "." : directory.c_str(), flags);
    if (fd < 0) return;
    (void)::fsync(fd);
    ::close(fd);
#else
    (void)directory;
#endif
}

// Install a completed sibling temporary without deleting the valid destination
// first. On failure both the old destination and the temporary remain available
// to the caller/guard for recovery or cleanup.
//
// Replacing a file that exists destroys the only copy of its old contents the
// moment the new name is visible, so the new contents are forced to disk first;
// otherwise a crash or power loss shortly after the rename can leave a truncated
// file where the good one used to be. Creating a new name has nothing to lose and
// skips the flush.
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
    if (exists) {
        if (const auto flush_error = sync_file(temporary)) {
            throw std::runtime_error("failed to flush new " + std::string(description) +
                                     " to disk: " + flush_error.message());
        }
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
    const bool exists = std::filesystem::exists(
        std::filesystem::symlink_status(destination, error));
    if (exists) {
        if (const auto flush_error = sync_file(temporary)) {
            throw std::runtime_error("failed to flush new " + std::string(description) +
                                     " to disk: " + flush_error.message());
        }
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        throw std::runtime_error("failed to install " + std::string(description) + ": " +
                                 error.message());
    }
    if (exists) sync_directory(destination.parent_path());
#endif
}

}  // namespace axiom::core
