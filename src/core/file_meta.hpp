#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace axiom::core {

// A named NTFS alternate data stream attached to a file (e.g. "Zone.Identifier").
// The default unnamed $DATA stream — the file's own contents — is never an ADS here.
struct AdsStream {
    std::string name;                  // stream name, UTF-8 (no ':' or ':$DATA')
    std::vector<std::uint8_t> data;    // stream bytes
};

// Whether `name` can be an alternate-data-stream name as Windows reports it:
// non-empty, with no path separator, no ':' (which separates a stream from its
// type) and no NUL. A name that fails this can only come from a hand-built
// archive, and appended to a file's path it would address something else.
inline bool is_valid_ads_name(std::string_view name) noexcept {
    return !name.empty() &&
           name.find_first_of(std::string_view("\\/:\0", 4)) == std::string_view::npos;
}

// A bounded named metadata value. On POSIX this carries an extended attribute;
// other platforms may use the same shape for future filesystem metadata.
struct MetadataBlob {
    std::string name;
    std::vector<std::uint8_t> data;
};

// A logical file can contain zero-filled holes that are not allocated on disk.
// The archive still stores the complete logical byte stream, while this map lets
// extraction recreate the allocation layout without changing content semantics.
struct SparseExtent {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct SparseFileMap {
    bool is_sparse = false;
    std::vector<SparseExtent> allocated;
};

struct SparseCaptureResult {
    std::optional<SparseFileMap> map;
    // Empty means the file was either dense or sparse capture was not applicable.
    // A non-empty message means the filesystem identified a sparse candidate but
    // the allocation map could not be captured completely.
    std::string warning;
};

// Largest alternate data stream captured per stream; larger ones are skipped (the
// host file is still archived). ADS are stored in the directory, so this bounds it.
inline constexpr std::uint64_t kMaxAdsBytes = 1u << 20;  // 1 MiB
inline constexpr std::uint64_t kMaxMetadataBlobBytes = 1u << 20;  // 1 MiB
inline constexpr std::size_t kMaxMetadataBlobCount = 128;
inline constexpr std::uint64_t kMaxSecurityDescriptorBytes = 64u << 10;
inline constexpr std::uint64_t kMaxReparseDataBytes = 16u << 10;

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
// A stream whose name could address anything but a plain named stream is not
// restored and is reported in the returned messages. A no-op on non-Windows builds.
std::vector<std::string> apply_ads(const std::filesystem::path& path,
                                   const std::vector<AdsStream>& streams);

// Capture the allocated ranges of a sparse file. Dense files return no map and
// no warning. A warning is returned only when the file appears sparse but the
// platform/filesystem cannot provide a complete range map.
SparseCaptureResult capture_sparse_file(const std::filesystem::path& path,
                                         std::uint64_t logical_size);

// Recreate a previously captured allocation map after the logical bytes have
// been written. The target must already have `logical_size` bytes. Returns false
// when sparse allocation is unsupported or the filesystem operation fails.
bool restore_sparse_file(const std::filesystem::path& path,
                         const SparseFileMap& map,
                         std::uint64_t logical_size,
                         std::string* error = nullptr);

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

    // Windows self-relative security descriptor. It contains the owner, group,
    // and DACL when the source permits them to be queried.
    bool has_windows_security_descriptor = false;
    std::vector<std::uint8_t> windows_security_descriptor;

    // POSIX extended attributes, captured without following a symlink.
    std::vector<MetadataBlob> xattrs;

    // Opaque Windows reparse data. Extraction only applies it after all normal
    // content writes have completed and rejects unsafe ancestor traversal.
    bool has_reparse_data = false;
    std::uint32_t reparse_tag = 0;
    std::vector<std::uint8_t> reparse_data;

    // Source-side capture failures are promoted to archive-level warnings by the
    // archive scanner. These are deliberately not serialized inside the entry.
    std::vector<std::string> capture_warnings;
};

// Read metadata from an existing path (file or directory). Unsupported fields or
// recoverable query failures are returned as capture_warnings so callers can show
// them or enforce strict metadata mode.
FileMetadata capture_metadata(const std::filesystem::path& path);

// What the archive writer asks about one regular source file, answered through
// a single open handle. Asked one by one, by path, these took seven filesystem
// lookups per file, which dominated archiving many small files. `opened` is
// false when the probe does not apply (a platform without it, a file that cannot
// be opened this way, or one that turns out to be a directory or reparse point);
// the caller then asks the individual path-based questions, which keep their
// own warnings.
struct SourceFileProbe {
    bool opened = false;
    FileMetadata meta;  // what capture_metadata(path) returns
    std::filesystem::file_time_type write_time{};  // as fs::last_write_time(path)
    std::uint64_t size = 0;                        // as fs::file_size(path)
    std::optional<FileId> hardlink;                // as hardlink_identity(path)
    bool sparse = false;         // marked sparse, so capture_sparse_file has work
    bool named_streams = true;   // false only when the stream list was read and
                                 // holds nothing but the file's own data
};

SourceFileProbe probe_source_file(const std::filesystem::path& path);

enum class SourceEntryKind {
    symlink,
    reparse_point,  // any other redirecting object; never descended into
    directory,
    regular_file,
};

struct SourceEntry {
    const std::filesystem::path& path;
    SourceEntryKind kind;
    std::optional<std::uint64_t> size;  // for a regular file, when the listing had it
};

// Visit every descendant of the directory `root`, depth first in directory
// order, as recursive_directory_iterator with skip_permission_denied would: a
// folder that cannot be opened for lack of permission is skipped, symlinks and
// other reparse points are reported but never descended into, and anything
// that is none of the four kinds is left out. On Windows the kind and size come
// from the directory listing itself; asking per path cost two lookups per entry.
// Returns false with `error` set when enumeration fails for another reason; the
// entries visited before the failure stand. `visit` may throw.
bool walk_source_tree(const std::filesystem::path& root,
                      const std::function<void(const SourceEntry&)>& visit,
                      std::error_code& error);

// Reapply metadata to an extracted path, best-effort. Attributes are always
// restored when present; timestamps are restored only when `restore_times` is set.
// Reapply metadata after content is safely materialized. The returned messages
// identify best-effort failures so callers can surface them or fail in strict
// mode. Empty means every requested operation succeeded or was not applicable.
//
// What the path is on disk decides how it is treated, never what the archive says
// it was: a symbolic link is never followed to change a mode, whatever mode the
// archive stored for it.
//
// Privileged metadata is withheld unless `restore_privileged` is set, so that an
// archive cannot plant a set-user-ID program or a file with capabilities by
// itself. On POSIX that is the set-user-ID/set-group-ID bits of files and the
// extended attributes outside the plain `user.` namespace (`security.*`,
// `trusted.*`, `system.*`: file capabilities, SELinux labels, POSIX ACLs); each
// file with withheld extended attributes yields one message naming them. Windows
// restores its attributes, security descriptor and times as before.
std::vector<std::string> apply_metadata(const std::filesystem::path& path,
                                        const FileMetadata& meta,
                                        bool restore_times,
                                        bool restore_privileged = false);

class ExclusiveFile;

// The extra access a staging file needs for finish_staged_file(), to pass to
// StagedFile; 0 where finishing through the handle does not apply. Ownership
// can only be restored through a handle that holds WRITE_OWNER, which creating
// the file may refuse.
std::uint32_t staged_finish_access(bool with_ownership);

// Move a freshly written staging file to `target` through its still-open handle,
// then apply what apply_metadata(target, meta, restore_times) would, through the
// same handle, and return its messages. Renaming and re-permissioning by name
// reopened the file for each step, the larger part of extracting a small file.
// Returns nullopt, having changed nothing, when the handle lacks the access or
// the rename is refused; the caller then closes, renames, and applies metadata
// by path. The caller must still close the file and treat a failed close as a
// failed write. For a regular file without named streams, sparse ranges, or
// reparse data, whose metadata has nothing to wait for.
std::optional<std::vector<std::string>> finish_staged_file(ExclusiveFile& file,
                                                           const std::filesystem::path& target,
                                                           const FileMetadata& meta,
                                                           bool restore_times);

// Apply an opaque Windows reparse point after the target has been created. On
// platforms without Windows reparse controls this reports a single failure.
bool apply_reparse_point(const std::filesystem::path& path, std::uint32_t tag,
                         const std::vector<std::uint8_t>& data,
                         std::string* error = nullptr);

}  // namespace axiom::core
