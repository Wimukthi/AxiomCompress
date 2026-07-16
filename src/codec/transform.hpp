#pragma once

#include "axiom/axiom.hpp"

#include <span>
#include <vector>

namespace axiom::codec {

struct TransformHint {
    CompressionTransform transform = CompressionTransform::none;
    std::uint8_t parameter = 0;
};

struct TarMemberSpan {
    std::size_t header_offset = 0;
    std::size_t content_offset = 0;
    std::size_t content_size = 0;
    std::size_t end_offset = 0;
    std::uint8_t type = 0;
};

TransformHint detect_transform_hint(std::span<const std::uint8_t> prefix);

// Returns every validated member of a POSIX ustar stream. An empty result means
// either non-tar input or an invalid/truncated stream.
std::vector<TarMemberSpan> detect_tar_members(
    std::span<const std::uint8_t> input);

// Detects whole-input filters and, for a valid POSIX tar stream, filters on
// individual regular-file payloads. The latter preserves tar headers while
// allowing scientific/image members to use their native numeric predictor and
// x86 ELF payloads inside one nested tar to use the branch transform.
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
