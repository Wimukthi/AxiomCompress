#include "core/file_meta.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/xattr.h>
#endif
#endif

namespace axiom::core {

#if defined(_WIN32)

namespace {

#pragma pack(push, 1)
struct ReparseDataHeader {
    std::uint32_t tag;
    std::uint16_t data_length;
    std::uint16_t reserved;
};
#pragma pack(pop)

constexpr std::size_t kReparseDataHeaderSize = sizeof(ReparseDataHeader);

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

void append_warning(FileMetadata& meta, std::string message) {
    meta.capture_warnings.push_back(std::move(message));
}

void capture_windows_security_descriptor(const std::filesystem::path& path,
                                         FileMetadata& meta) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    const auto status = GetNamedSecurityInfoW(
        path.c_str(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION,
        &owner, &group, &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS || descriptor == nullptr) {
        if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND &&
            status != ERROR_INVALID_FUNCTION) {
            append_warning(meta, "security descriptor could not be captured (error " +
                                      std::to_string(status) + ")");
        }
        return;
    }

    const DWORD length = GetSecurityDescriptorLength(descriptor);
    if (length == 0 || length > kMaxSecurityDescriptorBytes) {
        append_warning(meta, "security descriptor exceeded the archive metadata limit");
    } else {
        meta.has_windows_security_descriptor = true;
        meta.windows_security_descriptor.assign(
            static_cast<const std::uint8_t*>(descriptor),
            static_cast<const std::uint8_t*>(descriptor) + length);
    }
    LocalFree(descriptor);
}

void capture_windows_reparse_data(const std::filesystem::path& path,
                                  FileMetadata& meta) {
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        append_warning(meta, "reparse metadata could not be opened");
        return;
    }

    std::vector<std::uint8_t> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                                    buffer.data(), static_cast<DWORD>(buffer.size()),
                                    &returned, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!ok || returned < kReparseDataHeaderSize ||
        returned > kMaxReparseDataBytes) {
        if (error != ERROR_NOT_A_REPARSE_POINT && error != ERROR_INVALID_FUNCTION) {
            append_warning(meta, "reparse metadata could not be captured (error " +
                                      std::to_string(error) + ")");
        }
        return;
    }

    const auto* reparse = reinterpret_cast<const ReparseDataHeader*>(buffer.data());
    if (static_cast<std::size_t>(reparse->data_length) + kReparseDataHeaderSize !=
        returned) {
        append_warning(meta, "reparse metadata has an invalid length");
        return;
    }
    meta.has_reparse_data = true;
    meta.reparse_tag = reparse->tag;
    meta.reparse_data.assign(buffer.begin(), buffer.begin() + returned);
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
        capture_windows_security_descriptor(path, meta);
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            capture_windows_reparse_data(path, meta);
        }
    } else {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            append_warning(meta, "file attributes and timestamps could not be captured (error " +
                                      std::to_string(error) + ")");
        }
    }
    return meta;
}

std::vector<std::string> apply_metadata(const std::filesystem::path& path,
                                        const FileMetadata& meta,
                                        bool restore_times) {
    std::vector<std::string> warnings;
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
            if (!SetFileTime(handle, &creation, &access, &write)) {
                warnings.push_back("file timestamps could not be restored (error " +
                                   std::to_string(GetLastError()) + ")");
            }
            CloseHandle(handle);
        } else {
            warnings.push_back("file timestamps could not be opened for restore");
        }
    }

    if (meta.has_windows_attributes) {
        // Restore only the user-meaningful attributes; never replay structural bits
        // (directory, reparse point, etc.) that the filesystem already manages.
        constexpr DWORD kRestorable = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                                      FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
                                      FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
        const DWORD attributes = meta.windows_attributes & kRestorable;
        const DWORD desired_attributes = attributes == 0 ? FILE_ATTRIBUTE_NORMAL : attributes;
        if (!SetFileAttributesW(path.c_str(), desired_attributes)) {
            warnings.push_back("file attributes could not be restored (error " +
                               std::to_string(GetLastError()) + ")");
        }
    }

    if (meta.has_windows_security_descriptor) {
        auto* descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(
            const_cast<std::uint8_t*>(meta.windows_security_descriptor.data()));
        if (!SetFileSecurityW(path.c_str(),
                              OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                                  DACL_SECURITY_INFORMATION,
                              descriptor)) {
            warnings.push_back("security descriptor could not be restored (error " +
                               std::to_string(GetLastError()) + ")");
        }
    }
    return warnings;
}

bool apply_reparse_point(const std::filesystem::path& path, std::uint32_t tag,
                         const std::vector<std::uint8_t>& data, std::string* error) {
    const auto fail = [&](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };
    if (data.size() < kReparseDataHeaderSize ||
        data.size() > kMaxReparseDataBytes) {
        return fail("stored reparse data is outside its safety bounds");
    }
    const auto* reparse = reinterpret_cast<const ReparseDataHeader*>(data.data());
    if (reparse->tag != tag ||
        static_cast<std::size_t>(reparse->data_length) + kReparseDataHeaderSize !=
            data.size()) {
        return fail("stored reparse tag does not match its payload");
    }
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return fail("reparse point could not be opened for restore");
    }
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(
        handle, FSCTL_SET_REPARSE_POINT, const_cast<std::uint8_t*>(data.data()),
        static_cast<DWORD>(data.size()), nullptr, 0, &returned, nullptr);
    const DWORD win_error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!ok) {
        return fail("reparse point could not be restored (error " +
                    std::to_string(win_error) + ")");
    }
    return true;
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

SparseCaptureResult capture_sparse_file(const std::filesystem::path& path,
                                        std::uint64_t logical_size) {
    SparseCaptureResult result;
    if (logical_size == 0) {
        return result;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_SPARSE_FILE) == 0) {
        return result;
    }
    if (logical_size > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
        result.warning = "the sparse file is too large for the host allocation-query API";
        return result;
    }

    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        result.warning = "the file is marked sparse but its allocation map could not be opened";
        return result;
    }

    std::vector<FILE_ALLOCATED_RANGE_BUFFER> ranges(4096);
    std::vector<SparseExtent> allocated;
    std::uint64_t query_offset = 0;
    bool complete = true;
    while (query_offset < logical_size) {
        FILE_ALLOCATED_RANGE_BUFFER request{};
        request.FileOffset.QuadPart = static_cast<LONGLONG>(query_offset);
        request.Length.QuadPart = static_cast<LONGLONG>(logical_size - query_offset);
        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(
            handle, FSCTL_QUERY_ALLOCATED_RANGES, &request, sizeof(request),
            ranges.data(), static_cast<DWORD>(ranges.size() * sizeof(ranges[0])),
            &returned, nullptr);
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error != ERROR_MORE_DATA) {
            complete = false;
            break;
        }
        if (returned % sizeof(ranges[0]) != 0) {
            complete = false;
            break;
        }

        std::uint64_t next_offset = query_offset;
        const auto count = returned / static_cast<DWORD>(sizeof(ranges[0]));
        for (DWORD i = 0; i < count; ++i) {
            const auto& range = ranges[i];
            if (range.FileOffset.QuadPart < 0 || range.Length.QuadPart <= 0) {
                complete = false;
                break;
            }
            const auto offset = static_cast<std::uint64_t>(range.FileOffset.QuadPart);
            const auto length = static_cast<std::uint64_t>(range.Length.QuadPart);
            if (offset > logical_size || length > logical_size - offset) {
                complete = false;
                break;
            }
            const auto end = offset + length;
            if (!allocated.empty() && offset <= allocated.back().offset +
                                                   allocated.back().length) {
                const auto previous_end = allocated.back().offset +
                                          allocated.back().length;
                if (end > previous_end) {
                    allocated.back().length = end - allocated.back().offset;
                }
            } else {
                allocated.push_back({offset, length});
            }
            next_offset = std::max(next_offset, end);
        }
        if (!complete) {
            break;
        }
        if (ok && returned == 0) {
            // A successful query with no ranges means the remainder is a hole.
            break;
        }
        if (next_offset <= query_offset) {
            // A sparse query that neither returns a range nor advances cannot
            // be retried safely without spinning forever.
            complete = false;
            break;
        }
        query_offset = next_offset;
        if (ok) {
            break;
        }
    }
    CloseHandle(handle);

    if (!complete) {
        result.warning = "the file is marked sparse but its allocation map could not be captured";
        return result;
    }

    std::uint64_t allocated_bytes = 0;
    for (const auto& extent : allocated) {
        if (extent.length > std::numeric_limits<std::uint64_t>::max() - allocated_bytes) {
            result.warning = "the sparse allocation map overflowed its size bounds";
            return result;
        }
        allocated_bytes += extent.length;
    }
    if (allocated_bytes >= logical_size) {
        return result;  // sparse attribute can remain after all holes are filled
    }

    result.map = SparseFileMap{true, std::move(allocated)};
    return result;
}

bool restore_sparse_file(const std::filesystem::path& path,
                         const SparseFileMap& map,
                         std::uint64_t logical_size,
                         std::string* error) {
    const auto fail = [&](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };
    if (!map.is_sparse || logical_size == 0) {
        return true;
    }

    std::uint64_t previous_end = 0;
    for (const auto& extent : map.allocated) {
        if (extent.length == 0 || extent.offset < previous_end ||
            extent.offset > logical_size || extent.length > logical_size - extent.offset) {
            return fail("the stored sparse allocation map is invalid");
        }
        previous_end = extent.offset + extent.length;
    }

#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return fail("could not open the extracted file for sparse restoration");
    }

    DWORD returned = 0;
    if (!DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                         &returned, nullptr)) {
        const DWORD win_error = GetLastError();
        CloseHandle(handle);
        return fail("filesystem does not support sparse files (error " +
                    std::to_string(win_error) + ")");
    }

    auto zero_range = [&](std::uint64_t offset, std::uint64_t end) {
        if (end <= offset) return true;
        FILE_ZERO_DATA_INFORMATION zero{};
        zero.FileOffset.QuadPart = static_cast<LONGLONG>(offset);
        zero.BeyondFinalZero.QuadPart = static_cast<LONGLONG>(end);
        return DeviceIoControl(handle, FSCTL_SET_ZERO_DATA, &zero, sizeof(zero),
                               nullptr, 0, &returned, nullptr) != FALSE;
    };
    std::uint64_t hole_start = 0;
    for (const auto& extent : map.allocated) {
        if (!zero_range(hole_start, extent.offset)) {
            const DWORD win_error = GetLastError();
            CloseHandle(handle);
            return fail("could not deallocate a sparse hole (error " +
                        std::to_string(win_error) + ")");
        }
        hole_start = extent.offset + extent.length;
    }
    if (!zero_range(hole_start, logical_size)) {
        const DWORD win_error = GetLastError();
        CloseHandle(handle);
        return fail("could not deallocate the final sparse hole (error " +
                    std::to_string(win_error) + ")");
    }
    CloseHandle(handle);
    return true;
#else
#if defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
        return fail("could not open the extracted file for sparse restoration: " +
                    std::string(std::strerror(errno)));
    }
    if (::ftruncate(fd, static_cast<off_t>(logical_size)) != 0) {
        const std::string message = std::strerror(errno);
        ::close(fd);
        return fail("could not set the extracted file size: " + message);
    }
    auto zero_range = [&](std::uint64_t offset, std::uint64_t length) {
        return ::fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                           static_cast<off_t>(offset), static_cast<off_t>(length)) == 0;
    };
    std::uint64_t hole_start = 0;
    for (const auto& extent : map.allocated) {
        if (extent.offset > hole_start &&
            !zero_range(hole_start, extent.offset - hole_start)) {
            const std::string message = std::strerror(errno);
            ::close(fd);
            return fail("could not deallocate a sparse hole: " + message);
        }
        hole_start = extent.offset + extent.length;
    }
    if (hole_start < logical_size && !zero_range(hole_start, logical_size - hole_start)) {
        const std::string message = std::strerror(errno);
        ::close(fd);
        return fail("could not deallocate the final sparse hole: " + message);
    }
    ::close(fd);
    return true;
#else
    return fail("this platform does not expose sparse-file allocation controls");
#endif
#endif
}

#else

namespace {

void append_warning(FileMetadata& meta, std::string message) {
    meta.capture_warnings.push_back(std::move(message));
}

#if defined(__linux__) || defined(__APPLE__)

ssize_t list_xattrs_no_follow(const std::filesystem::path& path, char* names,
                              std::size_t size) {
#if defined(__APPLE__)
    return ::listxattr(path.c_str(), names, size, XATTR_NOFOLLOW);
#else
    return ::llistxattr(path.c_str(), names, size);
#endif
}

ssize_t get_xattr_no_follow(const std::filesystem::path& path, const char* name,
                            void* data, std::size_t size) {
#if defined(__APPLE__)
    return ::getxattr(path.c_str(), name, data, size, 0, XATTR_NOFOLLOW);
#else
    return ::lgetxattr(path.c_str(), name, data, size);
#endif
}

int set_xattr_no_follow(const std::filesystem::path& path, const char* name,
                        const void* data, std::size_t size) {
#if defined(__APPLE__)
    return ::setxattr(path.c_str(), name, data, size, 0, XATTR_NOFOLLOW);
#else
    return ::lsetxattr(path.c_str(), name, data, size, 0);
#endif
}

void capture_posix_xattrs(const std::filesystem::path& path, FileMetadata& meta) {
    const auto size = list_xattrs_no_follow(path, nullptr, 0);
    if (size <= 0) {
        bool expected_missing = errno == ENOTSUP || errno == EOPNOTSUPP;
#if defined(ENODATA)
        expected_missing = expected_missing || errno == ENODATA;
#endif
        if (size < 0 && !expected_missing) {
            append_warning(meta, "extended attributes could not be enumerated: " +
                                      std::string(std::strerror(errno)));
        }
        return;
    }
    constexpr std::size_t kMaxNameBytes = 4u << 10;
    constexpr std::size_t kMaxTotalBytes = 4u << 20;
    if (static_cast<std::uint64_t>(size) > kMaxTotalBytes) {
        append_warning(meta, "extended attribute names exceeded the archive metadata limit");
        return;
    }
    std::vector<char> names(static_cast<std::size_t>(size));
    const auto listed = list_xattrs_no_follow(path, names.data(), names.size());
    if (listed < 0) {
        append_warning(meta, "extended attributes could not be enumerated: " +
                                  std::string(std::strerror(errno)));
        return;
    }
    std::size_t total = 0;
    std::size_t cursor = 0;
    while (cursor < static_cast<std::size_t>(listed)) {
        const char* name = names.data() + cursor;
        const std::size_t remaining = static_cast<std::size_t>(listed) - cursor;
        std::size_t length = 0;
        while (length < remaining && name[length] != '\0') ++length;
        if (length == 0 || length >= remaining) {
            append_warning(meta, "extended attribute names were malformed");
            break;
        }
        cursor += length + 1;
        if (length > kMaxNameBytes || meta.xattrs.size() >= kMaxMetadataBlobCount) {
            append_warning(meta, "an extended attribute was skipped by the archive metadata limit");
            continue;
        }
        const auto value_size = get_xattr_no_follow(path, name, nullptr, 0);
        if (value_size < 0) {
            append_warning(meta, "extended attribute could not be read: " +
                                      std::string(name));
            continue;
        }
        if (static_cast<std::uint64_t>(value_size) > kMaxMetadataBlobBytes ||
            total > kMaxTotalBytes - static_cast<std::size_t>(value_size)) {
            append_warning(meta, "extended attribute was skipped because it is too large: " +
                                      std::string(name));
            continue;
        }
        MetadataBlob blob;
        blob.name.assign(name, length);
        blob.data.resize(static_cast<std::size_t>(value_size));
        if (value_size != 0 &&
            get_xattr_no_follow(path, name, blob.data.data(), blob.data.size()) != value_size) {
            append_warning(meta, "extended attribute could not be read: " +
                                      std::string(name));
            continue;
        }
        total += blob.data.size();
        meta.xattrs.push_back(std::move(blob));
    }
}

#endif

}  // namespace

FileMetadata capture_metadata(const std::filesystem::path& path) {
    FileMetadata meta;
    struct ::stat st {};
    if (::lstat(path.c_str(), &st) == 0) {
        meta.has_posix = true;
        meta.posix_mode = static_cast<std::uint32_t>(st.st_mode);
        meta.posix_uid = static_cast<std::uint32_t>(st.st_uid);
        meta.posix_gid = static_cast<std::uint32_t>(st.st_gid);
#if defined(__linux__) || defined(__APPLE__)
        capture_posix_xattrs(path, meta);
#endif
    } else if (errno != ENOENT && errno != ENOTDIR) {
        append_warning(meta, "POSIX file metadata could not be captured: " +
                                  std::string(std::strerror(errno)));
    }
    return meta;
}

std::vector<std::string> apply_metadata(const std::filesystem::path& path,
                                        const FileMetadata& meta, bool) {
    std::vector<std::string> warnings;
    if (meta.has_posix) {
        const bool symlink = S_ISLNK(static_cast<mode_t>(meta.posix_mode));
        if (!symlink && ::chmod(path.c_str(), static_cast<mode_t>(meta.posix_mode & 07777u)) != 0) {
            warnings.push_back("POSIX mode could not be restored: " +
                               std::string(std::strerror(errno)));
        }
        // Ownership restore is best-effort and normally requires privilege. lchown
        // intentionally addresses the link itself rather than following it.
        if (::lchown(path.c_str(), static_cast<uid_t>(meta.posix_uid),
                     static_cast<gid_t>(meta.posix_gid)) != 0 && errno != EPERM) {
            warnings.push_back("POSIX ownership could not be restored: " +
                               std::string(std::strerror(errno)));
        }
    }
#if defined(__linux__) || defined(__APPLE__)
    for (const auto& blob : meta.xattrs) {
        if (set_xattr_no_follow(path, blob.name.c_str(), blob.data.data(), blob.data.size()) != 0) {
            warnings.push_back("extended attribute could not be restored: " + blob.name);
        }
    }
#endif
    return warnings;
}

bool apply_reparse_point(const std::filesystem::path&, std::uint32_t,
                         const std::vector<std::uint8_t>& data, std::string* error) {
    if (error != nullptr) {
        *error = data.empty()
            ? "stored reparse data is empty"
            : "the host platform cannot restore Windows reparse metadata";
    }
    return false;
}

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

SparseCaptureResult capture_sparse_file(const std::filesystem::path& path,
                                        std::uint64_t logical_size) {
    SparseCaptureResult result;
    if (logical_size == 0) return result;

    struct ::stat st {};
    if (::stat(path.c_str(), &st) != 0) return result;
    if (st.st_blocks < 0 ||
        static_cast<std::uint64_t>(st.st_blocks) >
            std::numeric_limits<std::uint64_t>::max() / 512u) {
        return result;
    }
    const auto allocated_bytes = static_cast<std::uint64_t>(st.st_blocks) * 512u;
    if (allocated_bytes >= logical_size) return result;

#if defined(SEEK_DATA) && defined(SEEK_HOLE)
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        result.warning = "the file appears sparse but its allocation map could not be opened";
        return result;
    }
    SparseFileMap map;
    map.is_sparse = true;
    off_t cursor = 0;
    while (static_cast<std::uint64_t>(cursor) < logical_size) {
        const off_t data = ::lseek(fd, cursor, SEEK_DATA);
        if (data < 0) {
            if (errno == ENXIO) break;  // the remainder is a hole
            const std::string message = std::strerror(errno);
            ::close(fd);
            result.warning = "the sparse allocation map could not be queried: " + message;
            return result;
        }
        const off_t hole = ::lseek(fd, data, SEEK_HOLE);
        if (hole < data || hole < 0 || static_cast<std::uint64_t>(hole) > logical_size) {
            ::close(fd);
            result.warning = "the sparse allocation map returned invalid ranges";
            return result;
        }
        map.allocated.push_back({static_cast<std::uint64_t>(data),
                                 static_cast<std::uint64_t>(hole - data)});
        cursor = hole;
    }
    ::close(fd);
    result.map = std::move(map);
#else
    result.warning = "the file appears sparse but this platform cannot enumerate its ranges";
#endif
    return result;
}

bool restore_sparse_file(const std::filesystem::path& path,
                         const SparseFileMap& map,
                         std::uint64_t logical_size,
                         std::string* error) {
    const auto fail = [&](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };
    if (!map.is_sparse || logical_size == 0) return true;
#if defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
        return fail("could not open the extracted file for sparse restoration: " +
                    std::string(std::strerror(errno)));
    }
    if (::ftruncate(fd, static_cast<off_t>(logical_size)) != 0) {
        const std::string message = std::strerror(errno);
        ::close(fd);
        return fail("could not set the extracted file size: " + message);
    }
    auto punch_hole = [&](std::uint64_t offset, std::uint64_t length) {
        return ::fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                           static_cast<off_t>(offset), static_cast<off_t>(length)) == 0;
    };
    std::uint64_t hole_start = 0;
    for (const auto& extent : map.allocated) {
        if (extent.offset > hole_start &&
            !punch_hole(hole_start, extent.offset - hole_start)) {
            const std::string message = std::strerror(errno);
            ::close(fd);
            return fail("could not deallocate a sparse hole: " + message);
        }
        hole_start = extent.offset + extent.length;
    }
    if (hole_start < logical_size && !punch_hole(hole_start, logical_size - hole_start)) {
        const std::string message = std::strerror(errno);
        ::close(fd);
        return fail("could not deallocate the final sparse hole: " + message);
    }
    ::close(fd);
    return true;
#else
    return fail("this platform does not expose sparse-file allocation controls");
#endif
}

#endif

}  // namespace axiom::core
