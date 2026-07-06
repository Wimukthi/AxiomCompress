#pragma once

#include "axiom/axiom.hpp"

namespace axiom::codec {

ByteVector encode_lz77(std::span<const std::uint8_t> input,
                       const CompressionOptions& options);

// `greedy_tokens` optionally passes the already-computed greedy parse of the
// same input so the single-pass mode can measure its cost model without
// re-running the matcher; callers that have it lying around should pass it.
ByteVector encode_lz77_optimal(std::span<const std::uint8_t> input,
                               const CompressionOptions& options,
                               const ByteVector* greedy_tokens = nullptr);

ByteVector decode_lz77(std::span<const std::uint8_t> encoded,
                       std::size_t output_size);

void decode_lz77_into(std::span<const std::uint8_t> encoded,
                      std::span<std::uint8_t> output);

}  // namespace axiom::codec
