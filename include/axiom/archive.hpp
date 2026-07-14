#pragma once

#include "axiom/axiom.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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
    std::optional<std::uint64_t> packed_size;  // compressed bytes when the format exposes it
    bool packed_size_estimated = false;  // true for proportional solid-block estimates
    std::int64_t mtime = 0;      // seconds since Unix epoch
    std::uint32_t crc32 = 0;     // CRC-32 of file bytes when has_crc32 is true
    bool has_crc32 = false;      // false when the format/provider does not expose CRC-32
    bool has_blake3 = false;     // whether blake3 below is populated
    std::array<std::uint8_t, 32> blake3{};  // BLAKE3-256 of file bytes
};

enum class ArchiveFormat {
    axar,
    zip,
    seven_z,
    rar,
    tar,
    iso,
    cab,
};

enum class EstimateConfidence {
    low,
    medium,
    high,
};

struct CompressionEstimateSnapshot {
    std::uint64_t source_bytes = 0;
    std::uint64_t sampled_bytes = 0;
    std::uint64_t planned_sample_bytes = 0;
    std::uint64_t estimated_archive_bytes = 0;
    std::uint64_t completed_probes = 0;
    std::uint64_t total_probes = 0;
    double estimated_ratio = 0.0;
    double estimated_savings_percent = 0.0;
};

// A bounded, read-only compression prognosis. The estimator samples source data
// with the real codec and projects the result; it never creates an archive.
struct CompressionEstimateOptions {
    ArchiveFormat format = ArchiveFormat::axar;
    CompressionOptions compression;
    // Hard ceiling for adaptive sampling. The estimator may stop earlier once
    // representative batches converge or the time budget expires.
    std::size_t sample_budget = 8u << 20;
    std::size_t sample_chunk_size = 512u << 10;
    std::chrono::milliseconds time_budget{5000};
    std::uint64_t volume_size = 0;
    // Called after each completed probe. It reports the cumulative prognosis,
    // not operation completion; frontends can render a live savings meter.
    std::function<void(const CompressionEstimateSnapshot&)> progress_callback;
};

struct CompressionEstimateResult {
    ArchiveFormat format = ArchiveFormat::axar;
    EstimateConfidence confidence = EstimateConfidence::low;
    std::uint64_t source_bytes = 0;
    std::uint64_t sampled_bytes = 0;
    std::uint64_t estimated_archive_bytes = 0;
    std::uint64_t estimated_low_bytes = 0;
    std::uint64_t estimated_high_bytes = 0;
    std::uint64_t estimated_seconds = 0;
    std::uint64_t file_count = 0;
    std::uint64_t item_count = 0;
    std::uint64_t volume_count = 0;
    std::uint64_t final_volume_bytes = 0;
    double estimated_ratio = 0.0;
    double estimated_savings_percent = 0.0;
    // 95% half-width in percentage points for the sampled payload ratio.
    double confidence_margin_percent = 0.0;
    double sample_coverage = 0.0;
    std::vector<OperationWarning> warnings;
};

struct ArchiveFormatInfo {
    ArchiveFormat format = ArchiveFormat::axar;
    std::string_view id;
    std::string_view display_name;
    std::string_view file_type_name;
    std::string_view default_extension;
    std::wstring_view open_filter_name;
    std::wstring_view open_filter_pattern;
    bool native = false;
};

struct ArchiveCapabilities {
    bool list = false;
    bool extract = false;
    bool test = false;
    bool create = false;
    bool packed_sizes = false;
    bool selective_extract = false;
    bool update = false;
    bool delete_entries = false;
    bool move_entries = false;
    bool encryption = false;
    bool recovery_records = false;
    bool multi_volume = false;
    bool comments = false;
    bool lock = false;
    bool metadata = false;
    bool links = false;
    bool authenticity = false;
    bool sfx = false;
    bool locked = false;
    bool encrypted = false;
    bool directory_encrypted = false;
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
    std::size_t io_buffer_size = 0;  // 0 = automatic.
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

class ArchiveProvider {
public:
    virtual ~ArchiveProvider() = default;

    virtual const ArchiveFormatInfo& info() const = 0;
    virtual bool matches_path(const std::filesystem::path& path) const = 0;
    // Must not throw: callers query capabilities to enable UI commands, and the
    // path may name an archive that does not exist yet (about to be created) or
    // an unreadable file. Implementations fall back to the format's static
    // capabilities and leave archive-state flags (encrypted, locked, ...) at
    // their defaults when the file cannot be probed.
    virtual ArchiveCapabilities capabilities(const std::filesystem::path& archive_path,
                                             const std::string& password = {}) const = 0;
    virtual std::vector<ArchiveEntry> list(const std::filesystem::path& archive_path,
                                           const std::string& password = {}) const = 0;
    virtual void test(const std::filesystem::path& archive_path,
                      const DecompressionOptions& options = {}) const = 0;
    virtual void extract_all(const std::filesystem::path& archive_path,
                             const std::filesystem::path& dest_dir,
                             const ExtractOptions& options = {}) const = 0;
    virtual void extract_selected(const std::filesystem::path& archive_path,
                                  const std::vector<std::string>& entries,
                                  const std::filesystem::path& dest_dir,
                                  const ExtractOptions& options = {}) const = 0;
    virtual void create(const std::vector<std::filesystem::path>& inputs,
                        const std::filesystem::path& archive_path,
                        const CompressionOptions& options = {}) const = 0;
    virtual void add(const std::vector<std::filesystem::path>& inputs,
                     const std::filesystem::path& archive_path,
                     const CompressionOptions& options = {}) const = 0;
    virtual void add_mapped(const std::vector<ArchiveInput>& inputs,
                            const std::filesystem::path& archive_path,
                            const CompressionOptions& options = {}) const = 0;
    virtual void update(const std::vector<std::filesystem::path>& inputs,
                        const std::filesystem::path& archive_path,
                        const CompressionOptions& options = {},
                        bool fresh_only = false) const = 0;
    virtual void sync(const std::vector<std::filesystem::path>& inputs,
                      const std::filesystem::path& archive_path,
                      const CompressionOptions& options = {}) const = 0;
    virtual void delete_entries(const std::filesystem::path& archive_path,
                                const std::vector<std::string>& paths,
                                const CompressionOptions& options = {}) const = 0;
    virtual void move_entries(const std::filesystem::path& archive_path,
                              const std::vector<ArchiveMove>& moves,
                              const CompressionOptions& options = {}) const {
        (void)archive_path;
        (void)moves;
        (void)options;
        throw std::runtime_error("archive format does not support moving entries");
    }
};

std::span<const ArchiveFormatInfo> supported_archive_formats();
const ArchiveProvider* archive_provider_for_path(const std::filesystem::path& path);
bool is_supported_archive(const std::filesystem::path& path);
bool is_native_archive(const std::filesystem::path& path);
bool is_axiom_sfx_archive(const std::filesystem::path& path);

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

struct ArchiveRecoveryInfo {
    bool present = false;
    unsigned percent = 0;
    std::uint32_t data_shards = 0;
    std::uint32_t parity_shards = 0;
    std::uint64_t protected_size = 0;
};

ArchiveSigningKey generate_archive_signing_key();
void sign_archive(const std::filesystem::path& archive_path,
                  const ArchiveSigningKey& key,
                  const CompressionOptions& options = {});
ArchiveSignatureInfo verify_archive_signature(
    const std::filesystem::path& archive_path,
    const std::string& password = {},
    const std::optional<std::array<std::uint8_t, 32>>& trusted_key = std::nullopt);

// Inspect, add/replace, or remove (percent=0) the archive's Reed-Solomon recovery
// service. Repair writes atomically in place and returns false when no recovery
// service exists; malformed data beyond the available redundancy throws.
ArchiveRecoveryInfo archive_recovery_info(const std::filesystem::path& archive_path);
void set_archive_recovery(const std::filesystem::path& archive_path, unsigned percent,
                          const std::shared_ptr<OperationControl>& operation = nullptr);
bool repair_archive(const std::filesystem::path& archive_path,
                    const std::shared_ptr<OperationControl>& operation = nullptr);

struct ArchiveVolumeSetInfo {
    std::uint32_t data_volumes = 0;
    std::uint32_t recovery_volumes = 0;
    std::uint64_t volume_size = 0;
    std::uint64_t archive_size = 0;
};

// Split a completed archive into numbered `name.part001.axar` volumes and optional
// `name.rev001` Reed-Solomon recovery volumes. The source archive is preserved.
// Joining accepts any surviving data/recovery volume and reconstructs missing or
// corrupt data volumes when enough recovery volumes remain.
ArchiveVolumeSetInfo create_archive_volumes(
    const std::filesystem::path& archive_path, std::uint64_t volume_size,
    unsigned recovery_volume_count = 0,
    const std::shared_ptr<OperationControl>& operation = nullptr);
ArchiveVolumeSetInfo archive_volume_set_info(const std::filesystem::path& any_volume);
void join_archive_volumes(const std::filesystem::path& any_volume,
                          const std::filesystem::path& output_archive,
                          const std::shared_ptr<OperationControl>& operation = nullptr);

// Create a Windows self-extracting executable by appending an intact archive and
// an Axiom SFX trailer to a native Axiom GUI stub.
void create_sfx_archive(const std::filesystem::path& archive_path,
                        const std::filesystem::path& stub_executable,
                        const std::filesystem::path& output_executable,
                        const std::shared_ptr<OperationControl>& operation = nullptr,
                        std::size_t io_buffer_size = 0);

// Build a `.axar` archive from the given files/directories (directories are
// scanned recursively). Archive paths are stored relative to each input's
// parent, so adding "dir" yields entries "dir/...". Written atomically.
void create_archive(const std::vector<std::filesystem::path>& inputs,
                    const std::filesystem::path& archive_path,
                    const CompressionOptions& options = {});

CompressionEstimateResult estimate_compression(
    const std::vector<std::filesystem::path>& inputs,
    const CompressionEstimateOptions& options = {});

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
