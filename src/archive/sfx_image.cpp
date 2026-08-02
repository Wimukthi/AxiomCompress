#include "archive/sfx_image.hpp"

#include "core/checksum.hpp"

#include <algorithm>
#include <fstream>
#include <istream>
#include <limits>
#include <stdexcept>

namespace axiom {
namespace {

// PE structural constants, named so the offsets below read as more than magic.
constexpr std::uint64_t kDosLfanewOffset = 0x3C;
constexpr std::uint64_t kMinDosHeaderSize = 0x40;
constexpr std::uint64_t kCoffHeaderSize = 20;
constexpr std::uint64_t kPeSignatureSize = 4;
constexpr std::uint16_t kOptionalMagicPe32 = 0x10B;
constexpr std::uint16_t kOptionalMagicPe32Plus = 0x20B;
constexpr std::uint64_t kDataDirectoriesOffsetPe32 = 96;
constexpr std::uint64_t kDataDirectoriesOffsetPe32Plus = 112;
constexpr std::uint64_t kSecurityDirectoryIndex = 4;
constexpr std::uint64_t kDataDirectoryEntrySize = 8;
constexpr std::uint64_t kSectionHeaderSize = 40;
constexpr std::uint64_t kSectionRawSizeOffset = 16;
constexpr std::uint64_t kSectionRawPointerOffset = 20;

std::uint16_t load_u16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t load_u32(const std::uint8_t* bytes) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
    }
    return value;
}

std::uint64_t load_u64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    }
    return value;
}

void store_u16(std::uint8_t* bytes, std::uint16_t value) {
    bytes[0] = static_cast<std::uint8_t>(value & 0xFF);
    bytes[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

void store_u32(std::uint8_t* bytes, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        bytes[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFF);
    }
}

void store_u64(std::uint8_t* bytes, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        bytes[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFF);
    }
}

std::optional<std::uint64_t> stream_size(std::istream& stream) {
    stream.clear();
    if (!stream.seekg(0, std::ios::end)) return std::nullopt;
    const auto end = stream.tellg();
    if (end < 0) return std::nullopt;
    return static_cast<std::uint64_t>(end);
}

// Reads exactly `count` bytes at `offset`, or fails. Every PE field below goes
// through this so a truncated or hostile image cannot walk off the end.
bool read_at(std::istream& stream, std::uint64_t offset, std::uint8_t* out,
             std::size_t count) {
    stream.clear();
    if (offset > static_cast<std::uint64_t>(
                     std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    if (!stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg)) {
        return false;
    }
    stream.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(count));
    return stream.gcount() == static_cast<std::streamsize>(count);
}

// Offset of the PE header, validated against the file size.
std::optional<std::uint64_t> pe_header_offset(std::istream& stream,
                                              std::uint64_t file_size) {
    if (file_size < kMinDosHeaderSize) return std::nullopt;
    std::array<std::uint8_t, 2> dos{};
    if (!read_at(stream, 0, dos.data(), dos.size())) return std::nullopt;
    if (dos[0] != 'M' || dos[1] != 'Z') return std::nullopt;

    std::array<std::uint8_t, 4> lfanew{};
    if (!read_at(stream, kDosLfanewOffset, lfanew.data(), lfanew.size())) {
        return std::nullopt;
    }
    const std::uint64_t offset = load_u32(lfanew.data());
    if (offset < kMinDosHeaderSize) return std::nullopt;
    if (offset > file_size ||
        file_size - offset < kPeSignatureSize + kCoffHeaderSize) {
        return std::nullopt;
    }

    std::array<std::uint8_t, 4> signature{};
    if (!read_at(stream, offset, signature.data(), signature.size())) {
        return std::nullopt;
    }
    if (signature[0] != 'P' || signature[1] != 'E' || signature[2] != 0 ||
        signature[3] != 0) {
        return std::nullopt;
    }
    return offset;
}

struct CoffInfo {
    std::uint16_t section_count = 0;
    std::uint16_t optional_header_size = 0;
    std::uint64_t optional_header_offset = 0;
};

std::optional<CoffInfo> read_coff(std::istream& stream, std::uint64_t pe_offset) {
    std::array<std::uint8_t, kCoffHeaderSize> coff{};
    if (!read_at(stream, pe_offset + kPeSignatureSize, coff.data(), coff.size())) {
        return std::nullopt;
    }
    CoffInfo info;
    info.section_count = load_u16(coff.data() + 2);
    info.optional_header_size = load_u16(coff.data() + 16);
    info.optional_header_offset = pe_offset + kPeSignatureSize + kCoffHeaderSize;
    if (info.section_count == 0 || info.section_count > kSfxMaxSections) {
        return std::nullopt;
    }
    return info;
}

bool valid_optional_header(std::istream& stream, const CoffInfo& coff) {
    if (coff.optional_header_size < sizeof(std::uint16_t)) return false;
    std::array<std::uint8_t, 2> magic{};
    if (!read_at(stream, coff.optional_header_offset, magic.data(), magic.size())) {
        return false;
    }
    const auto value = load_u16(magic.data());
    return value == kOptionalMagicPe32 || value == kOptionalMagicPe32Plus;
}

}  // namespace

std::optional<std::uint64_t> pe_image_end(std::istream& stream) {
    const auto file_size = stream_size(stream);
    if (!file_size) return std::nullopt;
    const auto pe_offset = pe_header_offset(stream, *file_size);
    if (!pe_offset) return std::nullopt;
    const auto coff = read_coff(stream, *pe_offset);
    if (!coff) return std::nullopt;
    if (!valid_optional_header(stream, *coff)) return std::nullopt;

    const std::uint64_t table =
        coff->optional_header_offset + coff->optional_header_size;
    const std::uint64_t table_bytes =
        static_cast<std::uint64_t>(coff->section_count) * kSectionHeaderSize;
    if (table > *file_size || *file_size - table < table_bytes) {
        return std::nullopt;
    }

    const std::uint64_t headers_end = table + table_bytes;
    std::uint64_t image_end = headers_end;
    bool has_raw_section = false;
    for (std::uint16_t index = 0; index < coff->section_count; ++index) {
        std::array<std::uint8_t, kSectionHeaderSize> section{};
        if (!read_at(stream, table + index * kSectionHeaderSize, section.data(),
                     section.size())) {
            return std::nullopt;
        }
        const std::uint64_t raw_size =
            load_u32(section.data() + kSectionRawSizeOffset);
        const std::uint64_t raw_pointer =
            load_u32(section.data() + kSectionRawPointerOffset);
        // Sections holding only uninitialized data occupy no file range. A
        // nonzero raw size with a null pointer is malformed, not BSS.
        if (raw_size == 0) continue;
        if (raw_pointer == 0) return std::nullopt;
        if (raw_pointer < headers_end) return std::nullopt;
        const std::uint64_t end = raw_pointer + raw_size;
        if (end > *file_size) return std::nullopt;
        has_raw_section = true;
        image_end = std::max(image_end, end);
    }
    if (!has_raw_section) return std::nullopt;
    return image_end;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> pe_certificate_table(
    std::istream& stream) {
    const auto file_size = stream_size(stream);
    if (!file_size) return std::nullopt;
    const auto pe_offset = pe_header_offset(stream, *file_size);
    if (!pe_offset) return std::nullopt;
    const auto coff = read_coff(stream, *pe_offset);
    if (!coff) return std::nullopt;

    std::array<std::uint8_t, 2> magic{};
    if (!read_at(stream, coff->optional_header_offset, magic.data(), magic.size())) {
        return std::nullopt;
    }
    const std::uint16_t optional_magic = load_u16(magic.data());
    std::uint64_t directories_offset = 0;
    if (optional_magic == kOptionalMagicPe32) {
        directories_offset = kDataDirectoriesOffsetPe32;
    } else if (optional_magic == kOptionalMagicPe32Plus) {
        directories_offset = kDataDirectoriesOffsetPe32Plus;
    } else {
        return std::nullopt;
    }

    const std::uint64_t entry = coff->optional_header_offset + directories_offset +
                                kSecurityDirectoryIndex * kDataDirectoryEntrySize;
    // The directory must lie inside the declared optional header.
    if (directories_offset +
            (kSecurityDirectoryIndex + 1) * kDataDirectoryEntrySize >
        coff->optional_header_size) {
        return std::nullopt;
    }
    std::array<std::uint8_t, kDataDirectoryEntrySize> security{};
    if (!read_at(stream, entry, security.data(), security.size())) {
        return std::nullopt;
    }
    const std::uint64_t offset = load_u32(security.data());
    const std::uint64_t size = load_u32(security.data() + 4);
    if (offset == 0 || size == 0) return std::nullopt;
    if (offset > *file_size || *file_size - offset < size) return std::nullopt;
    return std::make_pair(offset, size);
}

std::array<std::uint8_t, kSfxDescriptorSize> sfx_encode_descriptor(
    std::uint64_t image_end, const SfxPayload& payload) {
    if (image_end > std::numeric_limits<std::uint64_t>::max() -
                        kSfxDescriptorSize) {
        throw std::invalid_argument("SFX image end overflows the file range");
    }
    const auto descriptor_end = image_end + kSfxDescriptorSize;
    if (descriptor_end < image_end || payload.flags != 0 ||
        payload.payload_size == 0 ||
        payload.payload_offset < descriptor_end) {
        throw std::invalid_argument("invalid SFX payload descriptor geometry");
    }
    if (payload.config_size > kSfxMaxConfigSize) {
        throw std::invalid_argument("SFX configuration is too large");
    }
    if (payload.payload_offset >
        std::numeric_limits<std::uint64_t>::max() - payload.payload_size) {
        throw std::invalid_argument("SFX payload size overflows the file range");
    }
    if (payload.config_size != 0 &&
        (payload.config_offset < descriptor_end ||
         payload.config_offset > payload.payload_offset ||
         payload.config_size > payload.payload_offset - payload.config_offset)) {
        throw std::invalid_argument("invalid SFX configuration geometry");
    }
    std::array<std::uint8_t, kSfxDescriptorSize> descriptor{};
    std::copy(kSfxDescriptorMagic.begin(), kSfxDescriptorMagic.end(),
              descriptor.begin());
    store_u16(descriptor.data() + 8, kSfxFormatVersion);
    store_u16(descriptor.data() + 10,
              static_cast<std::uint16_t>(kSfxDescriptorSize));
    store_u32(descriptor.data() + 12, payload.flags);
    store_u64(descriptor.data() + 16,
              payload.config_size == 0 ? 0 : payload.config_offset - image_end);
    store_u64(descriptor.data() + 24, payload.config_size);
    store_u64(descriptor.data() + 32, payload.payload_offset - image_end);
    store_u64(descriptor.data() + 40, payload.payload_size);
    std::copy(payload.payload_hash.begin(), payload.payload_hash.end(),
              descriptor.begin() + 48);
    store_u32(descriptor.data() + 56, 0);  // reserved
    const std::uint32_t crc = core::crc32(
        std::span<const std::uint8_t>(descriptor.data(), kSfxDescriptorSize - 4));
    store_u32(descriptor.data() + 60, crc);
    return descriptor;
}

namespace {

// Reads a v2 descriptor at `image_end`. `logical_end` excludes any certificate
// table, so a signed executable range-checks against its real payload extent.
std::optional<SfxPayload> read_v2(std::istream& stream, std::uint64_t image_end,
                                  std::uint64_t logical_end) {
    if (logical_end < image_end ||
        logical_end - image_end < kSfxDescriptorSize) {
        return std::nullopt;
    }
    std::array<std::uint8_t, kSfxDescriptorSize> descriptor{};
    if (!read_at(stream, image_end, descriptor.data(), descriptor.size())) {
        return std::nullopt;
    }
    if (!std::equal(kSfxDescriptorMagic.begin(), kSfxDescriptorMagic.end(),
                    descriptor.begin())) {
        return std::nullopt;
    }
    const std::uint32_t stored_crc = load_u32(descriptor.data() + 60);
    const std::uint32_t actual_crc = core::crc32(
        std::span<const std::uint8_t>(descriptor.data(), kSfxDescriptorSize - 4));
    if (stored_crc != actual_crc) return std::nullopt;

    const std::uint16_t version = load_u16(descriptor.data() + 8);
    const std::uint16_t header_size = load_u16(descriptor.data() + 10);
    // Reject a newer major layout rather than guessing at its meaning.
    if (version != kSfxFormatVersion || header_size != kSfxDescriptorSize ||
        load_u32(descriptor.data() + 56) != 0 ||
        load_u32(descriptor.data() + 12) != 0) {
        return std::nullopt;
    }

    SfxPayload payload;
    payload.format = SfxFormat::v2;
    payload.flags = load_u32(descriptor.data() + 12);
    const std::uint64_t config_relative = load_u64(descriptor.data() + 16);
    payload.config_size = load_u64(descriptor.data() + 24);
    const std::uint64_t payload_relative = load_u64(descriptor.data() + 32);
    payload.payload_size = load_u64(descriptor.data() + 40);
    std::copy(descriptor.begin() + 48, descriptor.begin() + 56,
              payload.payload_hash.begin());

    const std::uint64_t available = logical_end - image_end;
    if (payload.payload_size == 0 || payload_relative < kSfxDescriptorSize ||
        payload_relative > available ||
        available - payload_relative < payload.payload_size) {
        return std::nullopt;
    }
    if (payload.config_size > kSfxMaxConfigSize) return std::nullopt;
    if (payload.config_size != 0) {
        if (config_relative < kSfxDescriptorSize ||
            config_relative > payload_relative ||
            available - config_relative < payload.config_size ||
            payload.config_size > payload_relative - config_relative) {
            return std::nullopt;
        }
        payload.config_offset = image_end + config_relative;
    } else if (config_relative != 0) {
        return std::nullopt;
    }
    payload.payload_offset = image_end + payload_relative;
    return payload;
}

std::optional<SfxPayload> read_legacy(std::istream& stream,
                                      std::uint64_t file_size) {
    if (file_size < kSfxLegacyTrailerSize + 1) return std::nullopt;
    std::array<std::uint8_t, kSfxLegacyTrailerSize> trailer{};
    if (!read_at(stream, file_size - kSfxLegacyTrailerSize, trailer.data(),
                 trailer.size())) {
        return std::nullopt;
    }
    if (!std::equal(kSfxLegacyMagic.begin(), kSfxLegacyMagic.end(),
                    trailer.begin())) {
        return std::nullopt;
    }
    const std::uint64_t size = load_u64(trailer.data() + 8);
    if (size == 0 || size > file_size - kSfxLegacyTrailerSize) {
        return std::nullopt;
    }
    SfxPayload payload;
    payload.format = SfxFormat::legacy;
    payload.payload_offset = file_size - kSfxLegacyTrailerSize - size;
    payload.payload_size = size;
    return payload;
}

}  // namespace

std::optional<SfxPayload> sfx_locate_payload(std::istream& stream) {
    const auto file_size = stream_size(stream);
    if (!file_size) return std::nullopt;

    if (const auto image_end = pe_image_end(stream)) {
        // A certificate table is not part of the payload region.
        std::uint64_t logical_end = *file_size;
        if (const auto certificate = pe_certificate_table(stream)) {
            logical_end = std::min(logical_end, certificate->first);
        }
        if (auto payload = read_v2(stream, *image_end, logical_end)) {
            return payload;
        }
    }
    return read_legacy(stream, *file_size);
}

std::optional<SfxPayload> sfx_locate_payload(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    return sfx_locate_payload(stream);
}

}  // namespace axiom
