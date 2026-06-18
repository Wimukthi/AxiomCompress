#include "core/file_meta.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace axiom::core {

#if defined(_WIN32)

namespace {

std::uint64_t filetime_to_u64(const FILETIME& ft) {
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) |
           static_cast<std::uint64_t>(ft.dwLowDateTime);
}

FILETIME u64_to_filetime(std::uint64_t value) {
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(value & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(value >> 32);
    return ft;
}

}  // namespace

FileMetadata capture_metadata(const std::filesystem::path& path) {
    FileMetadata meta;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        meta.has_windows_attributes = true;
        meta.windows_attributes = data.dwFileAttributes;
        meta.has_windows_times = true;
        meta.windows_creation_time = filetime_to_u64(data.ftCreationTime);
        meta.windows_access_time = filetime_to_u64(data.ftLastAccessTime);
        meta.windows_write_time = filetime_to_u64(data.ftLastWriteTime);
    }
    return meta;
}

void apply_metadata(const std::filesystem::path& path, const FileMetadata& meta,
                    bool restore_times) {
    if (restore_times && meta.has_windows_times) {
        // FILE_FLAG_BACKUP_SEMANTICS lets us open a directory handle; the write
        // happens before attributes so a restored read-only flag cannot block it.
        const HANDLE handle = CreateFileW(
            path.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            const FILETIME creation = u64_to_filetime(meta.windows_creation_time);
            const FILETIME access = u64_to_filetime(meta.windows_access_time);
            const FILETIME write = u64_to_filetime(meta.windows_write_time);
            SetFileTime(handle, &creation, &access, &write);
            CloseHandle(handle);
        }
    }

    if (meta.has_windows_attributes) {
        // Restore only the user-meaningful attributes; never replay structural bits
        // (directory, reparse point, etc.) that the filesystem already manages.
        constexpr DWORD kRestorable = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                                      FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
                                      FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
        const DWORD attributes = meta.windows_attributes & kRestorable;
        if (attributes != 0) {
            SetFileAttributesW(path.c_str(), attributes);
        }
    }
}

#else

FileMetadata capture_metadata(const std::filesystem::path&) {
    return {};
}

void apply_metadata(const std::filesystem::path&, const FileMetadata&, bool) {}

#endif

}  // namespace axiom::core
