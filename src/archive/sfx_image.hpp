#pragma once

// Locating the payload embedded in an Axiom self-extracting executable.
//
// The v2 layout anchors on the end of the PE image rather than on the end of
// the file:
//
//     [PE stub image][descriptor][config][archive payload][certificate table?]
//                    ^ image_end                          ^ added by signtool
//
// Anchoring on image_end is what makes an Axiom SFX signable. Authenticode
// stores its certificate as trailing bytes at the physical end of the file, so
// a trailer read from EOF (the v1 layout, still supported below) is displaced
// the moment the executable is signed. Nothing in the v2 read path depends on
// the file's total length.
//
// Shared by the archive engine and by the SFX runtime, which must agree
// byte-for-byte on the layout.

#include <array>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <utility>

namespace axiom {

inline constexpr std::array<std::uint8_t, 8> kSfxDescriptorMagic = {
    'A', 'X', 'S', 'F', 'X', '2', '\0', '\0'};
// The v1 trailer magic, written at EOF-16 by releases through 0.8.0.0.
inline constexpr std::array<std::uint8_t, 8> kSfxLegacyMagic = {
    'A', 'X', 'I', 'O', 'M', 'S', 'F', 'X'};

inline constexpr std::size_t kSfxDescriptorSize = 64;
inline constexpr std::size_t kSfxLegacyTrailerSize = 16;
inline constexpr std::uint16_t kSfxFormatVersion = 2;
inline constexpr std::size_t kSfxMaxConfigSize = std::size_t{4} << 20;

// Guards against a corrupt section table producing an absurd allocation.
inline constexpr std::uint16_t kSfxMaxSections = 96;

enum class SfxFormat {
    legacy,  // v1: "AXIOMSFX" plus a u64 length, at EOF-16
    v2,      // "AXSFX2\0\0" descriptor at the end of the PE image
};

struct SfxPayload {
    SfxFormat format = SfxFormat::v2;
    // Absolute file offsets, already range-checked against the image.
    std::uint64_t payload_offset = 0;
    std::uint64_t payload_size = 0;
    std::uint64_t config_offset = 0;  // 0 when the SFX carries no config
    std::uint64_t config_size = 0;
    std::uint32_t flags = 0;
    // First eight bytes of the payload's BLAKE3-256. Zero for v1, which has no
    // integrity field. A corruption check, not a security control.
    std::array<std::uint8_t, 8> payload_hash{};
};

// Offset just past the last raw byte described by the PE section table, or
// nullopt when the stream does not hold a well-formed PE image.
std::optional<std::uint64_t> pe_image_end(std::istream& stream);

// File offset and size of the Authenticode certificate table, when the PE
// carries one. The security data directory stores a file offset, not an RVA.
std::optional<std::pair<std::uint64_t, std::uint64_t>> pe_certificate_table(
    std::istream& stream);

// Locates the embedded payload, preferring a v2 descriptor and falling back to
// the v1 trailer so executables produced by earlier releases keep working.
std::optional<SfxPayload> sfx_locate_payload(const std::filesystem::path& path);
std::optional<SfxPayload> sfx_locate_payload(std::istream& stream);

// Encodes the 64-byte v2 descriptor that is written at `image_end`. Offsets in
// `payload` are absolute; the descriptor stores them relative to `image_end`.
std::array<std::uint8_t, kSfxDescriptorSize> sfx_encode_descriptor(
    std::uint64_t image_end, const SfxPayload& payload);

}  // namespace axiom
