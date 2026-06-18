#pragma once

#include <cstdint>
#include <filesystem>

namespace axiom::core {

// OS file metadata captured at archive time and reapplied on extract. Fields are
// platform-tagged; a reader applies what is present and relevant to the extracting
// OS and ignores the rest. (POSIX mode/owner fields are added in a later phase.)
struct FileMetadata {
    bool has_windows_attributes = false;
    std::uint32_t windows_attributes = 0;  // Windows FILE_ATTRIBUTE_* bitmask

    // Windows FILETIME values: 100-ns ticks since 1601-01-01 UTC (full precision,
    // superseding the seconds-granularity mtime record when present).
    bool has_windows_times = false;
    std::uint64_t windows_creation_time = 0;
    std::uint64_t windows_access_time = 0;
    std::uint64_t windows_write_time = 0;
};

// Read metadata from an existing path (file or directory). On non-Windows builds,
// or if the OS query fails, returns an empty (all-absent) FileMetadata.
FileMetadata capture_metadata(const std::filesystem::path& path);

// Reapply metadata to an extracted path, best-effort. Attributes are always
// restored when present; timestamps are restored only when `restore_times` is set.
// A no-op on non-Windows builds.
void apply_metadata(const std::filesystem::path& path, const FileMetadata& meta,
                    bool restore_times);

}  // namespace axiom::core
