#pragma once

#include "axiom/axiom.hpp"

namespace axiom::codec {

// Copy of `options` whose encode_progress reports into the [begin, end) share
// of the parent's progress; a cheap way to compose multi-pass phase plans
// (compress_block places the greedy/optimal/entropy phases, and the optimal
// parser places its own passes, without either knowing about the other).
inline CompressionOptions scoped_progress_options(const CompressionOptions& options,
                                                  double begin,
                                                  double end) {
    auto scoped = options;
    if (options.encode_progress) {
        scoped.encode_progress = [parent = options.encode_progress, begin,
                                  width = end - begin](double fraction) {
            parent(begin + width * fraction);
        };
    }
    return scoped;
}

ByteVector encode_lz77(std::span<const std::uint8_t> input,
                       const CompressionOptions& options);

// `greedy_tokens` optionally passes the already-computed greedy parse of the
// same input so the single-pass mode can measure its cost model without
// re-running the matcher; callers that have it lying around should pass it.
ByteVector encode_lz77_optimal(std::span<const std::uint8_t> input,
                               const CompressionOptions& options,
                               const ByteVector* greedy_tokens = nullptr);

// Independent fixed-grain optimal parses whose encoder-selected recent-distance
// states are carried by format-visible checkpoint tokens. Returns nullopt when
// the input/executor cannot benefit; callers must still compare the complete
// encoded representation against the ordinary global parse.
std::optional<ByteVector> encode_lz77_optimal_checkpointed(
    std::span<const std::uint8_t> input,
    const CompressionOptions& options,
    const ByteVector& greedy_tokens);

ByteVector decode_lz77(std::span<const std::uint8_t> encoded,
                       std::size_t output_size);

void decode_lz77_into(std::span<const std::uint8_t> encoded,
                      std::span<std::uint8_t> output);

}  // namespace axiom::codec
