#pragma once

#include "axiom/axiom.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace axiom {

// One entry in a multi-file archive's central directory.
struct ArchiveEntry {
    std::string path;            // relative, '/'-separated, UTF-8
    bool is_directory = false;
    bool is_symlink = false;     // a symbolic link; link_target is its target
    bool is_hardlink = false;    // a hard link; link_target is the shared file's path
    std::string link_target;     // symlink target or hardlink path (empty otherwise)
    std::uint64_t size = 0;      // uncompressed bytes (0 for directories/links)
    std::int64_t mtime = 0;      // seconds since Unix epoch
    std::uint32_t crc32 = 0;     // CRC-32 of file bytes (0 for directories/links)
    bool has_blake3 = false;     // whether blake3 below is populated
    std::array<std::uint8_t, 32> blake3{};  // BLAKE3-256 of file bytes
};

struct ExtractOptions {
    enum class Overwrite {
        fail,       // error if a target already exists
        skip,       // leave existing targets untouched
        overwrite,  // replace existing targets
    };

    Overwrite overwrite = Overwrite::fail;
    bool restore_mtime = true;
    std::size_t thread_count = 0;
    std::shared_ptr<OperationControl> operation;
    // Password for an encrypted archive; required to read one, ignored otherwise.
    std::string password;
};

// A filesystem object mapped to an explicit path in an archive. For a directory,
// descendants are stored beneath destination_path. Archive paths are relative,
// UTF-8, and '/'-separated.
struct ArchiveInput {
    std::filesystem::path source;
    std::string destination_path;
};

struct ArchiveMove {
    std::string source_path;
    std::string destination_path;
};

enum class ArchiveEncryptionMode {
    none,
    data_only,
    data_and_directory,
};

struct ArchiveSigningKey {
    std::array<std::uint8_t, 64> secret_key{};
    std::array<std::uint8_t, 32> public_key{};
};

struct ArchiveSignatureInfo {
    bool present = false;
    bool valid = false;
    bool trusted_key = false;
    std::array<std::uint8_t, 32> public_key{};
};

ArchiveSigningKey generate_archive_signing_key();
void sign_archive(const std::filesystem::path& archive_path,
                  const ArchiveSigningKey& key,
                  const CompressionOptions& options = {});
ArchiveSignatureInfo verify_archive_signature(
    const std::filesystem::path& archive_path,
    const std::string& password = {},
    const std::optional<std::array<std::uint8_t, 32>>& trusted_key = std::nullopt);

// Create a Windows self-extracting executable by appending an intact archive and
// an Axiom SFX trailer to a native Axiom GUI stub.
void create_sfx_archive(const std::filesystem::path& archive_path,
                        const std::filesystem::path& stub_executable,
                        const std::filesystem::path& output_executable,
                        const std::shared_ptr<OperationControl>& operation = nullptr);

// Build a `.axar` archive from the given files/directories (directories are
// scanned recursively). Archive paths are stored relative to each input's
// parent, so adding "dir" yields entries "dir/...". Written atomically.
void create_archive(const std::vector<std::filesystem::path>& inputs,
                    const std::filesystem::path& archive_path,
                    const CompressionOptions& options = {});

// Add files/directories to an existing archive. Existing files are not
// recompressed — their solid blocks are copied verbatim and new files become new
// blocks appended after them, then the directory is rebuilt. An added path that
// matches an existing entry replaces it (the old data remains as dead space until a
// future repack). Written atomically.
void add_to_archive(const std::vector<std::filesystem::path>& inputs,
                    const std::filesystem::path& archive_path,
                    const CompressionOptions& options = {});

// Add files/directories at caller-selected archive paths. This is the path-aware
// form used by file-manager drag/drop into an archive subdirectory.
void add_to_archive(const std::vector<ArchiveInput>& inputs,
                    const std::filesystem::path& archive_path,
                    const CompressionOptions& options = {});

// Remove entries from an archive. A directory path removes its whole subtree. The
// archive is rebuilt keeping only the surviving entries, so the removed data's space
// is reclaimed (live files are re-solidified into fresh blocks). A hard link whose
// target is removed is dropped. Written atomically.
void delete_from_archive(const std::filesystem::path& archive_path,
                         const std::vector<std::string>& paths,
                         const CompressionOptions& options = {});

// Rebuild an archive in place, reclaiming dead space left by earlier replace/delete
// operations and re-solidifying the surviving files. Content is unchanged.
void repack_archive(const std::filesystem::path& archive_path,
                    const CompressionOptions& options = {});

// Refresh an archive from the inputs by modification time. A file is written only
// when it is newer than the archived copy; with fresh_only=false (`update`) brand-new
// files are also added, while fresh_only=true (`fresh`) touches only files already in
// the archive. If the archive is missing, `update` creates it and `fresh` does
// nothing. Existing unchanged files are not recompressed.
void update_archive(const std::vector<std::filesystem::path>& inputs,
                    const std::filesystem::path& archive_path,
                    const CompressionOptions& options = {}, bool fresh_only = false);

// Mirror the inputs into the archive: add new and newer files (by mtime), then
// remove any archived entry no longer present in the inputs. Written atomically.
void sync_archive(const std::vector<std::filesystem::path>& inputs,
                  const std::filesystem::path& archive_path,
                  const CompressionOptions& options = {});

// Rename or move entries without recompressing file data. Moving a directory moves
// its complete subtree and updates internal hard-link targets.
void move_archive_entries(const std::filesystem::path& archive_path,
                          const std::vector<ArchiveMove>& moves,
                          const CompressionOptions& options = {});

// Set (or clear, with an empty string) the archive's free-form comment. Refused if
// the archive is locked. Rewrites the directory only; block data is untouched.
void set_archive_comment(const std::filesystem::path& archive_path, const std::string& comment,
                         const CompressionOptions& options = {});

// Read the archive's comment ("" if none). A password is required only for an archive
// with an encrypted directory.
std::string archive_comment(const std::filesystem::path& archive_path,
                            const std::string& password = {});

// Mark the archive read-only. Once locked, every edit operation (add/update/sync/
// delete/repack/comment/lock) refuses with an error. There is no unlock.
void lock_archive(const std::filesystem::path& archive_path,
                  const CompressionOptions& options = {});

// Whether the archive is locked (read-only). A password is required only for an
// archive with an encrypted directory.
bool archive_is_locked(const std::filesystem::path& archive_path,
                       const std::string& password = {});

// Whether the archive is password-encrypted (block contents, and possibly the
// directory). Never needs a password itself.
bool archive_is_encrypted(const std::filesystem::path& archive_path);

// Distinguish editable block-only encryption from encrypted-directory mode, which
// currently requires a password even to list and does not support editing.
ArchiveEncryptionMode archive_encryption_mode(const std::filesystem::path& archive_path);

// Read the central directory without decompressing any block. A password is required
// only when the directory itself is encrypted (otherwise names are listable freely).
std::vector<ArchiveEntry> list_archive(const std::filesystem::path& archive_path,
                                       const std::string& password = {});

// Verify structure, per-block checksums, and per-file CRCs. Throws FormatError
// describing the first problem found; returns normally if the archive is intact.
void test_archive(const std::filesystem::path& archive_path,
                  const DecompressionOptions& options = {});

// Extract every entry beneath dest_dir. Paths are contained to dest_dir
// (archive entries that would escape it are rejected). Files are written
// atomically.
void extract_archive(const std::filesystem::path& archive_path,
                     const std::filesystem::path& dest_dir,
                     const ExtractOptions& options = {});

// Extract only the named entries. Selecting a directory includes its subtree;
// overlapping selections are deduplicated.
void extract_entries(const std::filesystem::path& archive_path,
                     const std::vector<std::string>& entries,
                     const std::filesystem::path& dest_dir,
                     const ExtractOptions& options = {});

}  // namespace axiom
