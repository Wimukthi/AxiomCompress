#pragma once

#include "axiom/axiom.hpp"

#include <span>
#include <vector>

namespace axiom::codec {

struct TransformHint {
    CompressionTransform transform = CompressionTransform::none;
    std::uint8_t parameter = 0;
};

TransformHint detect_transform_hint(std::span<const std::uint8_t> prefix);

// Detects whole-input filters and, for a valid POSIX tar stream, filters on
// individual regular-file payloads. The latter preserves tar headers while
// allowing scientific/image members to use their native numeric predictor.
std::vector<CompressionTransformRange> detect_transform_ranges(
    std::span<const std::uint8_t> input);

std::vector<CompressionTransformRange> normalize_transform_ranges(
    std::span<const CompressionTransformRange> ranges,
    std::size_t input_size);

ByteVector apply_transform_ranges(
    std::span<const std::uint8_t> input,
    std::span<const CompressionTransformRange> ranges);

void inverse_transform_ranges(
    ByteVector& bytes,
    std::span<const CompressionTransformRange> ranges);

bool transformed_sample_is_smaller(
    std::span<const std::uint8_t> original,
    std::span<const std::uint8_t> transformed,
    std::span<const CompressionTransformRange> ranges,
    const CompressionOptions& options);

ByteVector serialize_transform_ranges(
    std::span<const CompressionTransformRange> ranges);

std::vector<CompressionTransformRange> deserialize_transform_ranges(
    std::span<const std::uint8_t> metadata,
    std::uint64_t original_size);

}  // namespace axiom::codec
