#include "core/file_meta.hpp"

#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
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

std::optional<FileId> hardlink_identity(const std::filesystem::path& path) {
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = GetFileInformationByHandle(handle, &info);
    CloseHandle(handle);
    if (!ok || info.nNumberOfLinks <= 1) {
        return std::nullopt;
    }
    FileId id;
    id.volume = info.dwVolumeSerialNumber;
    id.index_high = info.nFileIndexHigh;
    id.index_low = info.nFileIndexLow;
    return id;
}

bool is_reparse_point(const std::filesystem::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

namespace {

std::string narrow_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), len,
                        nullptr, nullptr);
    return out;
}

std::wstring widen_utf8(const std::string& narrow) {
    if (narrow.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, narrow.data(), static_cast<int>(narrow.size()),
                                        nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, narrow.data(), static_cast<int>(narrow.size()), out.data(), len);
    return out;
}

}  // namespace

std::vector<AdsStream> capture_ads(const std::filesystem::path& path) {
    std::vector<AdsStream> streams;
    WIN32_FIND_STREAM_DATA found{};
    const HANDLE search = FindFirstStreamW(path.c_str(), FindStreamInfoStandard, &found, 0);
    if (search == INVALID_HANDLE_VALUE) {
        return streams;
    }
    do {
        // cStreamName looks like ":NAME:$DATA"; the default content stream is "::$DATA".
        const std::wstring full = found.cStreamName;
        const auto first = full.find(L':');
        const auto second = full.find(L':', first == std::wstring::npos ? 0 : first + 1);
        if (first == std::wstring::npos || second == std::wstring::npos) {
            continue;
        }
        const std::wstring name = full.substr(first + 1, second - first - 1);
        if (name.empty()) {
            continue;  // the file's own $DATA, not an alternate stream
        }
        if (found.StreamSize.QuadPart < 0 ||
            static_cast<std::uint64_t>(found.StreamSize.QuadPart) > kMaxAdsBytes) {
            continue;  // too large to keep in the directory; skip (file still archived)
        }
        const std::wstring stream_path = path.wstring() + L":" + name;
        const HANDLE handle = CreateFileW(stream_path.c_str(), GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            continue;
        }
        AdsStream stream;
        stream.name = narrow_utf8(name);
        stream.data.resize(static_cast<std::size_t>(found.StreamSize.QuadPart));
        std::size_t got = 0;
        bool ok = true;
        while (got < stream.data.size()) {
            DWORD read = 0;
            const DWORD want = static_cast<DWORD>(
                std::min<std::uint64_t>(stream.data.size() - got, 1u << 20));
            if (!ReadFile(handle, stream.data.data() + got, want, &read, nullptr) || read == 0) {
                ok = (read == 0 && got == stream.data.size());
                break;
            }
            got += read;
        }
        CloseHandle(handle);
        if (ok && got == stream.data.size()) {
            streams.push_back(std::move(stream));
        }
    } while (FindNextStreamW(search, &found));
    FindClose(search);
    return streams;
}

void apply_ads(const std::filesystem::path& path, const std::vector<AdsStream>& streams) {
    for (const auto& stream : streams) {
        const std::wstring stream_path = path.wstring() + L":" + widen_utf8(stream.name);
        const HANDLE handle = CreateFileW(stream_path.c_str(), GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            continue;  // best effort
        }
        std::size_t written = 0;
        while (written < stream.data.size()) {
            DWORD wrote = 0;
            const DWORD want = static_cast<DWORD>(
                std::min<std::uint64_t>(stream.data.size() - written, 1u << 20));
            if (!WriteFile(handle, stream.data.data() + written, want, &wrote, nullptr) ||
                wrote == 0) {
                break;
            }
            written += wrote;
        }
        CloseHandle(handle);
    }
}

#else

FileMetadata capture_metadata(const std::filesystem::path&) {
    return {};
}

void apply_metadata(const std::filesystem::path&, const FileMetadata&, bool) {}

std::optional<FileId> hardlink_identity(const std::filesystem::path& path) {
    struct ::stat st {};
    if (::stat(path.c_str(), &st) != 0 || st.st_nlink <= 1) {
        return std::nullopt;
    }
    FileId id;
    id.volume = static_cast<std::uint64_t>(st.st_dev);
    id.index_low = static_cast<std::uint64_t>(st.st_ino);
    return id;
}

bool is_reparse_point(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec));
}

std::vector<AdsStream> capture_ads(const std::filesystem::path&) {
    return {};
}

void apply_ads(const std::filesystem::path&, const std::vector<AdsStream>&) {}

#endif

}  // namespace axiom::core
