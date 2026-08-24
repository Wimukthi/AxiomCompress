#pragma once

#include "axiom/archive.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace axiom {

// Rewrites a completed single-file ZIP as a standards-compliant spanned ZIP
// (.z01, .z02, ..., .zip). Entries are copied raw, preserving compression,
// encryption, metadata, and CRC values without recompression.
void create_split_zip(const std::filesystem::path& source_zip,
                      const std::filesystem::path& output_zip,
                      std::uint64_t volume_size,
                      const std::shared_ptr<OperationControl>& operation = nullptr);

// Transactionally installs a complete staged volume set beside archive_path.
// The staging directory must be on the same filesystem so every member can be
// renamed atomically; an existing set is retained until installation succeeds.
void install_zip_volume_set(
    const std::filesystem::path& staged_archive,
    const std::filesystem::path& archive_path,
    const std::shared_ptr<OperationControl>& operation = nullptr);
std::uint64_t zip_volume_set_size(
    const std::filesystem::path& archive_path);

struct ZipBackendEntryInfo {
    std::string path;
    std::string comment;
    ByteVector extra;
    std::uint16_t version_made_by = 0;
    std::uint16_t version_needed = 0;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    std::uint16_t zipcrypto_verifier = 0;
    std::uint16_t aes_version = 0;
    std::uint8_t aes_strength = 0;
    std::uint32_t crc32 = 0;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::int64_t modified_time = 0;
    std::uint16_t internal_attributes = 0;
    std::uint32_t external_attributes = 0;
    bool zip64 = false;
    bool directory = false;
};

using ZipRawChunkCallback =
    std::function<void(std::span<const std::uint8_t>)>;

// One minizip-ng reader for ordinary files, standard split sets, and a bounded
// ZIP payload embedded in an SFX executable. Entry reads stay raw so Axiom's
// codec, password, validation, cancellation, and extraction-safety policy is
// identical for every container layout.
class ZipBackendReader {
public:
    explicit ZipBackendReader(
        const std::filesystem::path& archive_path,
        const std::shared_ptr<OperationControl>& operation = nullptr,
        const std::optional<std::pair<std::uint64_t, std::uint64_t>>& payload_range =
            std::nullopt);
    ~ZipBackendReader();
    ZipBackendReader(const ZipBackendReader&) = delete;
    ZipBackendReader& operator=(const ZipBackendReader&) = delete;
    ZipBackendReader(ZipBackendReader&&) noexcept;
    ZipBackendReader& operator=(ZipBackendReader&&) noexcept;

    const std::vector<ZipBackendEntryInfo>& entries() const;
    void read_raw_entry(std::size_t index, const ZipRawChunkCallback& callback);
    bool is_split() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class ZipBackendWriter;
};

struct ZipBackendWriteInfo {
    std::string path;
    std::string comment;
    ByteVector extra;
    std::uint16_t version_made_by = 0;
    std::uint16_t version_needed = 0;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    std::uint16_t internal_attributes = 0;
    std::uint32_t external_attributes = 0;
    std::uint64_t uncompressed_size = 0;
    std::int64_t modified_time = 0;
    bool directory = false;
};

// A raw minizip-ng writer. Compression and WinZip-AES transformation are fed by
// the Axiom layer, while minizip-ng owns headers, ZIP64, central directories,
// raw cloning, and disk transitions for both ordinary and split outputs.
class ZipBackendWriter {
public:
    explicit ZipBackendWriter(
        const std::filesystem::path& archive_path,
        std::uint64_t volume_size = 0,
        const std::shared_ptr<OperationControl>& operation = nullptr);
    ~ZipBackendWriter();
    ZipBackendWriter(const ZipBackendWriter&) = delete;
    ZipBackendWriter& operator=(const ZipBackendWriter&) = delete;
    ZipBackendWriter(ZipBackendWriter&&) noexcept;
    ZipBackendWriter& operator=(ZipBackendWriter&&) noexcept;

    void copy_entry(ZipBackendReader& source, std::size_t index,
                    const std::optional<std::string>& renamed_path = std::nullopt);
    void begin_raw_entry(const ZipBackendWriteInfo& info);
    void write_raw(std::span<const std::uint8_t> bytes);
    void finish_raw_entry(std::uint32_t crc32, std::uint64_t uncompressed_size);
    void finalize();
    std::uint64_t size() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace axiom
