#include "entropy/range.hpp"

#include "codec/varint.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace axiom::entropy {
namespace {

constexpr std::size_t kSymbolCount = 256;
constexpr std::uint64_t kFullRange = 1ULL << 32;
constexpr std::uint64_t kHalfRange = kFullRange >> 1;
constexpr std::uint64_t kQuarterRange = kHalfRange >> 1;
constexpr std::uint64_t kThreeQuarterRange = kQuarterRange * 3;
constexpr std::uint64_t kTopValue = kFullRange - 1;

class BitWriter {
public:
    void write_bit(std::uint8_t bit) {
        current_ = static_cast<std::uint8_t>((current_ << 1) | (bit & 1u));
        ++filled_;

        if (filled_ == 8) {
            bytes_.push_back(current_);
            current_ = 0;
            filled_ = 0;
        }
    }

    void write_bit_plus_pending(std::uint8_t bit, std::uint64_t& pending) {
        write_bit(bit);
        const auto inverted = static_cast<std::uint8_t>(bit ^ 1u);

        while (pending > 0) {
            write_bit(inverted);
            --pending;
        }
    }

    ByteVector finish() {
        if (filled_ != 0) {
            bytes_.push_back(static_cast<std::uint8_t>(current_ << (8 - filled_)));
        }

        return std::move(bytes_);
    }

private:
    ByteVector bytes_;
    std::uint8_t current_ = 0;
    std::uint8_t filled_ = 0;
};

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    std::uint8_t read_bit() {
        if (cursor_ >= bytes_.size()) {
            // The decoder legitimately reads up to one value-register width of
            // implicit zero bits past the encoder's final flush. Anything more
            // means the stream is truncated, so fail instead of synthesizing an
            // unbounded run of zeros.
            if (++overrun_ > 32) {
                throw FormatError("truncated order-1 range stream");
            }
            return 0;
        }

        const auto bit = static_cast<std::uint8_t>((bytes_[cursor_] >> (7 - bit_index_)) & 1u);
        ++bit_index_;
        if (bit_index_ == 8) {
            bit_index_ = 0;
            ++cursor_;
        }

        return bit;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_ = 0;
    std::uint32_t overrun_ = 0;
    std::uint8_t bit_index_ = 0;
};

std::array<std::uint64_t, kSymbolCount> count_frequencies(std::span<const std::uint8_t> input) {
    std::array<std::uint64_t, kSymbolCount> counts{};
    for (const auto byte : input) {
        ++counts[byte];
    }
    return counts;
}

std::size_t nonzero_symbols(const std::array<std::uint32_t, kSymbolCount>& frequencies) {
    return static_cast<std::size_t>(
        std::count_if(frequencies.begin(), frequencies.end(), [](std::uint32_t value) {
            return value != 0;
        }));
}

}  // namespace

namespace {

// Adaptive order-1 model. The total per context is held below 2^16 so that
// range * cumulative stays within 64 bits, matching the static coder's limits.
constexpr std::uint32_t kOrder1Increment = 24;
constexpr std::uint32_t kOrder1MaxTotal = 1u << 15;

// Adaptive order-1 model. The per-context cumulative-frequency queries that the
// arithmetic coder needs are served by a Fenwick (binary indexed) tree, turning
// the per-symbol cost from O(256) into O(log 256). The frequencies and update
// rule are unchanged, so the coded bitstream is identical to the linear version.
struct Order1Model {
    std::vector<std::uint16_t> frequencies;  // [ctx*256 + sym] raw counts
    std::vector<std::uint32_t> tree;         // Fenwick per context, 1-indexed
    std::array<std::uint32_t, kSymbolCount> totals{};

    Order1Model()
        : frequencies(kSymbolCount * kSymbolCount, 1),
          tree(kSymbolCount * (kSymbolCount + 1), 0) {
        totals.fill(static_cast<std::uint32_t>(kSymbolCount));
        for (std::size_t context = 0; context < kSymbolCount; ++context) {
            rebuild(context);
        }
    }

    std::uint16_t* row(std::size_t context) {
        return frequencies.data() + context * kSymbolCount;
    }

    std::uint32_t freq_at(std::size_t context, std::size_t symbol) const {
        return frequencies[context * kSymbolCount + symbol];
    }

    static std::size_t lowbit(std::size_t value) {
        return value & (~value + 1);
    }

    void rebuild(std::size_t context) {
        auto* node = tree.data() + context * (kSymbolCount + 1);
        const auto* freq = frequencies.data() + context * kSymbolCount;
        for (std::size_t i = 0; i <= kSymbolCount; ++i) {
            node[i] = 0;
        }
        for (std::size_t i = 1; i <= kSymbolCount; ++i) {
            node[i] += freq[i - 1];
            const auto parent = i + lowbit(i);
            if (parent <= kSymbolCount) {
                node[parent] += node[i];
            }
        }
    }

    // Sum of frequencies for symbols [0, symbol).
    std::uint32_t cumulative_before(std::size_t context, std::size_t symbol) const {
        const auto* node = tree.data() + context * (kSymbolCount + 1);
        std::uint32_t sum = 0;
        for (std::size_t i = symbol; i > 0; i -= lowbit(i)) {
            sum += node[i];
        }
        return sum;
    }

    // Symbol whose cumulative interval contains target; also yields the exclusive
    // cumulative sum below it. Returns kSymbolCount if target is out of range.
    std::size_t find(std::size_t context, std::uint32_t target, std::uint32_t& cumulative_low) const {
        const auto* node = tree.data() + context * (kSymbolCount + 1);
        std::size_t pos = 0;
        std::uint32_t remaining = target;
        for (std::size_t step = kSymbolCount; step > 0; step >>= 1) {
            const auto next = pos + step;
            if (next <= kSymbolCount && node[next] <= remaining) {
                pos = next;
                remaining -= node[next];
            }
        }
        cumulative_low = target - remaining;
        return pos;
    }

    void update(std::size_t context, std::uint8_t symbol) {
        auto* freq = row(context);
        freq[symbol] = static_cast<std::uint16_t>(freq[symbol] + kOrder1Increment);
        totals[context] += kOrder1Increment;

        auto* node = tree.data() + context * (kSymbolCount + 1);
        for (std::size_t i = static_cast<std::size_t>(symbol) + 1; i <= kSymbolCount; i += lowbit(i)) {
            node[i] += kOrder1Increment;
        }

        if (totals[context] > kOrder1MaxTotal) {
            std::uint32_t rescaled = 0;
            for (std::size_t i = 0; i < kSymbolCount; ++i) {
                freq[i] = static_cast<std::uint16_t>((freq[i] + 1) >> 1);
                rescaled += freq[i];
            }
            totals[context] = rescaled;
            rebuild(context);
        }
    }
};

void encode_interval(BitWriter& writer,
                     std::uint32_t cumulative_low,
                     std::uint32_t cumulative_high,
                     std::uint32_t total,
                     std::uint64_t& low,
                     std::uint64_t& high,
                     std::uint64_t& pending_bits) {
    const auto range = high - low + 1;
    high = low + ((range * cumulative_high) / total) - 1;
    low = low + ((range * cumulative_low) / total);

    while (true) {
        if (high < kHalfRange) {
            writer.write_bit_plus_pending(0, pending_bits);
        } else if (low >= kHalfRange) {
            writer.write_bit_plus_pending(1, pending_bits);
            low -= kHalfRange;
            high -= kHalfRange;
        } else if (low >= kQuarterRange && high < kThreeQuarterRange) {
            ++pending_bits;
            low -= kQuarterRange;
            high -= kQuarterRange;
        } else {
            break;
        }

        low = (low << 1) & kTopValue;
        high = ((high << 1) | 1u) & kTopValue;
    }
}

void decode_consume(BitReader& reader,
                    std::uint32_t cumulative_low,
                    std::uint32_t cumulative_high,
                    std::uint32_t total,
                    std::uint64_t& low,
                    std::uint64_t& high,
                    std::uint64_t& value) {
    const auto range = high - low + 1;
    high = low + ((range * cumulative_high) / total) - 1;
    low = low + ((range * cumulative_low) / total);

    while (true) {
        if (high < kHalfRange) {
        } else if (low >= kHalfRange) {
            value -= kHalfRange;
            low -= kHalfRange;
            high -= kHalfRange;
        } else if (low >= kQuarterRange && high < kThreeQuarterRange) {
            value -= kQuarterRange;
            low -= kQuarterRange;
            high -= kQuarterRange;
        } else {
            break;
        }

        low = (low << 1) & kTopValue;
        high = ((high << 1) | 1u) & kTopValue;
        value = ((value << 1) | reader.read_bit()) & kTopValue;
    }
}

}  // namespace

std::optional<ByteVector> encode_order1(std::span<const std::uint8_t> input) {
    if (input.empty()) {
        return std::nullopt;
    }

    Order1Model model;
    BitWriter writer;
    std::uint64_t low = 0;
    std::uint64_t high = kTopValue;
    std::uint64_t pending_bits = 0;
    std::size_t context = 0;

    for (const auto symbol : input) {
        const auto cumulative_low = model.cumulative_before(context, symbol);
        const auto cumulative_high = cumulative_low + model.freq_at(context, symbol);

        encode_interval(writer, cumulative_low, cumulative_high, model.totals[context],
                        low, high, pending_bits);
        model.update(context, symbol);
        context = symbol;
    }

    ++pending_bits;
    if (low < kQuarterRange) {
        writer.write_bit_plus_pending(0, pending_bits);
    } else {
        writer.write_bit_plus_pending(1, pending_bits);
    }

    ByteVector output;
    codec::write_varuint(output, input.size());
    auto bitstream = writer.finish();
    output.insert(output.end(), bitstream.begin(), bitstream.end());
    return output;
}

ByteVector decode_order1(std::span<const std::uint8_t> encoded,
                         std::size_t max_output_size) {
    std::size_t cursor = 0;
    const auto decoded_size = static_cast<std::size_t>(codec::read_varuint(encoded, cursor));
    if (decoded_size > max_output_size) {
        throw FormatError("order-1 output exceeds block limit");
    }
    if (decoded_size == 0) {
        return {};
    }

    Order1Model model;
    BitReader reader(encoded.subspan(cursor));
    std::uint64_t low = 0;
    std::uint64_t high = kTopValue;
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        value = ((value << 1) | reader.read_bit()) & kTopValue;
    }

    ByteVector output;
    codec::bounded_reserve(output, decoded_size);
    std::size_t context = 0;

    for (std::size_t produced = 0; produced < decoded_size; ++produced) {
        const auto total = model.totals[context];
        const auto range = high - low + 1;
        const auto target = static_cast<std::uint32_t>((((value - low + 1) * total) - 1) / range);

        std::uint32_t cumulative_low = 0;
        const auto symbol = model.find(context, target, cumulative_low);
        if (symbol >= kSymbolCount) {
            throw FormatError("order-1 decoder selected invalid symbol");
        }

        decode_consume(reader, cumulative_low, cumulative_low + model.freq_at(context, symbol),
                       total, low, high, value);

        const auto byte = static_cast<std::uint8_t>(symbol);
        output.push_back(byte);
        model.update(context, byte);
        context = byte;
    }

    return output;
}

namespace {

// Static order-0 rANS (after Giesen's rans_byte). The frequency total is a power
// of two so the coder uses shifts, and decode resolves each symbol through a
// flat slot table in O(1).
constexpr std::uint32_t kRansTotalBits = 15;  // matches the arithmetic coder's precision
constexpr std::uint32_t kRansTotal = 1u << kRansTotalBits;
constexpr std::uint32_t kRansLow = 1u << 23;  // a multiple of kRansTotal
constexpr std::size_t kRansLanes = 4;

// Precomputed reciprocal form of the rANS encoder update. The old hot loop
// divided twice per symbol; this is the same state transition expressed with
// one 64-bit multiply and shifts. The representation is independent of the
// rANS precision, so order-0 and clustered order-1 tables share it.
struct RansEncoderSymbol {
    std::uint32_t x_max = 0;
    std::uint32_t reciprocal = 0;
    std::uint32_t bias = 0;
    std::uint16_t complement = 0;
    std::uint8_t reciprocal_shift = 0;
};

RansEncoderSymbol make_rans_encoder_symbol(std::uint32_t start,
                                           std::uint32_t frequency,
                                           std::uint32_t scale_bits) {
    const auto total = std::uint32_t{1} << scale_bits;
    RansEncoderSymbol symbol;
    symbol.x_max = ((kRansLow >> scale_bits) << 8) * frequency;
    symbol.complement = static_cast<std::uint16_t>(total - frequency);

    if (frequency < 2) {
        symbol.reciprocal = std::numeric_limits<std::uint32_t>::max();
        symbol.bias = start + total - 1;
        return symbol;
    }

    const auto shift = static_cast<std::uint32_t>(std::bit_width(frequency - 1));
    symbol.reciprocal = static_cast<std::uint32_t>(
        (((std::uint64_t{1} << (shift + 31)) + frequency - 1) / frequency));
    symbol.bias = start;
    symbol.reciprocal_shift = static_cast<std::uint8_t>(shift - 1);
    return symbol;
}

std::uint32_t rans_encode_advance(std::uint32_t state,
                                  const RansEncoderSymbol& symbol) {
    const auto quotient = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(state) * symbol.reciprocal) >> 32) >>
        symbol.reciprocal_shift;
    return state + symbol.bias + quotient * symbol.complement;
}

std::array<std::uint32_t, kSymbolCount> rans_normalize(
    const std::array<std::uint64_t, kSymbolCount>& counts,
    std::uint64_t count_total,
    std::uint32_t target_total) {
    std::array<std::uint32_t, kSymbolCount> frequencies{};
    std::uint32_t total = 0;
    std::size_t best_symbol = 0;
    std::uint64_t best_count = 0;

    for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
        if (counts[symbol] == 0) {
            continue;
        }
        const auto scaled = static_cast<std::uint32_t>((counts[symbol] * target_total) / count_total);
        frequencies[symbol] = std::max<std::uint32_t>(1, scaled);
        total += frequencies[symbol];
        if (counts[symbol] > best_count) {
            best_count = counts[symbol];
            best_symbol = symbol;
        }
    }

    while (total > target_total) {
        auto reduce_symbol = kSymbolCount;
        for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
            if (frequencies[symbol] <= 1) {
                continue;
            }
            if (reduce_symbol == kSymbolCount || frequencies[symbol] > frequencies[reduce_symbol]) {
                reduce_symbol = symbol;
            }
        }
        if (reduce_symbol == kSymbolCount) {
            break;
        }
        --frequencies[reduce_symbol];
        --total;
    }
    if (total < target_total) {
        frequencies[best_symbol] += target_total - total;
    }

    return frequencies;
}

void write_u32_le(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8);
    out[2] = static_cast<std::uint8_t>(value >> 16);
    out[3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t read_u32_le(std::span<const std::uint8_t> encoded, std::size_t& cursor) {
    if (encoded.size() - cursor < 4) {
        throw FormatError("truncated rANS stream");
    }

    const auto value = static_cast<std::uint32_t>(encoded[cursor]) |
                       (static_cast<std::uint32_t>(encoded[cursor + 1]) << 8) |
                       (static_cast<std::uint32_t>(encoded[cursor + 2]) << 16) |
                       (static_cast<std::uint32_t>(encoded[cursor + 3]) << 24);
    cursor += 4;
    return value;
}

void rans_write_table(ByteVector& output,
                      const std::array<std::uint32_t, kSymbolCount>& frequencies) {
    codec::write_varuint(output, nonzero_symbols(frequencies));
    for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
        if (frequencies[symbol] == 0) {
            continue;
        }
        output.push_back(static_cast<std::uint8_t>(symbol));
        codec::write_varuint(output, frequencies[symbol]);
    }
}

void rans_write_model(ByteVector& output,
                      std::size_t decoded_size,
                      const std::array<std::uint32_t, kSymbolCount>& frequencies) {
    codec::write_varuint(output, decoded_size);
    rans_write_table(output, frequencies);
}

std::array<std::uint32_t, kSymbolCount> rans_read_table(std::span<const std::uint8_t> encoded,
                                                        std::size_t& cursor,
                                                        std::uint32_t target_total) {
    const auto symbol_count = static_cast<std::size_t>(codec::read_varuint(encoded, cursor));
    if (symbol_count == 0 || symbol_count > kSymbolCount) {
        throw FormatError("invalid rANS symbol count");
    }

    std::array<std::uint32_t, kSymbolCount> frequencies{};
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < symbol_count; ++i) {
        if (cursor >= encoded.size()) {
            throw FormatError("truncated rANS model");
        }
        const auto symbol = encoded[cursor++];
        const auto frequency = static_cast<std::uint32_t>(codec::read_varuint(encoded, cursor));
        if (frequency == 0 || frequency > target_total || frequencies[symbol] != 0) {
            throw FormatError("invalid rANS symbol frequency");
        }
        frequencies[symbol] = frequency;
        total += frequency;
    }
    if (total != target_total) {
        throw FormatError("rANS frequency table has invalid total");
    }

    return frequencies;
}

std::array<std::uint32_t, kSymbolCount> rans_read_model(std::span<const std::uint8_t> encoded,
                                                        std::size_t& cursor,
                                                        std::size_t& decoded_size,
                                                        std::size_t max_output_size) {
    decoded_size = static_cast<std::size_t>(codec::read_varuint(encoded, cursor));
    if (decoded_size > max_output_size) {
        throw FormatError("rANS output exceeds block limit");
    }

    return rans_read_table(encoded, cursor, kRansTotal);
}

}  // namespace

std::optional<ByteVector> encode_rans(std::span<const std::uint8_t> input) {
    if (input.empty()) {
        return std::nullopt;
    }

    const auto counts = count_frequencies(input);
    return encode_rans(input, counts);
}

std::optional<ByteVector> encode_rans(
    std::span<const std::uint8_t> input,
    const std::array<std::uint64_t, 256>& counts) {
    if (input.empty()) {
        return std::nullopt;
    }

    const auto frequencies = rans_normalize(counts, input.size(), kRansTotal);

    std::array<std::uint32_t, kSymbolCount + 1> cumulative{};
    std::array<RansEncoderSymbol, kSymbolCount> encoder_symbols{};
    for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
        cumulative[symbol + 1] = cumulative[symbol] + frequencies[symbol];
        if (frequencies[symbol] != 0) {
            encoder_symbols[symbol] = make_rans_encoder_symbol(
                cumulative[symbol], frequencies[symbol], kRansTotalBits);
        }
    }

    ByteVector output;
    rans_write_model(output, input.size(), frequencies);

    // Four interleaved states preserve the byte-for-byte order at the caller
    // boundary while breaking the decode dependency chain. The format is still
    // order-0 static rANS; it just stores four final states instead of one.
    // The backward-write scratch is reused per worker thread across streams —
    // it can reach twice the literal stream size, so reallocating it for every
    // trial encode is measurable. resize() keeps capacity; every byte consumed
    // below is written first.
    static thread_local std::vector<std::uint8_t> scratch;
    scratch.resize(input.size() * 2 + 64 + kRansLanes * 4);
    std::size_t head = scratch.size();
    std::array<std::uint32_t, kRansLanes> states{};
    states.fill(kRansLow);

    for (std::size_t i = input.size(); i-- > 0;) {
        const auto symbol = input[i];
        const auto& encoder_symbol = encoder_symbols[symbol];
        auto& state = states[i & (kRansLanes - 1)];
        while (state >= encoder_symbol.x_max) {
            scratch[--head] = static_cast<std::uint8_t>(state & 0xFFu);
            state >>= 8;
        }
        state = rans_encode_advance(state, encoder_symbol);
    }

    head -= kRansLanes * 4;
    for (std::size_t lane = 0; lane < kRansLanes; ++lane) {
        write_u32_le(scratch.data() + head + lane * 4, states[lane]);
    }

    output.insert(output.end(), scratch.begin() + static_cast<std::ptrdiff_t>(head), scratch.end());
    return output;
}

ByteVector decode_rans(std::span<const std::uint8_t> encoded, std::size_t max_output_size) {
    std::size_t cursor = 0;
    std::size_t decoded_size = 0;
    const auto frequencies = rans_read_model(encoded, cursor, decoded_size, max_output_size);
    if (decoded_size == 0) {
        return {};
    }

    std::array<std::uint32_t, kSymbolCount + 1> cumulative{};
    for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
        cumulative[symbol + 1] = cumulative[symbol] + frequencies[symbol];
    }

    // Flat slot -> symbol table for O(1) decode lookups. Reused per worker
    // thread; the construction below writes every slot (frequencies sum to
    // exactly kRansTotal), so no reset is needed.
    static thread_local std::vector<std::uint8_t> slot_to_symbol;
    slot_to_symbol.resize(kRansTotal);
    for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
        for (std::uint32_t slot = cumulative[symbol]; slot < cumulative[symbol + 1]; ++slot) {
            slot_to_symbol[slot] = static_cast<std::uint8_t>(symbol);
        }
    }

    std::array<std::uint32_t, kRansLanes> states{};
    for (auto& state : states) {
        state = read_u32_le(encoded, cursor);
    }

    ByteVector output(decoded_size);
    auto decode_one = [&](std::size_t lane, std::size_t out_index) {
        auto& state = states[lane];
        const auto slot = state & (kRansTotal - 1);
        const auto symbol = slot_to_symbol[slot];
        output[out_index] = symbol;

        state = frequencies[symbol] * (state >> kRansTotalBits) + slot - cumulative[symbol];
        while (state < kRansLow) {
            if (cursor >= encoded.size()) {
                throw FormatError("truncated rANS stream");
            }
            state = (state << 8) | encoded[cursor++];
        }
    };

    std::size_t i = 0;
    for (; i + kRansLanes <= decoded_size; i += kRansLanes) {
        decode_one(0, i + 0);
        decode_one(1, i + 1);
        decode_one(2, i + 2);
        decode_one(3, i + 3);
    }
    for (; i < decoded_size; ++i) {
        decode_one(i & (kRansLanes - 1), i);
    }

    if (cursor != encoded.size()) {
        throw FormatError("trailing bytes after rANS stream");
    }

    return output;
}

std::optional<ByteVector> encode_rans_contextual(
    std::span<const std::uint8_t> input,
    std::span<const std::uint8_t> contexts,
    std::size_t context_count,
    std::size_t symbol_count) {
    if (input.empty() || input.size() != contexts.size() || context_count == 0 ||
        context_count > kSymbolCount || symbol_count == 0 ||
        symbol_count > kSymbolCount) {
        return std::nullopt;
    }

    constexpr std::uint32_t kContextTotalBits = 12;
    constexpr std::uint32_t kContextTotal = 1u << kContextTotalBits;
    using Counts = std::array<std::uint64_t, kSymbolCount>;
    using Frequencies = std::array<std::uint32_t, kSymbolCount>;
    using Cumulative = std::array<std::uint32_t, kSymbolCount + 1>;
    std::vector<Counts> counts(context_count);
    std::vector<std::uint64_t> context_totals(context_count);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto context = static_cast<std::size_t>(contexts[i]);
        const auto symbol = static_cast<std::size_t>(input[i]);
        if (context >= context_count || symbol >= symbol_count) {
            return std::nullopt;
        }
        ++counts[context][symbol];
        ++context_totals[context];
    }

    std::vector<Frequencies> frequencies(context_count);
    std::vector<Cumulative> cumulative(context_count);
    std::vector<std::array<RansEncoderSymbol, kSymbolCount>> encoder_symbols(
        context_count);
    std::size_t used_contexts = 0;
    for (std::size_t context = 0; context < context_count; ++context) {
        if (context_totals[context] == 0) {
            continue;
        }
        ++used_contexts;
        frequencies[context] =
            rans_normalize(counts[context], context_totals[context], kContextTotal);
        for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
            cumulative[context][symbol + 1] =
                cumulative[context][symbol] + frequencies[context][symbol];
            if (frequencies[context][symbol] != 0) {
                encoder_symbols[context][symbol] = make_rans_encoder_symbol(
                    cumulative[context][symbol], frequencies[context][symbol],
                    kContextTotalBits);
            }
        }
    }

    ByteVector output;
    codec::write_varuint(output, input.size());
    codec::write_varuint(output, used_contexts);
    for (std::size_t context = 0; context < context_count; ++context) {
        if (context_totals[context] == 0) {
            continue;
        }
        output.push_back(static_cast<std::uint8_t>(context));
        rans_write_table(output, frequencies[context]);
    }

    static thread_local std::vector<std::uint8_t> scratch;
    scratch.resize(input.size() * 2 + 64 + kRansLanes * 4);
    std::size_t head = scratch.size();
    std::array<std::uint32_t, kRansLanes> states{};
    states.fill(kRansLow);

    for (std::size_t i = input.size(); i-- > 0;) {
        const auto context = static_cast<std::size_t>(contexts[i]);
        const auto symbol = static_cast<std::size_t>(input[i]);
        const auto& encoder_symbol = encoder_symbols[context][symbol];
        auto& state = states[i & (kRansLanes - 1)];
        while (state >= encoder_symbol.x_max) {
            scratch[--head] = static_cast<std::uint8_t>(state & 0xFFu);
            state >>= 8;
        }
        state = rans_encode_advance(state, encoder_symbol);
    }

    head -= kRansLanes * 4;
    for (std::size_t lane = 0; lane < kRansLanes; ++lane) {
        write_u32_le(scratch.data() + head + lane * 4, states[lane]);
    }
    output.insert(output.end(), scratch.begin() + static_cast<std::ptrdiff_t>(head),
                  scratch.end());
    return output;
}

ByteVector decode_rans_contextual(
    std::span<const std::uint8_t> encoded,
    std::span<const std::uint8_t> contexts,
    std::size_t context_count,
    std::size_t symbol_count) {
    constexpr std::uint32_t kContextTotalBits = 12;
    constexpr std::uint32_t kContextTotal = 1u << kContextTotalBits;
    if (context_count == 0 || context_count > kSymbolCount || symbol_count == 0 ||
        symbol_count > kSymbolCount) {
        throw FormatError("invalid contextual rANS dimensions");
    }

    std::size_t cursor = 0;
    const auto decoded_size =
        static_cast<std::size_t>(codec::read_varuint(encoded, cursor));
    if (decoded_size != contexts.size()) {
        throw FormatError("contextual rANS context count mismatch");
    }
    if (decoded_size == 0) {
        if (cursor != encoded.size()) {
            throw FormatError("trailing bytes after contextual rANS stream");
        }
        return {};
    }

    const auto used_context_count =
        static_cast<std::size_t>(codec::read_varuint(encoded, cursor));
    if (used_context_count == 0 || used_context_count > context_count) {
        throw FormatError("invalid contextual rANS table count");
    }

    using Frequencies = std::array<std::uint32_t, kSymbolCount>;
    using Cumulative = std::array<std::uint32_t, kSymbolCount + 1>;
    std::vector<Frequencies> frequencies(context_count);
    std::vector<Cumulative> cumulative(context_count);
    std::vector<bool> table_present(context_count);
    for (std::size_t table = 0; table < used_context_count; ++table) {
        if (cursor >= encoded.size()) {
            throw FormatError("truncated contextual rANS context id");
        }
        const auto context = static_cast<std::size_t>(encoded[cursor++]);
        if (context >= context_count || table_present[context]) {
            throw FormatError("invalid contextual rANS context id");
        }
        frequencies[context] = rans_read_table(encoded, cursor, kContextTotal);
        for (std::size_t symbol = symbol_count; symbol < kSymbolCount; ++symbol) {
            if (frequencies[context][symbol] != 0) {
                throw FormatError("contextual rANS symbol exceeds alphabet");
            }
        }
        for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
            cumulative[context][symbol + 1] =
                cumulative[context][symbol] + frequencies[context][symbol];
        }
        table_present[context] = true;
    }

    static thread_local std::vector<std::uint8_t> slot_to_symbol;
    slot_to_symbol.resize(context_count * kContextTotal);
    for (std::size_t context = 0; context < context_count; ++context) {
        if (!table_present[context]) {
            continue;
        }
        const auto base = context * kContextTotal;
        for (std::size_t symbol = 0; symbol < symbol_count; ++symbol) {
            for (std::uint32_t slot = cumulative[context][symbol];
                 slot < cumulative[context][symbol + 1]; ++slot) {
                slot_to_symbol[base + slot] = static_cast<std::uint8_t>(symbol);
            }
        }
    }

    std::array<std::uint32_t, kRansLanes> states{};
    for (auto& state : states) {
        state = read_u32_le(encoded, cursor);
    }

    ByteVector output(decoded_size);
    auto decode_one = [&](std::size_t lane, std::size_t out_index) {
        const auto context = static_cast<std::size_t>(contexts[out_index]);
        if (context >= context_count || !table_present[context]) {
            throw FormatError("contextual rANS stream uses a missing table");
        }
        auto& state = states[lane];
        const auto slot = state & (kContextTotal - 1);
        const auto symbol = slot_to_symbol[context * kContextTotal + slot];
        output[out_index] = symbol;
        state = frequencies[context][symbol] * (state >> kContextTotalBits) + slot -
                cumulative[context][symbol];
        while (state < kRansLow) {
            if (cursor >= encoded.size()) {
                throw FormatError("truncated contextual rANS stream");
            }
            state = (state << 8) | encoded[cursor++];
        }
    };

    std::size_t i = 0;
    for (; i + kRansLanes <= decoded_size; i += kRansLanes) {
        decode_one(0, i + 0);
        decode_one(1, i + 1);
        decode_one(2, i + 2);
        decode_one(3, i + 3);
    }
    for (; i < decoded_size; ++i) {
        decode_one(i & (kRansLanes - 1), i);
    }
    if (cursor != encoded.size()) {
        throw FormatError("trailing bytes after contextual rANS stream");
    }
    return output;
}

namespace {

// Static order-1 rANS. The 256 previous-byte contexts are clustered into a
// small set of frequency tables chosen by the encoder and transmitted with a
// 256-entry context map, so decode keeps the O(1) table-lookup inner loop of
// the order-0 coder (the bit-serial adaptive order-1 coder decodes ~30x
// slower for a fraction of a percent of ratio on these stream sizes).
//
// A smaller per-table total than order-0 keeps the K slot tables cache
// resident; the quantization loss is offset by the context modelling.
constexpr std::uint32_t kRansO1TotalBits = 12;
constexpr std::uint32_t kRansO1Total = 1u << kRansO1TotalBits;
constexpr std::size_t kRansO1MaxClusters = 16;
// Below this size the per-context statistics are too thin for clustered
// tables to beat the order-0 coder once headers are paid for.
constexpr std::size_t kRansO1MinInput = 4096;

}  // namespace

std::optional<ByteVector> encode_rans_order1(std::span<const std::uint8_t> input) {
    if (input.size() < kRansO1MinInput) {
        return std::nullopt;
    }

    // Order-1 joint histogram; the first byte uses context 0, matching decode.
    // Reused per worker thread: 256 KiB re-zeroed is cheaper than re-faulted.
    static thread_local std::vector<std::uint32_t> joint;
    joint.assign(kSymbolCount * kSymbolCount, 0);
    std::array<std::uint64_t, kSymbolCount> context_total{};
    {
        std::uint8_t prev = 0;
        for (const auto byte : input) {
            ++joint[(static_cast<std::size_t>(prev) << 8) | byte];
            ++context_total[prev];
            prev = byte;
        }
    }

    // Seed one cluster per heaviest context, then refine assignments against
    // the accumulated cluster distributions. The map is transmitted, so any
    // assignment is format-valid; quality only affects the compressed size.
    std::vector<std::size_t> occupied;
    occupied.reserve(kSymbolCount);
    for (std::size_t context = 0; context < kSymbolCount; ++context) {
        if (context_total[context] != 0) {
            occupied.push_back(context);
        }
    }
    if (occupied.empty()) {
        return std::nullopt;
    }

    auto cluster_count = std::min(kRansO1MaxClusters, occupied.size());
    std::vector<std::size_t> seeds(occupied);
    std::partial_sort(seeds.begin(), seeds.begin() + static_cast<std::ptrdiff_t>(cluster_count),
                      seeds.end(), [&](std::size_t a, std::size_t b) {
                          return context_total[a] > context_total[b];
                      });
    seeds.resize(cluster_count);

    std::array<std::uint8_t, kSymbolCount> cluster_map{};
    std::vector<std::uint64_t> cluster_hist(cluster_count * kSymbolCount, 0);
    for (std::size_t j = 0; j < cluster_count; ++j) {
        const auto* row = &joint[seeds[j] << 8];
        for (std::size_t s = 0; s < kSymbolCount; ++s) {
            cluster_hist[j * kSymbolCount + s] = row[s];
        }
    }

    std::vector<double> cost(cluster_count * kSymbolCount);
    constexpr int kRefinePasses = 3;
    for (int pass = 0; pass < kRefinePasses; ++pass) {
        // Per-cluster symbol costs in bits, +1-smoothed so a symbol absent from
        // the cluster still has a finite (large) cost during assignment.
        for (std::size_t j = 0; j < cluster_count; ++j) {
            std::uint64_t total = 0;
            for (std::size_t s = 0; s < kSymbolCount; ++s) {
                total += cluster_hist[j * kSymbolCount + s];
            }
            const auto denom = static_cast<double>(total + kSymbolCount);
            for (std::size_t s = 0; s < kSymbolCount; ++s) {
                const auto p = (cluster_hist[j * kSymbolCount + s] + 1) / denom;
                cost[j * kSymbolCount + s] = -std::log2(p);
            }
        }

        for (const auto context : occupied) {
            const auto* row = &joint[context << 8];
            std::size_t best = 0;
            double best_bits = std::numeric_limits<double>::infinity();
            for (std::size_t j = 0; j < cluster_count; ++j) {
                double bits = 0.0;
                const auto* cluster_cost = &cost[j * kSymbolCount];
                for (std::size_t s = 0; s < kSymbolCount; ++s) {
                    if (row[s] != 0) {
                        bits += row[s] * cluster_cost[s];
                    }
                }
                if (bits < best_bits) {
                    best_bits = bits;
                    best = j;
                }
            }
            cluster_map[context] = static_cast<std::uint8_t>(best);
        }

        std::fill(cluster_hist.begin(), cluster_hist.end(), 0);
        for (const auto context : occupied) {
            const auto* row = &joint[context << 8];
            auto* hist = &cluster_hist[cluster_map[context] * kSymbolCount];
            for (std::size_t s = 0; s < kSymbolCount; ++s) {
                hist[s] += row[s];
            }
        }
    }

    // Drop clusters that lost all their members during refinement and compact
    // the surviving ids so every transmitted table is used.
    std::array<std::uint8_t, kRansO1MaxClusters> remap{};
    std::size_t live_clusters = 0;
    for (std::size_t j = 0; j < cluster_count; ++j) {
        std::uint64_t total = 0;
        for (std::size_t s = 0; s < kSymbolCount; ++s) {
            total += cluster_hist[j * kSymbolCount + s];
        }
        if (total != 0) {
            if (live_clusters != j) {
                std::copy_n(&cluster_hist[j * kSymbolCount], kSymbolCount,
                            &cluster_hist[live_clusters * kSymbolCount]);
            }
            remap[j] = static_cast<std::uint8_t>(live_clusters);
            ++live_clusters;
        }
    }
    for (const auto context : occupied) {
        cluster_map[context] = remap[cluster_map[context]];
    }
    cluster_count = live_clusters;

    // Normalize each cluster to the shared power-of-two total.
    std::vector<std::uint32_t> frequencies(cluster_count * kSymbolCount);
    std::vector<std::uint32_t> cumulative(cluster_count * (kSymbolCount + 1), 0);
    std::vector<RansEncoderSymbol> encoder_symbols(cluster_count * kSymbolCount);
    for (std::size_t j = 0; j < cluster_count; ++j) {
        std::array<std::uint64_t, kSymbolCount> counts{};
        std::uint64_t total = 0;
        for (std::size_t s = 0; s < kSymbolCount; ++s) {
            counts[s] = cluster_hist[j * kSymbolCount + s];
            total += counts[s];
        }
        const auto table = rans_normalize(counts, total, kRansO1Total);
        auto* freq = &frequencies[j * kSymbolCount];
        auto* cumul = &cumulative[j * (kSymbolCount + 1)];
        for (std::size_t s = 0; s < kSymbolCount; ++s) {
            freq[s] = table[s];
            cumul[s + 1] = cumul[s] + table[s];
            if (table[s] != 0) {
                encoder_symbols[j * kSymbolCount + s] =
                    make_rans_encoder_symbol(cumul[s], table[s], kRansO1TotalBits);
            }
        }
    }

    ByteVector output;
    codec::write_varuint(output, input.size());
    output.push_back(static_cast<std::uint8_t>(cluster_count));
    output.insert(output.end(), cluster_map.begin(), cluster_map.end());
    for (std::size_t j = 0; j < cluster_count; ++j) {
        std::array<std::uint32_t, kSymbolCount> table{};
        std::copy_n(&frequencies[j * kSymbolCount], kSymbolCount, table.begin());
        rans_write_table(output, table);
    }

    // Backward interleaved encode, exactly the order-0 scheme with the
    // frequency table chosen by each position's context cluster. The scratch
    // is shared with the order-0 encoder's thread-local buffer semantics.
    static thread_local std::vector<std::uint8_t> scratch;
    scratch.resize(input.size() * 2 + 64 + kRansLanes * 4);
    std::size_t head = scratch.size();
    std::array<std::uint32_t, kRansLanes> states{};
    states.fill(kRansLow);

    for (std::size_t i = input.size(); i-- > 0;) {
        const auto context = i == 0 ? std::uint8_t{0} : input[i - 1];
        const auto cluster = cluster_map[context];
        const auto symbol = input[i];
        const auto& encoder_symbol = encoder_symbols[cluster * kSymbolCount + symbol];
        auto& state = states[i & (kRansLanes - 1)];
        while (state >= encoder_symbol.x_max) {
            scratch[--head] = static_cast<std::uint8_t>(state & 0xFFu);
            state >>= 8;
        }
        state = rans_encode_advance(state, encoder_symbol);
    }

    head -= kRansLanes * 4;
    for (std::size_t lane = 0; lane < kRansLanes; ++lane) {
        write_u32_le(scratch.data() + head + lane * 4, states[lane]);
    }

    output.insert(output.end(), scratch.begin() + static_cast<std::ptrdiff_t>(head), scratch.end());
    return output;
}

ByteVector decode_rans_order1(std::span<const std::uint8_t> encoded,
                              std::size_t max_output_size) {
    std::size_t cursor = 0;
    const auto decoded_size = static_cast<std::size_t>(codec::read_varuint(encoded, cursor));
    if (decoded_size > max_output_size) {
        throw FormatError("order-1 rANS output exceeds block limit");
    }
    if (decoded_size == 0) {
        return {};
    }

    if (cursor >= encoded.size()) {
        throw FormatError("truncated order-1 rANS cluster count");
    }
    const std::size_t cluster_count = encoded[cursor++];
    if (cluster_count == 0 || cluster_count > kRansO1MaxClusters) {
        throw FormatError("invalid order-1 rANS cluster count");
    }

    if (encoded.size() - cursor < kSymbolCount) {
        throw FormatError("truncated order-1 rANS context map");
    }
    std::array<std::uint8_t, kSymbolCount> cluster_map{};
    for (std::size_t context = 0; context < kSymbolCount; ++context) {
        const auto cluster = encoded[cursor++];
        if (cluster >= cluster_count) {
            throw FormatError("order-1 rANS context maps to missing cluster");
        }
        cluster_map[context] = cluster;
    }

    // Reused per worker thread. frequencies and the slot tables are fully
    // overwritten below; cumulative needs its per-cluster base re-zeroed
    // because the running sums start from cumul[0].
    static thread_local std::vector<std::uint32_t> frequencies;
    static thread_local std::vector<std::uint32_t> cumulative;
    static thread_local std::vector<std::uint8_t> slot_to_symbol;
    frequencies.resize(cluster_count * kSymbolCount);
    cumulative.resize(cluster_count * (kSymbolCount + 1));
    slot_to_symbol.resize(cluster_count * kRansO1Total);
    for (std::size_t j = 0; j < cluster_count; ++j) {
        const auto table = rans_read_table(encoded, cursor, kRansO1Total);
        auto* freq = &frequencies[j * kSymbolCount];
        auto* cumul = &cumulative[j * (kSymbolCount + 1)];
        auto* slots = &slot_to_symbol[j * kRansO1Total];
        cumul[0] = 0;
        for (std::size_t s = 0; s < kSymbolCount; ++s) {
            freq[s] = table[s];
            cumul[s + 1] = cumul[s] + table[s];
            for (std::uint32_t slot = cumul[s]; slot < cumul[s + 1]; ++slot) {
                slots[slot] = static_cast<std::uint8_t>(s);
            }
        }
    }

    std::array<std::uint32_t, kRansLanes> states{};
    for (auto& state : states) {
        state = read_u32_le(encoded, cursor);
    }

    ByteVector output(decoded_size);
    std::uint8_t previous = 0;
    for (std::size_t i = 0; i < decoded_size; ++i) {
        const auto cluster = cluster_map[previous];
        auto& state = states[i & (kRansLanes - 1)];
        const auto slot = state & (kRansO1Total - 1);
        const auto symbol = slot_to_symbol[cluster * kRansO1Total + slot];
        output[i] = symbol;

        state = frequencies[cluster * kSymbolCount + symbol] * (state >> kRansO1TotalBits) +
                slot - cumulative[cluster * (kSymbolCount + 1) + symbol];
        while (state < kRansLow) {
            if (cursor >= encoded.size()) {
                throw FormatError("truncated order-1 rANS stream");
            }
            state = (state << 8) | encoded[cursor++];
        }
        previous = symbol;
    }

    if (cursor != encoded.size()) {
        throw FormatError("trailing bytes after order-1 rANS stream");
    }

    return output;
}

}  // namespace axiom::entropy
