#pragma once

#include "axiom/archive.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
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

struct SplitZipEntryInfo {
    std::string path;
    std::string comment;
    ByteVector extra;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    std::uint16_t zipcrypto_verifier = 0;
    std::uint16_t aes_version = 0;
    std::uint16_t aes_actual_method = 0;
    std::uint8_t aes_strength = 0;
    std::uint32_t crc32 = 0;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::int64_t modified_time = 0;
    bool directory = false;
};

// Direct, read-only access to a standard .z01/.z02/.../.zip set. The bundled
// minizip split stream changes disks on demand; no complete temporary ZIP is
// created. Raw entry reads deliberately retain encryption and compression so
// Axiom's existing validation/decryption pipeline remains authoritative.
class SplitZipReader {
public:
    explicit SplitZipReader(
        const std::filesystem::path& final_zip,
        const std::shared_ptr<OperationControl>& operation = nullptr);
    ~SplitZipReader();
    SplitZipReader(const SplitZipReader&) = delete;
    SplitZipReader& operator=(const SplitZipReader&) = delete;
    SplitZipReader(SplitZipReader&&) noexcept;
    SplitZipReader& operator=(SplitZipReader&&) noexcept;

    const std::vector<SplitZipEntryInfo>& entries() const;
    ByteVector read_raw_entry(std::size_t index);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace axiom
