#include "entropy/range.hpp"

#include "codec/varint.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

std::array<std::uint32_t, kSymbolCount> rans_normalize(
    const std::array<std::uint64_t, kSymbolCount>& counts, std::size_t input_size) {
    std::array<std::uint32_t, kSymbolCount> frequencies{};
    std::uint32_t total = 0;
    std::size_t best_symbol = 0;
    std::uint64_t best_count = 0;

    for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
        if (counts[symbol] == 0) {
            continue;
        }
        const auto scaled = static_cast<std::uint32_t>((counts[symbol] * kRansTotal) / input_size);
        frequencies[symbol] = std::max<std::uint32_t>(1, scaled);
        total += frequencies[symbol];
        if (counts[symbol] > best_count) {
            best_count = counts[symbol];
            best_symbol = symbol;
        }
    }

    while (total > kRansTotal) {
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
    if (total < kRansTotal) {
        frequencies[best_symbol] += kRansTotal - total;
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

void rans_write_model(ByteVector& output,
                      std::size_t decoded_size,
                      const std::array<std::uint32_t, kSymbolCount>& frequencies) {
    codec::write_varuint(output, decoded_size);
    codec::write_varuint(output, nonzero_symbols(frequencies));
    for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
        if (frequencies[symbol] == 0) {
            continue;
        }
        output.push_back(static_cast<std::uint8_t>(symbol));
        codec::write_varuint(output, frequencies[symbol]);
    }
}

std::array<std::uint32_t, kSymbolCount> rans_read_model(std::span<const std::uint8_t> encoded,
                                                        std::size_t& cursor,
                                                        std::size_t& decoded_size,
                                                        std::size_t max_output_size) {
    decoded_size = static_cast<std::size_t>(codec::read_varuint(encoded, cursor));
    if (decoded_size > max_output_size) {
        throw FormatError("rANS output exceeds block limit");
    }

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
        if (frequency == 0 || frequency > kRansTotal || frequencies[symbol] != 0) {
            throw FormatError("invalid rANS symbol frequency");
        }
        frequencies[symbol] = frequency;
        total += frequency;
    }
    if (total != kRansTotal) {
        throw FormatError("rANS frequency table has invalid total");
    }

    return frequencies;
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

    const auto frequencies = rans_normalize(counts, input.size());

    std::array<std::uint32_t, kSymbolCount + 1> cumulative{};
    for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
        cumulative[symbol + 1] = cumulative[symbol] + frequencies[symbol];
    }

    ByteVector output;
    rans_write_model(output, input.size(), frequencies);

    // Four interleaved states preserve the byte-for-byte order at the caller
    // boundary while breaking the decode dependency chain. The format is still
    // order-0 static rANS; it just stores four final states instead of one.
    std::vector<std::uint8_t> scratch(input.size() * 2 + 64 + kRansLanes * 4);
    std::size_t head = scratch.size();
    std::array<std::uint32_t, kRansLanes> states{};
    states.fill(kRansLow);

    for (std::size_t i = input.size(); i-- > 0;) {
        const auto symbol = input[i];
        const auto frequency = frequencies[symbol];
        const auto x_max = ((kRansLow >> kRansTotalBits) << 8) * frequency;
        auto& state = states[i & (kRansLanes - 1)];
        while (state >= x_max) {
            scratch[--head] = static_cast<std::uint8_t>(state & 0xFFu);
            state >>= 8;
        }
        state = ((state / frequency) << kRansTotalBits) + (state % frequency) + cumulative[symbol];
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

    // Flat slot -> symbol table for O(1) decode lookups.
    std::vector<std::uint8_t> slot_to_symbol(kRansTotal);
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

}  // namespace axiom::entropy
