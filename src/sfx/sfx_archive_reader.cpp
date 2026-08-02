#include "sfx/sfx_archive_reader.hpp"

#include "archive/container_internal.hpp"
#include "archive/sfx_image.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

namespace axiom::sfx {
namespace {

constexpr std::array<std::uint8_t, 4> kZipLocalSignature = {'P', 'K', 0x03, 0x04};
constexpr std::array<std::uint8_t, 4> kZipEmptySignature = {'P', 'K', 0x05, 0x06};
constexpr std::array<std::uint8_t, 4> kZipSplitSignature = {'P', 'K', 0x07, 0x08};

bool has_prefix(std::span<const std::uint8_t> bytes,
                std::span<const std::uint8_t> expected) {
    return bytes.size() >= expected.size() &&
           std::equal(expected.begin(), expected.end(), bytes.begin());
}

std::optional<SfxArchiveFormat> detect_payload_format(
    const std::filesystem::path& executable, const SfxPayload& payload) {
    if (payload.payload_size < 4) return std::nullopt;

    std::ifstream stream(executable, std::ios::binary);
    if (!stream) return std::nullopt;
    if (payload.payload_offset >
        static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return std::nullopt;
    }
    stream.seekg(static_cast<std::streamoff>(payload.payload_offset), std::ios::beg);
    if (!stream) return std::nullopt;

    std::array<std::uint8_t, 8> prefix{};
    const auto count = static_cast<std::streamsize>(
        std::min<std::uint64_t>(payload.payload_size, prefix.size()));
    stream.read(reinterpret_cast<char*>(prefix.data()), count);
    if (stream.gcount() != count) return std::nullopt;

    if (payload.payload_size >= kArchiveMagic.size() &&
        has_prefix(std::span<const std::uint8_t>(prefix.data(),
                                                  static_cast<std::size_t>(count)),
                   std::span<const std::uint8_t>(kArchiveMagic))) {
        return SfxArchiveFormat::axar;
    }
    const auto bytes = std::span<const std::uint8_t>(
        prefix.data(), static_cast<std::size_t>(count));
    if (has_prefix(bytes, kZipLocalSignature) ||
        has_prefix(bytes, kZipEmptySignature) ||
        has_prefix(bytes, kZipSplitSignature)) {
        return SfxArchiveFormat::zip;
    }
    return std::nullopt;
}

ArchiveCapabilities axar_capabilities(const std::filesystem::path& executable,
                                       const std::string& password) {
    ArchiveCapabilities result;
    result.list = true;
    result.extract = true;
    result.test = true;
    result.packed_sizes = true;
    result.selective_extract = true;
    result.metadata = true;
    result.sparse_files = true;
    result.capture_warnings = true;
    result.links = true;
    result.authenticity = true;
    result.lock = true;

    const auto encryption_mode = archive_encryption_mode(executable);
    result.encrypted = encryption_mode != ArchiveEncryptionMode::none;
    result.directory_encrypted =
        encryption_mode == ArchiveEncryptionMode::data_and_directory;
    result.locked = archive_is_locked(executable, password);
    return result;
}

}  // namespace

std::optional<SfxArchiveReader> SfxArchiveReader::open(
    const std::filesystem::path& executable) {
    const auto payload = sfx_locate_payload(executable);
    if (!payload) return std::nullopt;
    const auto format = detect_payload_format(executable, *payload);
    if (!format) return std::nullopt;
    return SfxArchiveReader(executable, *format, payload->payload_offset,
                           payload->payload_size);
}

ArchiveCapabilities SfxArchiveReader::capabilities(
    const std::string& password) const {
    if (native()) return axar_capabilities(executable_, password);
    return sfx_zip_capabilities(executable_, password,
                                std::make_pair(payload_offset_, payload_size_));
}

std::vector<ArchiveEntry> SfxArchiveReader::list(
    const std::string& password) const {
    if (native()) return list_archive(executable_, password);
    return sfx_zip_list(executable_, password,
                        std::make_pair(payload_offset_, payload_size_));
}

void SfxArchiveReader::test(const DecompressionOptions& options) const {
    if (native()) {
        test_archive(executable_, options);
    } else {
        sfx_zip_test(executable_, options,
                     std::make_pair(payload_offset_, payload_size_));
    }
}

void SfxArchiveReader::extract_all(const std::filesystem::path& destination,
                                   const ExtractOptions& options) const {
    if (native()) {
        extract_archive(executable_, destination, options);
    } else {
        sfx_zip_extract_all(executable_, destination, options,
                            std::make_pair(payload_offset_, payload_size_));
    }
}

void SfxArchiveReader::extract_selected(
    const std::vector<std::string>& entries,
    const std::filesystem::path& destination,
    const ExtractOptions& options) const {
    if (native()) {
        extract_entries(executable_, entries, destination, options);
    } else {
        sfx_zip_extract_selected(executable_, entries, destination, options,
                                 std::make_pair(payload_offset_, payload_size_));
    }
}

ArchiveSignatureInfo SfxArchiveReader::signature(
    const std::string& password) const {
    if (!native()) return {};
    return verify_archive_signature(executable_, password);
}

std::string SfxArchiveReader::comment(const std::string& password) const {
    if (!native()) return {};
    return archive_comment(executable_, password);
}

}  // namespace axiom::sfx
