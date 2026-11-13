#include "core/file_meta.hpp"

#include "core/exclusive_file.hpp"
#include "core/path_text.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

constexpr SECURITY_INFORMATION kCapturedSecurityInformation =
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;

// Keep a descriptor returned by GetNamedSecurityInfoW or GetSecurityInfo, or say
// why there is none, the same way whichever query produced it. Frees it.
void store_windows_security_descriptor(DWORD status, PSECURITY_DESCRIPTOR descriptor,
                                       FileMetadata& meta) {
    if (status != ERROR_SUCCESS || descriptor == nullptr) {
        if (descriptor != nullptr) LocalFree(descriptor);
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

void capture_windows_security_descriptor(const std::filesystem::path& path,
                                         FileMetadata& meta) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    const auto status = GetNamedSecurityInfoW(
        path.c_str(), SE_FILE_OBJECT, kCapturedSecurityInformation,
        &owner, &group, &dacl, nullptr, &descriptor);
    store_windows_security_descriptor(status, descriptor, meta);
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

// Restore only the user-meaningful attributes; never replay structural bits
// (directory, reparse point, etc.) that the filesystem already manages.
static DWORD restorable_attributes(const FileMetadata& meta) {
    constexpr DWORD kRestorable = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                                  FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
                                  FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    const DWORD attributes = meta.windows_attributes & kRestorable;
    return attributes == 0 ? FILE_ATTRIBUTE_NORMAL : attributes;
}

// Ownership is restored whenever the extracting user may change it, as on POSIX.
// Setting an owner or group needs WRITE_OWNER (or the restore privilege for
// someone else's SID), which an unelevated user does not hold even for their own
// files, and one failed component fails the whole call. So when only the
// ownership is refused, restore the access list on its own; a refused ownership
// change is not worth a warning per file. `set` applies the stored descriptor
// for the given parts; returns ERROR_SUCCESS or the error that defeated it.
template <typename SetSecurity>
static DWORD restore_security_descriptor(const FileMetadata& meta, SetSecurity set) {
    auto* descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(
        const_cast<std::uint8_t*>(meta.windows_security_descriptor.data()));
    if (set(OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                DACL_SECURITY_INFORMATION,
            descriptor)) {
        return ERROR_SUCCESS;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_ACCESS_DENIED && error != ERROR_INVALID_OWNER &&
        error != ERROR_INVALID_PRIMARY_GROUP && error != ERROR_PRIVILEGE_NOT_HELD) {
        return error;
    }
    return set(DACL_SECURITY_INFORMATION, descriptor) ? ERROR_SUCCESS : GetLastError();
}

std::vector<std::string> apply_metadata(const std::filesystem::path& path,
                                        const FileMetadata& meta,
                                        bool restore_times,
                                        bool /*restore_privileged*/) {
    std::vector<std::string> warnings;
    if (restore_times && meta.has_windows_times) {
        // FILE_FLAG_BACKUP_SEMANTICS lets us open a directory handle; the write
        // happens before attributes so a restored read-only flag cannot block it.
        // FILE_FLAG_OPEN_REPARSE_POINT opens a symbolic link or junction itself
        // instead of whatever it points to, so the times of a link entry land on
        // the link and never on a file outside the extraction folder.
        const HANDLE handle = CreateFileW(
            path.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
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
        if (!SetFileAttributesW(path.c_str(), restorable_attributes(meta))) {
            warnings.push_back("file attributes could not be restored (error " +
                               std::to_string(GetLastError()) + ")");
        }
    }

    // SetFileSecurityW opens `path` following links, so on a link it would change
    // the owner and access list of the link's target. What an archive stores for a
    // link is the target's descriptor anyway (capture follows the link), so a link
    // is left with the default security of the folder it was created in.
    if (meta.has_windows_security_descriptor && !is_reparse_point(path)) {
        const DWORD error = restore_security_descriptor(
            meta, [&](SECURITY_INFORMATION parts, PSECURITY_DESCRIPTOR descriptor) {
                return SetFileSecurityW(path.c_str(), parts, descriptor) != FALSE;
            });
        if (error != ERROR_SUCCESS) {
            warnings.push_back("security descriptor could not be restored (error " +
                               std::to_string(error) + ")");
        }
    }
    return warnings;
}

std::uint32_t staged_finish_access(bool with_ownership) {
    return DELETE | READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES |
           (with_ownership ? WRITE_OWNER : 0);
}

std::optional<std::vector<std::string>> finish_staged_file(
    ExclusiveFile& file, const std::filesystem::path& target, const FileMetadata& meta,
    bool restore_times) {
    if (!file.extra_access_granted()) return std::nullopt;
    const HANDLE handle = file.native_handle();

    // Rename first, as the path-based finish does, so the metadata lands in the
    // same order. Renaming through the handle saves reopening the file by name.
    const std::wstring& name = target.native();
    const std::size_t name_bytes = name.size() * sizeof(wchar_t);
    if (name_bytes > MAXDWORD - sizeof(FILE_RENAME_INFO)) return std::nullopt;
    std::vector<std::uint8_t> buffer(sizeof(FILE_RENAME_INFO) + name_bytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->ReplaceIfExists = TRUE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(rename->FileName, name.data(), name_bytes);
    if (!SetFileInformationByHandle(handle, FileRenameInfo, rename,
                                    static_cast<DWORD>(buffer.size()))) {
        return std::nullopt;
    }

    // What apply_metadata(target, meta, restore_times) does, through the handle.
    // The staging file was created new and never through a link, so it is not a
    // reparse point and its descriptor is always applied.
    std::vector<std::string> warnings;
    if (restore_times && meta.has_windows_times) {
        const FILETIME creation = u64_to_filetime(meta.windows_creation_time);
        const FILETIME access = u64_to_filetime(meta.windows_access_time);
        const FILETIME write = u64_to_filetime(meta.windows_write_time);
        if (!SetFileTime(handle, &creation, &access, &write)) {
            warnings.push_back("file timestamps could not be restored (error " +
                               std::to_string(GetLastError()) + ")");
        }
    }
    if (meta.has_windows_attributes) {
        // Zero times in FILE_BASIC_INFO leave the ones just restored alone.
        FILE_BASIC_INFO basic{};
        basic.FileAttributes = restorable_attributes(meta);
        if (!SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic))) {
            warnings.push_back("file attributes could not be restored (error " +
                               std::to_string(GetLastError()) + ")");
        }
    }
    if (meta.has_windows_security_descriptor) {
        const DWORD error = restore_security_descriptor(
            meta, [&](SECURITY_INFORMATION parts, PSECURITY_DESCRIPTOR descriptor) {
                return SetKernelObjectSecurity(handle, parts, descriptor) != FALSE;
            });
        if (error != ERROR_SUCCESS) {
            warnings.push_back("security descriptor could not be restored (error " +
                               std::to_string(error) + ")");
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

std::vector<std::string> apply_ads(const std::filesystem::path& path,
                                   const std::vector<AdsStream>& streams) {
    std::vector<std::string> warnings;
    for (const auto& stream : streams) {
        // The name is appended to the file's path, so anything that could change
        // which file or stream that path means is refused: separators and ':'
        // (checked when the archive is read, repeated for callers that build
        // streams themselves), a leading '$' (NTFS attribute names such as $DATA
        // address the file's own contents) and control characters.
        const bool unsafe =
            !is_valid_ads_name(stream.name) || stream.name.front() == '$' ||
            std::any_of(stream.name.begin(), stream.name.end(),
                        [](unsigned char ch) { return ch < 0x20; });
        if (unsafe) {
            warnings.push_back(
                "an alternate data stream with an unsafe name was not restored");
            continue;
        }
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
    return warnings;
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

std::optional<std::string> read_download_mark(const std::filesystem::path& archive) {
    const std::wstring stream_path = archive.native() + L":Zone.Identifier";
    const HANDLE handle = CreateFileW(stream_path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    std::string content(64u << 10, '\0');
    DWORD got = 0;
    const BOOL ok = ReadFile(handle, content.data(), static_cast<DWORD>(content.size()), &got,
                             nullptr);
    CloseHandle(handle);
    if (!ok) return std::nullopt;
    content.resize(got);

    // Zone.Identifier is an INI file: a [ZoneTransfer] section with ZoneId and,
    // from browsers, ReferrerUrl and HostUrl.
    std::optional<int> zone;
    std::string referrer;
    std::string host;
    bool in_zone_transfer = false;
    const auto trim = [](std::string_view text) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
            text.remove_suffix(1);
        }
        return text;
    };
    const auto equals_folded = [](std::string_view left, std::string_view right) {
        return left.size() == right.size() &&
               std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) ==
                          std::tolower(static_cast<unsigned char>(b));
               });
    };
    // Only a printable, bounded URL is carried over into another file's mark.
    const auto plain_value = [](std::string_view value) {
        if (value.empty() || value.size() > 2048) return false;
        return std::all_of(value.begin(), value.end(), [](char c) {
            return static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F;
        });
    };
    std::string_view rest(content);
    while (!rest.empty()) {
        const auto end = rest.find('\n');
        std::string_view line = trim(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        if (line.empty() || line.front() == ';') continue;
        if (line.front() == '[') {
            in_zone_transfer = equals_folded(line, "[ZoneTransfer]");
            continue;
        }
        if (!in_zone_transfer) continue;
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        if (equals_folded(key, "ZoneId")) {
            if (value.size() == 1 && value.front() >= '0' && value.front() <= '9') {
                zone = value.front() - '0';
            }
        } else if (equals_folded(key, "ReferrerUrl") && plain_value(value)) {
            referrer.assign(value);
        } else if (equals_folded(key, "HostUrl") && plain_value(value)) {
            host.assign(value);
        }
    }
    const int zone_id = zone.value_or(3);
    if (zone_id < 3) return std::nullopt;  // local machine, intranet, trusted sites

    std::string mark = "[ZoneTransfer]\r\nZoneId=" + std::to_string(zone_id) + "\r\n";
    if (!referrer.empty()) mark += "ReferrerUrl=" + referrer + "\r\n";
    if (!host.empty()) mark += "HostUrl=" + host + "\r\n";
    return mark;
}

std::string write_download_mark(const std::filesystem::path& path, std::string_view mark,
                                bool keep_times) {
    HANDLE file = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION before{};
    if (keep_times) {
        file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file, &before)) {
            const DWORD error = GetLastError();
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            return "mark of the web could not be added (error " + std::to_string(error) + ")";
        }
        if ((before.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
            FILE_BASIC_INFO writable{};
            writable.FileAttributes = before.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY;
            if (writable.FileAttributes == 0) writable.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            SetFileInformationByHandle(file, FileBasicInfo, &writable, sizeof(writable));
        }
    }

    std::string failure;
    const std::wstring stream_path = path.native() + L":Zone.Identifier";
    const HANDLE stream = CreateFileW(stream_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stream == INVALID_HANDLE_VALUE) {
        failure = "mark of the web could not be added (error " +
                  std::to_string(GetLastError()) + ")";
    } else {
        DWORD wrote = 0;
        if (!WriteFile(stream, mark.data(), static_cast<DWORD>(mark.size()), &wrote, nullptr) ||
            wrote != mark.size()) {
            failure = "mark of the web could not be written (error " +
                      std::to_string(GetLastError()) + ")";
        }
        if (!CloseHandle(stream) && failure.empty()) {
            failure = "mark of the web could not be written (error " +
                      std::to_string(GetLastError()) + ")";
        }
    }

    if (keep_times) {
        SetFileTime(file, &before.ftCreationTime, &before.ftLastAccessTime,
                    &before.ftLastWriteTime);
        if ((before.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
            FILE_BASIC_INFO restored{};
            restored.FileAttributes = before.dwFileAttributes;
            SetFileInformationByHandle(file, FileBasicInfo, &restored, sizeof(restored));
        }
        CloseHandle(file);
    }
    return failure;
}

SourceFileProbe probe_source_file(const std::filesystem::path& path) {
    SourceFileProbe probe;
    // READ_CONTROL for the security descriptor and FILE_READ_ATTRIBUTES for the
    // rest. Opening follows a link, as GetNamedSecurityInfoW did; a reparse
    // point is refused below so the path-based capture keeps handling those.
    const HANDLE handle = CreateFileW(
        path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return probe;
    struct HandleCloser {
        HANDLE handle;
        ~HandleCloser() { CloseHandle(handle); }
    } closer{handle};

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return probe;
    }

    // The same fields capture_metadata() takes from GetFileAttributesExW.
    probe.meta.has_windows_attributes = true;
    probe.meta.windows_attributes = info.dwFileAttributes;
    probe.meta.has_windows_times = true;
    probe.meta.windows_creation_time = filetime_to_u64(info.ftCreationTime);
    probe.meta.windows_access_time = filetime_to_u64(info.ftLastAccessTime);
    probe.meta.windows_write_time = filetime_to_u64(info.ftLastWriteTime);
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        PSID owner = nullptr;
        PSID group = nullptr;
        PACL dacl = nullptr;
        const DWORD status = GetSecurityInfo(handle, SE_FILE_OBJECT,
                                             kCapturedSecurityInformation, &owner,
                                             &group, &dacl, nullptr, &descriptor);
        store_windows_security_descriptor(status, descriptor, probe.meta);
    }

    // std::filesystem's file_time_type counts FILETIME ticks on Windows, so this
    // is the value fs::last_write_time() reports for the same file.
    probe.write_time = std::filesystem::file_time_type(
        std::filesystem::file_time_type::duration(
            static_cast<std::filesystem::file_time_type::rep>(
                filetime_to_u64(info.ftLastWriteTime))));
    probe.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) |
                 info.nFileSizeLow;
    if (info.nNumberOfLinks > 1) {
        FileId id;
        id.volume = info.dwVolumeSerialNumber;
        id.index_high = info.nFileIndexHigh;
        id.index_low = info.nFileIndexLow;
        probe.hardlink = id;
    }
    probe.sparse = (info.dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;

    // Almost every file has only its unnamed data stream. Confirming that here
    // saves capture_ads() a lookup of its own; a file with named streams, or a
    // list that does not fit, still goes through capture_ads().
    alignas(FILE_STREAM_INFO) std::uint8_t streams[1024];
    if (GetFileInformationByHandleEx(handle, FileStreamInfo, streams, sizeof(streams))) {
        bool named = false;
        std::size_t offset = 0;
        for (;;) {
            const auto* stream = reinterpret_cast<const FILE_STREAM_INFO*>(streams + offset);
            const std::wstring_view name(stream->StreamName,
                                         stream->StreamNameLength / sizeof(wchar_t));
            if (name != L"::$DATA") named = true;
            if (stream->NextEntryOffset == 0) break;
            offset += stream->NextEntryOffset;
            if (offset + sizeof(FILE_STREAM_INFO) > sizeof(streams)) {
                named = true;
                break;
            }
        }
        probe.named_streams = named;
    }

    probe.opened = true;
    return probe;
}

bool walk_source_tree(const std::filesystem::path& root,
                      const std::function<void(const SourceEntry&)>& visit,
                      std::error_code& error) {
    error.clear();
    struct Level {
        HANDLE find = INVALID_HANDLE_VALUE;
        std::filesystem::path directory;
        Level() = default;
        Level(Level&& other) noexcept
            : find(std::exchange(other.find, INVALID_HANDLE_VALUE)),
              directory(std::move(other.directory)) {}
        Level& operator=(Level&&) = delete;
        ~Level() {
            if (find != INVALID_HANDLE_VALUE) FindClose(find);
        }
    };
    // An explicit stack keeps a very deep tree off the thread's stack, and
    // visiting a folder's contents before its next sibling gives the same
    // pre-order as recursive_directory_iterator.
    std::vector<Level> levels;
    WIN32_FIND_DATAW data{};

    // Opens `directory` and leaves its first entry in `data`. False means it is
    // skipped (permission denied, as skip_permission_denied does) or failed.
    const auto open_level = [&](const std::filesystem::path& directory) {
        const std::filesystem::path pattern = directory / L"*";
        const HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                             FindExSearchNameMatch, nullptr,
                                             FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (code != ERROR_ACCESS_DENIED) {
                error.assign(static_cast<int>(code), std::system_category());
            }
            return false;
        }
        Level level;
        level.find = find;
        level.directory = directory;
        levels.push_back(std::move(level));
        return true;
    };

    if (!open_level(root)) return !error;
    bool have_entry = true;  // `data` holds an entry of levels.back() not yet visited
    while (!levels.empty()) {
        if (!have_entry) {
            if (!FindNextFileW(levels.back().find, &data)) {
                const DWORD code = GetLastError();
                if (code != ERROR_NO_MORE_FILES) {
                    error.assign(static_cast<int>(code), std::system_category());
                    return false;
                }
                levels.pop_back();
                continue;
            }
        }
        have_entry = false;
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }

        const std::filesystem::path path = levels.back().directory / data.cFileName;
        const DWORD attributes = data.dwFileAttributes;
        SourceEntryKind kind;
        std::optional<std::uint64_t> size;
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            // dwReserved0 carries the reparse tag. Only a true symbolic link is
            // a symlink to std::filesystem; a junction or any other tag is the
            // opaque reparse point that is_reparse_point() reported before.
            kind = data.dwReserved0 == IO_REPARSE_TAG_SYMLINK
                ? SourceEntryKind::symlink
                : SourceEntryKind::reparse_point;
        } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            kind = SourceEntryKind::directory;
        } else {
            kind = SourceEntryKind::regular_file;
            size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) |
                   data.nFileSizeLow;
        }
        visit(SourceEntry{path, kind, size});
        if (kind == SourceEntryKind::directory) {
            if (open_level(path)) {
                have_entry = true;
            } else if (error) {
                return false;
            }
        }
    }
    return true;
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

// Extended attributes that can grant privilege or change access for other users:
// file capabilities, MAC labels, integrity data, POSIX ACLs. Only Linux names its
// attributes by namespace; on macOS every attribute is an ordinary one and the
// kernel decides what a caller may set.
bool is_privileged_xattr(const std::string& name) {
#if defined(__linux__)
    return !name.starts_with("user.");
#else
    (void)name;
    return false;
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
                                        const FileMetadata& meta, bool,
                                        bool restore_privileged) {
    std::vector<std::string> warnings;
    // What is on disk decides how the path is treated. The stored mode describes
    // the archive's source, so an entry that claims to be something other than a
    // link must not get a link chmod'ed through to whatever it points at.
    struct ::stat current {};
    if (::lstat(path.c_str(), &current) != 0) {
        warnings.push_back("POSIX metadata could not be restored: " +
                           std::string(std::strerror(errno)));
        return warnings;
    }
    const bool symlink = S_ISLNK(current.st_mode);
    if (meta.has_posix) {
        // Ownership first: changing a file's owner clears its set-id bits, so a
        // mode applied before the chown could be partly undone. Best-effort and
        // normally needs privilege; lchown addresses the link itself, not its
        // target.
        const auto uid = static_cast<uid_t>(meta.posix_uid);
        const auto gid = static_cast<gid_t>(meta.posix_gid);
        if ((current.st_uid != uid || current.st_gid != gid) &&
            ::lchown(path.c_str(), uid, gid) != 0 && errno != EPERM) {
            warnings.push_back("POSIX ownership could not be restored: " +
                               std::string(std::strerror(errno)));
        }
        if (!symlink) {
            auto mode = static_cast<mode_t>(meta.posix_mode & 07777u);
            if (!restore_privileged) {
                // A set-user-ID/set-group-ID bit makes a program run with someone
                // else's rights. Directories keep set-group-ID (it only decides
                // which group new entries get).
                mode &= static_cast<mode_t>(~S_ISUID);
                if (!S_ISDIR(current.st_mode)) mode &= static_cast<mode_t>(~S_ISGID);
            }
            if (::chmod(path.c_str(), mode) != 0) {
                warnings.push_back("POSIX mode could not be restored: " +
                                   std::string(std::strerror(errno)));
            }
        }
    }
#if defined(__linux__) || defined(__APPLE__)
    std::string withheld;
    for (const auto& blob : meta.xattrs) {
        if (!restore_privileged && is_privileged_xattr(blob.name)) {
            if (!withheld.empty()) withheld += ", ";
            withheld += blob.name;
            continue;
        }
        if (set_xattr_no_follow(path, blob.name.c_str(), blob.data.data(), blob.data.size()) != 0) {
            warnings.push_back("extended attribute could not be restored: " + blob.name);
        }
    }
    if (!withheld.empty()) {
        warnings.push_back(
            "privileged extended attributes were not restored (opt in to restore "
            "privileged metadata): " + withheld);
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

std::vector<std::string> apply_ads(const std::filesystem::path&, const std::vector<AdsStream>&) {
    return {};
}

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

bool restore_sparse_file([[maybe_unused]] const std::filesystem::path& path,
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

std::optional<std::string> read_download_mark(const std::filesystem::path&) {
    return std::nullopt;
}

std::string write_download_mark(const std::filesystem::path&, std::string_view, bool) {
    return {};
}

SourceFileProbe probe_source_file(const std::filesystem::path&) {
    return {};
}

std::uint32_t staged_finish_access(bool) {
    return 0;
}

std::optional<std::vector<std::string>> finish_staged_file(
    ExclusiveFile&, const std::filesystem::path&, const FileMetadata&, bool) {
    return std::nullopt;
}

bool walk_source_tree(const std::filesystem::path& root,
                      const std::function<void(const SourceEntry&)>& visit,
                      std::error_code& error) {
    namespace fs = std::filesystem;
    error.clear();
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                             error),
         end;
         it != end && !error; it.increment(error)) {
        const auto& entry = *it;
        std::error_code status_error;
        const fs::file_status status = fs::symlink_status(entry.path(), status_error);
        if (status_error) continue;
        if (fs::is_symlink(status)) {
            visit(SourceEntry{entry.path(), SourceEntryKind::symlink, std::nullopt});
        } else if (is_reparse_point(entry.path())) {
            visit(SourceEntry{entry.path(), SourceEntryKind::reparse_point, std::nullopt});
            it.disable_recursion_pending();
        } else if (fs::is_directory(status)) {
            visit(SourceEntry{entry.path(), SourceEntryKind::directory, std::nullopt});
        } else if (fs::is_regular_file(status)) {
            visit(SourceEntry{entry.path(), SourceEntryKind::regular_file, std::nullopt});
        }
    }
    return !error;
}

#endif

bool is_risky_download_type(const std::filesystem::path& name) {
    // Kept to types that run code, open with active content, or carry other
    // files; everything else is marked only under the "all" policy.
    static constexpr std::string_view kRisky[] = {
        // programs, installers, libraries
        ".exe", ".com", ".scr", ".pif", ".msi", ".msp", ".msix", ".msixbundle", ".appx",
        ".appxbundle", ".appinstaller", ".application", ".appref-ms", ".cpl", ".dll", ".ocx",
        ".sys", ".drv", ".efi", ".jar", ".gadget", ".msc", ".reg", ".inf",
        // scripts
        ".bat", ".cmd", ".ps1", ".psm1", ".psd1", ".ps1xml", ".psc1", ".vb", ".vbs", ".vbe",
        ".js", ".jse", ".ws", ".wsf", ".wsc", ".wsh", ".hta", ".sct", ".py", ".pyw",
        ".pl", ".rb", ".sh", ".lua",
        // shortcuts and links
        ".lnk", ".url", ".website", ".scf", ".library-ms", ".search-ms", ".settingcontent-ms",
        // documents with active content or a viewer of their own
        ".doc", ".docx", ".docm", ".dot", ".dotx", ".dotm", ".rtf", ".xls", ".xlsx",
        ".xlsm", ".xlsb", ".xlt", ".xltx", ".xltm", ".xla", ".xlam", ".ppt", ".pptx",
        ".pptm", ".pot", ".potx", ".potm", ".pps", ".ppsx", ".ppsm", ".ppa", ".ppam",
        ".one", ".onepkg", ".pub", ".vsd", ".vsdx", ".vsdm", ".mdb", ".accdb", ".accde",
        ".odt", ".ods", ".odp", ".pdf", ".xps", ".chm", ".htm", ".html", ".mht", ".mhtml",
        ".svg", ".xml", ".xsl",
        // disk images and archives, which would carry their own contents past the mark
        ".iso", ".img", ".vhd", ".vhdx", ".udf", ".zip", ".7z", ".rar", ".cab", ".tar",
        ".gz", ".tgz", ".bz2", ".xz", ".zst", ".arj", ".lzh", ".axar", ".axc",
    };
    std::string extension = path_to_utf8(name.extension());
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return std::find(std::begin(kRisky), std::end(kRisky), extension) != std::end(kRisky);
}

}  // namespace axiom::core
