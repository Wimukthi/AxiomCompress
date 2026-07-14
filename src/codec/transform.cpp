#include "codec/transform.hpp"

#include "codec/fast_lz.hpp"
#include "codec/varint.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace axiom::codec {
namespace {

constexpr std::size_t kMinTransformSize = 4096;
constexpr std::size_t kSampleWindow = std::size_t{256} << 10;
constexpr std::size_t kMaxSampleWindows = 4;
constexpr std::uint8_t kMaxDeltaStride = 64;

std::uint16_t read_le16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1]) << 8;
}

std::uint32_t read_le32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    }
    return value;
}

void write_le32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset++] = static_cast<std::uint8_t>(value >> shift);
    }
}

bool has_bytes(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t count) {
    return offset <= bytes.size() && count <= bytes.size() - offset;
}

bool matches(std::span<const std::uint8_t> bytes, std::size_t offset,
             std::initializer_list<std::uint8_t> expected) {
    return has_bytes(bytes, offset, expected.size()) &&
           std::equal(expected.begin(), expected.end(), bytes.begin() + offset);
}

TransformHint detect_pe(std::span<const std::uint8_t> prefix) {
    if (!matches(prefix, 0, {'M', 'Z'}) || !has_bytes(prefix, 0x3c, 4)) return {};
    const auto pe_offset = static_cast<std::size_t>(read_le32(prefix, 0x3c));
    if (!matches(prefix, pe_offset, {'P', 'E', 0, 0}) ||
        !has_bytes(prefix, pe_offset + 4, 2)) {
        return {};
    }
    const auto machine = read_le16(prefix, pe_offset + 4);
    if (machine == 0x014c || machine == 0x8664) {
        return {CompressionTransform::x86_branch, 0};
    }
    return {};
}

TransformHint detect_wave(std::span<const std::uint8_t> prefix) {
    if (!matches(prefix, 0, {'R', 'I', 'F', 'F'}) ||
        !matches(prefix, 8, {'W', 'A', 'V', 'E'})) {
        return {};
    }
    std::size_t cursor = 12;
    while (has_bytes(prefix, cursor, 8)) {
        const auto chunk_size = static_cast<std::size_t>(read_le32(prefix, cursor + 4));
        if (matches(prefix, cursor, {'f', 'm', 't', ' '}) && chunk_size >= 16 &&
            has_bytes(prefix, cursor + 8, 16)) {
            const auto format = read_le16(prefix, cursor + 8);
            const auto block_align = read_le16(prefix, cursor + 20);
            const auto bits = read_le16(prefix, cursor + 22);
            if (format == 1 && block_align > 0 && block_align <= kMaxDeltaStride &&
                (bits == 8 || bits == 16 || bits == 24 || bits == 32)) {
                return {CompressionTransform::delta,
                        static_cast<std::uint8_t>(block_align)};
            }
            return {};
        }
        if (chunk_size > prefix.size()) break;
        const auto advance = std::size_t{8} + chunk_size + (chunk_size & 1u);
        if (advance > prefix.size() - cursor) break;
        cursor += advance;
    }
    return {};
}

TransformHint detect_bmp(std::span<const std::uint8_t> prefix) {
    if (!matches(prefix, 0, {'B', 'M'}) || !has_bytes(prefix, 14, 40)) return {};
    const auto dib_size = read_le32(prefix, 14);
    if (dib_size < 40) return {};
    const auto bits = read_le16(prefix, 28);
    const auto compression = read_le32(prefix, 30);
    if ((compression == 0 || compression == 3) &&
        (bits == 8 || bits == 16 || bits == 24 || bits == 32)) {
        return {CompressionTransform::delta,
                static_cast<std::uint8_t>(std::max<std::uint16_t>(1, bits / 8))};
    }
    return {};
}

void x86_filter(std::span<std::uint8_t> bytes, std::uint64_t source_offset,
                bool encode) {
    for (std::size_t i = 0; i + 4 < bytes.size(); ++i) {
        if (bytes[i] != 0xe8 && bytes[i] != 0xe9) continue;
        const auto relative = read_le32(bytes, i + 1);
        const auto position = static_cast<std::uint32_t>(source_offset + i + 5);
        write_le32(bytes, i + 1, encode ? relative + position : relative - position);
        i += 4;
    }
}

void delta_filter(std::span<std::uint8_t> bytes, std::uint8_t stride, bool encode) {
    if (stride == 0 || bytes.size() <= stride) return;
    if (encode) {
        for (std::size_t i = bytes.size(); i-- > stride;) {
            bytes[i] = static_cast<std::uint8_t>(bytes[i] - bytes[i - stride]);
        }
    } else {
        for (std::size_t i = stride; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::uint8_t>(bytes[i] + bytes[i - stride]);
        }
    }
}

void transform_one(ByteVector& bytes, const CompressionTransformRange& range,
                   bool encode) {
    const auto offset = static_cast<std::size_t>(range.offset);
    const auto size = static_cast<std::size_t>(range.size);
    auto part = std::span<std::uint8_t>(bytes).subspan(offset, size);
    if (range.transform == CompressionTransform::x86_branch) {
        x86_filter(part, range.source_offset, encode);
    } else if (range.transform == CompressionTransform::delta) {
        delta_filter(part, range.parameter, encode);
    }
}

ByteVector sampled_bytes(std::span<const std::uint8_t> input) {
    if (input.size() <= kSampleWindow * kMaxSampleWindows) {
        return ByteVector(input.begin(), input.end());
    }
    ByteVector sample;
    sample.reserve(kSampleWindow * kMaxSampleWindows);
    for (std::size_t i = 0; i < kMaxSampleWindows; ++i) {
        const auto available = input.size() - kSampleWindow;
        const auto start = available * i / (kMaxSampleWindows - 1);
        sample.insert(sample.end(), input.begin() + start,
                      input.begin() + start + kSampleWindow);
    }
    return sample;
}

}  // namespace

TransformHint detect_transform_hint(std::span<const std::uint8_t> prefix) {
    if (const auto pe = detect_pe(prefix); pe.transform != CompressionTransform::none) {
        return pe;
    }
    if (const auto wave = detect_wave(prefix); wave.transform != CompressionTransform::none) {
        return wave;
    }
    return detect_bmp(prefix);
}

std::vector<CompressionTransformRange> normalize_transform_ranges(
    std::span<const CompressionTransformRange> ranges, std::size_t input_size) {
    std::vector<CompressionTransformRange> result;
    result.reserve(ranges.size());
    for (const auto& range : ranges) {
        if (range.transform == CompressionTransform::none || range.size < kMinTransformSize) {
            continue;
        }
        if (range.offset > input_size || range.size > input_size - range.offset) {
            throw FormatError("transform range exceeds input size");
        }
        if (range.transform == CompressionTransform::x86_branch) {
            if (range.parameter != 0) throw FormatError("invalid x86 transform parameter");
        } else if (range.transform == CompressionTransform::delta) {
            if (range.parameter == 0 || range.parameter > kMaxDeltaStride) {
                throw FormatError("invalid delta transform stride");
            }
        } else {
            throw FormatError("unknown compression transform");
        }
        result.push_back(range);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.offset < right.offset;
    });
    std::uint64_t end = 0;
    for (const auto& range : result) {
        if (range.offset < end) throw FormatError("overlapping transform ranges");
        end = range.offset + range.size;
    }
    return result;
}

ByteVector apply_transform_ranges(std::span<const std::uint8_t> input,
                                  std::span<const CompressionTransformRange> ranges) {
    ByteVector result(input.begin(), input.end());
    for (const auto& range : ranges) transform_one(result, range, true);
    return result;
}

void inverse_transform_ranges(ByteVector& bytes,
                              std::span<const CompressionTransformRange> ranges) {
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        transform_one(bytes, *it, false);
    }
}

bool transformed_sample_is_smaller(std::span<const std::uint8_t> original,
                                   std::span<const std::uint8_t> transformed,
                                   std::span<const CompressionTransformRange> ranges,
                                   const CompressionOptions& options) {
    if (original.size() != transformed.size() || ranges.empty()) {
        return false;
    }
    auto trial = options;
    trial.use_fast_lz = true;
    trial.use_tree_matcher = false;
    trial.enable_optimal_parser = false;
    trial.fast_entropy = true;
    trial.lazy_matching = false;
    trial.thread_count = 1;
    trial.operation.reset();
    trial.encoded_bytes_progress = {};
    trial.encode_progress = {};
    trial.transform_ranges.clear();
    trial.enable_file_filters = false;

    // Sample the candidate ranges themselves. Sampling the whole solid block can
    // completely miss a small executable or image embedded in a much larger block.
    std::size_t raw_size = 0;
    std::size_t filtered_size = 0;
    for (const auto& range : ranges) {
        const auto offset = static_cast<std::size_t>(range.offset);
        const auto size = static_cast<std::size_t>(range.size);
        const auto raw_sample = sampled_bytes(original.subspan(offset, size));
        const auto filtered_sample = sampled_bytes(transformed.subspan(offset, size));
        raw_size += encode_fast_lz(raw_sample, trial).size();
        filtered_size += encode_fast_lz(filtered_sample, trial).size();
    }

    // Cover both sampling noise and the bounded transform metadata envelope.
    const auto metadata_cost = std::size_t{16} + ranges.size() * 24;
    const auto required = std::max({std::size_t{64}, raw_size / 400, metadata_cost});
    return filtered_size + required < raw_size;
}

ByteVector serialize_transform_ranges(
    std::span<const CompressionTransformRange> ranges) {
    ByteVector metadata;
    if (ranges.empty()) return metadata;
    write_varuint(metadata, ranges.size());
    for (const auto& range : ranges) {
        metadata.push_back(static_cast<std::uint8_t>(range.transform));
        metadata.push_back(range.parameter);
        write_varuint(metadata, range.offset);
        write_varuint(metadata, range.size);
        write_varuint(metadata, range.source_offset);
    }
    return metadata;
}

std::vector<CompressionTransformRange> deserialize_transform_ranges(
    std::span<const std::uint8_t> metadata, std::uint64_t original_size) {
    std::size_t cursor = 0;
    const auto count = read_varuint(metadata, cursor);
    if (count > (original_size / kMinTransformSize) + 1 ||
        count > std::numeric_limits<std::size_t>::max()) {
        throw FormatError("implausible transform range count");
    }
    std::vector<CompressionTransformRange> ranges;
    ranges.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        if (cursor + 2 > metadata.size()) throw FormatError("truncated transform metadata");
        CompressionTransformRange range;
        range.transform = static_cast<CompressionTransform>(metadata[cursor++]);
        range.parameter = metadata[cursor++];
        range.offset = read_varuint(metadata, cursor);
        range.size = read_varuint(metadata, cursor);
        range.source_offset = read_varuint(metadata, cursor);
        ranges.push_back(range);
    }
    if (cursor != metadata.size()) throw FormatError("trailing transform metadata");
    if (original_size > std::numeric_limits<std::size_t>::max()) {
        throw FormatError("transform output size exceeds platform limit");
    }
    return normalize_transform_ranges(ranges, static_cast<std::size_t>(original_size));
}

}  // namespace axiom::codec
