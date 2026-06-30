#include "codec/fast_lz.hpp"

#include "codec/match_copy.hpp"
#include "codec/varint.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <vector>

namespace axiom::codec {
namespace {

constexpr std::size_t kHashBits = 16;
constexpr std::size_t kHashSize = std::size_t{1} << kHashBits;
// Level 1 favors throughput: two probes per hash row keep the matcher fast while
// staying well above gzip on ratio (measured ~2.73x on enwik8 vs ~2.82x at 4-way,
// for ~40% faster matching). Higher levels use the chain/tree matchers instead.
constexpr std::size_t kRowWays = 2;
constexpr std::uint32_t kInvalidPos = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kMinMatch = 4;
constexpr std::size_t kLongDistanceMinMatch = 6;
constexpr std::size_t kMaxFastMatch = 273;
constexpr std::uint8_t kLiteralToken = 0;
constexpr std::uint8_t kMatchToken = 1;
constexpr std::uint8_t kRepTokenBase = 2;
constexpr std::size_t kRepCount = 4;

using Row = std::array<std::uint32_t, kRowWays>;

std::uint32_t load_u32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::size_t hash4(const std::uint8_t* p) {
    return (load_u32_le(p) * 2654435761u) >> (32 - kHashBits);
}

std::size_t common_prefix(const std::uint8_t* left,
                          const std::uint8_t* right,
                          std::size_t limit) {
    std::size_t matched = 0;
    while (matched + 8 <= limit) {
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        std::memcpy(&a, left + matched, sizeof(a));
        std::memcpy(&b, right + matched, sizeof(b));
        const auto diff = a ^ b;
        if (diff == 0) {
            matched += 8;
            continue;
        }
        if constexpr (std::endian::native == std::endian::little) {
            return matched + (std::countr_zero(diff) / 8);
        } else {
            return matched + (std::countl_zero(diff) / 8);
        }
    }

    while (matched < limit && left[matched] == right[matched]) {
        ++matched;
    }
    return matched;
}

void remember(std::vector<Row>& table, std::span<const std::uint8_t> input, std::size_t pos) {
    if (pos + kMinMatch > input.size() || pos > kInvalidPos - 1) {
        return;
    }

    auto& row = table[hash4(input.data() + pos)];
    for (std::size_t i = kRowWays - 1; i > 0; --i) {
        row[i] = row[i - 1];
    }
    row[0] = static_cast<std::uint32_t>(pos);
}

void write_len(ByteVector& output, std::size_t value) {
    while (value >= 255) {
        output.push_back(255);
        value -= 255;
    }
    output.push_back(static_cast<std::uint8_t>(value));
}

std::size_t varuint_size(std::uint64_t value) {
    std::size_t size = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++size;
    }
    return size;
}

void append_counted(ByteVector& output,
                    Lz77SplitStreams::Histogram& histogram,
                    std::uint8_t byte) {
    output.push_back(byte);
    ++histogram[byte];
}

void write_varuint_counted(ByteVector& output,
                           Lz77SplitStreams::Histogram& histogram,
                           std::uint64_t value) {
    while (value >= 0x80) {
        append_counted(output, histogram, static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    append_counted(output, histogram, static_cast<std::uint8_t>(value));
}

void append_range_counted(ByteVector& output,
                          Lz77SplitStreams::Histogram& histogram,
                          std::span<const std::uint8_t> input) {
    output.reserve(output.size() + input.size());
    for (const auto byte : input) {
        append_counted(output, histogram, byte);
    }
}

std::size_t read_len(std::span<const std::uint8_t> encoded,
                     std::size_t& cursor,
                     std::size_t base) {
    std::size_t value = base;
    while (true) {
        if (cursor >= encoded.size()) {
            throw FormatError("truncated fast-lz length");
        }
        const auto byte = encoded[cursor++];
        if (value > std::numeric_limits<std::size_t>::max() - byte) {
            throw FormatError("fast-lz length overflow");
        }
        value += byte;
        if (byte != 255) {
            return value;
        }
    }
}

void write_distance(ByteVector& output, std::size_t distance) {
    if (distance == 0 || distance > 0xFFFFFFu) {
        throw FormatError("fast-lz distance out of range");
    }

    if (distance <= 0xFFFFu) {
        output.push_back(static_cast<std::uint8_t>(distance));
        output.push_back(static_cast<std::uint8_t>(distance >> 8));
        return;
    }

    // A zero 16-bit distance is impossible for a match, so it escapes to a
    // 24-bit distance. Short matches keep the cheaper two-byte form, while fast
    // mode still has enough reach to find repeated text beyond 64 KiB.
    output.push_back(0);
    output.push_back(0);
    output.push_back(static_cast<std::uint8_t>(distance));
    output.push_back(static_cast<std::uint8_t>(distance >> 8));
    output.push_back(static_cast<std::uint8_t>(distance >> 16));
}

std::size_t read_distance(std::span<const std::uint8_t> encoded, std::size_t& cursor) {
    if (encoded.size() - cursor < 2) {
        throw FormatError("truncated fast-lz distance");
    }

    auto distance = static_cast<std::size_t>(encoded[cursor]) |
                    (static_cast<std::size_t>(encoded[cursor + 1]) << 8);
    cursor += 2;
    if (distance != 0) {
        return distance;
    }

    if (encoded.size() - cursor < 3) {
        throw FormatError("truncated fast-lz long distance");
    }
    distance = static_cast<std::size_t>(encoded[cursor]) |
               (static_cast<std::size_t>(encoded[cursor + 1]) << 8) |
               (static_cast<std::size_t>(encoded[cursor + 2]) << 16);
    cursor += 3;
    if (distance == 0) {
        throw FormatError("invalid fast-lz distance");
    }
    return distance;
}

void emit_sequence(ByteVector& output,
                   std::span<const std::uint8_t> input,
                   std::size_t literal_start,
                   std::size_t match_start,
                   std::size_t match_length,
                   std::size_t distance) {
    const auto literal_length = match_start - literal_start;
    const auto literal_nibble = std::min<std::size_t>(literal_length, 15);
    const auto match_nibble = std::min<std::size_t>(match_length - kMinMatch, 15);
    output.push_back(static_cast<std::uint8_t>((literal_nibble << 4) | match_nibble));

    if (literal_nibble == 15) {
        write_len(output, literal_length - 15);
    }
    output.insert(output.end(),
                  input.begin() + static_cast<std::ptrdiff_t>(literal_start),
                  input.begin() + static_cast<std::ptrdiff_t>(match_start));

    write_distance(output, distance);
    if (match_nibble == 15) {
        write_len(output, match_length - kMinMatch - 15);
    }
}

void emit_final_literals(ByteVector& output,
                         std::span<const std::uint8_t> input,
                         std::size_t literal_start) {
    const auto literal_length = input.size() - literal_start;
    if (literal_length == 0) {
        return;
    }

    const auto literal_nibble = std::min<std::size_t>(literal_length, 15);
    output.push_back(static_cast<std::uint8_t>(literal_nibble << 4));
    if (literal_nibble == 15) {
        write_len(output, literal_length - 15);
    }
    output.insert(output.end(),
                  input.begin() + static_cast<std::ptrdiff_t>(literal_start),
                  input.end());
}

void emit_lz77_sequence(ByteVector& output,
                        std::span<const std::uint8_t> input,
                        std::size_t literal_start,
                        std::size_t match_start,
                        std::size_t match_length,
                        std::size_t distance) {
    const auto literal_length = match_start - literal_start;
    if (literal_length != 0) {
        output.push_back(kLiteralToken);
        write_varuint(output, literal_length);
        output.insert(output.end(),
                      input.begin() + static_cast<std::ptrdiff_t>(literal_start),
                      input.begin() + static_cast<std::ptrdiff_t>(match_start));
    }

    output.push_back(kMatchToken);
    write_varuint(output, match_length);
    write_varuint(output, distance);
}

void emit_lz77_rep_sequence(ByteVector& output,
                            std::span<const std::uint8_t> input,
                            std::size_t literal_start,
                            std::size_t match_start,
                            std::size_t match_length,
                            std::size_t rep_index) {
    const auto literal_length = match_start - literal_start;
    if (literal_length != 0) {
        output.push_back(kLiteralToken);
        write_varuint(output, literal_length);
        output.insert(output.end(),
                      input.begin() + static_cast<std::ptrdiff_t>(literal_start),
                      input.begin() + static_cast<std::ptrdiff_t>(match_start));
    }

    output.push_back(static_cast<std::uint8_t>(kRepTokenBase + rep_index));
    write_varuint(output, match_length);
}

void emit_lz77_final_literals(ByteVector& output,
                              std::span<const std::uint8_t> input,
                              std::size_t literal_start) {
    const auto literal_length = input.size() - literal_start;
    if (literal_length == 0) {
        return;
    }

    output.push_back(kLiteralToken);
    write_varuint(output, literal_length);
    output.insert(output.end(),
                  input.begin() + static_cast<std::ptrdiff_t>(literal_start),
                  input.end());
}

void copy_match(std::span<std::uint8_t> output,
                std::size_t& out,
                std::size_t distance,
                std::size_t length) {
    copy_lz_match(output, out, distance, length, "invalid fast-lz match",
                  "invalid fast-lz match");
}

}  // namespace

ByteVector encode_fast_lz(std::span<const std::uint8_t> input,
                          const CompressionOptions& options) {
    if (input.empty()) {
        return {};
    }
    if (input.size() > kInvalidPos - 1) {
        throw FormatError("fast-lz block exceeds position limit");
    }

    std::vector<Row> table(kHashSize);
    for (auto& row : table) {
        row.fill(kInvalidPos);
    }

    const auto max_distance = std::min<std::size_t>(
        std::max<std::size_t>(1, options.window_size), 0xFFFFFFu);
    const auto max_match = std::min<std::size_t>(
        std::max<std::size_t>(kMinMatch, options.max_match), kMaxFastMatch);

    ByteVector output;
    output.reserve(input.size());

    std::size_t literal_start = 0;
    std::size_t pos = 0;
    while (pos + kMinMatch <= input.size()) {
        const auto& row = table[hash4(input.data() + pos)];
        std::size_t best_length = 0;
        std::size_t best_distance = 0;

        for (const auto candidate32 : row) {
            if (candidate32 == kInvalidPos) {
                continue;
            }
            const auto candidate = static_cast<std::size_t>(candidate32);
            if (candidate >= pos) {
                continue;
            }
            const auto distance = pos - candidate;
            if (distance > max_distance) {
                continue;
            }
            if (load_u32_le(input.data() + candidate) != load_u32_le(input.data() + pos)) {
                continue;
            }

            const auto limit = std::min({max_match, input.size() - pos, input.size() - candidate});
            const auto length = common_prefix(input.data() + candidate, input.data() + pos, limit);
            if (length > best_length) {
                best_length = length;
                best_distance = distance;
                if (length >= options.nice_length) {
                    break;
                }
            }
        }

        remember(table, input, pos);

        const auto required = best_distance > 0xFFFFu ? kLongDistanceMinMatch : kMinMatch;
        if (best_length >= required) {
            emit_sequence(output, input, literal_start, pos, best_length, best_distance);

            const auto insert_end = std::min<std::size_t>(pos + best_length, pos + 16);
            for (std::size_t insert = pos + 1; insert < insert_end; ++insert) {
                remember(table, input, insert);
            }

            pos += best_length;
            literal_start = pos;
        } else {
            ++pos;
        }
    }

    emit_final_literals(output, input, literal_start);
    return output;
}

ByteVector encode_fast_lz77(std::span<const std::uint8_t> input,
                            const CompressionOptions& options) {
    if (input.empty()) {
        return {};
    }
    if (input.size() > kInvalidPos - 1) {
        throw FormatError("fast-lz block exceeds position limit");
    }

    std::vector<Row> table(kHashSize);
    for (auto& row : table) {
        row.fill(kInvalidPos);
    }

    const auto max_distance = std::min<std::size_t>(
        std::max<std::size_t>(1, options.window_size), 0xFFFFFFu);
    const auto max_match = std::min<std::size_t>(
        std::max<std::size_t>(kMinMatch, options.max_match), kMaxFastMatch);

    ByteVector output;
    output.reserve(input.size() / 2);

    std::size_t literal_start = 0;
    std::size_t pos = 0;
    std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};
    while (pos + kMinMatch <= input.size()) {
        const auto& row = table[hash4(input.data() + pos)];
        std::size_t best_length = 0;
        std::size_t best_distance = 0;

        for (const auto candidate32 : row) {
            if (candidate32 == kInvalidPos) {
                continue;
            }
            const auto candidate = static_cast<std::size_t>(candidate32);
            if (candidate >= pos) {
                continue;
            }
            const auto distance = pos - candidate;
            if (distance > max_distance) {
                continue;
            }
            if (load_u32_le(input.data() + candidate) != load_u32_le(input.data() + pos)) {
                continue;
            }

            const auto limit = std::min({max_match, input.size() - pos, input.size() - candidate});
            const auto length = common_prefix(input.data() + candidate, input.data() + pos, limit);
            if (length > best_length) {
                best_length = length;
                best_distance = distance;
                if (length >= options.nice_length) {
                    break;
                }
            }
        }

        std::size_t best_rep_length = 0;
        std::size_t best_rep_index = 0;
        const auto rep_limit = std::min(max_match, input.size() - pos);
        for (std::size_t i = 0; i < reps.size(); ++i) {
            const auto distance = reps[i];
            if (distance == 0 || distance > pos) {
                continue;
            }
            if (load_u32_le(input.data() + pos - distance) !=
                load_u32_le(input.data() + pos)) {
                continue;
            }
            const auto length = common_prefix(input.data() + pos - distance,
                                              input.data() + pos,
                                              rep_limit);
            if (length > best_rep_length) {
                best_rep_length = length;
                best_rep_index = i;
            }
        }

        remember(table, input, pos);

        const auto required = best_distance > 0xFFFFu ? kLongDistanceMinMatch : kMinMatch;
        const auto take_rep = best_rep_length >= kMinMatch && best_rep_length >= best_length;
        if (take_rep) {
            emit_lz77_rep_sequence(output, input, literal_start, pos,
                                   best_rep_length, best_rep_index);

            const auto chosen = reps[best_rep_index];
            for (std::size_t i = best_rep_index; i > 0; --i) {
                reps[i] = reps[i - 1];
            }
            reps[0] = chosen;

            const auto insert_end = std::min<std::size_t>(pos + best_rep_length, pos + 16);
            for (std::size_t insert = pos + 1; insert < insert_end; ++insert) {
                remember(table, input, insert);
            }

            pos += best_rep_length;
            literal_start = pos;
        } else if (best_length >= required) {
            emit_lz77_sequence(output, input, literal_start, pos, best_length, best_distance);

            reps = {best_distance, reps[0], reps[1], reps[2]};

            const auto insert_end = std::min<std::size_t>(pos + best_length, pos + 16);
            for (std::size_t insert = pos + 1; insert < insert_end; ++insert) {
                remember(table, input, insert);
            }

            pos += best_length;
            literal_start = pos;
        } else {
            ++pos;
        }
    }

    emit_lz77_final_literals(output, input, literal_start);
    return output;
}

void emit_lz77_split_sequence(std::size_t& lz77_size,
                              Lz77SplitStreams& streams,
                              std::span<const std::uint8_t> input,
                              std::size_t literal_start,
                              std::size_t match_start,
                              std::size_t match_length,
                              std::size_t distance) {
    const auto literal_length = match_start - literal_start;
    if (literal_length != 0) {
        lz77_size += 1 + varuint_size(literal_length) + literal_length;
        append_counted(streams.commands, streams.commands_hist, kLiteralToken);
        write_varuint_counted(streams.literal_lengths, streams.literal_lengths_hist,
                              literal_length);
        append_range_counted(
            streams.literals, streams.literals_hist,
            input.subspan(literal_start, literal_length));
    }

    lz77_size += 1 + varuint_size(match_length) + varuint_size(distance);
    append_counted(streams.commands, streams.commands_hist, kMatchToken);
    write_varuint_counted(streams.match_lengths, streams.match_lengths_hist, match_length);
    write_varuint_counted(streams.distances, streams.distances_hist, distance);
}

void emit_lz77_split_rep_sequence(std::size_t& lz77_size,
                                  Lz77SplitStreams& streams,
                                  std::span<const std::uint8_t> input,
                                  std::size_t literal_start,
                                  std::size_t match_start,
                                  std::size_t match_length,
                                  std::size_t rep_index) {
    const auto literal_length = match_start - literal_start;
    if (literal_length != 0) {
        lz77_size += 1 + varuint_size(literal_length) + literal_length;
        append_counted(streams.commands, streams.commands_hist, kLiteralToken);
        write_varuint_counted(streams.literal_lengths, streams.literal_lengths_hist,
                              literal_length);
        append_range_counted(
            streams.literals, streams.literals_hist,
            input.subspan(literal_start, literal_length));
    }

    lz77_size += 1 + varuint_size(match_length);
    append_counted(streams.commands, streams.commands_hist,
                   static_cast<std::uint8_t>(kRepTokenBase + rep_index));
    write_varuint_counted(streams.match_lengths, streams.match_lengths_hist, match_length);
}

void emit_lz77_split_final_literals(std::size_t& lz77_size,
                                    Lz77SplitStreams& streams,
                                    std::span<const std::uint8_t> input,
                                    std::size_t literal_start) {
    const auto literal_length = input.size() - literal_start;
    if (literal_length == 0) {
        return;
    }

    lz77_size += 1 + varuint_size(literal_length) + literal_length;
    append_counted(streams.commands, streams.commands_hist, kLiteralToken);
    write_varuint_counted(streams.literal_lengths, streams.literal_lengths_hist,
                          literal_length);
    append_range_counted(streams.literals, streams.literals_hist,
                         input.subspan(literal_start));
}

FastLz77SplitPayloads encode_fast_lz77_split_payloads(
    std::span<const std::uint8_t> input,
    const CompressionOptions& options) {
    if (input.empty()) {
        return {};
    }
    if (input.size() > kInvalidPos - 1) {
        throw FormatError("fast-lz block exceeds position limit");
    }

    std::vector<Row> table(kHashSize);
    for (auto& row : table) {
        row.fill(kInvalidPos);
    }

    const auto max_distance = std::min<std::size_t>(
        std::max<std::size_t>(1, options.window_size), 0xFFFFFFu);
    const auto max_match = std::min<std::size_t>(
        std::max<std::size_t>(kMinMatch, options.max_match), kMaxFastMatch);

    std::size_t lz77_size = 0;
    Lz77SplitStreams streams;
    streams.has_histograms = true;
    streams.commands.reserve(input.size() / 8);
    streams.literal_lengths.reserve(input.size() / 64);
    streams.match_lengths.reserve(input.size() / 64);
    streams.distances.reserve(input.size() / 64);
    streams.literals.reserve(input.size() / 2);

    std::size_t literal_start = 0;
    std::size_t pos = 0;
    std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};
    while (pos + kMinMatch <= input.size()) {
        const auto& row = table[hash4(input.data() + pos)];
        std::size_t best_length = 0;
        std::size_t best_distance = 0;

        for (const auto candidate32 : row) {
            if (candidate32 == kInvalidPos) {
                continue;
            }
            const auto candidate = static_cast<std::size_t>(candidate32);
            if (candidate >= pos) {
                continue;
            }
            const auto distance = pos - candidate;
            if (distance > max_distance) {
                continue;
            }
            if (load_u32_le(input.data() + candidate) != load_u32_le(input.data() + pos)) {
                continue;
            }

            const auto limit = std::min({max_match, input.size() - pos, input.size() - candidate});
            const auto length = common_prefix(input.data() + candidate, input.data() + pos, limit);
            if (length > best_length) {
                best_length = length;
                best_distance = distance;
                if (length >= options.nice_length) {
                    break;
                }
            }
        }

        std::size_t best_rep_length = 0;
        std::size_t best_rep_index = 0;
        const auto rep_limit = std::min(max_match, input.size() - pos);
        for (std::size_t i = 0; i < reps.size(); ++i) {
            const auto distance = reps[i];
            if (distance == 0 || distance > pos) {
                continue;
            }
            if (load_u32_le(input.data() + pos - distance) !=
                load_u32_le(input.data() + pos)) {
                continue;
            }
            const auto length = common_prefix(input.data() + pos - distance,
                                              input.data() + pos,
                                              rep_limit);
            if (length > best_rep_length) {
                best_rep_length = length;
                best_rep_index = i;
            }
        }

        remember(table, input, pos);

        const auto required = best_distance > 0xFFFFu ? kLongDistanceMinMatch : kMinMatch;
        const auto take_rep = best_rep_length >= kMinMatch && best_rep_length >= best_length;
        if (take_rep) {
            emit_lz77_split_rep_sequence(lz77_size, streams, input, literal_start, pos,
                                         best_rep_length, best_rep_index);

            const auto chosen = reps[best_rep_index];
            for (std::size_t i = best_rep_index; i > 0; --i) {
                reps[i] = reps[i - 1];
            }
            reps[0] = chosen;

            const auto insert_end = std::min<std::size_t>(pos + best_rep_length, pos + 16);
            for (std::size_t insert = pos + 1; insert < insert_end; ++insert) {
                remember(table, input, insert);
            }

            pos += best_rep_length;
            literal_start = pos;
        } else if (best_length >= required) {
            emit_lz77_split_sequence(lz77_size, streams, input, literal_start, pos,
                                     best_length, best_distance);

            reps = {best_distance, reps[0], reps[1], reps[2]};

            const auto insert_end = std::min<std::size_t>(pos + best_length, pos + 16);
            for (std::size_t insert = pos + 1; insert < insert_end; ++insert) {
                remember(table, input, insert);
            }

            pos += best_length;
            literal_start = pos;
        } else {
            ++pos;
        }
    }

    emit_lz77_split_final_literals(lz77_size, streams, input, literal_start);
    return {lz77_size, std::move(streams)};
}

void decode_fast_lz_into(std::span<const std::uint8_t> encoded,
                         std::span<std::uint8_t> output) {
    const auto output_size = output.size();
    std::size_t cursor = 0;
    std::size_t out = 0;

    while (out < output_size) {
        if (cursor >= encoded.size()) {
            throw FormatError("truncated fast-lz token");
        }
        const auto token = encoded[cursor++];
        auto literal_length = static_cast<std::size_t>(token >> 4);
        if (literal_length == 15) {
            literal_length = read_len(encoded, cursor, literal_length);
        }
        if (literal_length > encoded.size() - cursor ||
            literal_length > output.size() - out) {
            throw FormatError("fast-lz literal exceeds output");
        }
        std::memcpy(output.data() + out, encoded.data() + cursor, literal_length);
        cursor += literal_length;
        out += literal_length;

        if (out == output_size) {
            break;
        }

        const auto distance = read_distance(encoded, cursor);
        auto match_length = static_cast<std::size_t>(token & 0x0Fu) + kMinMatch;
        if ((token & 0x0Fu) == 15) {
            match_length = read_len(encoded, cursor, match_length);
        }
        copy_match(output, out, distance, match_length);
    }

    if (cursor != encoded.size()) {
        throw FormatError("trailing bytes after fast-lz stream");
    }
}

ByteVector decode_fast_lz(std::span<const std::uint8_t> encoded,
                          std::size_t output_size) {
    ByteVector output(output_size);
    decode_fast_lz_into(encoded, output);
    return output;
}

}  // namespace axiom::codec
