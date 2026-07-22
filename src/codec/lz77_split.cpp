#include "codec/lz77_split.hpp"

#include "codec/match_copy.hpp"
#include "codec/varint.hpp"
#include "core/task_executor.hpp"
#include "entropy/huffman.hpp"
#include "entropy/range.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace axiom::codec {
namespace {

constexpr std::uint8_t kLiteralToken = 0;
constexpr std::uint8_t kMatchToken = 1;
constexpr std::uint8_t kRepTokenBase = 2;
constexpr std::size_t kRepCount = 4;
constexpr std::uint8_t kCheckpointToken = kRepTokenBase + kRepCount;
constexpr std::uint8_t kCheckpointExplicit = kRepCount;
constexpr std::size_t kMinMatch = 4;

enum class StreamCodec : std::uint8_t {
    store = 0,
    huffman = 1,
    order1 = 2,  // legacy adaptive coder: still decoded, no longer emitted
    rans = 3,
    rans_order1 = 4,
};

// Order-0 entropy of a byte stream, in bits per byte (0..8). The lower bound on
// what any order-0 coder (Huffman, rANS) can achieve, used to decide cheaply
// whether coding is worth attempting at all.
double order0_bits_per_byte(std::span<const std::uint8_t> raw) {
    if (raw.empty()) {
        return 0.0;
    }
    std::array<std::uint32_t, 256> hist{};
    for (const auto byte : raw) {
        ++hist[byte];
    }
    const auto total = static_cast<double>(raw.size());
    double bits = 0.0;
    for (const auto count : hist) {
        if (count != 0) {
            const auto p = count / total;
            bits -= p * std::log2(p);
        }
    }
    return bits;
}

double order0_bits_per_byte(const Lz77SplitStreams::Histogram& hist, std::size_t size) {
    if (size == 0) {
        return 0.0;
    }
    const auto total = static_cast<double>(size);
    double bits = 0.0;
    for (const auto count : hist) {
        if (count != 0) {
            const auto p = static_cast<double>(count) / total;
            bits -= p * std::log2(p);
        }
    }
    return bits;
}

// Order-1 (previous-byte context) entropy in bits per byte. Costs one O(n) pass
// plus a 64 KiB histogram — far cheaper than the bit-serial order-1 *encode*, so
// it is worth computing to decide whether that encode will pay off.
double order1_bits_per_byte(std::span<const std::uint8_t> raw) {
    if (raw.size() < 2) {
        return order0_bits_per_byte(raw);
    }
    thread_local std::array<std::uint32_t, 256 * 256> joint;
    joint.fill(0);
    std::array<std::uint32_t, 256> context_total{};
    std::uint8_t prev = 0;
    for (const auto byte : raw) {
        ++joint[(static_cast<std::size_t>(prev) << 8) | byte];
        ++context_total[prev];
        prev = byte;
    }
    // H(X|prev) = sum_prev P(prev) * H(X | prev), weighted by context frequency.
    const auto total = static_cast<double>(raw.size());
    double bits = 0.0;
    for (std::size_t c = 0; c < 256; ++c) {
        const auto ctx = context_total[c];
        if (ctx == 0) {
            continue;
        }
        const auto ctx_d = static_cast<double>(ctx);
        const auto* row = &joint[c << 8];
        double row_bits = 0.0;
        for (std::size_t s = 0; s < 256; ++s) {
            if (row[s] != 0) {
                const auto p = row[s] / ctx_d;
                row_bits -= p * std::log2(p);
            }
        }
        bits += (ctx_d / total) * row_bits;
    }
    return bits;
}

double sampled_order1_bits_per_byte(std::span<const std::uint8_t> raw) {
    constexpr std::size_t kChunkSize = std::size_t{64} << 10;
    constexpr std::size_t kChunkCount = 4;
    constexpr std::size_t kSampleSize = kChunkSize * kChunkCount;
    if (raw.size() <= kSampleSize) {
        return order1_bits_per_byte(raw);
    }

    ByteVector sample;
    sample.reserve(kSampleSize);
    for (std::size_t chunk = 0; chunk < kChunkCount; ++chunk) {
        const auto start = ((raw.size() - kChunkSize) * chunk) / (kChunkCount - 1);
        sample.insert(sample.end(), raw.begin() + static_cast<std::ptrdiff_t>(start),
                      raw.begin() + static_cast<std::ptrdiff_t>(start + kChunkSize));
    }
    return order1_bits_per_byte(sample);
}

std::size_t varuint_size(std::uint64_t value) {
    std::size_t bytes = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++bytes;
    }
    return bytes;
}

double stream_entropy_lower_bound(double bits_per_byte, std::size_t raw_size) {
    const double payload_bytes =
        (bits_per_byte * static_cast<double>(raw_size)) / 8.0;
    const auto payload_floor = static_cast<std::uint64_t>(payload_bytes);
    return static_cast<double>(varuint_size(raw_size) + 1 + varuint_size(payload_floor)) +
           payload_bytes;
}

std::size_t decoded_stream_limit(std::size_t output_size) {
    constexpr std::size_t kMinimumStreamLimit = 1024;
    constexpr std::size_t kStreamLimitScale = 4;

    if (output_size > (std::numeric_limits<std::size_t>::max() - kMinimumStreamLimit) /
                          kStreamLimitScale) {
        return std::numeric_limits<std::size_t>::max();
    }

    return std::max(kMinimumStreamLimit, (output_size * kStreamLimitScale) +
                                             kMinimumStreamLimit);
}

void write_stream(ByteVector& output,
                  std::span<const std::uint8_t> raw,
                  bool try_order1 = false,
                  bool fast = false,
                  bool prefer_rans = false,
                  const Lz77SplitStreams::Histogram* histogram = nullptr,
                  bool screen_order1 = false) {
    write_varuint(output, raw.size());

    auto codec = StreamCodec::store;
    ByteVector payload;
    std::size_t best_size = raw.size();

    auto consider = [&](StreamCodec candidate_codec, std::optional<ByteVector> candidate) {
        if (candidate && candidate->size() < best_size) {
            codec = candidate_codec;
            payload = std::move(*candidate);
            best_size = payload.size();
            return true;
        }
        return false;
    };

    if (fast) {
        // Fast mode either tries rANS directly for streams that are known to be
        // structured, or uses a cheap entropy gate for streams like distance
        // footers that are often near-uniform and not worth coding.
        constexpr std::size_t kMinWorthCoding = 64;
        if (prefer_rans && raw.size() >= kMinWorthCoding) {
            (void)consider(StreamCodec::rans,
                           histogram ? entropy::encode_rans(raw, *histogram)
                                     : entropy::encode_rans(raw));
        } else {
            const auto o0 = histogram ? order0_bits_per_byte(*histogram, raw.size())
                                      : order0_bits_per_byte(raw);
            if (raw.size() >= kMinWorthCoding && o0 < 7.5) {
                bool coded = false;
                if (try_order1 && order1_bits_per_byte(raw) + 0.10 < o0) {
                    coded = consider(StreamCodec::rans_order1,
                                     entropy::encode_rans_order1(raw));
                }
                if (!coded) {
                    (void)consider(StreamCodec::rans,
                                   histogram ? entropy::encode_rans(raw, *histogram)
                                             : entropy::encode_rans(raw));
                }
            }
        }

        output.push_back(static_cast<std::uint8_t>(codec));
        write_varuint(output, best_size);
        if (codec == StreamCodec::store) {
            output.insert(output.end(), raw.begin(), raw.end());
        } else {
            output.insert(output.end(), payload.begin(), payload.end());
        }
        return;
    }

    (void)consider(StreamCodec::huffman, entropy::encode_huffman(raw));

    // rANS is the order-0 choice; the older bit-serial order-0 range stream is
    // intentionally unsupported while the format is still free to change.
    (void)consider(StreamCodec::rans,
                   histogram ? entropy::encode_rans(raw, *histogram)
                             : entropy::encode_rans(raw));

    // Order-1 modelling only pays off on the literal stream; the command,
    // length, and distance streams have little previous-symbol structure, so we
    // skip it there. The clustered static-table rANS replaced the adaptive
    // bit-serial coder: within a fraction of a percent on ratio, ~30x faster
    // to decode. It runs after plain rANS so it must strictly beat it.
    bool order1_is_plausible = try_order1;
    if (order1_is_plausible && screen_order1 && raw.size() >= 4096) {
        const auto order0 = histogram ? order0_bits_per_byte(*histogram, raw.size())
                                      : order0_bits_per_byte(raw);
        // New literal representations can have twice as many lanes. A sampled
        // gate avoids paying for a full clustered model when its entropy is
        // clearly behind order-0; the complete representation still has to win
        // the outer byte-size bake-off.
        order1_is_plausible = sampled_order1_bits_per_byte(raw) < order0 + 0.10;
    }
    if (order1_is_plausible) {
        (void)consider(StreamCodec::rans_order1, entropy::encode_rans_order1(raw));
    }

    output.push_back(static_cast<std::uint8_t>(codec));
    write_varuint(output, best_size);
    if (codec == StreamCodec::store) {
        output.insert(output.end(), raw.begin(), raw.end());
    } else {
        output.insert(output.end(), payload.begin(), payload.end());
    }
}

ByteVector read_stream(std::span<const std::uint8_t> encoded,
                       std::size_t& cursor,
                       std::size_t max_raw_size) {
    const auto raw_size = static_cast<std::size_t>(read_varuint(encoded, cursor));
    if (raw_size > max_raw_size) {
        throw FormatError("split stream output exceeds block limit");
    }

    if (cursor >= encoded.size()) {
        throw FormatError("truncated split-stream codec id");
    }

    const auto codec = encoded[cursor++];
    const auto payload_size = static_cast<std::size_t>(read_varuint(encoded, cursor));

    if (payload_size > encoded.size() - cursor) {
        throw FormatError("split stream payload exceeds codec payload");
    }

    const auto payload = encoded.subspan(cursor, payload_size);
    cursor += payload_size;

    if (codec == static_cast<std::uint8_t>(StreamCodec::store)) {
        if (payload.size() != raw_size) {
            throw FormatError("stored split stream size mismatch");
        }

        return ByteVector(payload.begin(), payload.end());
    }

    if (codec == static_cast<std::uint8_t>(StreamCodec::huffman)) {
        return entropy::decode_huffman(payload, raw_size);
    }

    if (codec == static_cast<std::uint8_t>(StreamCodec::order1)) {
        return entropy::decode_order1(payload, raw_size);
    }

    if (codec == static_cast<std::uint8_t>(StreamCodec::rans)) {
        return entropy::decode_rans(payload, raw_size);
    }

    if (codec == static_cast<std::uint8_t>(StreamCodec::rans_order1)) {
        return entropy::decode_rans_order1(payload, raw_size);
    }

    throw FormatError("unknown split-stream codec");
}

Lz77SplitStreams split_lz77_payload(std::span<const std::uint8_t> lz77_payload);

// Distance slot coding maps d >= 1 to p = d - 1, then splits it into a small
// magnitude bucket and near-uniform footer bits. The slot stream entropy-codes
// well while the footer stays as raw packed bits for cheap decode.
constexpr std::uint32_t kSlotDirectLimit = 4;

std::uint32_t distance_to_slot(std::uint32_t distance,
                               std::uint32_t& footer_bits,
                               std::uint32_t& footer_value) {
    const std::uint32_t p = distance - 1;
    if (p < kSlotDirectLimit) {
        footer_bits = 0;
        footer_value = 0;
        return p;
    }

    const auto num_bits = static_cast<std::uint32_t>(std::bit_width(p)) - 1;
    const std::uint32_t slot = (num_bits << 1) | ((p >> (num_bits - 1)) & 1u);
    footer_bits = num_bits - 1;
    const std::uint32_t base = (2u | (slot & 1u)) << footer_bits;
    footer_value = p - base;
    return slot;
}

std::uint32_t slot_footer_bits(std::uint32_t slot) {
    return slot < kSlotDirectLimit ? 0u : (slot >> 1) - 1u;
}

std::uint32_t slot_to_distance(std::uint32_t slot, std::uint32_t footer_value) {
    if (slot < kSlotDirectLimit) {
        return slot + 1;
    }

    const std::uint32_t footer_bits = (slot >> 1) - 1u;
    const std::uint32_t base = (2u | (slot & 1u)) << footer_bits;
    return base + footer_value + 1;
}

// LSB-first bit packing for the near-uniform footer bits.
class BitPacker {
public:
    void reserve(std::size_t byte_count) {
        bytes_.reserve(byte_count);
    }

    void put(std::uint32_t value, std::uint32_t bits) {
        accumulator_ |= static_cast<std::uint64_t>(value) << filled_;
        filled_ += bits;
        while (filled_ >= 8) {
            bytes_.push_back(static_cast<std::uint8_t>(accumulator_ & 0xFFu));
            accumulator_ >>= 8;
            filled_ -= 8;
        }
    }

    std::size_t estimated_size() const {
        return bytes_.size() + (filled_ == 0 ? 0 : 1);
    }

    ByteVector finish() {
        if (filled_ > 0) {
            bytes_.push_back(static_cast<std::uint8_t>(accumulator_ & 0xFFu));
            accumulator_ = 0;
            filled_ = 0;
        }
        return std::move(bytes_);
    }

private:
    ByteVector bytes_;
    std::uint64_t accumulator_ = 0;
    std::uint32_t filled_ = 0;
};

class BitUnpacker {
public:
    explicit BitUnpacker(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    std::uint32_t get(std::uint32_t bits) {
        if (bits == 0) {
            return 0;
        }
        while (filled_ < bits) {
            std::uint64_t byte = 0;
            if (position_ < bytes_.size()) {
                byte = bytes_[position_++];
            } else {
                overrun_ = true;
            }
            accumulator_ |= byte << filled_;
            filled_ += 8;
        }
        const auto mask = (static_cast<std::uint64_t>(1) << bits) - 1;
        const auto value = static_cast<std::uint32_t>(accumulator_ & mask);
        accumulator_ >>= bits;
        filled_ -= bits;
        return value;
    }

    bool finish() const {
        if (overrun_ || position_ != bytes_.size() || filled_ >= 8) {
            return false;
        }
        if (filled_ == 0) {
            return true;
        }
        const auto mask = (std::uint64_t{1} << filled_) - 1;
        return (accumulator_ & mask) == 0;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0;
    std::uint64_t accumulator_ = 0;
    std::uint32_t filled_ = 0;
    bool overrun_ = false;
};

constexpr std::size_t kLiteralContextLanes = 8;
constexpr std::uint8_t kSequenceLiteralRaw = 0;
constexpr std::uint8_t kSequenceLiteralRepXor = 1;
constexpr std::uint8_t kSequenceLiteralMatchByte = 2;
constexpr std::uint8_t kSequenceLiteralFullPrevious = 3;
constexpr std::size_t kFullPreviousMaxClusters = 16;
constexpr std::uint8_t kSequenceCodesPlain = 0;
constexpr std::uint8_t kDirectValueCodes = 16;
constexpr std::uint8_t kMaximumValueCode = 43;  // direct 0..15, then 2^4..2^31

struct ValueCode {
    std::uint8_t code = 0;
    std::uint32_t extra_bits = 0;
    std::uint32_t extra_value = 0;
};

ValueCode encode_value_code(std::uint32_t value) {
    if (value < kDirectValueCodes) {
        return {static_cast<std::uint8_t>(value), 0, 0};
    }
    const auto bits = static_cast<std::uint32_t>(std::bit_width(value) - 1);
    const auto code = static_cast<std::uint8_t>(kDirectValueCodes + bits - 4);
    return {code, bits, value - (std::uint32_t{1} << bits)};
}

std::uint32_t decode_value_code(std::uint8_t code, BitUnpacker& extra) {
    if (code < kDirectValueCodes) {
        return code;
    }
    if (code > kMaximumValueCode) {
        throw FormatError("invalid sequence value code");
    }
    const auto bits = static_cast<std::uint32_t>(4 + code - kDirectValueCodes);
    return (std::uint32_t{1} << bits) + extra.get(bits);
}

void append_value_code(ByteVector& codes, BitPacker& extra, std::uint32_t value) {
    const auto encoded = encode_value_code(value);
    codes.push_back(encoded.code);
    extra.put(encoded.extra_value, encoded.extra_bits);
}

std::size_t literal_context_lane(std::span<const std::uint8_t> input,
                                 std::size_t position) {
    return position == 0 ? 0 : input[position - 1] >> 5;
}

void write_raw_blob(ByteVector& output, const ByteVector& bytes) {
    write_varuint(output, bytes.size());
    output.insert(output.end(), bytes.begin(), bytes.end());
}

std::span<const std::uint8_t> read_raw_blob(std::span<const std::uint8_t> encoded,
                                            std::size_t& cursor,
                                            std::size_t maximum_size) {
    const auto encoded_size = read_varuint(encoded, cursor);
    if (encoded_size > maximum_size || encoded_size > encoded.size() - cursor) {
        throw FormatError("sequence extra-bit stream exceeds block limit");
    }
    const auto size = static_cast<std::size_t>(encoded_size);
    const auto result = encoded.subspan(cursor, size);
    cursor += size;
    return result;
}

void write_sequence_code_stream(ByteVector& output,
                                std::span<const std::uint8_t> raw) {
    Lz77SplitStreams::Histogram plain_hist{};
    for (const auto symbol : raw) {
        ++plain_hist[symbol];
    }
    output.push_back(kSequenceCodesPlain);
    write_stream(output, raw, /*try_order1=*/false,
                 /*fast=*/true, /*prefer_rans=*/true, &plain_hist);
}

ByteVector read_sequence_code_stream(std::span<const std::uint8_t> encoded,
                                     std::size_t& cursor,
                                     std::size_t expected_size) {
    if (cursor >= encoded.size()) {
        throw FormatError("truncated sequence code mode");
    }
    const auto mode = encoded[cursor++];
    if (mode != kSequenceCodesPlain) {
        throw FormatError("unknown sequence code mode");
    }
    auto result = read_stream(encoded, cursor, expected_size);
    if (result.size() != expected_size) {
        throw FormatError("sequence code stream count mismatch");
    }
    return result;
}

struct SequenceLiteralRun {
    std::size_t position = 0;
    std::size_t length = 0;
    std::size_t rep0 = 1;
    bool has_match_context = false;
};

struct Lz77SequenceAnalysis {
    ByteVector literal_length_codes;
    ByteVector match_length_codes;
    ByteVector offset_codes;
    BitPacker literal_length_extra;
    BitPacker match_length_extra;
    BitPacker offset_extra;
    std::array<ByteVector, kLiteralContextLanes> raw_literals;
    std::array<ByteVector, kLiteralContextLanes> residual_literals;
    std::array<ByteVector, kLiteralContextLanes> match_base_literals;
    std::array<ByteVector, kLiteralContextLanes> match_literals;
    std::vector<SequenceLiteralRun> literal_runs;
    std::array<Lz77SplitStreams::Histogram, kLiteralContextLanes> raw_histograms{};
    std::array<Lz77SplitStreams::Histogram, kLiteralContextLanes> residual_histograms{};
    std::array<Lz77SplitStreams::Histogram, kLiteralContextLanes> match_base_histograms{};
    std::array<Lz77SplitStreams::Histogram, kLiteralContextLanes> match_histograms{};
    std::size_t sequence_count = 0;
    std::size_t trailing_literals = 0;
    bool literals_materialized = false;
    bool has_checkpoints = false;
};

struct Lz77PayloadAnalysis {
    Lz77SplitStreams split;
    std::optional<Lz77SequenceAnalysis> sequence;
};

void append_split_bytes(ByteVector& output,
                        std::span<const std::uint8_t> bytes) {
    output.insert(output.end(), bytes.begin(), bytes.end());
}

Lz77PayloadAnalysis analyze_lz77_payload(std::span<const std::uint8_t> input,
                                         std::span<const std::uint8_t> lz77_payload,
                                         bool build_sequence,
                                         bool build_split = true) {
    Lz77PayloadAnalysis analysis;
    auto& split = analysis.split;
    if (build_split) {
        split.commands.reserve(std::max<std::size_t>(1, lz77_payload.size() / 8));
        split.literal_lengths.reserve(std::max<std::size_t>(1, lz77_payload.size() / 64));
        split.match_lengths.reserve(std::max<std::size_t>(1, lz77_payload.size() / 64));
        split.distances.reserve(std::max<std::size_t>(1, lz77_payload.size() / 64));
        split.literals.reserve(lz77_payload.size() / 2);
    }
    if (build_sequence) {
        analysis.sequence.emplace();
        constexpr std::size_t kMaterializeBothLimit = std::size_t{8} << 20;
        analysis.sequence->literals_materialized = input.size() <= kMaterializeBothLimit;
    }

    std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};
    std::array<std::size_t, kRepCount> stream_reps{1, 2, 3, 4};
    std::size_t output_position = 0;
    std::size_t decoded_position = 0;
    std::size_t pending_literals = 0;
    bool has_match_context = false;
    std::size_t cursor = 0;

    while (cursor < lz77_payload.size()) {
        const auto token = lz77_payload[cursor++];
        if (build_split) {
            split.commands.push_back(token);
        }

        if (token == kLiteralToken) {
            const auto length_start = cursor;
            const auto encoded_length = read_varuint(lz77_payload, cursor);
            if (build_split) {
                append_split_bytes(split.literal_lengths,
                                   lz77_payload.subspan(length_start, cursor - length_start));
            }
            if (encoded_length > lz77_payload.size() - cursor) {
                throw FormatError("literal run exceeds LZ77 payload");
            }
            const auto length = static_cast<std::size_t>(encoded_length);
            const auto literals = lz77_payload.subspan(cursor, length);
            if (build_split) {
                append_split_bytes(split.literals, literals);
            }

            if (analysis.sequence) {
                if (encoded_length > std::numeric_limits<std::uint32_t>::max() ||
                    length > input.size() - output_position) {
                    analysis.sequence.reset();
                } else {
                    auto& sequence = *analysis.sequence;
                    sequence.literal_runs.push_back(
                        {output_position, length, reps[0], has_match_context});
                    for (std::size_t i = 0; i < length; ++i) {
                        const auto position = output_position + i;
                        const auto literal = literals[i];
                        if (literal != input[position]) {
                            analysis.sequence.reset();
                            break;
                        }
                        const auto lane = literal_context_lane(input, position);
                        ++sequence.raw_histograms[lane][literal];
                        const auto predicted = reps[0] <= position
                                                   ? input[position - reps[0]]
                                                   : std::uint8_t{0};
                        const auto residual = static_cast<std::uint8_t>(literal ^ predicted);
                        ++sequence.residual_histograms[lane][residual];
                        const bool use_match_context =
                            i == 0 && has_match_context && reps[0] <= position;
                        const auto match_lane =
                            static_cast<std::size_t>(predicted >> 5);
                        if (use_match_context) {
                            ++sequence.match_histograms[match_lane][residual];
                        } else {
                            ++sequence.match_base_histograms[lane][literal];
                        }
                        if (sequence.literals_materialized) {
                            sequence.raw_literals[lane].push_back(literal);
                            sequence.residual_literals[lane].push_back(residual);
                            if (use_match_context) {
                                sequence.match_literals[match_lane].push_back(residual);
                            } else {
                                sequence.match_base_literals[lane].push_back(literal);
                            }
                        }
                    }
                    if (analysis.sequence) {
                        output_position += length;
                        pending_literals += length;
                        has_match_context = false;
                    }
                }
            }
            cursor += length;
            if (length > std::numeric_limits<std::size_t>::max() - decoded_position) {
                throw FormatError("LZ77 payload output position overflows");
            }
            decoded_position += length;
            continue;
        }

        if (token == kCheckpointToken) {
            std::array<std::size_t, kRepCount> checkpoint_reps{};
            const auto old_reps = stream_reps;
            for (auto& distance : checkpoint_reps) {
                if (build_split) {
                    if (cursor >= lz77_payload.size()) {
                        throw FormatError("truncated LZ77 parser checkpoint");
                    }
                    split.match_lengths.push_back(lz77_payload[cursor]);
                }
                if (cursor >= lz77_payload.size()) {
                    throw FormatError("truncated LZ77 parser checkpoint");
                }
                const auto descriptor = lz77_payload[cursor++];
                std::uint64_t encoded_distance = 0;
                if (descriptor < kRepCount) {
                    encoded_distance = old_reps[descriptor];
                } else if (descriptor == kCheckpointExplicit) {
                    const auto distance_start = cursor;
                    encoded_distance = read_varuint(lz77_payload, cursor);
                    if (build_split) {
                        append_split_bytes(
                            split.distances,
                            lz77_payload.subspan(distance_start,
                                                 cursor - distance_start));
                    }
                } else {
                    throw FormatError("invalid LZ77 parser checkpoint descriptor");
                }
                if (encoded_distance == 0 || encoded_distance > decoded_position ||
                    encoded_distance > std::numeric_limits<std::uint32_t>::max()) {
                    throw FormatError("invalid LZ77 parser checkpoint distance");
                }
                distance = static_cast<std::size_t>(encoded_distance);
            }
            if (analysis.sequence) {
                analysis.sequence->has_checkpoints = true;
                reps = checkpoint_reps;
                has_match_context = false;
            }
            stream_reps = checkpoint_reps;
            continue;
        }

        if (token != kMatchToken &&
            !(token >= kRepTokenBase && token < kRepTokenBase + kRepCount)) {
            throw FormatError("unknown LZ77 token while splitting streams");
        }

        const auto length_start = cursor;
        const auto encoded_match_length = read_varuint(lz77_payload, cursor);
        if (build_split) {
            append_split_bytes(split.match_lengths,
                               lz77_payload.subspan(length_start, cursor - length_start));
        }

        std::uint64_t encoded_distance = 0;
        if (token == kMatchToken) {
            const auto distance_start = cursor;
            encoded_distance = read_varuint(lz77_payload, cursor);
            if (build_split) {
                append_split_bytes(split.distances,
                                   lz77_payload.subspan(distance_start, cursor - distance_start));
            }
        }

        const auto match_output_position = decoded_position;
        if (encoded_match_length >
            std::numeric_limits<std::size_t>::max() - decoded_position) {
            throw FormatError("LZ77 payload output position overflows");
        }
        decoded_position += static_cast<std::size_t>(encoded_match_length);

        if (token == kMatchToken) {
            if (encoded_distance == 0 || encoded_distance > match_output_position ||
                encoded_distance > std::numeric_limits<std::uint32_t>::max()) {
                throw FormatError("invalid explicit LZ77 distance");
            }
            stream_reps = {static_cast<std::size_t>(encoded_distance),
                           stream_reps[0], stream_reps[1], stream_reps[2]};
        } else {
            const auto index = static_cast<std::size_t>(token - kRepTokenBase);
            if (stream_reps[index] > match_output_position) {
                throw FormatError("invalid LZ77 repeat distance");
            }
            const auto chosen = stream_reps[index];
            for (std::size_t i = index; i > 0; --i) {
                stream_reps[i] = stream_reps[i - 1];
            }
            stream_reps[0] = chosen;
        }

        if (!analysis.sequence) {
            continue;
        }

        auto& sequence = *analysis.sequence;
        if (encoded_match_length > std::numeric_limits<std::uint32_t>::max()) {
            analysis.sequence.reset();
            continue;
        }
        const auto match_length = static_cast<std::size_t>(encoded_match_length);
        if (match_length < kMinMatch ||
            match_length > input.size() - output_position ||
            match_length - kMinMatch > std::numeric_limits<std::uint32_t>::max()) {
            analysis.sequence.reset();
            continue;
        }

        if (token == kMatchToken) {
            if (encoded_distance == 0 || encoded_distance > output_position ||
                encoded_distance > std::numeric_limits<std::uint32_t>::max()) {
                analysis.sequence.reset();
                continue;
            }
            const auto distance = static_cast<std::uint32_t>(encoded_distance);
            std::uint32_t footer_bits = 0;
            std::uint32_t footer_value = 0;
            const auto slot = distance_to_slot(distance, footer_bits, footer_value);
            sequence.offset_codes.push_back(static_cast<std::uint8_t>(kRepCount + slot));
            sequence.offset_extra.put(footer_value, footer_bits);
            reps = {static_cast<std::size_t>(distance), reps[0], reps[1], reps[2]};
        } else {
            const auto index = static_cast<std::size_t>(token - kRepTokenBase);
            if (reps[index] > output_position) {
                analysis.sequence.reset();
                continue;
            }
            sequence.offset_codes.push_back(static_cast<std::uint8_t>(index));
            const auto chosen = reps[index];
            for (std::size_t i = index; i > 0; --i) {
                reps[i] = reps[i - 1];
            }
            reps[0] = chosen;
        }

        append_value_code(sequence.literal_length_codes, sequence.literal_length_extra,
                          static_cast<std::uint32_t>(pending_literals));
        append_value_code(sequence.match_length_codes, sequence.match_length_extra,
                          static_cast<std::uint32_t>(match_length - kMinMatch));
        pending_literals = 0;
        output_position += match_length;
        has_match_context = true;
        ++sequence.sequence_count;
    }

    if (analysis.sequence) {
        auto& sequence = *analysis.sequence;
        if (output_position != input.size() ||
            sequence.sequence_count != sequence.literal_length_codes.size() ||
            sequence.sequence_count != sequence.match_length_codes.size() ||
            sequence.sequence_count != sequence.offset_codes.size()) {
            analysis.sequence.reset();
        } else {
            sequence.trailing_literals = pending_literals;
        }
    }
    return analysis;
}

Lz77SplitStreams split_lz77_payload(std::span<const std::uint8_t> lz77_payload) {
    return analyze_lz77_payload({}, lz77_payload, /*build_sequence=*/false,
                                /*build_split=*/true).split;
}

std::size_t histogram_size(const Lz77SplitStreams::Histogram& histogram) {
    std::uint64_t total = 0;
    for (const auto count : histogram) {
        total += count;
    }
    if (total > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("literal histogram exceeds addressable size");
    }
    return static_cast<std::size_t>(total);
}

struct FullPreviousLiteralStreams {
    std::array<std::uint8_t, 256> context_map{};
    std::vector<ByteVector> streams;
};

std::optional<FullPreviousLiteralStreams> cluster_full_previous_literals(
    std::span<const std::uint8_t> input,
    std::span<const SequenceLiteralRun> runs,
    std::size_t incumbent_size) {
    constexpr std::size_t kMinimumLiterals = 4096;
    static thread_local std::vector<std::uint32_t> joint;
    joint.assign(256 * 256, 0);
    std::array<std::uint64_t, 256> context_total{};
    std::size_t literal_total = 0;
    for (const auto& run : runs) {
        for (std::size_t i = 0; i < run.length; ++i) {
            const auto position = run.position + i;
            const auto context = position == 0 ? std::uint8_t{0}
                                               : input[position - 1];
            const auto symbol = input[position];
            ++joint[(static_cast<std::size_t>(context) << 8) | symbol];
            ++context_total[context];
            ++literal_total;
        }
    }
    if (literal_total < kMinimumLiterals) {
        return std::nullopt;
    }

    // The unclustered conditional entropy is an optimistic lower bound. If it
    // cannot beat the already-serialized winner after the context map alone,
    // no clustered representation can win, so skip all k-means/table work.
    double conditional_bits = 0.0;
    std::vector<std::size_t> occupied;
    occupied.reserve(256);
    for (std::size_t context = 0; context < 256; ++context) {
        if (context_total[context] == 0) {
            continue;
        }
        occupied.push_back(context);
        const auto total = static_cast<double>(context_total[context]);
        const auto* row = &joint[context << 8];
        for (std::size_t symbol = 0; symbol < 256; ++symbol) {
            if (row[symbol] != 0) {
                const auto count = static_cast<double>(row[symbol]);
                conditional_bits -= count * std::log2(count / total);
            }
        }
    }
    constexpr std::size_t kFixedFraming = 1 + 1 + 256;
    const auto optimistic_size = kFixedFraming +
        static_cast<std::size_t>(std::ceil(conditional_bits / 8.0));
    if (optimistic_size >= incumbent_size || occupied.empty()) {
        return std::nullopt;
    }

    auto cluster_count = std::min(kFullPreviousMaxClusters, occupied.size());
    std::vector<std::size_t> seeds(occupied);
    std::partial_sort(
        seeds.begin(), seeds.begin() + static_cast<std::ptrdiff_t>(cluster_count),
        seeds.end(), [&](std::size_t left, std::size_t right) {
            return context_total[left] > context_total[right];
        });
    seeds.resize(cluster_count);

    FullPreviousLiteralStreams result;
    std::vector<std::uint64_t> cluster_hist(cluster_count * 256, 0);
    for (std::size_t cluster = 0; cluster < cluster_count; ++cluster) {
        const auto* row = &joint[seeds[cluster] << 8];
        for (std::size_t symbol = 0; symbol < 256; ++symbol) {
            cluster_hist[cluster * 256 + symbol] = row[symbol];
        }
    }

    std::vector<double> costs(cluster_count * 256);
    constexpr int kRefinePasses = 3;
    for (int pass = 0; pass < kRefinePasses; ++pass) {
        for (std::size_t cluster = 0; cluster < cluster_count; ++cluster) {
            std::uint64_t total = 0;
            for (std::size_t symbol = 0; symbol < 256; ++symbol) {
                total += cluster_hist[cluster * 256 + symbol];
            }
            const auto denominator = static_cast<double>(total + 256);
            for (std::size_t symbol = 0; symbol < 256; ++symbol) {
                const auto probability =
                    (cluster_hist[cluster * 256 + symbol] + 1) / denominator;
                costs[cluster * 256 + symbol] = -std::log2(probability);
            }
        }

        for (const auto context : occupied) {
            const auto* row = &joint[context << 8];
            std::size_t best_cluster = 0;
            double best_bits = std::numeric_limits<double>::infinity();
            for (std::size_t cluster = 0; cluster < cluster_count; ++cluster) {
                double bits = 0.0;
                const auto* cost = &costs[cluster * 256];
                for (std::size_t symbol = 0; symbol < 256; ++symbol) {
                    if (row[symbol] != 0) {
                        bits += row[symbol] * cost[symbol];
                    }
                }
                if (bits < best_bits) {
                    best_bits = bits;
                    best_cluster = cluster;
                }
            }
            result.context_map[context] =
                static_cast<std::uint8_t>(best_cluster);
        }

        std::fill(cluster_hist.begin(), cluster_hist.end(), 0);
        for (const auto context : occupied) {
            const auto* row = &joint[context << 8];
            auto* histogram =
                &cluster_hist[result.context_map[context] * 256];
            for (std::size_t symbol = 0; symbol < 256; ++symbol) {
                histogram[symbol] += row[symbol];
            }
        }
    }

    std::array<std::uint8_t, kFullPreviousMaxClusters> remap{};
    std::size_t live_clusters = 0;
    for (std::size_t cluster = 0; cluster < cluster_count; ++cluster) {
        const auto total = std::accumulate(
            cluster_hist.begin() + static_cast<std::ptrdiff_t>(cluster * 256),
            cluster_hist.begin() + static_cast<std::ptrdiff_t>((cluster + 1) * 256),
            std::uint64_t{0});
        if (total == 0) {
            continue;
        }
        if (live_clusters != cluster) {
            std::copy_n(&cluster_hist[cluster * 256], 256,
                        &cluster_hist[live_clusters * 256]);
        }
        remap[cluster] = static_cast<std::uint8_t>(live_clusters++);
    }
    for (const auto context : occupied) {
        result.context_map[context] = remap[result.context_map[context]];
    }
    cluster_count = live_clusters;
    result.streams.resize(cluster_count);
    for (std::size_t cluster = 0; cluster < cluster_count; ++cluster) {
        const auto total = std::accumulate(
            cluster_hist.begin() + static_cast<std::ptrdiff_t>(cluster * 256),
            cluster_hist.begin() + static_cast<std::ptrdiff_t>((cluster + 1) * 256),
            std::uint64_t{0});
        result.streams[cluster].reserve(static_cast<std::size_t>(total));
    }
    for (const auto& run : runs) {
        for (std::size_t i = 0; i < run.length; ++i) {
            const auto position = run.position + i;
            const auto context = position == 0 ? std::uint8_t{0}
                                               : input[position - 1];
            result.streams[result.context_map[context]].push_back(input[position]);
        }
    }
    return result;
}

void write_context_literal_streams(ByteVector& result,
                                   std::span<const std::uint8_t> input,
                                   Lz77SequenceAnalysis& analysis,
                                   core::TaskExecutor* executor) {
    auto estimate_literal_mode = [](const auto& histograms) {
        double bits = 0.0;
        for (const auto& histogram : histograms) {
            const auto size = histogram_size(histogram);
            const auto nonzero = static_cast<std::size_t>(std::count_if(
                histogram.begin(), histogram.end(), [](std::uint64_t count) {
                    return count != 0;
                }));
            bits += order0_bits_per_byte(histogram, size) * static_cast<double>(size);
            bits += static_cast<double>(nonzero * 16 + 24);
        }
        return bits;
    };

    const auto raw_score = estimate_literal_mode(analysis.raw_histograms);
    const auto residual_score = estimate_literal_mode(analysis.residual_histograms);
    const bool use_residual = residual_score < raw_score;
    const auto& selected_histograms = use_residual ? analysis.residual_histograms
                                                   : analysis.raw_histograms;
    const auto match_score = estimate_literal_mode(analysis.match_base_histograms) +
                             estimate_literal_mode(analysis.match_histograms);
    // The estimate is deliberately permissive: it only avoids building a
    // clearly losing second representation. Exact serialized bytes still make
    // the final decision whenever match-byte is remotely competitive.
    const auto selected_score = std::min(raw_score, residual_score);
    const bool try_match = match_score < selected_score;

    std::array<ByteVector, kLiteralContextLanes> selected_literals;
    std::array<ByteVector, kLiteralContextLanes> match_base_literals;
    std::array<ByteVector, kLiteralContextLanes> match_literals;
    if (analysis.literals_materialized) {
        selected_literals = use_residual ? std::move(analysis.residual_literals)
                                         : std::move(analysis.raw_literals);
        if (try_match) {
            match_base_literals = std::move(analysis.match_base_literals);
            match_literals = std::move(analysis.match_literals);
        }
    } else {
        for (std::size_t lane = 0; lane < kLiteralContextLanes; ++lane) {
            selected_literals[lane].reserve(histogram_size(selected_histograms[lane]));
            if (try_match) {
                match_base_literals[lane].reserve(
                    histogram_size(analysis.match_base_histograms[lane]));
                match_literals[lane].reserve(
                    histogram_size(analysis.match_histograms[lane]));
            }
        }
        for (const auto& run : analysis.literal_runs) {
            for (std::size_t i = 0; i < run.length; ++i) {
                const auto position = run.position + i;
                const auto literal = input[position];
                const auto lane = literal_context_lane(input, position);
                if (use_residual) {
                    const auto predicted = run.rep0 <= position
                                               ? input[position - run.rep0]
                                               : std::uint8_t{0};
                    selected_literals[lane].push_back(
                        static_cast<std::uint8_t>(literal ^ predicted));
                } else {
                    selected_literals[lane].push_back(literal);
                }

                if (try_match) {
                    const bool use_match_context =
                        i == 0 && run.has_match_context && run.rep0 <= position;
                    const auto predicted = use_match_context
                                               ? input[position - run.rep0]
                                               : std::uint8_t{0};
                    if (use_match_context) {
                        const auto match_lane =
                            static_cast<std::size_t>(predicted >> 5);
                        match_literals[match_lane].push_back(
                            static_cast<std::uint8_t>(literal ^ predicted));
                    } else {
                        match_base_literals[lane].push_back(literal);
                    }
                }
            }
        }
    }

    auto encode_lane = [&](std::size_t candidate, std::size_t lane) {
        ByteVector encoded;
        const ByteVector* literals = &selected_literals[lane];
        const Lz77SplitStreams::Histogram* histogram = &selected_histograms[lane];
        if (candidate == 1) {
            literals = &match_base_literals[lane];
            histogram = &analysis.match_base_histograms[lane];
        } else if (candidate == 2) {
            literals = &match_literals[lane];
            histogram = &analysis.match_histograms[lane];
        }
        write_stream(encoded, *literals, /*try_order1=*/true,
                     /*fast=*/false, /*prefer_rans=*/false,
                     histogram, /*screen_order1=*/candidate != 0);
        return encoded;
    };

    std::array<ByteVector, kLiteralContextLanes> encoded_selected_lanes;
    std::array<ByteVector, kLiteralContextLanes> encoded_match_base_lanes;
    std::array<ByteVector, kLiteralContextLanes> encoded_match_lanes;
    const auto selected_literal_count = std::accumulate(
        selected_literals.begin(), selected_literals.end(), std::size_t{0},
        [](std::size_t total, const ByteVector& lane) { return total + lane.size(); });
    constexpr std::size_t kParallelLaneThreshold = std::size_t{256} << 10;
    if (executor != nullptr && selected_literal_count >= kParallelLaneThreshold) {
        std::array<std::future<ByteVector>, kLiteralContextLanes * 3> tasks;
        const auto task_count = try_match ? tasks.size() : kLiteralContextLanes;
        for (std::size_t index = 0; index < task_count; ++index) {
            tasks[index] = executor->submit([&, index] {
                return encode_lane(index / kLiteralContextLanes,
                                   index % kLiteralContextLanes);
            });
        }
        std::exception_ptr task_error;
        for (std::size_t index = 0; index < task_count; ++index) {
            try {
                auto* destination =
                    &encoded_selected_lanes[index % kLiteralContextLanes];
                if (index >= kLiteralContextLanes * 2) {
                    destination = &encoded_match_lanes[index - kLiteralContextLanes * 2];
                } else if (index >= kLiteralContextLanes) {
                    destination = &encoded_match_base_lanes[index - kLiteralContextLanes];
                }
                *destination = executor->wait(tasks[index]);
            } catch (...) {
                if (!task_error) {
                    task_error = std::current_exception();
                }
            }
        }
        if (task_error) {
            std::rethrow_exception(task_error);
        }
    } else {
        for (std::size_t lane = 0; lane < kLiteralContextLanes; ++lane) {
            encoded_selected_lanes[lane] = encode_lane(0, lane);
            if (try_match) {
                encoded_match_base_lanes[lane] = encode_lane(1, lane);
                encoded_match_lanes[lane] = encode_lane(2, lane);
            }
        }
    }

    auto serialize_lanes = [](std::uint8_t mode, const auto& lanes) {
        ByteVector encoded{mode};
        for (const auto& lane : lanes) {
            encoded.insert(encoded.end(), lane.begin(), lane.end());
        }
        return encoded;
    };
    auto selected = serialize_lanes(
        use_residual ? kSequenceLiteralRepXor : kSequenceLiteralRaw,
        encoded_selected_lanes);
    ByteVector matched;
    if (try_match) {
        matched = serialize_lanes(kSequenceLiteralMatchByte,
                                  encoded_match_base_lanes);
        for (const auto& lane : encoded_match_lanes) {
            matched.insert(matched.end(), lane.begin(), lane.end());
        }
    }
    // The new representation is format-visible only when its complete suffix,
    // including all lane tables and framing, beats the previous winner.
    const ByteVector* winner =
        try_match && matched.size() < selected.size() ? &matched : &selected;

    ByteVector full_previous;
    if (auto clustered = cluster_full_previous_literals(
            input, analysis.literal_runs, winner->size())) {
        full_previous.push_back(kSequenceLiteralFullPrevious);
        full_previous.push_back(
            static_cast<std::uint8_t>(clustered->streams.size()));
        full_previous.insert(full_previous.end(), clustered->context_map.begin(),
                             clustered->context_map.end());

        std::vector<ByteVector> encoded_clusters(clustered->streams.size());
        auto encode_cluster = [&](std::size_t cluster) {
            ByteVector encoded;
            write_stream(encoded, clustered->streams[cluster],
                         /*try_order1=*/false, /*fast=*/false,
                         /*prefer_rans=*/true);
            return encoded;
        };
        if (executor != nullptr && selected_literal_count >= kParallelLaneThreshold &&
            clustered->streams.size() > 1) {
            std::vector<std::future<ByteVector>> tasks(clustered->streams.size());
            for (std::size_t cluster = 0; cluster < tasks.size(); ++cluster) {
                tasks[cluster] = executor->submit([&, cluster] {
                    return encode_cluster(cluster);
                });
            }
            std::exception_ptr task_error;
            for (std::size_t cluster = 0; cluster < tasks.size(); ++cluster) {
                try {
                    encoded_clusters[cluster] = executor->wait(tasks[cluster]);
                } catch (...) {
                    if (!task_error) {
                        task_error = std::current_exception();
                    }
                }
            }
            if (task_error) {
                std::rethrow_exception(task_error);
            }
        } else {
            for (std::size_t cluster = 0; cluster < encoded_clusters.size(); ++cluster) {
                encoded_clusters[cluster] = encode_cluster(cluster);
            }
        }
        for (const auto& cluster : encoded_clusters) {
            full_previous.insert(full_previous.end(), cluster.begin(), cluster.end());
        }
        if (full_previous.size() < winner->size()) {
            winner = &full_previous;
        }
    }

    // Every format-visible mode competes as a complete suffix, including its
    // map, tables, streams, and framing.
    result.insert(result.end(), winner->begin(), winner->end());
}

ByteVector encode_sequence_analysis(std::span<const std::uint8_t> input,
                                    Lz77SequenceAnalysis analysis,
                                    core::TaskExecutor* executor) {
    ByteVector common;
    write_varuint(common, analysis.sequence_count);
    write_varuint(common, analysis.trailing_literals);
    write_sequence_code_stream(common, analysis.literal_length_codes);
    write_sequence_code_stream(common, analysis.match_length_codes);
    write_sequence_code_stream(common, analysis.offset_codes);
    write_raw_blob(common, analysis.literal_length_extra.finish());
    write_raw_blob(common, analysis.match_length_extra.finish());
    write_raw_blob(common, analysis.offset_extra.finish());

    ByteVector result = std::move(common);
    write_context_literal_streams(result, input, analysis, executor);
    return result;
}

double estimate_legacy_split_size(const Lz77SplitStreams& streams,
                                  const Lz77SequenceAnalysis& sequence,
                                  bool fast) {
    auto estimate_stream = [&](std::span<const std::uint8_t> raw, bool try_order1) {
        auto bits = order0_bits_per_byte(raw);
        if (try_order1 && !raw.empty()) {
            bits = std::min(bits, sampled_order1_bits_per_byte(raw));
        }
        return stream_entropy_lower_bound(bits, raw.size());
    };

    Lz77SplitStreams::Histogram literal_histogram{};
    for (const auto& lane : sequence.raw_histograms) {
        for (std::size_t symbol = 0; symbol < literal_histogram.size(); ++symbol) {
            literal_histogram[symbol] += lane[symbol];
        }
    }
    auto literal_bits = order0_bits_per_byte(literal_histogram, streams.literals.size());
    if (!streams.literals.empty()) {
        literal_bits = std::min(literal_bits,
                                sampled_order1_bits_per_byte(streams.literals));
    }
    const auto shared = estimate_stream(streams.commands, !fast) +
                        estimate_stream(streams.literal_lengths, !fast) +
                        estimate_stream(streams.match_lengths, !fast) +
                        stream_entropy_lower_bound(literal_bits, streams.literals.size());
    const auto plain = shared + estimate_stream(streams.distances, false);

    ByteVector distance_slots;
    distance_slots.reserve(streams.distances.size());
    BitPacker extra;
    std::size_t cursor = 0;
    while (cursor < streams.distances.size()) {
        const auto encoded_distance = read_varuint(streams.distances, cursor);
        if (encoded_distance == 0 ||
            encoded_distance > std::numeric_limits<std::uint32_t>::max()) {
            return plain;
        }
        std::uint32_t footer_bits = 0;
        std::uint32_t footer_value = 0;
        const auto slot = distance_to_slot(static_cast<std::uint32_t>(encoded_distance),
                                           footer_bits, footer_value);
        distance_slots.push_back(static_cast<std::uint8_t>(slot));
        extra.put(footer_value, footer_bits);
    }
    const auto footer = extra.finish();
    const auto slotted = shared + estimate_stream(distance_slots, !fast) +
                         estimate_stream(footer, false);
    return std::min(plain, slotted);
}

double estimate_sequence_size(const Lz77SequenceAnalysis& sequence) {
    auto estimate_stream = [](std::span<const std::uint8_t> raw) {
        Lz77SplitStreams::Histogram histogram{};
        for (const auto byte : raw) {
            ++histogram[byte];
        }
        const auto entropy = stream_entropy_lower_bound(
            order0_bits_per_byte(histogram, raw.size()), raw.size());
        const auto stored = static_cast<double>(varuint_size(raw.size()) + 1 +
                                                varuint_size(raw.size()) + raw.size());
        return std::min(entropy, stored);
    };
    auto estimate_histogram_stream = [](const Lz77SplitStreams::Histogram& histogram) {
        const auto size = histogram_size(histogram);
        const auto entropy = stream_entropy_lower_bound(
            order0_bits_per_byte(histogram, size), size);
        const auto stored = static_cast<double>(varuint_size(size) + 1 +
                                                varuint_size(size) + size);
        return std::min(entropy, stored);
    };
    auto raw_blob_size = [](std::size_t size) {
        return static_cast<double>(varuint_size(size) + size);
    };

    double estimate = static_cast<double>(varuint_size(sequence.sequence_count) +
                                          varuint_size(sequence.trailing_literals));
    estimate += 1 + estimate_stream(sequence.literal_length_codes);
    estimate += 1 + estimate_stream(sequence.match_length_codes);
    estimate += 1 + estimate_stream(sequence.offset_codes);
    estimate += raw_blob_size(sequence.literal_length_extra.estimated_size());
    estimate += raw_blob_size(sequence.match_length_extra.estimated_size());
    estimate += raw_blob_size(sequence.offset_extra.estimated_size());
    estimate += 1;  // literal mode

    double raw_literals = 0.0;
    double residual_literals = 0.0;
    double match_literals = 0.0;
    for (std::size_t lane = 0; lane < kLiteralContextLanes; ++lane) {
        raw_literals += estimate_histogram_stream(sequence.raw_histograms[lane]);
        residual_literals += estimate_histogram_stream(sequence.residual_histograms[lane]);
        match_literals += estimate_histogram_stream(sequence.match_base_histograms[lane]);
        match_literals += estimate_histogram_stream(sequence.match_histograms[lane]);
    }
    return estimate + std::min({raw_literals, residual_literals, match_literals});
}

}  // namespace

ByteVector encode_lz77_split_streams(std::span<const std::uint8_t> lz77_payload, bool fast) {
    const auto streams = split_lz77_payload(lz77_payload);

    ByteVector output;
    write_stream(output, streams.commands, /*try_order1=*/false, fast, /*prefer_rans=*/fast);
    write_stream(output, streams.literal_lengths, /*try_order1=*/false, fast, /*prefer_rans=*/fast);
    write_stream(output, streams.match_lengths, /*try_order1=*/false, fast, /*prefer_rans=*/fast);
    write_stream(output, streams.distances, /*try_order1=*/false, fast);
    // Fast levels code literals with rANS, not order-1: order-1 wins a little
    // ratio on text but decodes bit-serially (~12 MB/s) and dominates decode
    // time, so the speed levels take the far faster rANS literal stream.
    write_stream(output, streams.literals, /*try_order1=*/true, fast, /*prefer_rans=*/fast);
    return output;
}

class StreamReconstructor {
public:
    explicit StreamReconstructor(std::span<std::uint8_t> output) : output_(output) {}

    std::size_t position() const {
        return out_;
    }

    void copy_literal(const ByteVector& literals,
                      std::size_t& literal_cursor,
                      std::size_t length) {
        if (length > literals.size() - literal_cursor) {
            throw FormatError("split literal stream underflow");
        }
        if (length > output_.size() - out_) {
            throw FormatError("split literal exceeds declared output size");
        }

        std::memcpy(output_.data() + out_, literals.data() + literal_cursor, length);
        out_ += length;
        literal_cursor += length;
    }

    void copy_match(std::size_t length, std::size_t distance) {
        copy_lz_match(output_, out_, distance, length, "invalid split match reference",
                      "split match exceeds declared output size");
    }

private:
    std::span<std::uint8_t> output_;
    std::size_t out_ = 0;
};

class SlotDistanceReader {
public:
    explicit SlotDistanceReader(std::span<const std::uint8_t> packed_footer)
        : packed_(packed_footer) {}

    SlotDistanceReader(std::span<const std::uint8_t> packed_high,
                       std::span<const std::uint8_t> contextual_low)
        : packed_(packed_high), contextual_low_(contextual_low), contextual_(true) {}

    std::size_t read(const ByteVector& distance_slots,
                     std::size_t& distance_slot_cursor) {
        if (distance_slot_cursor >= distance_slots.size()) {
            throw FormatError("distance slot stream underflow");
        }

        const std::uint32_t slot = distance_slots[distance_slot_cursor++];
        if (slot > 63) {
            throw FormatError("invalid distance slot");
        }
        const auto footer_bits = slot_footer_bits(slot);
        std::uint32_t footer_value = 0;
        if (!contextual_) {
            footer_value = packed_.get(footer_bits);
        } else if (footer_bits != 0) {
            if (contextual_cursor_ >= contextual_low_.size()) {
                throw FormatError("contextual distance footer underflow");
            }
            const auto low = static_cast<std::uint32_t>(
                contextual_low_[contextual_cursor_++]);
            const auto low_bits = std::min<std::uint32_t>(footer_bits, 4);
            if (low >= (std::uint32_t{1} << low_bits)) {
                throw FormatError("contextual distance footer exceeds slot width");
            }
            footer_value = low;
            if (footer_bits > 4) {
                footer_value |= packed_.get(footer_bits - 4) << 4;
            }
        }
        return static_cast<std::size_t>(slot_to_distance(slot, footer_value));
    }

    bool finish() const {
        return packed_.finish() &&
               (!contextual_ || contextual_cursor_ == contextual_low_.size());
    }

private:
    BitUnpacker packed_;
    std::span<const std::uint8_t> contextual_low_;
    std::size_t contextual_cursor_ = 0;
    bool contextual_ = false;
};

ByteVector contextual_footer_contexts(const ByteVector& distance_slots) {
    ByteVector contexts;
    contexts.reserve(distance_slots.size());
    for (const auto slot_byte : distance_slots) {
        const auto slot = static_cast<std::uint32_t>(slot_byte);
        if (slot > 63) {
            throw FormatError("invalid distance slot");
        }
        if (slot_footer_bits(slot) != 0) {
            contexts.push_back(slot_byte);
        }
    }
    return contexts;
}

void reconstruct_from_streams_into(const ByteVector& commands,
                                   const ByteVector& literal_lengths,
                                   const ByteVector& match_lengths,
                                   const ByteVector& distances,
                                   const ByteVector& literals,
                                   std::span<std::uint8_t> output) {
    std::size_t literal_length_cursor = 0;
    std::size_t match_length_cursor = 0;
    std::size_t distance_cursor = 0;
    std::size_t literal_cursor = 0;
    StreamReconstructor reconstructor(output);

    std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};

    for (const auto command : commands) {
        if (command == kLiteralToken) {
            const auto length = static_cast<std::size_t>(
                read_varuint(literal_lengths, literal_length_cursor));

            reconstructor.copy_literal(literals, literal_cursor, length);
        } else if (command == kMatchToken) {
            const auto length = static_cast<std::size_t>(
                read_varuint(match_lengths, match_length_cursor));
            const auto distance = static_cast<std::size_t>(
                read_varuint(distances, distance_cursor));

            reconstructor.copy_match(length, distance);
            reps = {distance, reps[0], reps[1], reps[2]};
        } else if (command >= kRepTokenBase && command < kRepTokenBase + kRepCount) {
            const auto index = static_cast<std::size_t>(command - kRepTokenBase);
            const auto length = static_cast<std::size_t>(
                read_varuint(match_lengths, match_length_cursor));
            const auto distance = reps[index];

            reconstructor.copy_match(length, distance);

            const auto chosen = reps[index];
            for (std::size_t j = index; j > 0; --j) {
                reps[j] = reps[j - 1];
            }
            reps[0] = chosen;
        } else {
            throw FormatError("unknown split-stream command");
        }
    }

    if (reconstructor.position() != output.size() ||
        literal_length_cursor != literal_lengths.size() ||
        match_length_cursor != match_lengths.size() ||
        distance_cursor != distances.size() ||
        literal_cursor != literals.size()) {
        throw FormatError("split streams did not consume exactly");
    }
}

void reconstruct_from_slot_streams_into(const ByteVector& commands,
                                        const ByteVector& literal_lengths,
                                        const ByteVector& match_lengths,
                                        const ByteVector& distance_slots,
                                        SlotDistanceReader& distance_reader,
                                        const ByteVector& literals,
                                        std::span<std::uint8_t> output) {
    std::size_t literal_length_cursor = 0;
    std::size_t match_length_cursor = 0;
    std::size_t distance_slot_cursor = 0;
    std::size_t literal_cursor = 0;
    StreamReconstructor reconstructor(output);

    std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};

    for (const auto command : commands) {
        if (command == kLiteralToken) {
            const auto length = static_cast<std::size_t>(
                read_varuint(literal_lengths, literal_length_cursor));
            reconstructor.copy_literal(literals, literal_cursor, length);
        } else if (command == kMatchToken) {
            const auto length = static_cast<std::size_t>(
                read_varuint(match_lengths, match_length_cursor));
            const auto distance =
                distance_reader.read(distance_slots, distance_slot_cursor);

            reconstructor.copy_match(length, distance);
            reps = {distance, reps[0], reps[1], reps[2]};
        } else if (command >= kRepTokenBase && command < kRepTokenBase + kRepCount) {
            const auto index = static_cast<std::size_t>(command - kRepTokenBase);
            const auto length = static_cast<std::size_t>(
                read_varuint(match_lengths, match_length_cursor));
            const auto distance = reps[index];

            reconstructor.copy_match(length, distance);

            const auto chosen = reps[index];
            for (std::size_t j = index; j > 0; --j) {
                reps[j] = reps[j - 1];
            }
            reps[0] = chosen;
        } else {
            throw FormatError("unknown split-stream command");
        }
    }

    if (reconstructor.position() != output.size() ||
        literal_length_cursor != literal_lengths.size() ||
        match_length_cursor != match_lengths.size() ||
        distance_slot_cursor != distance_slots.size() ||
        literal_cursor != literals.size() || !distance_reader.finish()) {
        throw FormatError("slot split streams did not consume exactly");
    }
}

void decode_lz77_split_streams_into(std::span<const std::uint8_t> encoded,
                                    std::span<std::uint8_t> output) {
    std::size_t cursor = 0;
    const auto stream_limit = decoded_stream_limit(output.size());
    const auto commands = read_stream(encoded, cursor, stream_limit);
    const auto literal_lengths = read_stream(encoded, cursor, stream_limit);
    const auto match_lengths = read_stream(encoded, cursor, stream_limit);
    const auto distances = read_stream(encoded, cursor, stream_limit);
    const auto literals = read_stream(encoded, cursor, stream_limit);

    if (cursor != encoded.size()) {
        throw FormatError("trailing bytes after split streams");
    }

    reconstruct_from_streams_into(commands, literal_lengths, match_lengths,
                                  distances, literals, output);
}

ByteVector decode_lz77_split_streams(std::span<const std::uint8_t> encoded,
                                     std::size_t output_size) {
    ByteVector output(output_size);
    decode_lz77_split_streams_into(encoded, output);
    return output;
}

std::optional<ByteVector> encode_lz77_split_streams_slots(
    std::span<const std::uint8_t> lz77_payload, bool fast) {
    const auto streams = split_lz77_payload(lz77_payload);

    // Transcode the varint distance stream into slot bytes plus packed footers.
    ByteVector distance_slots;
    distance_slots.reserve(streams.distances.size());
    BitPacker extra;
    extra.reserve(streams.distances.size());
    std::size_t cursor = 0;
    while (cursor < streams.distances.size()) {
        const auto distance = read_varuint(streams.distances, cursor);
        if (distance == 0 ||
            distance > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }

        std::uint32_t footer_bits = 0;
        std::uint32_t footer_value = 0;
        const auto slot = distance_to_slot(static_cast<std::uint32_t>(distance),
                                           footer_bits, footer_value);
        distance_slots.push_back(static_cast<std::uint8_t>(slot));
        extra.put(footer_value, footer_bits);
    }

    ByteVector output;
    write_stream(output, streams.commands, /*try_order1=*/false, fast, /*prefer_rans=*/fast);
    write_stream(output, streams.literal_lengths, /*try_order1=*/false, fast, /*prefer_rans=*/fast);
    write_stream(output, streams.match_lengths, /*try_order1=*/false, fast, /*prefer_rans=*/fast);
    write_stream(output, distance_slots, /*try_order1=*/false, fast, /*prefer_rans=*/fast);
    write_stream(output, extra.finish(), /*try_order1=*/false, fast);
    // Fast levels code literals with rANS, not order-1: order-1 wins a little
    // ratio on text but decodes bit-serially (~12 MB/s) and dominates decode
    // time, so the speed levels take the far faster rANS literal stream.
    write_stream(output, streams.literals, /*try_order1=*/true, fast, /*prefer_rans=*/fast);
    return output;
}

Lz77SplitPayloads encode_lz77_split_payloads(
    const Lz77SplitStreams& streams, bool fast, core::TaskExecutor* executor) {
    const auto* commands_hist = streams.has_histograms ? &streams.commands_hist : nullptr;
    const auto* literal_lengths_hist =
        streams.has_histograms ? &streams.literal_lengths_hist : nullptr;
    const auto* match_lengths_hist =
        streams.has_histograms ? &streams.match_lengths_hist : nullptr;
    const auto* distances_hist = streams.has_histograms ? &streams.distances_hist : nullptr;
    const auto* literals_hist = streams.has_histograms ? &streams.literals_hist : nullptr;

    // Transcode distances into slots + packed footers for the slot layout.
    ByteVector distance_slots;
    distance_slots.reserve(streams.distances.size());
    Lz77SplitStreams::Histogram distance_slots_hist{};
    const bool has_distance_slots_hist = streams.has_histograms;
    BitPacker extra;
    extra.reserve(streams.distances.size());
    BitPacker contextual_high;
    contextual_high.reserve(streams.distances.size());
    ByteVector contextual_symbols;
    ByteVector contextual_slots;
    contextual_symbols.reserve(streams.distances.size());
    contextual_slots.reserve(streams.distances.size());
    bool slots_ok = true;
    std::size_t cursor = 0;
    while (cursor < streams.distances.size()) {
        const auto distance = read_varuint(streams.distances, cursor);
        if (distance == 0 || distance > std::numeric_limits<std::uint32_t>::max()) {
            slots_ok = false;
            break;
        }
        std::uint32_t footer_bits = 0;
        std::uint32_t footer_value = 0;
        const auto slot = distance_to_slot(static_cast<std::uint32_t>(distance),
                                           footer_bits, footer_value);
        distance_slots.push_back(static_cast<std::uint8_t>(slot));
        if (has_distance_slots_hist) {
            ++distance_slots_hist[slot];
        }
        extra.put(footer_value, footer_bits);
        if (footer_bits != 0) {
            contextual_slots.push_back(static_cast<std::uint8_t>(slot));
            if (footer_bits <= 4) {
                contextual_symbols.push_back(static_cast<std::uint8_t>(footer_value));
            } else {
                contextual_symbols.push_back(
                    static_cast<std::uint8_t>(footer_value & 0x0Fu));
                contextual_high.put(footer_value >> 4, footer_bits - 4);
            }
        }
    }

    auto encode_stream = [fast](std::span<const std::uint8_t> raw,
                                bool try_order1,
                                bool prefer_rans,
                                const Lz77SplitStreams::Histogram* histogram) {
        ByteVector encoded;
        write_stream(encoded, raw, try_order1, fast, prefer_rans, histogram);
        return encoded;
    };

    ByteVector distance_extra;
    ByteVector contextual_high_extra;
    if (slots_ok) {
        distance_extra = extra.finish();
        contextual_high_extra = contextual_high.finish();
    }

    ByteVector commands_encoded;
    ByteVector literal_lengths_encoded;
    ByteVector match_lengths_encoded;
    ByteVector literals_encoded;
    ByteVector plain_distances;
    ByteVector slot_distances;
    ByteVector distance_slots_encoded;
    ByteVector distance_extra_encoded;
    std::optional<ByteVector> contextual_slot_distances;

    const auto raw_size = streams.commands.size() + streams.literal_lengths.size() +
                          streams.match_lengths.size() + streams.literals.size() +
                          streams.distances.size() + distance_slots.size() +
                          distance_extra.size() + contextual_high_extra.size() +
                          contextual_symbols.size();
    constexpr std::size_t kParallelStreamThreshold = std::size_t{256} << 10;
    if (executor != nullptr && raw_size >= kParallelStreamThreshold) {
        // Each entropy stream is independent and concatenation below retains
        // the format's original order. Cooperative waits let a block worker
        // execute sibling tasks instead of reserving a thread while idle.
        auto commands_task = executor->submit([&] {
            return encode_stream(streams.commands, !fast, fast, commands_hist);
        });
        auto literal_lengths_task = executor->submit([&] {
            return encode_stream(streams.literal_lengths, !fast, fast,
                                 literal_lengths_hist);
        });
        auto match_lengths_task = executor->submit([&] {
            return encode_stream(streams.match_lengths, !fast, fast,
                                 match_lengths_hist);
        });
        auto literals_task = executor->submit([&] {
            return encode_stream(streams.literals, true, fast, literals_hist);
        });
        auto plain_distances_task = executor->submit([&] {
            return encode_stream(streams.distances, false, false, distances_hist);
        });
        std::optional<std::future<ByteVector>> distance_slots_task;
        std::optional<std::future<ByteVector>> distance_extra_task;
        std::optional<std::future<std::optional<ByteVector>>> contextual_slots_task;
        if (slots_ok) {
            distance_slots_task.emplace(executor->submit([&] {
                return encode_stream(
                    distance_slots, !fast, fast,
                    has_distance_slots_hist ? &distance_slots_hist : nullptr);
            }));
            distance_extra_task.emplace(executor->submit([&] {
                return encode_stream(distance_extra, false, false, nullptr);
            }));
            contextual_slots_task.emplace(executor->submit([&] {
                auto align = entropy::encode_rans_contextual(
                    contextual_symbols, contextual_slots,
                    /*context_count=*/64, /*symbol_count=*/16);
                if (!align) {
                    return std::optional<ByteVector>{};
                }
                auto encoded = encode_stream(contextual_high_extra, false, false, nullptr);
                write_raw_blob(encoded, *align);
                return std::optional<ByteVector>{std::move(encoded)};
            }));
        }

        std::exception_ptr task_error;
        auto collect = [&](auto& task, auto& destination) {
            try {
                destination = executor->wait(task);
            } catch (...) {
                if (!task_error) {
                    task_error = std::current_exception();
                }
            }
        };
        // All tasks capture stream/local buffers by reference. Drain every
        // sibling before propagating a failure so those references stay valid.
        collect(commands_task, commands_encoded);
        collect(literal_lengths_task, literal_lengths_encoded);
        collect(match_lengths_task, match_lengths_encoded);
        collect(literals_task, literals_encoded);
        collect(plain_distances_task, plain_distances);
        if (distance_slots_task) {
            collect(*distance_slots_task, distance_slots_encoded);
        }
        if (distance_extra_task) {
            collect(*distance_extra_task, distance_extra_encoded);
        }
        if (contextual_slots_task) {
            collect(*contextual_slots_task, contextual_slot_distances);
        }
        if (task_error) {
            std::rethrow_exception(task_error);
        }
    } else {
        commands_encoded = encode_stream(streams.commands, !fast, fast, commands_hist);
        literal_lengths_encoded = encode_stream(streams.literal_lengths, !fast, fast,
                                                literal_lengths_hist);
        match_lengths_encoded = encode_stream(streams.match_lengths, !fast, fast,
                                              match_lengths_hist);
        literals_encoded = encode_stream(streams.literals, true, fast, literals_hist);
        plain_distances = encode_stream(streams.distances, false, false, distances_hist);
        if (slots_ok) {
            distance_slots_encoded = encode_stream(
                distance_slots, !fast, fast,
                has_distance_slots_hist ? &distance_slots_hist : nullptr);
            distance_extra_encoded =
                encode_stream(distance_extra, false, false, nullptr);
            if (auto align = entropy::encode_rans_contextual(
                    contextual_symbols, contextual_slots,
                    /*context_count=*/64, /*symbol_count=*/16)) {
                ByteVector contextual =
                    encode_stream(contextual_high_extra, false, false, nullptr);
                write_raw_blob(contextual, *align);
                contextual_slot_distances = std::move(contextual);
            }
        }
    }

    if (slots_ok) {
        slot_distances.reserve(distance_slots_encoded.size() +
                               distance_extra_encoded.size());
        slot_distances.insert(slot_distances.end(), distance_slots_encoded.begin(),
                              distance_slots_encoded.end());
        slot_distances.insert(slot_distances.end(), distance_extra_encoded.begin(),
                              distance_extra_encoded.end());
    }
    if (contextual_slot_distances) {
        ByteVector framed;
        framed.reserve(distance_slots_encoded.size() +
                       contextual_slot_distances->size());
        framed.insert(framed.end(), distance_slots_encoded.begin(),
                      distance_slots_encoded.end());
        framed.insert(framed.end(), contextual_slot_distances->begin(),
                      contextual_slot_distances->end());
        contextual_slot_distances = std::move(framed);
    }

    // Encode the streams the two layouts share exactly once. The sequence
    // streams carry previous-symbol structure (commands repeat in runs, length
    // varint bytes correlate with their predecessors), so the thorough levels
    // let the clustered order-1 coder compete for them too; it only wins a
    // stream when strictly smaller, and it decodes at rANS speed.
    ByteVector shared;
    shared.reserve(commands_encoded.size() + literal_lengths_encoded.size() +
                   match_lengths_encoded.size());
    shared.insert(shared.end(), commands_encoded.begin(), commands_encoded.end());
    shared.insert(shared.end(), literal_lengths_encoded.begin(),
                  literal_lengths_encoded.end());
    shared.insert(shared.end(), match_lengths_encoded.begin(), match_lengths_encoded.end());

    auto concat = [](const ByteVector& a, const ByteVector& b, const ByteVector& c) {
        ByteVector out;
        out.reserve(a.size() + b.size() + c.size());
        out.insert(out.end(), a.begin(), a.end());
        out.insert(out.end(), b.begin(), b.end());
        out.insert(out.end(), c.begin(), c.end());
        return out;
    };

    auto plain = concat(shared, plain_distances, literals_encoded);

    std::optional<ByteVector> slots;
    if (slots_ok) {
        // (An order-0 entropy lower bound used to prune the slot trial here;
        // with the order-1 coder now competing for the slot stream that bound
        // no longer limits what the trial can achieve, so it always runs.)
        slots = concat(shared, slot_distances, literals_encoded);
    }

    std::optional<ByteVector> contextual_slots_payload;
    if (contextual_slot_distances) {
        contextual_slots_payload =
            concat(shared, *contextual_slot_distances, literals_encoded);
    }

    return {std::move(plain), std::move(slots),
            std::move(contextual_slots_payload)};
}

Lz77SplitPayloads encode_lz77_split_payloads(
    std::span<const std::uint8_t> lz77_payload,
    bool fast,
    core::TaskExecutor* executor) {
    return encode_lz77_split_payloads(split_lz77_payload(lz77_payload), fast, executor);
}

Lz77ContextSplitPayloads encode_lz77_context_split_streams(
    std::span<const std::uint8_t> input,
    std::span<const std::uint8_t> lz77_payload,
    std::span<const std::uint8_t> slot_payload,
    std::span<const std::uint8_t> contextual_slot_payload,
    core::TaskExecutor* executor) {
    auto analysis = analyze_lz77_payload(input, lz77_payload, /*build_sequence=*/true,
                                         /*build_split=*/false);
    if (!analysis.sequence) {
        return {};
    }
    if (slot_payload.empty()) {
        return {};
    }

    ByteVector literal_suffix;
    write_context_literal_streams(literal_suffix, input, *analysis.sequence, executor);

    auto replace_literals = [&](std::span<const std::uint8_t> payload,
                                bool contextual_footer) -> std::optional<ByteVector> {
        if (payload.empty()) {
            return std::nullopt;
        }
        ByteVector result(payload.begin(), payload.end());
        std::size_t cursor = 0;
        // Both layouts begin with commands, literal lengths, match lengths,
        // slots, and one packed-footer stream. The v8 layout then carries its
        // contextual low-nibble payload as a bounded raw blob.
        for (unsigned stream = 0; stream < 5; ++stream) {
            (void)read_varuint(result, cursor);
            if (cursor >= result.size()) {
                throw FormatError("truncated generated split stream");
            }
            ++cursor;
            const auto payload_size =
                static_cast<std::size_t>(read_varuint(result, cursor));
            if (payload_size > result.size() - cursor) {
                throw FormatError("generated split stream exceeds payload");
            }
            cursor += payload_size;
        }
        if (contextual_footer) {
            (void)read_raw_blob(result, cursor, result.size());
        }
        result.resize(cursor);
        result.insert(result.end(), literal_suffix.begin(), literal_suffix.end());
        return result;
    };

    return {replace_literals(slot_payload, /*contextual_footer=*/false),
            replace_literals(contextual_slot_payload,
                             /*contextual_footer=*/true)};
}

std::optional<ByteVector> encode_lz77_checkpoint_context_streams(
    std::span<const std::uint8_t> input,
    std::span<const std::uint8_t> lz77_payload,
    core::TaskExecutor* executor) {
    auto split = encode_lz77_split_payloads(
        lz77_payload, /*fast=*/false, executor);
    if (!split.slots) {
        return std::nullopt;
    }
    const auto contextual = split.contextual_slots
        ? std::span<const std::uint8_t>(*split.contextual_slots)
        : std::span<const std::uint8_t>{};
    auto payloads = encode_lz77_context_split_streams(
        input, lz77_payload, *split.slots, contextual, executor);

    std::optional<ByteVector> winner;
    auto consider = [&](std::uint8_t mode, std::optional<ByteVector>& candidate) {
        if (!candidate) {
            return;
        }
        ByteVector framed;
        framed.reserve(candidate->size() + 1);
        framed.push_back(mode);
        framed.insert(framed.end(), candidate->begin(), candidate->end());
        if (!winner || framed.size() < winner->size()) {
            winner = std::move(framed);
        }
    };
    consider(0, payloads.slots);
    consider(1, payloads.contextual_slots);
    return winner;
}

std::optional<ByteVector> encode_lz77_sequence_streams(
    std::span<const std::uint8_t> input,
    std::span<const std::uint8_t> lz77_payload,
    bool fast,
    std::size_t maximum_useful_size,
    core::TaskExecutor* executor) {
    (void)fast;
    auto analysis = analyze_lz77_payload(input, lz77_payload, /*build_sequence=*/true,
                                         /*build_split=*/false);
    if (!analysis.sequence || analysis.sequence->has_checkpoints) {
        return std::nullopt;
    }
    constexpr double kSequenceRejectionMargin = 1.02;
    if (maximum_useful_size != std::numeric_limits<std::size_t>::max() &&
        estimate_sequence_size(*analysis.sequence) >=
            static_cast<double>(maximum_useful_size) * kSequenceRejectionMargin) {
        return std::nullopt;
    }
    return encode_sequence_analysis(input, std::move(*analysis.sequence), executor);
}

Lz77PayloadCandidates encode_lz77_payload_candidates(
    std::span<const std::uint8_t> input,
    std::span<const std::uint8_t> lz77_payload,
    bool fast,
    core::TaskExecutor* executor) {
    auto analysis = analyze_lz77_payload(input, lz77_payload, /*build_sequence=*/true,
                                         /*build_split=*/true);
    double estimated_legacy_size = 0.0;
    std::optional<ByteVector> sequence;
    if (analysis.sequence && !analysis.sequence->has_checkpoints) {
        estimated_legacy_size =
            estimate_legacy_split_size(analysis.split, *analysis.sequence, fast);
        constexpr double kSequenceEstimateTolerance = 1.07;
        if (estimate_sequence_size(*analysis.sequence) <
            estimated_legacy_size * kSequenceEstimateTolerance) {
            sequence = encode_sequence_analysis(input, std::move(*analysis.sequence), executor);
        }
    }
    std::optional<ByteVector> split;
    std::optional<ByteVector> slots;
    std::optional<ByteVector> contextual_slots;
    if (!sequence || static_cast<double>(sequence->size()) > estimated_legacy_size) {
        auto legacy = encode_lz77_split_payloads(analysis.split, fast, executor);
        split = std::move(legacy.split);
        slots = std::move(legacy.slots);
        contextual_slots = std::move(legacy.contextual_slots);
    }
    return {std::move(split), std::move(slots),
            std::move(contextual_slots), std::move(sequence)};
}

void decode_lz77_sequence_streams_into(std::span<const std::uint8_t> encoded,
                                       std::span<std::uint8_t> output) {
    std::size_t cursor = 0;
    const auto encoded_sequence_count = read_varuint(encoded, cursor);
    const auto encoded_trailing_literals = read_varuint(encoded, cursor);
    if (encoded_sequence_count > output.size() ||
        encoded_trailing_literals > output.size()) {
        throw FormatError("sequence counts exceed declared output size");
    }
    const auto sequence_count = static_cast<std::size_t>(encoded_sequence_count);
    const auto trailing_literals = static_cast<std::size_t>(encoded_trailing_literals);

    const auto literal_length_codes =
        read_sequence_code_stream(encoded, cursor, sequence_count);
    const auto match_length_codes =
        read_sequence_code_stream(encoded, cursor, sequence_count);
    const auto offset_codes =
        read_sequence_code_stream(encoded, cursor, sequence_count);

    const auto literal_length_extra = read_raw_blob(encoded, cursor, output.size());
    const auto match_length_extra = read_raw_blob(encoded, cursor, output.size());
    const auto offset_extra = read_raw_blob(encoded, cursor, output.size());
    if (cursor >= encoded.size()) {
        throw FormatError("truncated sequence literal mode");
    }
    const auto literal_mode = encoded[cursor++];
    if (literal_mode != kSequenceLiteralRaw &&
        literal_mode != kSequenceLiteralRepXor &&
        literal_mode != kSequenceLiteralMatchByte &&
        literal_mode != kSequenceLiteralFullPrevious) {
        throw FormatError("unknown sequence literal mode");
    }

    std::array<ByteVector, kLiteralContextLanes> literal_lanes;
    std::array<ByteVector, kLiteralContextLanes> match_literal_lanes;
    std::array<std::size_t, kLiteralContextLanes> literal_cursors{};
    std::array<std::size_t, kLiteralContextLanes> match_literal_cursors{};
    std::array<std::uint8_t, 256> full_context_map{};
    std::array<ByteVector, kFullPreviousMaxClusters> full_context_lanes;
    std::array<const std::uint8_t*, kFullPreviousMaxClusters> full_context_readers{};
    std::array<const std::uint8_t*, kFullPreviousMaxClusters> full_context_ends{};
    std::size_t full_context_cluster_count = 0;
    std::size_t literal_total = 0;
    if (literal_mode == kSequenceLiteralFullPrevious) {
        if (cursor >= encoded.size()) {
            throw FormatError("truncated full-context literal cluster count");
        }
        full_context_cluster_count =
            static_cast<std::size_t>(encoded[cursor++]);
        if (full_context_cluster_count == 0 ||
            full_context_cluster_count > kFullPreviousMaxClusters) {
            throw FormatError("invalid full-context literal cluster count");
        }
        if (encoded.size() - cursor < full_context_map.size()) {
            throw FormatError("truncated full-context literal map");
        }
        for (auto& cluster : full_context_map) {
            cluster = encoded[cursor++];
            if (cluster >= full_context_cluster_count) {
                throw FormatError("full-context literal maps to missing cluster");
            }
        }
        for (std::size_t cluster = 0; cluster < full_context_cluster_count;
             ++cluster) {
            auto& lane = full_context_lanes[cluster];
            lane = read_stream(encoded, cursor, output.size());
            if (lane.size() > output.size() - literal_total) {
                throw FormatError("sequence literals exceed declared output size");
            }
            literal_total += lane.size();
            full_context_readers[cluster] = lane.data();
            full_context_ends[cluster] = lane.empty()
                ? lane.data()
                : lane.data() + lane.size();
        }
    } else {
        for (auto& lane : literal_lanes) {
            lane = read_stream(encoded, cursor, output.size());
            if (lane.size() > output.size() - literal_total) {
                throw FormatError("sequence literals exceed declared output size");
            }
            literal_total += lane.size();
        }
        if (literal_mode == kSequenceLiteralMatchByte) {
            for (auto& lane : match_literal_lanes) {
                lane = read_stream(encoded, cursor, output.size());
                if (lane.size() > output.size() - literal_total) {
                    throw FormatError(
                        "sequence match literals exceed declared output size");
                }
                literal_total += lane.size();
            }
        }
    }
    if (cursor != encoded.size()) {
        throw FormatError("trailing bytes after sequence streams");
    }

    BitUnpacker literal_length_bits(literal_length_extra);
    BitUnpacker match_length_bits(match_length_extra);
    BitUnpacker offset_bits(offset_extra);
    std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};
    std::size_t out = 0;
    std::size_t literals_used = 0;

    auto copy_literals = [&](std::size_t length, bool has_match_context) {
        if (length > output.size() - out) {
            throw FormatError("sequence literal run exceeds declared output size");
        }
        if (literal_mode == kSequenceLiteralFullPrevious) {
            for (std::size_t i = 0; i < length; ++i) {
                const auto context = out == 0 ? std::uint8_t{0}
                                              : output[out - 1];
                const auto cluster = full_context_map[context];
                auto& reader = full_context_readers[cluster];
                if (reader == full_context_ends[cluster]) {
                    throw FormatError("sequence full-context literal underflow");
                }
                output[out++] = *reader++;
            }
            literals_used += length;
            return;
        }
        for (std::size_t i = 0; i < length; ++i) {
            const bool use_match_context =
                literal_mode == kSequenceLiteralMatchByte && i == 0 &&
                has_match_context && reps[0] <= out;
            const auto predicted = use_match_context
                                       ? output[out - reps[0]]
                                       : std::uint8_t{0};
            const auto lane = use_match_context
                                  ? static_cast<std::size_t>(predicted >> 5)
                                  : literal_context_lane(output, out);
            auto& lanes = use_match_context ? match_literal_lanes : literal_lanes;
            auto& cursors = use_match_context ? match_literal_cursors : literal_cursors;
            if (cursors[lane] >= lanes[lane].size()) {
                throw FormatError("sequence literal lane underflow");
            }
            auto literal = lanes[lane][cursors[lane]++];
            if (use_match_context) {
                literal = static_cast<std::uint8_t>(literal ^ predicted);
            } else if (literal_mode == kSequenceLiteralRepXor && reps[0] <= out) {
                literal = static_cast<std::uint8_t>(literal ^ output[out - reps[0]]);
            }
            output[out++] = literal;
            ++literals_used;
        }
    };

    for (std::size_t i = 0; i < sequence_count; ++i) {
        const auto literal_length = static_cast<std::size_t>(
            decode_value_code(literal_length_codes[i], literal_length_bits));
        copy_literals(literal_length, i != 0);

        const auto match_base = decode_value_code(match_length_codes[i], match_length_bits);
        if (match_base > std::numeric_limits<std::uint32_t>::max() - kMinMatch) {
            throw FormatError("sequence match length overflows");
        }
        const auto match_length = static_cast<std::size_t>(match_base + kMinMatch);

        const auto offset_code = offset_codes[i];
        std::size_t distance = 0;
        if (offset_code < kRepCount) {
            const auto index = static_cast<std::size_t>(offset_code);
            distance = reps[index];
            const auto chosen = reps[index];
            for (std::size_t j = index; j > 0; --j) {
                reps[j] = reps[j - 1];
            }
            reps[0] = chosen;
        } else {
            const auto slot = static_cast<std::uint32_t>(offset_code - kRepCount);
            if (slot > 63) {
                throw FormatError("invalid sequence offset code");
            }
            distance = slot_to_distance(slot, offset_bits.get(slot_footer_bits(slot)));
            reps = {distance, reps[0], reps[1], reps[2]};
        }
        copy_lz_match(output, out, distance, match_length,
                      "invalid sequence match reference",
                      "sequence match exceeds declared output size");
    }

    copy_literals(trailing_literals, sequence_count != 0);
    if (out != output.size() || literals_used != literal_total ||
        !literal_length_bits.finish() || !match_length_bits.finish() ||
        !offset_bits.finish()) {
        throw FormatError("sequence streams did not consume exactly");
    }
    if (literal_mode == kSequenceLiteralFullPrevious) {
        for (std::size_t cluster = 0; cluster < full_context_cluster_count;
             ++cluster) {
            if (full_context_readers[cluster] != full_context_ends[cluster]) {
                throw FormatError(
                    "sequence full-context literals did not consume exactly");
            }
        }
    } else {
        for (std::size_t lane = 0; lane < kLiteralContextLanes; ++lane) {
            if (literal_cursors[lane] != literal_lanes[lane].size() ||
                match_literal_cursors[lane] != match_literal_lanes[lane].size()) {
                throw FormatError("sequence literal lanes did not consume exactly");
            }
        }
    }
}

ByteVector decode_lz77_sequence_streams(std::span<const std::uint8_t> encoded,
                                        std::size_t output_size) {
    ByteVector output(output_size);
    decode_lz77_sequence_streams_into(encoded, output);
    return output;
}

void decode_lz77_context_split_streams_impl(
    std::span<const std::uint8_t> encoded,
    std::span<std::uint8_t> output,
    bool contextual_footer,
    bool allow_checkpoints) {
    std::size_t cursor = 0;
    const auto stream_limit = decoded_stream_limit(output.size());
    const auto commands = read_stream(encoded, cursor, stream_limit);
    const auto literal_lengths = read_stream(encoded, cursor, stream_limit);
    const auto match_lengths = read_stream(encoded, cursor, stream_limit);
    const auto distance_slots = read_stream(encoded, cursor, stream_limit);
    const auto distance_packed = read_stream(encoded, cursor, stream_limit);

    ByteVector contextual_low;
    if (contextual_footer) {
        const auto contextual_payload = read_raw_blob(encoded, cursor, stream_limit);
        const auto contexts = contextual_footer_contexts(distance_slots);
        contextual_low = entropy::decode_rans_contextual(
            contextual_payload, contexts,
            /*context_count=*/64, /*symbol_count=*/16);
    }

    if (cursor >= encoded.size()) {
        throw FormatError("truncated context-split literal mode");
    }
    const auto literal_mode = encoded[cursor++];
    if (literal_mode != kSequenceLiteralRaw &&
        literal_mode != kSequenceLiteralRepXor &&
        literal_mode != kSequenceLiteralMatchByte &&
        literal_mode != kSequenceLiteralFullPrevious) {
        throw FormatError("unknown context-split literal mode");
    }

    std::array<ByteVector, kLiteralContextLanes> literal_lanes;
    std::array<ByteVector, kLiteralContextLanes> match_literal_lanes;
    std::array<std::size_t, kLiteralContextLanes> literal_cursors{};
    std::array<std::size_t, kLiteralContextLanes> match_literal_cursors{};
    std::array<std::uint8_t, 256> full_context_map{};
    std::array<ByteVector, kFullPreviousMaxClusters> full_context_lanes;
    std::array<const std::uint8_t*, kFullPreviousMaxClusters> full_context_readers{};
    std::array<const std::uint8_t*, kFullPreviousMaxClusters> full_context_ends{};
    std::size_t full_context_cluster_count = 0;
    std::size_t literal_total = 0;
    if (literal_mode == kSequenceLiteralFullPrevious) {
        if (cursor >= encoded.size()) {
            throw FormatError("truncated full-context literal cluster count");
        }
        full_context_cluster_count =
            static_cast<std::size_t>(encoded[cursor++]);
        if (full_context_cluster_count == 0 ||
            full_context_cluster_count > kFullPreviousMaxClusters) {
            throw FormatError("invalid full-context literal cluster count");
        }
        if (encoded.size() - cursor < full_context_map.size()) {
            throw FormatError("truncated full-context literal map");
        }
        for (auto& cluster : full_context_map) {
            cluster = encoded[cursor++];
            if (cluster >= full_context_cluster_count) {
                throw FormatError("full-context literal maps to missing cluster");
            }
        }
        for (std::size_t cluster = 0; cluster < full_context_cluster_count;
             ++cluster) {
            auto& lane = full_context_lanes[cluster];
            lane = read_stream(encoded, cursor, output.size());
            if (lane.size() > output.size() - literal_total) {
                throw FormatError("context-split literals exceed declared output size");
            }
            literal_total += lane.size();
            full_context_readers[cluster] = lane.data();
            full_context_ends[cluster] = lane.empty()
                ? lane.data()
                : lane.data() + lane.size();
        }
    } else {
        for (auto& lane : literal_lanes) {
            lane = read_stream(encoded, cursor, output.size());
            if (lane.size() > output.size() - literal_total) {
                throw FormatError("context-split literals exceed declared output size");
            }
            literal_total += lane.size();
        }
        if (literal_mode == kSequenceLiteralMatchByte) {
            for (auto& lane : match_literal_lanes) {
                lane = read_stream(encoded, cursor, output.size());
                if (lane.size() > output.size() - literal_total) {
                    throw FormatError(
                        "context-split match literals exceed declared output size");
                }
                literal_total += lane.size();
            }
        }
    }
    if (cursor != encoded.size()) {
        throw FormatError("trailing bytes after context-split streams");
    }

    std::size_t literal_length_cursor = 0;
    std::size_t match_length_cursor = 0;
    std::size_t distance_slot_cursor = 0;
    std::size_t literals_used = 0;
    std::size_t out = 0;
    std::optional<SlotDistanceReader> distance_reader;
    if (contextual_footer) {
        distance_reader.emplace(distance_packed, contextual_low);
    } else {
        distance_reader.emplace(distance_packed);
    }
    std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};
    bool has_match_context = false;

    auto copy_literals = [&](std::size_t length) {
        if (length > output.size() - out) {
            throw FormatError("context-split literal exceeds declared output size");
        }
        if (literal_mode == kSequenceLiteralFullPrevious) {
            for (std::size_t i = 0; i < length; ++i) {
                const auto context = out == 0 ? std::uint8_t{0}
                                              : output[out - 1];
                const auto cluster = full_context_map[context];
                auto& reader = full_context_readers[cluster];
                if (reader == full_context_ends[cluster]) {
                    throw FormatError(
                        "context-split full-context literal underflow");
                }
                output[out++] = *reader++;
            }
            literals_used += length;
            return;
        }
        for (std::size_t i = 0; i < length; ++i) {
            const bool use_match_context =
                literal_mode == kSequenceLiteralMatchByte && i == 0 &&
                has_match_context && reps[0] <= out;
            const auto predicted = use_match_context
                                       ? output[out - reps[0]]
                                       : std::uint8_t{0};
            const auto lane = use_match_context
                                  ? static_cast<std::size_t>(predicted >> 5)
                                  : literal_context_lane(output, out);
            auto& lanes = use_match_context ? match_literal_lanes : literal_lanes;
            auto& cursors = use_match_context ? match_literal_cursors : literal_cursors;
            if (cursors[lane] >= lanes[lane].size()) {
                throw FormatError("context-split literal lane underflow");
            }
            auto literal = lanes[lane][cursors[lane]++];
            if (use_match_context) {
                literal = static_cast<std::uint8_t>(literal ^ predicted);
            } else if (literal_mode == kSequenceLiteralRepXor && reps[0] <= out) {
                literal = static_cast<std::uint8_t>(literal ^ output[out - reps[0]]);
            }
            output[out++] = literal;
            ++literals_used;
        }
    };

    for (const auto command : commands) {
        if (command == kLiteralToken) {
            const auto length = static_cast<std::size_t>(
                read_varuint(literal_lengths, literal_length_cursor));
            copy_literals(length);
            if (length != 0) {
                has_match_context = false;
            }
        } else if (command == kMatchToken) {
            const auto length = static_cast<std::size_t>(
                read_varuint(match_lengths, match_length_cursor));
            const auto distance =
                distance_reader->read(distance_slots, distance_slot_cursor);
            copy_lz_match(output, out, distance, length,
                          "invalid context-split match reference",
                          "context-split match exceeds declared output size");
            reps = {distance, reps[0], reps[1], reps[2]};
            has_match_context = true;
        } else if (command >= kRepTokenBase &&
                   command < kRepTokenBase + kRepCount) {
            const auto index = static_cast<std::size_t>(command - kRepTokenBase);
            const auto length = static_cast<std::size_t>(
                read_varuint(match_lengths, match_length_cursor));
            const auto distance = reps[index];
            copy_lz_match(output, out, distance, length,
                          "invalid context-split rep reference",
                          "context-split rep exceeds declared output size");
            const auto chosen = reps[index];
            for (std::size_t j = index; j > 0; --j) {
                reps[j] = reps[j - 1];
            }
            reps[0] = chosen;
            has_match_context = true;
        } else if (command == kCheckpointToken && allow_checkpoints) {
            if (out == 0) {
                throw FormatError("parser checkpoint cannot precede output");
            }
            std::array<std::size_t, kRepCount> checkpoint_reps{};
            const auto old_reps = reps;
            for (auto& distance : checkpoint_reps) {
                if (match_length_cursor >= match_lengths.size()) {
                    throw FormatError("parser checkpoint descriptor underflow");
                }
                const auto descriptor = match_lengths[match_length_cursor++];
                if (descriptor < kRepCount) {
                    distance = old_reps[descriptor];
                } else if (descriptor == kCheckpointExplicit) {
                    distance = distance_reader->read(distance_slots,
                                                     distance_slot_cursor);
                } else {
                    throw FormatError("invalid parser checkpoint descriptor");
                }
                if (distance == 0 || distance > out) {
                    throw FormatError("invalid parser checkpoint distance");
                }
            }
            reps = checkpoint_reps;
            has_match_context = false;
        } else {
            throw FormatError("unknown context-split command");
        }
    }

    if (out != output.size() || literals_used != literal_total ||
        literal_length_cursor != literal_lengths.size() ||
        match_length_cursor != match_lengths.size() ||
        distance_slot_cursor != distance_slots.size() ||
        !distance_reader->finish()) {
        throw FormatError("context-split streams did not consume exactly");
    }
    if (literal_mode == kSequenceLiteralFullPrevious) {
        for (std::size_t cluster = 0; cluster < full_context_cluster_count;
             ++cluster) {
            if (full_context_readers[cluster] != full_context_ends[cluster]) {
                throw FormatError(
                    "context-split full-context literals did not consume exactly");
            }
        }
    } else {
        for (std::size_t lane = 0; lane < kLiteralContextLanes; ++lane) {
            if (literal_cursors[lane] != literal_lanes[lane].size() ||
                match_literal_cursors[lane] != match_literal_lanes[lane].size()) {
                throw FormatError(
                    "context-split literal lanes did not consume exactly");
            }
        }
    }
}

void decode_lz77_context_split_streams_into(
    std::span<const std::uint8_t> encoded,
    std::span<std::uint8_t> output) {
    decode_lz77_context_split_streams_impl(
        encoded, output, /*contextual_footer=*/false,
        /*allow_checkpoints=*/false);
}

void decode_lz77_contextual_slot_context_split_streams_into(
    std::span<const std::uint8_t> encoded,
    std::span<std::uint8_t> output) {
    decode_lz77_context_split_streams_impl(
        encoded, output, /*contextual_footer=*/true,
        /*allow_checkpoints=*/false);
}

void decode_lz77_checkpoint_context_streams_into(
    std::span<const std::uint8_t> encoded,
    std::span<std::uint8_t> output) {
    if (encoded.empty() || encoded.front() > 1) {
        throw FormatError("invalid parser-checkpoint footer mode");
    }
    decode_lz77_context_split_streams_impl(
        encoded.subspan(1), output,
        /*contextual_footer=*/encoded.front() != 0,
        /*allow_checkpoints=*/true);
}

void decode_lz77_split_streams_slots_into(std::span<const std::uint8_t> encoded,
                                          std::span<std::uint8_t> output) {
    std::size_t cursor = 0;
    const auto stream_limit = decoded_stream_limit(output.size());
    const auto commands = read_stream(encoded, cursor, stream_limit);
    const auto literal_lengths = read_stream(encoded, cursor, stream_limit);
    const auto match_lengths = read_stream(encoded, cursor, stream_limit);
    const auto distance_slots = read_stream(encoded, cursor, stream_limit);
    const auto distance_extra = read_stream(encoded, cursor, stream_limit);
    const auto literals = read_stream(encoded, cursor, stream_limit);

    if (cursor != encoded.size()) {
        throw FormatError("trailing bytes after slot split streams");
    }

    SlotDistanceReader distance_reader(distance_extra);
    reconstruct_from_slot_streams_into(commands, literal_lengths, match_lengths,
                                       distance_slots, distance_reader, literals, output);
}

void decode_lz77_contextual_slot_streams_into(
    std::span<const std::uint8_t> encoded,
    std::span<std::uint8_t> output) {
    std::size_t cursor = 0;
    const auto stream_limit = decoded_stream_limit(output.size());
    const auto commands = read_stream(encoded, cursor, stream_limit);
    const auto literal_lengths = read_stream(encoded, cursor, stream_limit);
    const auto match_lengths = read_stream(encoded, cursor, stream_limit);
    const auto distance_slots = read_stream(encoded, cursor, stream_limit);
    const auto distance_high = read_stream(encoded, cursor, stream_limit);
    const auto contextual_payload = read_raw_blob(encoded, cursor, stream_limit);
    const auto literals = read_stream(encoded, cursor, stream_limit);
    if (cursor != encoded.size()) {
        throw FormatError("trailing bytes after contextual slot streams");
    }

    const auto contexts = contextual_footer_contexts(distance_slots);
    const auto contextual_low = entropy::decode_rans_contextual(
        contextual_payload, contexts,
        /*context_count=*/64, /*symbol_count=*/16);
    SlotDistanceReader distance_reader(distance_high, contextual_low);
    reconstruct_from_slot_streams_into(commands, literal_lengths, match_lengths,
                                       distance_slots, distance_reader, literals, output);
}

ByteVector decode_lz77_split_streams_slots(std::span<const std::uint8_t> encoded,
                                           std::size_t output_size) {
    ByteVector output(output_size);
    decode_lz77_split_streams_slots_into(encoded, output);
    return output;
}

}  // namespace axiom::codec
