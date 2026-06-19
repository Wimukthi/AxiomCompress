#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace axiom::core {

// A named NTFS alternate data stream attached to a file (e.g. "Zone.Identifier").
// The default unnamed $DATA stream — the file's own contents — is never an ADS here.
struct AdsStream {
    std::string name;                  // stream name, UTF-8 (no ':' or ':$DATA')
    std::vector<std::uint8_t> data;    // stream bytes
};

// Largest alternate data stream captured per stream; larger ones are skipped (the
// host file is still archived). ADS are stored in the directory, so this bounds it.
inline constexpr std::uint64_t kMaxAdsBytes = 1u << 20;  // 1 MiB

// Stable identity of a file on its volume. Two paths with the same FileId are the
// same physical file (hard links to one inode). On Windows it is the volume serial
// plus the 64-bit file index; on POSIX it is st_dev plus st_ino.
struct FileId {
    std::uint64_t volume = 0;
    std::uint64_t index_high = 0;
    std::uint64_t index_low = 0;
};

// Returns the file's identity *only when it has more than one hard link* (so the
// common single-link case costs nothing to track); returns nullopt otherwise, on
// failure, or on platforms without link support.
std::optional<FileId> hardlink_identity(const std::filesystem::path& path);

// True if `path` is a redirecting link: a symlink on any platform, and additionally
// an NTFS junction / mount point (directory reparse point) on Windows. Used by safe
// extraction to refuse writing *through* such a component. std::filesystem's
// is_symlink alone misses Windows junctions, which need no privilege to create.
bool is_reparse_point(const std::filesystem::path& path);

// Capture a file's named alternate data streams (Windows/NTFS). Skips the default
// $DATA stream and any stream larger than kMaxAdsBytes. Empty on non-Windows or none.
std::vector<AdsStream> capture_ads(const std::filesystem::path& path);

// Recreate named alternate data streams beside an extracted file, best-effort.
// A no-op on non-Windows builds.
void apply_ads(const std::filesystem::path& path, const std::vector<AdsStream>& streams);

// OS file metadata captured at archive time and reapplied on extract. Fields are
// platform-tagged; a reader applies what is present and relevant to the extracting
// OS and ignores the rest.
struct FileMetadata {
    bool has_windows_attributes = false;
    std::uint32_t windows_attributes = 0;  // Windows FILE_ATTRIBUTE_* bitmask

    // Windows FILETIME values: 100-ns ticks since 1601-01-01 UTC (full precision,
    // superseding the seconds-granularity mtime record when present).
    bool has_windows_times = false;
    std::uint64_t windows_creation_time = 0;
    std::uint64_t windows_access_time = 0;
    std::uint64_t windows_write_time = 0;

    bool has_posix = false;
    std::uint32_t posix_mode = 0;
    std::uint32_t posix_uid = 0;
    std::uint32_t posix_gid = 0;
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
