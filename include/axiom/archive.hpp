#pragma once

#include "axiom/axiom.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace axiom {

// One entry in a multi-file archive's central directory.
struct ArchiveEntry {
    std::string path;            // relative, '/'-separated, UTF-8
    bool is_directory = false;
    bool is_symlink = false;     // a symbolic link; symlink_target is its target
    std::string symlink_target;  // link target, verbatim (empty unless is_symlink)
    std::uint64_t size = 0;      // uncompressed bytes (0 for directories/symlinks)
    std::int64_t mtime = 0;      // seconds since Unix epoch
    std::uint32_t crc32 = 0;     // CRC-32 of file bytes (0 for directories/symlinks)
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
};

// Build a `.axar` archive from the given files/directories (directories are
// scanned recursively). Archive paths are stored relative to each input's
// parent, so adding "dir" yields entries "dir/...". Written atomically.
void create_archive(const std::vector<std::filesystem::path>& inputs,
                    const std::filesystem::path& archive_path,
                    const CompressionOptions& options = {});

// Read the central directory without decompressing any block.
std::vector<ArchiveEntry> list_archive(const std::filesystem::path& archive_path);

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

}  // namespace axiom
