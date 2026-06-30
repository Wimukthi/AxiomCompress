#pragma once

#include "axiom/axiom.hpp"
#include "codec/lz77_split.hpp"

namespace axiom::codec {

struct FastLz77SplitPayloads {
    std::size_t lz77_size = 0;
    Lz77SplitStreams streams;
};

ByteVector encode_fast_lz(std::span<const std::uint8_t> input,
                          const CompressionOptions& options);

ByteVector encode_fast_lz77(std::span<const std::uint8_t> input,
                            const CompressionOptions& options);

FastLz77SplitPayloads encode_fast_lz77_split_payloads(
    std::span<const std::uint8_t> input,
    const CompressionOptions& options);

ByteVector decode_fast_lz(std::span<const std::uint8_t> encoded,
                          std::size_t output_size);

void decode_fast_lz_into(std::span<const std::uint8_t> encoded,
                         std::span<std::uint8_t> output);

}  // namespace axiom::codec
