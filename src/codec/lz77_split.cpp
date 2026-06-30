#include "codec/lz77_split.hpp"

#include "codec/match_copy.hpp"
#include "codec/varint.hpp"
#include "entropy/huffman.hpp"
#include "entropy/range.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace axiom::codec {
namespace {

constexpr std::uint8_t kLiteralToken = 0;
constexpr std::uint8_t kMatchToken = 1;
constexpr std::uint8_t kRepTokenBase = 2;
constexpr std::size_t kRepCount = 4;

enum class StreamCodec : std::uint8_t {
    store = 0,
    huffman = 1,
    order1 = 2,
    rans = 3,
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
    std::vector<std::uint32_t> joint(256 * 256, 0);
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

void write_stream(ByteVector& output,
                  std::span<const std::uint8_t> raw,
                  bool try_order1 = false,
                  bool fast = false,
                  bool prefer_rans = false,
                  const Lz77SplitStreams::Histogram* histogram = nullptr) {
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
                    coded = consider(StreamCodec::order1, entropy::encode_order1(raw));
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

    // Order-1 modelling only pays off on the literal stream; the command,
    // length, and distance streams have little previous-symbol structure, so we
    // skip the expensive coder there. (It is also the slowest one to encode.)
    if (try_order1) {
        (void)consider(StreamCodec::order1, entropy::encode_order1(raw));
    }

    // rANS is the order-0 choice; the older bit-serial order-0 range stream is
    // intentionally unsupported while the format is still free to change.
    (void)consider(StreamCodec::rans,
                   histogram ? entropy::encode_rans(raw, *histogram)
                             : entropy::encode_rans(raw));

    output.push_back(static_cast<std::uint8_t>(codec));
    write_varuint(output, best_size);
    if (codec == StreamCodec::store) {
        output.insert(output.end(), raw.begin(), raw.end());
    } else {
        output.insert(output.end(), payload.begin(), payload.end());
    }
}

ByteVector read_stream(std::span<const std::uint8_t> encoded, std::size_t& cursor) {
    const auto raw_size = static_cast<std::size_t>(read_varuint(encoded, cursor));

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

    throw FormatError("unknown split-stream codec");
}

Lz77SplitStreams split_lz77_payload(std::span<const std::uint8_t> lz77_payload) {
    Lz77SplitStreams streams;
    std::size_t cursor = 0;

    while (cursor < lz77_payload.size()) {
        const auto token = lz77_payload[cursor++];
        streams.commands.push_back(token);

        if (token == kLiteralToken) {
            const auto length_start = cursor;
            const auto length = static_cast<std::size_t>(read_varuint(lz77_payload, cursor));
            streams.literal_lengths.insert(streams.literal_lengths.end(),
                                           lz77_payload.begin() + static_cast<std::ptrdiff_t>(length_start),
                                           lz77_payload.begin() + static_cast<std::ptrdiff_t>(cursor));

            if (length > lz77_payload.size() - cursor) {
                throw FormatError("literal run exceeds LZ77 payload");
            }

            streams.literals.insert(streams.literals.end(),
                                    lz77_payload.begin() + static_cast<std::ptrdiff_t>(cursor),
                                    lz77_payload.begin() + static_cast<std::ptrdiff_t>(cursor + length));
            cursor += length;
        } else if (token == kMatchToken) {
            const auto length_start = cursor;
            (void)read_varuint(lz77_payload, cursor);
            streams.match_lengths.insert(streams.match_lengths.end(),
                                         lz77_payload.begin() + static_cast<std::ptrdiff_t>(length_start),
                                         lz77_payload.begin() + static_cast<std::ptrdiff_t>(cursor));

            const auto distance_start = cursor;
            (void)read_varuint(lz77_payload, cursor);
            streams.distances.insert(streams.distances.end(),
                                     lz77_payload.begin() + static_cast<std::ptrdiff_t>(distance_start),
                                     lz77_payload.begin() + static_cast<std::ptrdiff_t>(cursor));
        } else if (token >= kRepTokenBase && token < kRepTokenBase + kRepCount) {
            // Repeat-offset matches carry only a length; the distance is implied
            // by the rep index held in the command byte.
            const auto length_start = cursor;
            (void)read_varuint(lz77_payload, cursor);
            streams.match_lengths.insert(streams.match_lengths.end(),
                                         lz77_payload.begin() + static_cast<std::ptrdiff_t>(length_start),
                                         lz77_payload.begin() + static_cast<std::ptrdiff_t>(cursor));
        } else {
            throw FormatError("unknown LZ77 token while splitting streams");
        }
    }

    return streams;
}

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
    void put(std::uint32_t value, std::uint32_t bits) {
        accumulator_ |= static_cast<std::uint64_t>(value) << filled_;
        filled_ += bits;
        while (filled_ >= 8) {
            bytes_.push_back(static_cast<std::uint8_t>(accumulator_ & 0xFFu));
            accumulator_ >>= 8;
            filled_ -= 8;
        }
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
            const std::uint64_t byte = position_ < bytes_.size() ? bytes_[position_++] : 0;
            accumulator_ |= byte << filled_;
            filled_ += 8;
        }
        const auto mask = (static_cast<std::uint64_t>(1) << bits) - 1;
        const auto value = static_cast<std::uint32_t>(accumulator_ & mask);
        accumulator_ >>= bits;
        filled_ -= bits;
        return value;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0;
    std::uint64_t accumulator_ = 0;
    std::uint32_t filled_ = 0;
};

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

std::size_t read_slot_distance(const ByteVector& distance_slots,
                               std::size_t& distance_slot_cursor,
                               BitUnpacker& extra) {
    if (distance_slot_cursor >= distance_slots.size()) {
        throw FormatError("distance slot stream underflow");
    }

    const std::uint32_t slot = distance_slots[distance_slot_cursor++];
    if (slot > 63) {
        throw FormatError("invalid distance slot");
    }

    const auto footer_value = extra.get(slot_footer_bits(slot));
    return static_cast<std::size_t>(slot_to_distance(slot, footer_value));
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
                                        const ByteVector& distance_extra,
                                        const ByteVector& literals,
                                        std::span<std::uint8_t> output) {
    std::size_t literal_length_cursor = 0;
    std::size_t match_length_cursor = 0;
    std::size_t distance_slot_cursor = 0;
    std::size_t literal_cursor = 0;
    BitUnpacker extra(distance_extra);
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
            const auto distance = read_slot_distance(distance_slots, distance_slot_cursor, extra);

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
        literal_cursor != literals.size()) {
        throw FormatError("slot split streams did not consume exactly");
    }
}

void decode_lz77_split_streams_into(std::span<const std::uint8_t> encoded,
                                    std::span<std::uint8_t> output) {
    std::size_t cursor = 0;
    const auto commands = read_stream(encoded, cursor);
    const auto literal_lengths = read_stream(encoded, cursor);
    const auto match_lengths = read_stream(encoded, cursor);
    const auto distances = read_stream(encoded, cursor);
    const auto literals = read_stream(encoded, cursor);

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
    BitPacker extra;
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

std::pair<ByteVector, std::optional<ByteVector>> encode_lz77_split_payloads(
    const Lz77SplitStreams& streams, bool fast) {
    const auto* commands_hist = streams.has_histograms ? &streams.commands_hist : nullptr;
    const auto* literal_lengths_hist =
        streams.has_histograms ? &streams.literal_lengths_hist : nullptr;
    const auto* match_lengths_hist =
        streams.has_histograms ? &streams.match_lengths_hist : nullptr;
    const auto* distances_hist = streams.has_histograms ? &streams.distances_hist : nullptr;
    const auto* literals_hist = streams.has_histograms ? &streams.literals_hist : nullptr;

    // Encode the streams the two layouts share exactly once.
    ByteVector shared;
    write_stream(shared, streams.commands, /*try_order1=*/false, fast, /*prefer_rans=*/fast,
                 commands_hist);
    write_stream(shared, streams.literal_lengths, /*try_order1=*/false, fast, /*prefer_rans=*/fast,
                 literal_lengths_hist);
    write_stream(shared, streams.match_lengths, /*try_order1=*/false, fast, /*prefer_rans=*/fast,
                 match_lengths_hist);

    ByteVector literals_encoded;
    write_stream(literals_encoded, streams.literals, /*try_order1=*/true, fast,
                 /*prefer_rans=*/fast, literals_hist);

    ByteVector plain_distances;
    write_stream(plain_distances, streams.distances, /*try_order1=*/false, fast,
                 /*prefer_rans=*/false, distances_hist);

    // Transcode distances into slots + packed footers for the slot layout.
    ByteVector distance_slots;
    Lz77SplitStreams::Histogram distance_slots_hist{};
    const bool has_distance_slots_hist = streams.has_histograms;
    BitPacker extra;
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
    }

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
        ByteVector slot_distances;
        write_stream(slot_distances, distance_slots, /*try_order1=*/false, fast,
                     /*prefer_rans=*/fast,
                     has_distance_slots_hist ? &distance_slots_hist : nullptr);
        write_stream(slot_distances, extra.finish(), /*try_order1=*/false, fast);
        slots = concat(shared, slot_distances, literals_encoded);
    }

    return {std::move(plain), std::move(slots)};
}

std::pair<ByteVector, std::optional<ByteVector>> encode_lz77_split_payloads(
    std::span<const std::uint8_t> lz77_payload, bool fast) {
    return encode_lz77_split_payloads(split_lz77_payload(lz77_payload), fast);
}

void decode_lz77_split_streams_slots_into(std::span<const std::uint8_t> encoded,
                                          std::span<std::uint8_t> output) {
    std::size_t cursor = 0;
    const auto commands = read_stream(encoded, cursor);
    const auto literal_lengths = read_stream(encoded, cursor);
    const auto match_lengths = read_stream(encoded, cursor);
    const auto distance_slots = read_stream(encoded, cursor);
    const auto distance_extra = read_stream(encoded, cursor);
    const auto literals = read_stream(encoded, cursor);

    if (cursor != encoded.size()) {
        throw FormatError("trailing bytes after slot split streams");
    }

    reconstruct_from_slot_streams_into(commands, literal_lengths, match_lengths,
                                       distance_slots, distance_extra, literals, output);
}

ByteVector decode_lz77_split_streams_slots(std::span<const std::uint8_t> encoded,
                                           std::size_t output_size) {
    ByteVector output(output_size);
    decode_lz77_split_streams_slots_into(encoded, output);
    return output;
}

}  // namespace axiom::codec
