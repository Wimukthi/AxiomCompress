#pragma once

#include "axiom/axiom.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace axiom::codec {

// External AXC codecs frame each independently decoded chunk. Offsets are
// relative to the external-codec payload (not the surrounding AXC stream).
struct ExternalCodecFrame {
    std::uint64_t uncompressed_offset = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t frame_offset = 0;
    std::uint64_t frame_size = 0;
    std::uint64_t payload_offset = 0;
    std::uint64_t payload_size = 0;
    bool stored = false;
    std::uint8_t lzma_property = 0;
};

std::vector<ExternalCodecFrame> inspect_external_codec_frames(
    std::span<const std::uint8_t> payload,
    CompressionMethod method,
    std::size_t expected_size);

// Decode one complete external-codec frame returned by
// inspect_external_codec_frames().
ByteVector decode_external_codec_frame(std::span<const std::uint8_t> frame,
                                       CompressionMethod method,
                                       std::size_t expected_size,
                                       std::uint8_t lzma_property = 0);

ByteVector encode_external_codec(std::span<const std::uint8_t> input,
                                 CompressionMethod method,
                                 const CompressionOptions& options);

// Variant used by the disk-streamed large-solid writer. The bound is stable
// across separately encoded chunks that share one AXEC property byte.
ByteVector encode_external_codec_with_dictionary_bound(
    std::span<const std::uint8_t> input,
    CompressionMethod method,
    const CompressionOptions& options,
    std::size_t dictionary_input_bound);

ByteVector decode_external_codec(std::span<const std::uint8_t> payload,
                                 CompressionMethod method,
                                 std::size_t expected_size,
                                 const DecompressionOptions& options);

}  // namespace axiom::codec
