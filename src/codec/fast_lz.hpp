#pragma once

#include "axiom/axiom.hpp"

namespace axiom::codec {

ByteVector encode_fast_lz(std::span<const std::uint8_t> input,
                          const CompressionOptions& options);

ByteVector encode_fast_lz77(std::span<const std::uint8_t> input,
                            const CompressionOptions& options);

ByteVector decode_fast_lz(std::span<const std::uint8_t> encoded,
                          std::size_t output_size);

void decode_fast_lz_into(std::span<const std::uint8_t> encoded,
                         std::span<std::uint8_t> output);

}  // namespace axiom::codec
