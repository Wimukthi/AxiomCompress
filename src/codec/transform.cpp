#include "codec/transform.hpp"

#include "codec/fast_lz.hpp"
#include "codec/varint.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace axiom::codec {
namespace {

constexpr std::size_t kMinTransformSize = 4096;
constexpr std::size_t kSampleWindow = std::size_t{256} << 10;
constexpr std::size_t kMaxSampleWindows = 4;
constexpr std::uint8_t kMaxDeltaStride = 64;
constexpr std::size_t kTarBlockSize = 512;

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

void write_le16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
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

TransformHint detect_elf(std::span<const std::uint8_t> prefix) {
    if (!matches(prefix, 0, {0x7f, 'E', 'L', 'F'}) ||
        !has_bytes(prefix, 0, 20) ||
        (prefix[4] != 1 && prefix[4] != 2) || prefix[6] != 1) {
        return {};
    }
    // Both supported x86 ELF machines are little-endian architectures.
    if (prefix[5] != 1) {
        return {};
    }
    const auto machine = read_le16(prefix, 18);
    if (machine == 3 || machine == 62) {
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

std::uint16_t zigzag16(std::uint16_t value) {
    const auto signed_value = static_cast<std::int16_t>(value);
    const auto widened = static_cast<std::int32_t>(signed_value);
    return static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(widened) << 1) ^
        static_cast<std::uint32_t>(widened >> 15));
}

std::uint16_t unzigzag16(std::uint16_t value) {
    const auto decoded = static_cast<std::uint16_t>(
        (value >> 1) ^ static_cast<std::uint16_t>(0u - (value & 1u)));
    return decoded;
}

void word16_predict_filter(std::span<std::uint8_t> bytes,
                           std::uint8_t parameter,
                           bool encode) {
    const auto word_count = bytes.size() / 2;
    if (word_count == 0) {
        return;
    }
    const std::size_t row_width = parameter == 0 ? 0 : std::size_t{1} << parameter;
    std::vector<std::uint16_t> words(word_count);

    if (encode) {
        for (std::size_t i = 0; i < word_count; ++i) {
            words[i] = read_le16(bytes, i * 2);
        }
        if (row_width == 0) {
            for (std::size_t i = word_count; i-- > 1;) {
                words[i] = static_cast<std::uint16_t>(words[i] - words[i - 1]);
            }
        } else if (word_count > row_width + 1) {
            for (std::size_t i = word_count; i-- > row_width + 1;) {
                const auto prediction = static_cast<std::uint16_t>(
                    words[i - 1] + words[i - row_width] - words[i - row_width - 1]);
                words[i] = static_cast<std::uint16_t>(words[i] - prediction);
            }
        }
        for (std::size_t i = 0; i < word_count; ++i) {
            const auto residual = zigzag16(words[i]);
            bytes[i] = static_cast<std::uint8_t>(residual);
            bytes[word_count + i] = static_cast<std::uint8_t>(residual >> 8);
        }
        return;
    }

    for (std::size_t i = 0; i < word_count; ++i) {
        const auto packed = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[i]) |
            static_cast<std::uint16_t>(bytes[word_count + i]) << 8);
        words[i] = unzigzag16(packed);
    }
    if (row_width == 0) {
        for (std::size_t i = 1; i < word_count; ++i) {
            words[i] = static_cast<std::uint16_t>(words[i] + words[i - 1]);
        }
    } else if (word_count > row_width + 1) {
        for (std::size_t i = row_width + 1; i < word_count; ++i) {
            const auto prediction = static_cast<std::uint16_t>(
                words[i - 1] + words[i - row_width] - words[i - row_width - 1]);
            words[i] = static_cast<std::uint16_t>(words[i] + prediction);
        }
    }
    for (std::size_t i = 0; i < word_count; ++i) {
        write_le16(bytes, i * 2, words[i]);
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
    } else if (range.transform == CompressionTransform::word16_predict) {
        word16_predict_filter(part, range.parameter, encode);
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

double histogram_entropy(const std::array<std::uint64_t, 256>& histogram,
                         std::uint64_t total) {
    if (total == 0) {
        return 8.0;
    }
    double entropy = 0.0;
    for (const auto count : histogram) {
        if (count == 0) {
            continue;
        }
        const auto probability = static_cast<double>(count) /
                                 static_cast<double>(total);
        entropy -= probability * std::log2(probability);
    }
    return entropy;
}

TransformHint detect_word16_predictor(std::span<const std::uint8_t> input) {
    constexpr std::size_t kMinimumWords = std::size_t{128} << 10;
    constexpr std::size_t kMaximumSamples = std::size_t{1} << 20;
    constexpr std::array<std::uint8_t, 6> kParameters{0, 7, 8, 9, 10, 11};
    const auto word_count = input.size() / 2;
    if (word_count < kMinimumWords) {
        return {};
    }

    double best_entropy = 8.0;
    double best_raw_entropy = 0.0;
    std::uint8_t best_parameter = 0;
    for (const auto parameter : kParameters) {
        const auto row_width = parameter == 0 ? 0 : std::size_t{1} << parameter;
        const auto first = row_width == 0 ? std::size_t{1} : row_width + 1;
        if (first >= word_count) {
            continue;
        }
        const auto available = word_count - first;
        const auto step = std::max<std::size_t>(1, available / kMaximumSamples);
        std::array<std::uint64_t, 256> raw_histogram{};
        std::array<std::uint64_t, 256> residual_histogram{};
        std::uint64_t sampled_bytes = 0;
        for (std::size_t i = first; i < word_count; i += step) {
            const auto current = read_le16(input, i * 2);
            const auto prediction = row_width == 0
                ? read_le16(input, (i - 1) * 2)
                : static_cast<std::uint16_t>(
                      read_le16(input, (i - 1) * 2) +
                      read_le16(input, (i - row_width) * 2) -
                      read_le16(input, (i - row_width - 1) * 2));
            const auto residual = zigzag16(
                static_cast<std::uint16_t>(current - prediction));
            ++raw_histogram[static_cast<std::uint8_t>(current)];
            ++raw_histogram[static_cast<std::uint8_t>(current >> 8)];
            ++residual_histogram[static_cast<std::uint8_t>(residual)];
            ++residual_histogram[static_cast<std::uint8_t>(residual >> 8)];
            sampled_bytes += 2;
        }
        const auto raw_entropy = histogram_entropy(raw_histogram, sampled_bytes);
        const auto residual_entropy = histogram_entropy(residual_histogram, sampled_bytes);
        if (residual_entropy < best_entropy) {
            best_entropy = residual_entropy;
            best_raw_entropy = raw_entropy;
            best_parameter = parameter;
        }
    }

    // A broad transform changes match structure as well as entropy. Require a
    // large symbol-level win so ordinary text, executables, and databases never
    // enter this specialized numeric path on sampling noise alone.
    if (best_entropy + 0.50 < best_raw_entropy) {
        return {CompressionTransform::word16_predict, best_parameter};
    }
    return {};
}

std::optional<std::uint64_t> parse_tar_octal(std::span<const std::uint8_t> field) {
    std::uint64_t value = 0;
    bool saw_digit = false;
    for (const auto byte : field) {
        if (byte == 0 || byte == ' ') {
            if (saw_digit) {
                break;
            }
            continue;
        }
        if (byte < '0' || byte > '7') {
            return std::nullopt;
        }
        saw_digit = true;
        if (value > (std::numeric_limits<std::uint64_t>::max() >> 3)) {
            return std::nullopt;
        }
        value = (value << 3) | static_cast<std::uint64_t>(byte - '0');
    }
    return saw_digit ? std::optional<std::uint64_t>(value) : std::nullopt;
}

bool valid_tar_header(std::span<const std::uint8_t> header) {
    if (header.size() != kTarBlockSize ||
        std::memcmp(header.data() + 257, "ustar", 5) != 0) {
        return false;
    }
    const auto stored = parse_tar_octal(header.subspan(148, 8));
    if (!stored) {
        return false;
    }
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < header.size(); ++i) {
        sum += i >= 148 && i < 156 ? static_cast<std::uint8_t>(' ') : header[i];
    }
    return sum == *stored;
}

}  // namespace

TransformHint detect_transform_hint(std::span<const std::uint8_t> prefix) {
    if (const auto pe = detect_pe(prefix); pe.transform != CompressionTransform::none) {
        return pe;
    }
    if (const auto elf = detect_elf(prefix);
        elf.transform != CompressionTransform::none) {
        return elf;
    }
    if (const auto wave = detect_wave(prefix); wave.transform != CompressionTransform::none) {
        return wave;
    }
    return detect_bmp(prefix);
}

std::vector<TarMemberSpan> detect_tar_members(
    std::span<const std::uint8_t> input) {
    std::vector<TarMemberSpan> members;
    std::size_t cursor = 0;
    while (cursor + kTarBlockSize <= input.size()) {
        const auto header = input.subspan(cursor, kTarBlockSize);
        if (std::all_of(header.begin(), header.end(), [](std::uint8_t byte) {
                return byte == 0;
            })) {
            break;
        }
        if (!valid_tar_header(header)) {
            return {};
        }
        const auto encoded_size = parse_tar_octal(header.subspan(124, 12));
        if (!encoded_size || *encoded_size > std::numeric_limits<std::size_t>::max()) {
            return {};
        }
        const auto member_size = static_cast<std::size_t>(*encoded_size);
        const auto content_offset = cursor + kTarBlockSize;
        if (member_size > input.size() - content_offset) {
            return {};
        }
        const auto padded_size = (member_size + kTarBlockSize - 1) &
                                 ~(kTarBlockSize - 1);
        if (padded_size > input.size() - content_offset) {
            return {};
        }
        const auto end_offset = content_offset + padded_size;
        members.push_back({cursor, content_offset, member_size, end_offset,
                           header[156]});
        cursor = end_offset;
    }
    return members;
}

std::vector<CompressionTransformRange> detect_transform_ranges(
    std::span<const std::uint8_t> input) {
    if (const auto hint = detect_transform_hint(input);
        hint.transform != CompressionTransform::none) {
        return {{hint.transform, 0, static_cast<std::uint64_t>(input.size()), 0,
                 hint.parameter}};
    }
    const auto members = detect_tar_members(input);
    std::vector<CompressionTransformRange> tar_ranges;
    for (const auto& span : members) {
        if ((span.type == 0 || span.type == '0') &&
            span.content_size >= kMinTransformSize) {
            const auto member = input.subspan(span.content_offset, span.content_size);
            auto hint = detect_transform_hint(member);
            if (hint.transform == CompressionTransform::none) {
                hint = detect_word16_predictor(member);
            }
            if (hint.transform != CompressionTransform::none) {
                tar_ranges.push_back({hint.transform,
                                      static_cast<std::uint64_t>(span.content_offset),
                                      static_cast<std::uint64_t>(span.content_size), 0,
                                      hint.parameter});
                continue;
            }

            // Some benchmark and source-distribution members are tars themselves.
            // Descend exactly one level and reuse the existing static x86 filter for
            // validated ELF payloads; decoder behavior and metadata stay unchanged.
            for (const auto& nested_span : detect_tar_members(member)) {
                if ((nested_span.type != 0 && nested_span.type != '0') ||
                    nested_span.content_size < kMinTransformSize) {
                    continue;
                }
                const auto nested_member = member.subspan(
                    nested_span.content_offset, nested_span.content_size);
                const auto nested_hint = detect_transform_hint(nested_member);
                if (nested_hint.transform != CompressionTransform::x86_branch) {
                    continue;
                }
                tar_ranges.push_back({
                    nested_hint.transform,
                    static_cast<std::uint64_t>(span.content_offset) +
                        static_cast<std::uint64_t>(nested_span.content_offset),
                    static_cast<std::uint64_t>(nested_span.content_size), 0,
                    nested_hint.parameter});
            }
        }
    }
    if (!members.empty()) {
        return tar_ranges;
    }
    if (const auto hint = detect_word16_predictor(input);
        hint.transform != CompressionTransform::none) {
        return {{hint.transform, 0, static_cast<std::uint64_t>(input.size()), 0,
                 hint.parameter}};
    }
    return {};
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
        } else if (range.transform == CompressionTransform::word16_predict) {
            if (range.parameter != 0 &&
                (range.parameter < 7 || range.parameter > 15)) {
                throw FormatError("invalid word16 predictor parameter");
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
    if (original_size > std::numeric_limits<std::size_t>::max()) {
        throw FormatError("transform output size exceeds platform limit");
    }
    // Every serialized range needs two fixed bytes and three one-byte varints.
    // Reject an impossible count before reserve() so corrupt metadata cannot turn
    // an otherwise tiny archive into a multi-gigabyte allocation request.
    constexpr std::size_t kMinSerializedRangeSize = 5;
    if (count > (original_size / kMinTransformSize) + 1 ||
        count > (metadata.size() - cursor) / kMinSerializedRangeSize ||
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
    return normalize_transform_ranges(ranges, static_cast<std::size_t>(original_size));
}

}  // namespace axiom::codec
