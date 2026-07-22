#include "codec/lz77.hpp"

#include "codec/lz77_split.hpp"
#include "codec/match_copy.hpp"
#include "codec/varint.hpp"
#include "core/cpu.hpp"
#include "core/task_executor.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <numeric>
#include <optional>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#define AXIOM_HAS_SSE2 1
#else
#define AXIOM_HAS_SSE2 0
#endif

namespace axiom::codec {
namespace {

// The binary-tree descent is memory-latency bound: each step needs the next
// node's child slots, a dependent random load. Prefetching them while the
// current node's bytes are compared overlaps that miss. Purely a scheduling
// hint — the token stream is unchanged. (Measured no benefit on the shorter
// hash-chain walks, so only the tree matcher uses it.)
inline void prefetch_read(const void* address) {
#if defined(_MSC_VER)
    _mm_prefetch(static_cast<const char*>(address), _MM_HINT_T0);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(address);
#else
    (void)address;
#endif
}

// Emits options.encode_progress ticks from a parse loop: call tick(position)
// each iteration; the callback fires once per kStep of scanned input, so the
// hot loop pays one compare. Fractions are of this pass's own work; callers
// wrap the callback to place a pass inside a larger phase plan.
class ParseProgressTicker {
public:
    ParseProgressTicker(const std::function<void(double)>& callback, std::size_t total)
        : callback_(callback && total != 0 ? &callback : nullptr),
          total_(total),
          next_(kStep) {}

    void tick(std::size_t position) {
        if (callback_ != nullptr && position >= next_) {
            next_ = position + kStep;
            (*callback_)(static_cast<double>(position) / static_cast<double>(total_));
        }
    }

private:
    static constexpr std::size_t kStep = 256u << 10;

    const std::function<void(double)>* callback_;
    std::size_t total_;
    std::size_t next_;
};

constexpr std::uint8_t kLiteralToken = 0;
constexpr std::uint8_t kMatchToken = 1;
// Repeat-offset matches reuse one of the last kRepCount distances. Rep i is
// encoded as command byte (kRepTokenBase + i) followed by only a length: no
// distance is stored, which is the whole point of the optimization.
constexpr std::uint8_t kRepTokenBase = 2;
constexpr std::size_t kRepCount = 4;
// AXC v9 only: consumes no output and replaces the four recent distances with
// encoder-chosen static values. Four descriptor bytes follow: 0..3 reuse one
// of the decoder's current reps, while 4 carries an explicit distance varuint.
// Descriptors and explicit values use the existing length/distance streams.
constexpr std::uint8_t kCheckpointToken = kRepTokenBase + kRepCount;
constexpr std::uint8_t kCheckpointExplicit = kRepCount;
constexpr std::size_t kMinMatch = 4;
constexpr std::size_t kHashBits = 20;
constexpr std::size_t kHashSize = 1u << kHashBits;
constexpr std::size_t kParserCandidateLimit = 64;

struct Match {
    std::uint16_t length = 0;
    std::uint32_t distance = 0;
};

// The optimal parser asks the matcher for candidates at every input position.
// A std::vector here used to mean millions of tiny constructions (and frequent
// heap traffic once the candidate count grew) in a level-9 block. The maximum
// is a codec invariant, so a compact fixed list is both faster and easier to
// keep in the matcher hot path.
struct MatchList {
    std::array<Match, kParserCandidateLimit> values{};
    std::size_t count = 0;

    void clear() {
        count = 0;
    }

    const Match& operator[](std::size_t index) const {
        return values[index];
    }
};

enum class ParseKind : std::uint8_t { literal, match, rep };

struct ParseDecision {
    std::uint32_t distance = 0;       // only meaningful for a normal match
    std::uint16_t length = 0;
    ParseKind kind = ParseKind::literal;
    std::uint8_t rep_index = 0;       // only meaningful for a rep match

    constexpr ParseDecision() = default;
    constexpr ParseDecision(std::uint16_t length_value,
                            std::uint32_t distance_value,
                            ParseKind kind_value,
                            std::uint8_t rep_index_value)
        : distance(distance_value),
          length(length_value),
          kind(kind_value),
          rep_index(rep_index_value) {}
};
static_assert(sizeof(ParseDecision) == 8);

std::size_t hash4_value(std::uint32_t value) {
    return (value * 2654435761u) >> (32 - kHashBits);
}

std::uint32_t load_u32_le(const std::uint8_t* data) {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::size_t hash4(std::span<const std::uint8_t> input, std::size_t position) {
    return hash4_value(load_u32_le(input.data() + position));
}

// Length of the common prefix of two byte runs, capped at `limit`. The first
// eight bytes use a cheap scalar probe; longer matches switch to 16-byte SSE2
// comparisons on x64. Both pointers read the original input buffer, so
// overlapping match sources compare exactly as a byte-serial loop would.
std::size_t common_prefix(const std::uint8_t* a, const std::uint8_t* b, std::size_t limit) {
    std::size_t i = 0;
    // Most probes diverge in their first eight bytes, where the scalar XOR +
    // trailing-zero path is cheaper than setting up SIMD.
    if (i + 8 <= limit) {
        std::uint64_t x = 0;
        std::uint64_t y = 0;
        std::memcpy(&x, a + i, 8);
        std::memcpy(&y, b + i, 8);
        const std::uint64_t diff = x ^ y;
        if (diff != 0) {
            const auto bit = std::endian::native == std::endian::little
                                 ? std::countr_zero(diff)
                                 : std::countl_zero(diff);
            return static_cast<std::size_t>(bit) >> 3;
        }
        i += 8;
    }
#if AXIOM_HAS_SSE2
    // Once eight bytes already matched, long runs are common enough that two
    // cache-line-friendly 16-byte comparisons amortize the SIMD mask cost.
    while (i + 16 <= limit) {
        const auto left = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
        const auto right = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
        const auto equal = static_cast<std::uint32_t>(
            _mm_movemask_epi8(_mm_cmpeq_epi8(left, right)));
        if (equal != 0xFFFFu) {
            return i + std::countr_zero((~equal) & 0xFFFFu);
        }
        i += 16;
    }
#endif
    while (i + 8 <= limit) {
        std::uint64_t x = 0;
        std::uint64_t y = 0;
        std::memcpy(&x, a + i, 8);
        std::memcpy(&y, b + i, 8);
        const std::uint64_t diff = x ^ y;
        if (diff != 0) {
            // The first mismatching byte is the lowest-addressed one, which is the
            // lowest-order byte on little-endian and the highest-order on big.
            const auto bit = std::endian::native == std::endian::little
                                 ? std::countr_zero(diff)
                                 : std::countl_zero(diff);
            return i + (static_cast<std::size_t>(bit) >> 3);
        }
        i += 8;
    }
    while (i < limit && a[i] == b[i]) {
        ++i;
    }
    return i;
}

// Length of the match obtained by copying from `distance` bytes back, using the
// same overlapping-copy semantics the decoder applies.
std::size_t match_length_at(std::span<const std::uint8_t> input,
                            std::size_t position,
                            std::size_t distance,
                            std::size_t limit) {
    if (distance == 0 || distance > position) {
        return 0;
    }

    const auto source = position - distance;
    if (limit >= kMinMatch &&
        load_u32_le(input.data() + source) != load_u32_le(input.data() + position)) {
        return 0;
    }
    return common_prefix(input.data() + source, input.data() + position, limit);
}

void write_literal_run(ByteVector& output,
                       std::span<const std::uint8_t> input,
                       std::size_t start,
                       std::size_t length) {
    if (length == 0) {
        return;
    }

    output.push_back(kLiteralToken);
    write_varuint(output, length);
    output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(start),
                  input.begin() + static_cast<std::ptrdiff_t>(start + length));
}

std::uint32_t varuint_cost(std::size_t value) {
    std::uint32_t bytes = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++bytes;
    }

    return bytes;
}

std::uint32_t literal_cost() {
    return 9;
}

std::uint32_t match_cost(std::size_t length, std::size_t distance) {
    return 12 + (varuint_cost(length) * 5) + (varuint_cost(distance) * 7);
}

// A repeat-offset match codes no distance, so it costs the match overhead and a
// length only. This is what makes the parser prefer reusing a recent distance.
std::uint32_t rep_cost(std::size_t length) {
    return 12 + (varuint_cost(length) * 5);
}

// Number of footer (low) bits a distance contributes under the position-slot
// scheme used by the slot split codec; this is the part that scales with the
// distance magnitude and dominates its real coded cost.
std::uint32_t distance_footer_bits(std::size_t distance) {
    const auto p = distance - 1;
    if (p < 4) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::bit_width(p)) - 2;
}

std::uint32_t distance_slot(std::size_t distance) {
    const auto p = distance - 1;
    if (p < 4) {
        return static_cast<std::uint32_t>(p);
    }
    const auto num_bits = static_cast<std::uint32_t>(std::bit_width(p)) - 1;
    return (num_bits << 1) | static_cast<std::uint32_t>((p >> (num_bits - 1)) & 1u);
}

// Costs are kept in scaled bits so fractional entropy estimates survive integer
// arithmetic. The crude model reproduces the original fixed weights; the measured
// model is built from the actual byte streams of a first parse. The split codec
// entropy-codes varint *bytes*, not abstract lengths, so retaining their symbol
// identities gives the DP a much closer approximation of its final payload.
constexpr std::uint64_t kCostScale = 16;

struct ParseCosts {
    bool measured = false;
    std::array<std::uint64_t, 256> literal{};
    std::array<std::uint64_t, 256> command{};
    std::array<std::uint64_t, 256> match_length{};
    std::array<std::uint64_t, 64> slot{};
};

std::uint64_t cost_literal_model(const ParseCosts& model, std::uint8_t literal) {
    return model.measured ? model.literal[literal] : static_cast<std::uint64_t>(literal_cost());
}

std::uint64_t cost_varuint_model(const std::array<std::uint64_t, 256>& byte_costs,
                                 std::size_t value) {
    std::uint64_t cost = 0;
    while (value >= 0x80) {
        cost += byte_costs[(value & 0x7Fu) | 0x80u];
        value >>= 7;
    }
    return cost + byte_costs[value];
}

std::uint64_t cost_match_model(const ParseCosts& model, std::size_t length, std::size_t distance) {
    if (!model.measured) {
        return match_cost(length, distance);
    }
    return model.command[kMatchToken] +
           cost_varuint_model(model.match_length, length) +
           model.slot[distance_slot(distance)] +
           static_cast<std::uint64_t>(distance_footer_bits(distance)) * kCostScale;
}

std::uint64_t cost_rep_model(const ParseCosts& model,
                             std::size_t length,
                             std::size_t rep_index) {
    if (!model.measured) {
        return rep_cost(length);
    }
    return model.command[kRepTokenBase + rep_index] +
           cost_varuint_model(model.match_length, length);
}

void add_match_candidate(MatchList& matches,
                         std::size_t length,
                         std::size_t distance,
                         std::size_t max_candidates) {
    if (length < kMinMatch || distance == 0) {
        return;
    }

    const auto narrowed_length = static_cast<std::uint16_t>(
        std::min<std::size_t>(length, std::numeric_limits<std::uint16_t>::max()));
    const auto narrowed_distance = static_cast<std::uint32_t>(
        std::min<std::size_t>(distance, std::numeric_limits<std::uint32_t>::max()));

    for (std::size_t i = 0; i < matches.count; ++i) {
        auto& match = matches.values[i];
        if (match.distance == narrowed_distance) {
            match.length = std::max(match.length, narrowed_length);
            return;
        }
    }

    if (matches.count >= max_candidates) {
        const auto& worst = matches.values[matches.count - 1];
        if (narrowed_length < worst.length ||
            (narrowed_length == worst.length && narrowed_distance >= worst.distance)) {
            return;
        }
    }

    auto is_better = [](const Match& left, const Match& right) {
        if (left.length != right.length) {
            return left.length > right.length;
        }
        return left.distance < right.distance;
    };

    // Keep the list in the same order as the former vector + std::sort path,
    // but insert in place because this list has at most 64 elements.
    auto insert_at = matches.count;
    if (matches.count < matches.values.size()) {
        ++matches.count;
    } else {
        // max_candidates is clamped to kParserCandidateLimit by the caller.
        // Reaching this branch means the new candidate displaced the worst.
        insert_at = matches.count - 1;
    }
    matches.values[insert_at] = Match{narrowed_length, narrowed_distance};
    while (insert_at > 0 && is_better(matches.values[insert_at], matches.values[insert_at - 1])) {
        std::swap(matches.values[insert_at], matches.values[insert_at - 1]);
        --insert_at;
    }

    if (matches.count > max_candidates) {
        matches.count = max_candidates;
    }
}

template <typename Index>
void find_matches_at(std::span<const std::uint8_t> input,
                     const std::vector<Index>& hash_heads,
                     const std::vector<Index>& previous,
                     std::size_t position,
                     std::size_t window_size,
                     std::size_t max_match,
                     std::size_t max_chain_depth,
                     std::size_t max_candidates,
                     MatchList& matches) {
    constexpr Index kNoPos = std::numeric_limits<Index>::max();
    if (position + kMinMatch > input.size()) {
        return;
    }

    const auto current = load_u32_le(input.data() + position);
    auto candidate = hash_heads[hash4_value(current)];
    const auto limit = std::min(max_match, input.size() - position);
    std::size_t depth = 0;

    while (candidate != kNoPos && depth < max_chain_depth) {
        const auto next = previous[candidate];
        if (candidate >= position) {
            candidate = next;
            ++depth;
            continue;
        }

        const auto distance = position - candidate;
        if (distance > window_size) {
            break;
        }

        if (matches.count >= max_candidates) {
            const auto threshold = static_cast<std::size_t>(matches.values[matches.count - 1].length);
            if (threshold > kMinMatch &&
                input[candidate + threshold - 1] != input[position + threshold - 1]) {
                candidate = next;
                ++depth;
                continue;
            }
        }

        if (load_u32_le(input.data() + candidate) != current) {
            candidate = next;
            ++depth;
            continue;
        }

        const std::size_t length =
            common_prefix(input.data() + candidate, input.data() + position, limit);

        add_match_candidate(matches, length, distance, max_candidates);
        // Hash chains are newest first, so the first maximum-length match is also
        // the nearest one. Older equal-length matches only add distance cost.
        if (length == limit) {
            break;
        }

        candidate = next;
        ++depth;
    }

}

void add_length_choice(std::array<std::uint16_t, 16>& lengths,
                       std::size_t& count,
                       std::size_t value,
                       std::size_t max_length) {
    if (value < kMinMatch || value > max_length ||
        value > std::numeric_limits<std::uint16_t>::max()) {
        return;
    }

    const auto narrowed = static_cast<std::uint16_t>(value);
    for (std::size_t i = 0; i < count; ++i) {
        if (lengths[i] == narrowed) {
            return;
        }
    }

    lengths[count++] = narrowed;
}

std::array<std::uint16_t, 16> useful_lengths(std::size_t max_length, std::size_t& count) {
    std::array<std::uint16_t, 16> lengths{};
    count = 0;

    for (std::size_t length = kMinMatch; length <= std::min<std::size_t>(8, max_length); ++length) {
        add_length_choice(lengths, count, length, max_length);
    }

    for (const auto length : {12u, 16u, 24u, 32u, 48u, 64u, 96u, 128u, 192u}) {
        add_length_choice(lengths, count, length, max_length);
    }

    add_length_choice(lengths, count, max_length, max_length);
    std::sort(lengths.begin(), lengths.begin() + static_cast<std::ptrdiff_t>(count));
    return lengths;
}

}  // namespace

// The match-finder chain arrays hold one index per input byte. Storing them as
// the narrowest type that can address the input (32-bit for inputs up to ~4 GB)
// halves their footprint versus std::size_t; encode_lz77 dispatches on size.
template <typename Index>
ByteVector encode_lz77_impl(std::span<const std::uint8_t> input,
                            const CompressionOptions& options) {
    constexpr Index kNoPos = std::numeric_limits<Index>::max();

    ByteVector output;
    output.reserve(input.size());

    // Pool workers encode blocks back to back; reusing the multi-megabyte
    // match-finder arrays per thread avoids reallocating and re-faulting them
    // for every block. assign() keeps capacity while writing the reset values
    // the old fresh allocations held.
    static thread_local std::vector<Index> hash_heads;
    static thread_local std::vector<Index> previous;
    hash_heads.assign(kHashSize, kNoPos);
    previous.assign(input.size(), kNoPos);

    const auto window_size = std::max<std::size_t>(kMinMatch, options.window_size);
    const auto max_match = std::max<std::size_t>(kMinMatch, options.max_match);
    const auto max_chain_depth = std::max<std::size_t>(1, options.max_chain_depth);
    const auto nice_length = std::max<std::size_t>(kMinMatch, options.nice_length);

    auto insert_position = [&](std::size_t position) {
        if (position + kMinMatch <= input.size()) {
            const auto hash = hash4(input, position);
            previous[position] = hash_heads[hash];
            hash_heads[hash] = static_cast<Index>(position);
        }
    };

    // Walk the hash chain at `pos` for the longest match within the window. Does
    // not insert `pos`; the caller controls insertion order. Reused for the lazy
    // one-position lookahead.
    auto find_best = [&](std::size_t pos, std::size_t& out_length, std::size_t& out_distance) {
        out_length = 0;
        out_distance = 0;
        if (pos + kMinMatch > input.size()) {
            return;
        }

        const auto current = load_u32_le(input.data() + pos);
        auto candidate = hash_heads[hash4_value(current)];
        const auto limit = std::min(max_match, input.size() - pos);
        const auto nice = std::min(nice_length, limit);
        std::size_t depth = 0;
        std::size_t best_length = 0;
        std::size_t best_distance = 0;

        // Hash chains keep recent candidates in newest-to-oldest order. Once a
        // candidate falls outside the window, the rest of the chain is older too.
        while (candidate != kNoPos && depth < max_chain_depth) {
            const auto next = previous[candidate];
            if (candidate >= pos) {
                candidate = next;
                ++depth;
                continue;
            }

            const auto distance = pos - candidate;
            if (distance > window_size) {
                break;
            }

            if (best_length < limit &&
                input[candidate + best_length] == input[pos + best_length] &&
                load_u32_le(input.data() + candidate) == current) {
                const std::size_t length =
                    common_prefix(input.data() + candidate, input.data() + pos, limit);

                if (length > best_length) {
                    best_length = length;
                    best_distance = distance;

                    if (best_length >= nice) {
                        break;
                    }
                }
            }

            candidate = next;
            ++depth;
        }

        out_length = best_length;
        out_distance = best_distance;
    };

    std::size_t position = 0;
    std::size_t literal_start = 0;
    std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};

    // When lazy matching defers a match by emitting a literal, it has already
    // searched position+1. Carry that result forward so the next iteration reuses
    // it instead of searching the same position again — the hash state is
    // unchanged between the lookahead and the reuse, so the match is identical.
    bool have_lookahead = false;
    std::size_t lookahead_length = 0;
    std::size_t lookahead_distance = 0;

    ParseProgressTicker progress(options.encode_progress, input.size());
    while (position < input.size()) {
        progress.tick(position);
        std::size_t best_length = 0;
        std::size_t best_distance = 0;
        if (have_lookahead) {
            best_length = lookahead_length;
            best_distance = lookahead_distance;
            have_lookahead = false;
        } else {
            find_best(position, best_length, best_distance);
        }

        insert_position(position);

        // A repeat-offset match reuses a recent distance, so it never stores a
        // distance and is strictly cheaper than a fresh match of the same length.
        // Prefer it whenever it is at least as long as the best normal match.
        std::size_t best_rep_length = 0;
        std::size_t best_rep_index = 0;
        if (position + kMinMatch <= input.size()) {
            const auto limit = std::min(max_match, input.size() - position);
            for (std::size_t i = 0; i < kRepCount; ++i) {
                const auto length = match_length_at(input, position, reps[i], limit);
                if (length > best_rep_length) {
                    best_rep_length = length;
                    best_rep_index = i;
                }
            }
        }

        const bool take_rep = best_rep_length >= kMinMatch && best_rep_length >= best_length;
        bool take_match = !take_rep && best_length >= kMinMatch;

        // Lazy matching: a normal match that is not already long enough may be
        // worse than one starting one byte later. Peek at position+1 and defer
        // (emit a literal now) when the deferred path is actually cheaper under
        // the token cost model — this keeps the old strictly-longer deferrals
        // and adds equal-or-similar-length ones that land a much nearer
        // distance, while refusing barely-longer matches that jump far away.
        // A repeat-offset available at position+1 also defers: reps code no
        // distance, so even an equal-length rep beats this match. (Emitting a
        // literal leaves the recent-distance list unchanged, so reps checked
        // here are exactly the reps the next iteration will see.)
        if (take_match && options.lazy_matching && best_length < nice_length &&
            position + 1 < input.size()) {
            std::size_t next_length = 0;
            std::size_t next_distance = 0;
            find_best(position + 1, next_length, next_distance);

            bool defer = false;
            if (next_length >= kMinMatch) {
                // Compare per-byte cost of taking the match now over
                // best_length bytes vs a literal plus the next match over
                // 1 + next_length bytes (cross-multiplied to stay integral).
                const auto take_now = static_cast<std::uint64_t>(
                    match_cost(best_length, best_distance));
                const auto deferred = static_cast<std::uint64_t>(literal_cost()) +
                                      match_cost(next_length, next_distance);
                defer = deferred * best_length < take_now * (1 + next_length);
            }
            if (!defer && position + kMinMatch + 1 <= input.size()) {
                const auto rep_look_limit =
                    std::min(max_match, input.size() - (position + 1));
                for (std::size_t i = 0; i < kRepCount; ++i) {
                    const auto rep_length =
                        match_length_at(input, position + 1, reps[i], rep_look_limit);
                    if (rep_length >= best_length) {
                        defer = true;
                        break;
                    }
                }
            }

            if (defer) {
                take_match = false;  // defer to a literal; position+1 wins next round
                // The loop advances to position+1 next (a literal consumes one
                // byte) and nothing is inserted in between, so this search is
                // exactly what the next iteration would repeat — reuse it.
                have_lookahead = true;
                lookahead_length = next_length;
                lookahead_distance = next_distance;
            }
        }

        if (take_rep) {
            write_literal_run(output, input, literal_start, position - literal_start);

            output.push_back(static_cast<std::uint8_t>(kRepTokenBase + best_rep_index));
            write_varuint(output, best_rep_length);

            // Move the reused distance to the front of the recent-distance list.
            const auto chosen = reps[best_rep_index];
            for (std::size_t j = best_rep_index; j > 0; --j) {
                reps[j] = reps[j - 1];
            }
            reps[0] = chosen;

            for (std::size_t offset = 1; offset < best_rep_length; ++offset) {
                insert_position(position + offset);
            }

            position += best_rep_length;
            literal_start = position;
        } else if (take_match) {
            write_literal_run(output, input, literal_start, position - literal_start);

            output.push_back(kMatchToken);
            write_varuint(output, best_length);
            write_varuint(output, best_distance);

            reps = {best_distance, reps[0], reps[1], reps[2]};

            // The decoder will copy through overlapping matches. The encoder still
            // indexes each covered position so later matches can refer inside this run.
            for (std::size_t offset = 1; offset < best_length; ++offset) {
                insert_position(position + offset);
            }

            position += best_length;
            literal_start = position;
        } else {
            ++position;
        }
    }

    write_literal_run(output, input, literal_start, input.size() - literal_start);
    return output;
}

// Experimental swarm parse: encode ONE logical block with the full-block match
// window while parsing fixed segments concurrently. Phase 1 builds immutable
// per-segment hash-chain indexes — insertion depends only on input bytes, never
// on parse decisions, and the stored links equal exactly what sequential
// insertion would have produced. Phase 2 parses every segment in parallel; a
// query walks the segment's own incrementally revealed chain and then earlier
// segments' completed chains, which visits the same candidates in the same
// newest-to-oldest order under the same depth budget as the serial matcher.
// Matches never extend past a segment boundary and the recent-distance list
// restarts per segment, so phase 2 emits explicit distances only; the serial
// phase 3 rewrites distance==rep matches into rep tokens under the true
// decoder-visible state and merges adjacent literal runs. The result is a
// standard token payload — the decoder is untouched — and it is deterministic
// for a given input and segmentation regardless of worker timing.
ByteVector finalize_swarm_tokens(std::span<const std::uint8_t> tokens) {
    ByteVector output;
    output.reserve(tokens.size());
    std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};
    ByteVector pending_literals;
    std::size_t cursor = 0;

    const auto flush_literals = [&] {
        if (!pending_literals.empty()) {
            output.push_back(kLiteralToken);
            write_varuint(output, pending_literals.size());
            output.insert(output.end(), pending_literals.begin(), pending_literals.end());
            pending_literals.clear();
        }
    };

    while (cursor < tokens.size()) {
        const auto command = tokens[cursor++];
        if (command == kLiteralToken) {
            const auto length = static_cast<std::size_t>(read_varuint(tokens, cursor));
            if (length > tokens.size() - cursor) {
                throw FormatError("swarm literal run exceeds token buffer");
            }
            pending_literals.insert(
                pending_literals.end(),
                tokens.begin() + static_cast<std::ptrdiff_t>(cursor),
                tokens.begin() + static_cast<std::ptrdiff_t>(cursor + length));
            cursor += length;
        } else if (command == kMatchToken) {
            flush_literals();
            const auto length = read_varuint(tokens, cursor);
            const auto distance = static_cast<std::size_t>(read_varuint(tokens, cursor));
            std::size_t rep_index = kRepCount;
            for (std::size_t i = 0; i < kRepCount; ++i) {
                if (reps[i] == distance) {
                    rep_index = i;
                    break;
                }
            }
            if (rep_index < kRepCount) {
                output.push_back(static_cast<std::uint8_t>(kRepTokenBase + rep_index));
                write_varuint(output, length);
                const auto chosen = reps[rep_index];
                for (std::size_t j = rep_index; j > 0; --j) {
                    reps[j] = reps[j - 1];
                }
                reps[0] = chosen;
            } else {
                output.push_back(kMatchToken);
                write_varuint(output, length);
                write_varuint(output, distance);
                reps = {distance, reps[0], reps[1], reps[2]};
            }
        } else {
            // Phase 2 never emits rep commands; anything else is a logic error.
            throw FormatError("unexpected command in swarm token stream");
        }
    }
    flush_literals();
    return output;
}

template <typename Index>
ByteVector encode_lz77_swarm_impl(std::span<const std::uint8_t> input,
                                  const CompressionOptions& options) {
    constexpr Index kNoPos = std::numeric_limits<Index>::max();

    // Segment size balances parallel grain against per-segment head-table
    // memory (kHashSize indexes each); large blocks grow segments instead of
    // accumulating tables.
    constexpr std::size_t kMinSegment = std::size_t{2} << 20;
    constexpr std::size_t kMaxSegments = 64;
    const auto segment_size = std::max(
        kMinSegment, (input.size() + kMaxSegments - 1) / kMaxSegments);
    const auto segment_count =
        input.size() == 0 ? std::size_t{0}
                          : (input.size() + segment_size - 1) / segment_size;
    if (segment_count <= 1) {
        return encode_lz77_impl<Index>(input, options);
    }

    const auto window_size = std::max<std::size_t>(kMinMatch, options.window_size);
    const auto max_match = std::max<std::size_t>(kMinMatch, options.max_match);
    const auto max_chain_depth = std::max<std::size_t>(1, options.max_chain_depth);
    const auto nice_length = std::max<std::size_t>(kMinMatch, options.nice_length);

    const auto segment_begin = [segment_size](std::size_t segment) {
        return segment * segment_size;
    };
    const auto segment_end_at = [segment_size, &input](std::size_t segment) {
        return std::min(input.size(), segment * segment_size + segment_size);
    };

    // Phase 1 state. `previous` links each position to the prior same-hash
    // position of the same segment; phase 2 treats it as read-only.
    std::vector<Index> previous(input.size(), kNoPos);
    std::vector<std::vector<Index>> segment_heads(segment_count);

    const auto build_segment = [&](std::size_t segment) {
        auto& heads = segment_heads[segment];
        heads.assign(kHashSize, kNoPos);
        const auto end = segment_end_at(segment);
        for (auto position = segment_begin(segment); position < end; ++position) {
            if (position + kMinMatch <= input.size()) {
                const auto hash = hash4(input, position);
                previous[position] = heads[hash];
                heads[hash] = static_cast<Index>(position);
            }
        }
    };

    const auto parse_segment = [&](std::size_t segment) {
        ByteVector output;
        const auto start = segment_begin(segment);
        const auto end = segment_end_at(segment);
        output.reserve((end - start) / 8 + 64);

        // Reveals the segment's own chain: heads advance as positions are
        // scanned while the links themselves were precomputed in phase 1.
        thread_local std::vector<Index> local_heads;
        local_heads.assign(kHashSize, kNoPos);

        const auto insert_position = [&](std::size_t position) {
            if (position + kMinMatch <= input.size()) {
                local_heads[hash4(input, position)] = static_cast<Index>(position);
            }
        };

        const auto find_best = [&](std::size_t pos, std::size_t& out_length,
                                   std::size_t& out_distance) {
            out_length = 0;
            out_distance = 0;
            if (pos + kMinMatch > input.size()) {
                return;
            }

            const auto current = load_u32_le(input.data() + pos);
            const auto hash = hash4_value(current);
            const auto limit = std::min(max_match, end - pos);
            const auto nice = std::min(nice_length, limit);
            std::size_t depth = 0;
            std::size_t best_length = 0;
            std::size_t best_distance = 0;

            // Walk one chain newest-to-oldest. Returns false once the search
            // should stop entirely: the window is exhausted (older segments are
            // farther still) or a nice-length match was found.
            const auto consider_chain = [&](Index candidate) {
                while (candidate != kNoPos && depth < max_chain_depth) {
                    const auto next = previous[candidate];
                    if (candidate >= pos) {  // defensive: chains hold older positions
                        candidate = next;
                        ++depth;
                        continue;
                    }

                    const auto distance = pos - candidate;
                    if (distance > window_size) {
                        return false;
                    }

                    if (best_length < limit &&
                        input[candidate + best_length] == input[pos + best_length] &&
                        load_u32_le(input.data() + candidate) == current) {
                        const std::size_t length = common_prefix(
                            input.data() + candidate, input.data() + pos, limit);
                        if (length > best_length) {
                            best_length = length;
                            best_distance = distance;
                            if (best_length >= nice) {
                                return false;
                            }
                        }
                    }

                    candidate = next;
                    ++depth;
                }
                return depth < max_chain_depth;
            };

            bool keep_searching = consider_chain(local_heads[hash]);
            for (std::size_t prior = segment; keep_searching && prior > 0;) {
                --prior;
                const auto newest_position = segment_end_at(prior) - 1;
                if (pos - newest_position > window_size) {
                    break;
                }
                keep_searching = consider_chain(segment_heads[prior][hash]);
            }

            out_length = best_length;
            out_distance = best_distance;
        };

        std::size_t position = start;
        std::size_t literal_start = start;
        std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};
        bool have_lookahead = false;
        std::size_t lookahead_length = 0;
        std::size_t lookahead_distance = 0;

        while (position < end) {
            std::size_t best_length = 0;
            std::size_t best_distance = 0;
            if (have_lookahead) {
                best_length = lookahead_length;
                best_distance = lookahead_distance;
                have_lookahead = false;
            } else {
                find_best(position, best_length, best_distance);
            }

            insert_position(position);

            std::size_t best_rep_length = 0;
            std::size_t best_rep_index = 0;
            if (position + kMinMatch <= input.size()) {
                const auto limit = std::min(max_match, end - position);
                for (std::size_t i = 0; i < kRepCount; ++i) {
                    const auto length = match_length_at(input, position, reps[i], limit);
                    if (length > best_rep_length) {
                        best_rep_length = length;
                        best_rep_index = i;
                    }
                }
            }

            const bool take_rep =
                best_rep_length >= kMinMatch && best_rep_length >= best_length;
            bool take_match = !take_rep && best_length >= kMinMatch;

            if (take_match && options.lazy_matching && best_length < nice_length &&
                position + 1 < end) {
                std::size_t next_length = 0;
                std::size_t next_distance = 0;
                find_best(position + 1, next_length, next_distance);

                bool defer = false;
                if (next_length >= kMinMatch) {
                    const auto take_now = static_cast<std::uint64_t>(
                        match_cost(best_length, best_distance));
                    const auto deferred = static_cast<std::uint64_t>(literal_cost()) +
                                          match_cost(next_length, next_distance);
                    defer = deferred * best_length < take_now * (1 + next_length);
                }
                if (!defer && position + kMinMatch + 1 <= input.size()) {
                    const auto rep_look_limit =
                        std::min(max_match, end - (position + 1));
                    for (std::size_t i = 0; i < kRepCount; ++i) {
                        const auto rep_length = match_length_at(
                            input, position + 1, reps[i], rep_look_limit);
                        if (rep_length >= best_length) {
                            defer = true;
                            break;
                        }
                    }
                }

                if (defer) {
                    take_match = false;
                    have_lookahead = true;
                    lookahead_length = next_length;
                    lookahead_distance = next_distance;
                }
            }

            if (take_rep) {
                write_literal_run(output, input, literal_start, position - literal_start);

                // The segment-local recent-distance list may disagree with the
                // decoder's, so the distance is stored explicitly; phase 3
                // restores rep coding wherever the true stream state allows.
                output.push_back(kMatchToken);
                write_varuint(output, best_rep_length);
                write_varuint(output, reps[best_rep_index]);

                const auto chosen = reps[best_rep_index];
                for (std::size_t j = best_rep_index; j > 0; --j) {
                    reps[j] = reps[j - 1];
                }
                reps[0] = chosen;

                for (std::size_t offset = 1; offset < best_rep_length; ++offset) {
                    insert_position(position + offset);
                }

                position += best_rep_length;
                literal_start = position;
            } else if (take_match) {
                write_literal_run(output, input, literal_start, position - literal_start);

                output.push_back(kMatchToken);
                write_varuint(output, best_length);
                write_varuint(output, best_distance);

                reps = {best_distance, reps[0], reps[1], reps[2]};

                for (std::size_t offset = 1; offset < best_length; ++offset) {
                    insert_position(position + offset);
                }

                position += best_length;
                literal_start = position;
            } else {
                ++position;
            }
        }

        write_literal_run(output, input, literal_start, end - literal_start);
        return output;
    };

    std::vector<ByteVector> parsed(segment_count);
    const auto run_phases = [&](core::TaskExecutor& executor) {
        {
            std::vector<std::future<void>> builds;
            builds.reserve(segment_count);
            for (std::size_t segment = 0; segment < segment_count; ++segment) {
                builds.push_back(executor.submit([&, segment] {
                    build_segment(segment);
                }));
            }
            for (auto& task : builds) {
                executor.wait(task);
            }
        }
        {
            std::vector<std::future<ByteVector>> parses;
            parses.reserve(segment_count);
            for (std::size_t segment = 0; segment < segment_count; ++segment) {
                parses.push_back(executor.submit([&, segment] {
                    return parse_segment(segment);
                }));
            }
            for (std::size_t segment = 0; segment < segment_count; ++segment) {
                parsed[segment] = executor.wait(parses[segment]);
                if (options.encode_progress) {
                    options.encode_progress(static_cast<double>(segment + 1) /
                                            static_cast<double>(segment_count));
                }
            }
        }
    };

    if (auto* ambient = core::TaskExecutor::current()) {
        // Inside a block worker (or a scoped serial-analysis executor) the
        // segment tasks join the operation's existing budget; cooperative
        // waits keep the calling worker productive and no pool is nested.
        run_phases(*ambient);
    } else {
        const auto requested_threads = options.thread_count == 0
            ? core::physical_core_count()
            : options.thread_count;
        const auto worker_budget =
            std::min<std::size_t>(std::max<std::size_t>(1, requested_threads),
                                  segment_count);
        if (worker_budget <= 1) {
            for (std::size_t segment = 0; segment < segment_count; ++segment) {
                build_segment(segment);
            }
            for (std::size_t segment = 0; segment < segment_count; ++segment) {
                parsed[segment] = parse_segment(segment);
                if (options.encode_progress) {
                    options.encode_progress(static_cast<double>(segment + 1) /
                                            static_cast<double>(segment_count));
                }
            }
        } else {
            core::TaskExecutor executor(worker_budget);
            run_phases(executor);
        }
    }

    std::size_t total = 0;
    for (const auto& tokens : parsed) {
        total += tokens.size();
    }
    ByteVector tokens;
    tokens.reserve(total);
    for (auto& part : parsed) {
        tokens.insert(tokens.end(), part.begin(), part.end());
        part.clear();
        part.shrink_to_fit();
    }
    return finalize_swarm_tokens(tokens);
}

// Greedy parse driven by a binary search tree of suffixes (an LZMA-style bt4
// finder over a *cyclic window*). The son array is sized to the window, not the
// input: position q occupies cyclic slot q % cyclic_size, so slots are reused as
// the window slides. A candidate that has fallen out of the window (delta >=
// cyclic_size) terminates the descent — this is the implicit node deletion that
// both bounds memory to the window and keeps the descent short on large inputs.
// Stored values are absolute positions so the window check is a plain subtraction;
// stale son pointers that reference evicted positions are never dereferenced
// because each candidate is window-checked before its slot is read. Recorded
// matches are validated by byte comparison, so any tree-logic error can only
// reduce ratio, never produce an invalid match.
template <typename Index>
ByteVector encode_lz77_tree(std::span<const std::uint8_t> input,
                            const CompressionOptions& options) {
    constexpr Index kNoPos = std::numeric_limits<Index>::max();
    const std::size_t n = input.size();

    ByteVector output;
    output.reserve(n);
    if (n == 0) {
        return output;
    }

    const auto window_size = std::max<std::size_t>(kMinMatch, options.window_size);
    // The cyclic buffer holds one slot per in-window position. When the window is
    // as large as the input there is no wrap (cyclic_size == n), so this degrades
    // to the original full-window tree; otherwise memory is bounded to the window.
    const auto cyclic_size = std::min(window_size, n);

    // Reused per worker thread across blocks (see encode_lz77_impl).
    static thread_local std::vector<Index> head;
    static thread_local std::vector<Index> son;
    head.assign(kHashSize, kNoPos);
    // son[2*slot] smaller subtree, son[2*slot+1] larger subtree, indexed by the
    // candidate's cyclic slot rather than its absolute position.
    son.assign(2 * cyclic_size, kNoPos);

    const auto max_match = std::max<std::size_t>(kMinMatch, options.max_match);
    const auto depth_cutoff = std::max<std::size_t>(1, options.max_chain_depth);
    const auto nice_length = std::max<std::size_t>(kMinMatch, options.nice_length);

    // cyclic_pos == p % cyclic_size for the position currently being processed.
    // process() is called for strictly increasing, contiguous positions, so the
    // caller advances it incrementally and we avoid a modulo in the hot loop.
    auto process = [&](std::size_t p, std::size_t cyclic_pos, bool record,
                       std::size_t& best_len, std::size_t& best_dist) {
        best_len = 0;
        best_dist = 0;
        if (p + kMinMatch > n) {
            return;
        }

        const auto hash = hash4(input, p);
        auto node = head[hash];
        head[hash] = static_cast<Index>(p);

        Index* ptr_smaller = &son[2 * cyclic_pos];
        Index* ptr_larger = &son[2 * cyclic_pos + 1];
        std::size_t len_smaller = 0;
        std::size_t len_larger = 0;
        const auto limit = std::min(max_match, n - p);
        std::size_t cycles = depth_cutoff;

        while (true) {
            // node < p always (positions are inserted in increasing order), so the
            // delta below never underflows once node is known to be a real position.
            if (node == kNoPos) {
                *ptr_smaller = kNoPos;
                *ptr_larger = kNoPos;
                break;
            }

            const std::size_t np = node;
            const std::size_t delta = p - np;
            if (delta >= cyclic_size || cycles-- == 0) {
                // Out of window (deleted) or search budget exhausted.
                *ptr_smaller = kNoPos;
                *ptr_larger = kNoPos;
                break;
            }

            const auto node_slot =
                cyclic_pos >= delta ? cyclic_pos - delta : cyclic_pos + cyclic_size - delta;

            // The descent needs this node's children after the byte compare;
            // fetching them under the compare hides the tree-array miss.
            prefetch_read(&son[2 * node_slot]);

            std::size_t len = std::min(len_smaller, len_larger);
            len += common_prefix(input.data() + np + len, input.data() + p + len, limit - len);

            if (record && len > best_len) {
                best_len = len;
                best_dist = delta;
            }

            if (len == limit) {
                // Suffixes match as far as comparable; splice node's children in.
                *ptr_smaller = son[2 * node_slot];
                *ptr_larger = son[2 * node_slot + 1];
                break;
            }

            if (input[np + len] < input[p + len]) {
                *ptr_smaller = static_cast<Index>(np);
                ptr_smaller = &son[2 * node_slot + 1];
                node = son[2 * node_slot + 1];
                len_smaller = len;
            } else {
                *ptr_larger = static_cast<Index>(np);
                ptr_larger = &son[2 * node_slot];
                node = son[2 * node_slot];
                len_larger = len;
            }
        }
    };

    // Search without inserting. Level 7 uses this for a one-byte lazy lookahead:
    // the next real process() call still owns insertion, so the cyclic tree stays
    // identical whether the current match is taken or deferred.
    auto find_best = [&](std::size_t p, std::size_t& best_len,
                         std::size_t& best_dist) {
        best_len = 0;
        best_dist = 0;
        if (p + kMinMatch > n) {
            return;
        }

        auto node = head[hash4(input, p)];
        std::size_t len_smaller = 0;
        std::size_t len_larger = 0;
        const auto limit = std::min(max_match, n - p);
        std::size_t cycles = depth_cutoff;
        while (node != kNoPos) {
            const std::size_t np = node;
            const std::size_t delta = p - np;
            if (delta >= cyclic_size || cycles-- == 0) {
                return;
            }

            const auto p_slot = p % cyclic_size;
            const auto node_slot =
                p_slot >= delta ? p_slot - delta : p_slot + cyclic_size - delta;
            prefetch_read(&son[2 * node_slot]);

            std::size_t len = std::min(len_smaller, len_larger);
            len += common_prefix(input.data() + np + len, input.data() + p + len,
                                 limit - len);
            if (len > best_len) {
                best_len = len;
                best_dist = delta;
            }
            if (len == limit) {
                return;
            }

            if (input[np + len] < input[p + len]) {
                node = son[2 * node_slot + 1];
                len_smaller = len;
            } else {
                node = son[2 * node_slot];
                len_larger = len;
            }
        }
    };

    std::size_t position = 0;
    std::size_t literal_start = 0;
    std::size_t cyclic_pos = 0;
    std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};

    auto advance_cyclic = [&] {
        if (++cyclic_pos == cyclic_size) {
            cyclic_pos = 0;
        }
    };

    auto skip_cover = [&](std::size_t run) {
        std::size_t unused_len = 0;
        std::size_t unused_dist = 0;
        for (std::size_t offset = 1; offset < run; ++offset) {
            advance_cyclic();
            process(position + offset, cyclic_pos, false, unused_len, unused_dist);
        }
    };

    ParseProgressTicker progress(options.encode_progress, n);
    while (position < n) {
        progress.tick(position);
        std::size_t best_length = 0;
        std::size_t best_distance = 0;
        process(position, cyclic_pos, true, best_length, best_distance);

        std::size_t best_rep_length = 0;
        std::size_t best_rep_index = 0;
        if (position + kMinMatch <= n) {
            const auto limit = std::min(max_match, n - position);
            for (std::size_t i = 0; i < kRepCount; ++i) {
                const auto length = match_length_at(input, position, reps[i], limit);
                if (length > best_rep_length) {
                    best_rep_length = length;
                    best_rep_index = i;
                }
            }
        }

        const bool take_rep = best_rep_length >= kMinMatch && best_rep_length >= best_length;
        bool take_match = !take_rep && best_length >= kMinMatch;

        if (take_match && options.lazy_matching && best_length < nice_length &&
            position + 1 < n) {
            std::size_t next_length = 0;
            std::size_t next_distance = 0;
            find_best(position + 1, next_length, next_distance);

            bool defer = false;
            if (next_length >= kMinMatch) {
                const auto take_now = static_cast<std::uint64_t>(
                    match_cost(best_length, best_distance));
                const auto deferred = static_cast<std::uint64_t>(literal_cost()) +
                                      match_cost(next_length, next_distance);
                defer = deferred * best_length < take_now * (1 + next_length);
            }
            if (!defer && position + kMinMatch + 1 <= n) {
                const auto rep_limit = std::min(max_match, n - (position + 1));
                for (std::size_t i = 0; i < kRepCount; ++i) {
                    if (match_length_at(input, position + 1, reps[i], rep_limit) >=
                        best_length) {
                        defer = true;
                        break;
                    }
                }
            }
            if (defer) {
                take_match = false;
            }
        }

        if (take_rep) {
            write_literal_run(output, input, literal_start, position - literal_start);
            output.push_back(static_cast<std::uint8_t>(kRepTokenBase + best_rep_index));
            write_varuint(output, best_rep_length);

            const auto chosen = reps[best_rep_index];
            for (std::size_t j = best_rep_index; j > 0; --j) {
                reps[j] = reps[j - 1];
            }
            reps[0] = chosen;

            skip_cover(best_rep_length);
            position += best_rep_length;
            literal_start = position;
        } else if (take_match) {
            write_literal_run(output, input, literal_start, position - literal_start);
            output.push_back(kMatchToken);
            write_varuint(output, best_length);
            write_varuint(output, best_distance);

            reps = {best_distance, reps[0], reps[1], reps[2]};

            skip_cover(best_length);
            position += best_length;
            literal_start = position;
        } else {
            ++position;
        }

        // Keep cyclic_pos == position % cyclic_size. skip_cover already advanced
        // it across the covered run; this single step accounts for the position
        // the loop now moves to (the literal byte or the slot past the match).
        advance_cyclic();
    }

    write_literal_run(output, input, literal_start, n - literal_start);
    return output;
}

// LZMA-style candidate enumeration over the same cyclic-window binary tree the
// greedy tree matcher uses: advancing to a position both inserts it and yields
// the improving (length, distance) pairs met during the descent. Because the
// tree orders suffixes, a bounded descent surfaces far better candidates than
// an equally bounded hash-chain walk, which is what the optimal parser's DP
// wants: several distinct lengths, each at its nearest distance.
template <typename Index>
class TreeMatchSource {
public:
    TreeMatchSource(std::span<const std::uint8_t> input,
                    std::size_t window_size,
                    std::size_t max_match,
                    std::size_t depth_cutoff)
        : input_(input),
          n_(input.size()),
          cyclic_size_(std::min(std::max<std::size_t>(kMinMatch, window_size),
                                std::max<std::size_t>(1, n_))),
          max_match_(max_match),
          depth_cutoff_(depth_cutoff) {
        head_.assign(kHashSize, kNoPos);
        son_.assign(2 * cyclic_size_, kNoPos);
    }

    // Must be called for every position in increasing order (the tree needs
    // each position inserted). When `matches` is non-null the improving
    // candidates are recorded through add_match_candidate.
    void advance(std::size_t p, MatchList* matches, std::size_t max_candidates) {
        const auto cyclic_pos = cyclic_pos_;
        if (++cyclic_pos_ == cyclic_size_) {
            cyclic_pos_ = 0;
        }
        if (p + kMinMatch > n_) {
            return;
        }

        const auto hash = hash4(input_, p);
        auto node = head_[hash];
        head_[hash] = static_cast<Index>(p);

        Index* ptr_smaller = &son_[2 * cyclic_pos];
        Index* ptr_larger = &son_[2 * cyclic_pos + 1];
        std::size_t len_smaller = 0;
        std::size_t len_larger = 0;
        const auto limit = std::min(max_match_, n_ - p);
        std::size_t cycles = depth_cutoff_;
        std::size_t best_len = 0;

        while (true) {
            if (node == kNoPos) {
                *ptr_smaller = kNoPos;
                *ptr_larger = kNoPos;
                return;
            }

            const std::size_t np = node;
            const std::size_t delta = p - np;
            if (delta >= cyclic_size_ || cycles-- == 0) {
                *ptr_smaller = kNoPos;
                *ptr_larger = kNoPos;
                return;
            }

            const auto node_slot =
                cyclic_pos >= delta ? cyclic_pos - delta : cyclic_pos + cyclic_size_ - delta;
            prefetch_read(&son_[2 * node_slot]);

            std::size_t len = std::min(len_smaller, len_larger);
            len += common_prefix(input_.data() + np + len, input_.data() + p + len,
                                 limit - len);

            if (matches != nullptr && len > best_len) {
                best_len = len;
                add_match_candidate(*matches, len, delta, max_candidates);
            }

            if (len == limit) {
                *ptr_smaller = son_[2 * node_slot];
                *ptr_larger = son_[2 * node_slot + 1];
                return;
            }

            if (input_[np + len] < input_[p + len]) {
                *ptr_smaller = static_cast<Index>(np);
                ptr_smaller = &son_[2 * node_slot + 1];
                node = son_[2 * node_slot + 1];
                len_smaller = len;
            } else {
                *ptr_larger = static_cast<Index>(np);
                ptr_larger = &son_[2 * node_slot];
                node = son_[2 * node_slot];
                len_larger = len;
            }
        }
    }

private:
    static constexpr Index kNoPos = std::numeric_limits<Index>::max();

    std::span<const std::uint8_t> input_;
    std::size_t n_;
    std::size_t cyclic_size_;
    std::size_t max_match_;
    std::size_t depth_cutoff_;
    std::size_t cyclic_pos_ = 0;
    std::vector<Index> head_;
    std::vector<Index> son_;
};

// Exact producer-consumer split for the global tree matcher and optimal DP.
// The tree is inherently ordered, but it does not depend on DP decisions. A
// worker therefore discovers the same candidates as the serial parser one tile
// ahead while the calling worker consumes the current tile. Each tile is a
// separate work-stealable task; a waiting consumer cooperatively executes it if
// every helper is busy with another block. This bounded double buffer avoids a
// per-position whole-block reservoir, and requesting candidates at formerly
// unreachable positions cannot affect tree mutation.
template <typename Index>
class TreeCandidatePipeline {
    struct Tile {
        std::size_t start = 0;
        std::vector<std::uint8_t> counts;
        std::vector<Match> matches;
    };

public:
    TreeCandidatePipeline(std::span<const std::uint8_t> input,
                          std::size_t window_size,
                          std::size_t max_match,
                          std::size_t chain_depth,
                          std::size_t max_candidates,
                          core::TaskExecutor& executor)
        : input_(input),
          max_match_(max_match),
          max_candidates_(max_candidates),
          executor_(executor),
          tree_(input, window_size, max_match, chain_depth) {
        schedule_next();
    }

    TreeCandidatePipeline(const TreeCandidatePipeline&) = delete;
    TreeCandidatePipeline& operator=(const TreeCandidatePipeline&) = delete;

    ~TreeCandidatePipeline() {
        if (next_tile_.valid()) {
            try {
                (void)executor_.wait(next_tile_);
            } catch (...) {
                // next() normally surfaces producer errors; suppress a second
                // exception only when the parser is already unwinding.
            }
        }
    }

    std::span<const Match> next(std::size_t position) {
        if (!current_ || current_offset_ == current_->counts.size()) {
            if (!next_tile_.valid()) {
                throw FormatError("tree candidate pipeline ended early");
            }
            current_.emplace(executor_.wait(next_tile_));
            current_offset_ = 0;
            current_match_ = 0;
            schedule_next();
        }

        if (current_->start + current_offset_ != position) {
            throw FormatError("tree candidate pipeline lost position order");
        }
        const auto count = static_cast<std::size_t>(
            current_->counts[current_offset_++]);
        if (current_match_ > current_->matches.size() ||
            count > current_->matches.size() - current_match_) {
            throw FormatError("tree candidate pipeline has invalid framing");
        }
        const auto result = std::span<const Match>(current_->matches)
                                .subspan(current_match_, count);
        current_match_ += count;
        return result;
    }

    void finish() {
        if (next_tile_.valid()) {
            (void)executor_.wait(next_tile_);
        }
    }

private:
    Tile produce_tile(std::size_t start) {
        const auto length = std::min(kTileSize, input_.size() - start);
        Tile tile;
        tile.start = start;
        tile.counts.resize(length);
        tile.matches.reserve(length * std::min<std::size_t>(2, max_candidates_));
        for (std::size_t offset = 0; offset < length; ++offset) {
            MatchList matches;
            tree_.advance(start + offset, &matches, max_candidates_);
            tile.counts[offset] = static_cast<std::uint8_t>(matches.count);
            tile.matches.insert(tile.matches.end(), matches.values.begin(),
                                matches.values.begin() +
                                    static_cast<std::ptrdiff_t>(matches.count));
        }
        return tile;
    }

    void schedule_next() {
        if (next_start_ >= input_.size()) {
            next_tile_ = {};
            return;
        }
        const auto start = next_start_;
        next_start_ += std::min(kTileSize, input_.size() - next_start_);
        next_tile_ = executor_.submit([this, start] {
            return produce_tile(start);
        });
    }

    static constexpr std::size_t kTileSize = std::size_t{256} << 10;

    std::span<const std::uint8_t> input_;
    std::size_t max_match_;
    std::size_t max_candidates_;
    core::TaskExecutor& executor_;
    TreeMatchSource<Index> tree_;
    std::future<Tile> next_tile_;
    std::optional<Tile> current_;
    std::size_t next_start_ = 0;
    std::size_t current_offset_ = 0;
    std::size_t current_match_ = 0;
};

// Tree-swarm greedy parse for levels 8-9. Each segment keeps the preset's
// binary-tree matcher for nearby candidates and consults immutable hash-chain
// snapshots from earlier segments for full-window reach. Segment-local rep
// state is emitted as explicit distances; finalize_swarm_tokens restores the
// true stream-wide rep state after the parallel phase.
template <typename Index>
ByteVector encode_lz77_tree_swarm_impl(std::span<const std::uint8_t> input,
                                      const CompressionOptions& options) {
    constexpr Index kNoPos = std::numeric_limits<Index>::max();
    constexpr std::size_t kMinSegment = std::size_t{2} << 20;
    constexpr std::size_t kMaxSegments = 64;

    const auto segment_size = std::max(
        kMinSegment, (input.size() + kMaxSegments - 1) / kMaxSegments);
    const auto segment_count = input.empty()
        ? std::size_t{0}
        : (input.size() + segment_size - 1) / segment_size;
    if (segment_count <= 1 || options.lazy_matching) {
        return encode_lz77_tree<Index>(input, options);
    }

    const auto segment_begin = [segment_size](std::size_t segment) {
        return segment * segment_size;
    };
    const auto segment_end = [segment_size, &input](std::size_t segment) {
        return std::min(input.size(), (segment + 1) * segment_size);
    };
    const auto window_size = std::max<std::size_t>(kMinMatch, options.window_size);
    const auto max_match = std::max<std::size_t>(kMinMatch, options.max_match);
    const auto depth_limit = std::max<std::size_t>(1, options.max_chain_depth);
    // The local tree retains the preset's full greedy depth. Cross-segment hash
    // probes only seed the measured-cost DP, so its smaller candidate depth is
    // the useful bound and avoids repeating hundreds of chain hops per byte.
    const auto prior_depth_limit =
        std::max<std::size_t>(1, options.optimal_chain_depth);

    std::vector<Index> previous(input.size(), kNoPos);
    std::vector<std::vector<Index>> segment_heads(segment_count);
    const auto build_segment = [&](std::size_t segment) {
        auto& heads = segment_heads[segment];
        heads.assign(kHashSize, kNoPos);
        for (auto position = segment_begin(segment);
             position < segment_end(segment); ++position) {
            if (position + kMinMatch <= input.size()) {
                const auto hash = hash4(input, position);
                previous[position] = heads[hash];
                heads[hash] = static_cast<Index>(position);
            }
        }
    };

    const auto parse_segment = [&](std::size_t segment) {
        const auto start = segment_begin(segment);
        const auto end = segment_end(segment);
        const auto local = input.subspan(start, end - start);
        TreeMatchSource<Index> tree(local, std::min(window_size, local.size()),
                                    max_match, depth_limit);
        ByteVector output;
        output.reserve(local.size() / 8 + 64);
        std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};
        std::size_t position = start;
        std::size_t literal_start = start;

        const auto prior_matches = [&](std::size_t absolute, MatchList& matches) {
            if (absolute + kMinMatch > end) return;
            const auto current = load_u32_le(input.data() + absolute);
            const auto hash = hash4_value(current);
            const auto limit = std::min(max_match, end - absolute);
            std::size_t depth = 0;
            for (std::size_t prior = segment;
                 prior > 0 && depth < prior_depth_limit;) {
                --prior;
                const auto newest = segment_end(prior) - 1;
                if (absolute - newest > window_size) break;
                auto candidate = segment_heads[prior][hash];
                while (candidate != kNoPos && depth < prior_depth_limit) {
                    const auto next = previous[candidate];
                    const auto distance = absolute - static_cast<std::size_t>(candidate);
                    if (distance > window_size) break;
                    if (load_u32_le(input.data() + candidate) == current) {
                        const auto length = common_prefix(
                            input.data() + candidate, input.data() + absolute, limit);
                        add_match_candidate(matches, length, distance, 1);
                        if (length == limit) return;
                    }
                    candidate = next;
                    ++depth;
                }
            }
        };

        while (position < end) {
            MatchList matches;
            tree.advance(position - start, &matches, 1);
            prior_matches(position, matches);
            const auto best_length = matches.count == 0
                ? std::size_t{0} : static_cast<std::size_t>(matches[0].length);
            const auto best_distance = matches.count == 0
                ? std::size_t{0} : static_cast<std::size_t>(matches[0].distance);

            std::size_t best_rep_length = 0;
            std::size_t best_rep_index = 0;
            const auto limit = std::min(max_match, end - position);
            for (std::size_t i = 0; i < kRepCount; ++i) {
                const auto length = match_length_at(input, position, reps[i], limit);
                if (length > best_rep_length) {
                    best_rep_length = length;
                    best_rep_index = i;
                }
            }

            const bool take_rep = best_rep_length >= kMinMatch &&
                                  best_rep_length >= best_length;
            const bool take_match = !take_rep && best_length >= kMinMatch;
            const auto run = take_rep ? best_rep_length
                           : take_match ? best_length : std::size_t{1};

            if (take_rep || take_match) {
                write_literal_run(output, input, literal_start,
                                  position - literal_start);
                const auto distance = take_rep ? reps[best_rep_index] : best_distance;
                output.push_back(kMatchToken);
                write_varuint(output, run);
                write_varuint(output, distance);
                if (take_rep) {
                    const auto chosen = reps[best_rep_index];
                    for (std::size_t i = best_rep_index; i > 0; --i) {
                        reps[i] = reps[i - 1];
                    }
                    reps[0] = chosen;
                } else {
                    reps = {distance, reps[0], reps[1], reps[2]};
                }
                for (std::size_t offset = 1; offset < run; ++offset) {
                    tree.advance(position - start + offset, nullptr, 0);
                }
                position += run;
                literal_start = position;
            } else {
                ++position;
            }
        }
        write_literal_run(output, input, literal_start, end - literal_start);
        return output;
    };

    std::vector<ByteVector> parsed(segment_count);
    const auto run = [&](core::TaskExecutor* executor) {
        if (executor != nullptr) {
            std::vector<std::future<void>> builds;
            builds.reserve(segment_count);
            for (std::size_t segment = 0; segment < segment_count; ++segment) {
                builds.push_back(executor->submit([&, segment] { build_segment(segment); }));
            }
            for (auto& task : builds) executor->wait(task);

            std::vector<std::future<ByteVector>> parses;
            parses.reserve(segment_count);
            for (std::size_t segment = 0; segment < segment_count; ++segment) {
                parses.push_back(executor->submit([&, segment] {
                    return parse_segment(segment);
                }));
            }
            for (std::size_t segment = 0; segment < segment_count; ++segment) {
                parsed[segment] = executor->wait(parses[segment]);
                if (options.encode_progress) {
                    options.encode_progress(static_cast<double>(segment + 1) /
                                            static_cast<double>(segment_count));
                }
            }
        } else {
            for (std::size_t segment = 0; segment < segment_count; ++segment) {
                build_segment(segment);
            }
            for (std::size_t segment = 0; segment < segment_count; ++segment) {
                parsed[segment] = parse_segment(segment);
            }
        }
    };

    if (auto* ambient = core::TaskExecutor::current()) {
        run(ambient);
    } else {
        const auto requested = options.thread_count == 0
            ? core::physical_core_count() : options.thread_count;
        const auto workers = std::min(std::max<std::size_t>(1, requested), segment_count);
        if (workers > 1) {
            core::TaskExecutor executor(workers);
            run(&executor);
        } else {
            run(nullptr);
        }
    }

    std::size_t total = 0;
    for (const auto& part : parsed) total += part.size();
    ByteVector tokens;
    tokens.reserve(total);
    for (auto& part : parsed) {
        tokens.insert(tokens.end(), part.begin(), part.end());
    }
    return finalize_swarm_tokens(tokens);
}

ByteVector encode_lz77(std::span<const std::uint8_t> input,
                       const CompressionOptions& options) {
    // 32-bit indices suffice (and halve match-finder memory) until the input
    // reaches the point where a position would collide with the sentinel.
    if (options.swarm_parse && options.use_tree_matcher &&
        options.enable_optimal_parser &&
        options.optimal_chain_depth < 32 &&
        input.size() < std::numeric_limits<std::uint32_t>::max()) {
        return encode_lz77_tree_swarm_impl<std::uint32_t>(input, options);
    }
    if (options.use_tree_matcher && input.size() < std::numeric_limits<std::uint32_t>::max()) {
        return encode_lz77_tree<std::uint32_t>(input, options);
    }
    if (options.swarm_parse && !options.use_tree_matcher) {
        if (input.size() < std::numeric_limits<std::uint32_t>::max()) {
            return encode_lz77_swarm_impl<std::uint32_t>(input, options);
        }
        return encode_lz77_swarm_impl<std::size_t>(input, options);
    }
    if (input.size() < std::numeric_limits<std::uint32_t>::max()) {
        return encode_lz77_impl<std::uint32_t>(input, options);
    }

    return encode_lz77_impl<std::size_t>(input, options);
}

ByteVector optimal_parse_with_costs(std::span<const std::uint8_t> input,
                                    const CompressionOptions& options,
                                    const ParseCosts& model) {
    constexpr std::uint64_t kInf = std::numeric_limits<std::uint64_t>::max() / 4;

    const auto window_size = std::max<std::size_t>(kMinMatch, options.window_size);
    const auto max_match = std::max<std::size_t>(kMinMatch, options.max_match);
    const auto max_chain_depth = std::max<std::size_t>(1, options.optimal_chain_depth);
    const auto max_candidates =
        std::clamp<std::size_t>(options.max_parser_candidates, 1, kParserCandidateLimit);

    using Index = std::uint32_t;
    constexpr Index kNoPos = std::numeric_limits<Index>::max();
    // The tree levels source parser candidates from the sorted-suffix tree:
    // an equally bounded descent yields several distinct lengths at their
    // nearest distances, where a hash-chain walk mostly re-visits one length.
    const bool use_tree = options.use_tree_matcher;
    std::optional<TreeMatchSource<Index>> tree;
    std::optional<TreeCandidatePipeline<Index>> candidate_pipeline;
    if (use_tree) {
        auto* executor = core::TaskExecutor::current();
        constexpr std::size_t kMinimumPipelineInput = std::size_t{512} << 10;
        // At shallow level-8 depth the tile framing costs more than the tree/DP
        // overlap saves. Level 9 (and custom depths >= 32) has enough discovery
        // work to amortize the bounded pipeline.
        if (max_chain_depth >= 32 && executor != nullptr &&
            executor->worker_count() > 1 && input.size() >= kMinimumPipelineInput) {
            candidate_pipeline.emplace(input, window_size, max_match,
                                       max_chain_depth, max_candidates, *executor);
        } else {
            tree.emplace(input, window_size, max_match, max_chain_depth);
        }
    }

    // Reused per worker thread across blocks (see encode_lz77_impl); the DP
    // arrays here are the largest allocations in the codec.
    static thread_local std::vector<Index> hash_heads;
    static thread_local std::vector<Index> previous;
    static thread_local std::vector<std::uint64_t> costs;
    static thread_local std::vector<ParseDecision> decisions;
    if (!use_tree) {
        hash_heads.assign(kHashSize, kNoPos);
        previous.assign(input.size(), kNoPos);
    }
    // No edge reaches farther than max_match, and a settled position's cost is
    // never read again. Retain only that live frontier; decisions remain
    // whole-input so path reconstruction is unchanged. At level 9 this turns
    // another 8 bytes/input-byte allocation into a few KiB ring.
    const auto max_transition = std::min(max_match, input.size());
    costs.assign(max_transition + 1, kInf);
    decisions.assign(input.size() + 1, ParseDecision{});
    // The recent-distance list depends on the path taken, so each position keeps
    // the rep state of the lowest-cost path that reaches it. Because costs[pos]
    // is final once the loop reaches pos (every in-edge comes from an earlier
    // position), the recorded decision lets us settle reps_at[pos] deterministically.
    // Optimal parsing is bounded below 4 GiB (64 MiB in every preset), and
    // ParseDecision already stores explicit distances as uint32_t. Keeping the
    // four-state history equally narrow saves 16 bytes per input byte: one GiB
    // for a 64 MiB level-9 block, without changing parser decisions or framing.
    using RepState = std::array<Index, kRepCount>;
    static thread_local std::vector<RepState> reps_at;
    reps_at.assign(input.size() + 1, {});
    costs[0] = 0;

    auto insert_position = [&](std::size_t position) {
        if (position + kMinMatch <= input.size()) {
            const auto hash = hash4(input, position);
            previous[position] = hash_heads[hash];
            hash_heads[hash] = static_cast<Index>(position);
        }
    };

    MatchList matches;
    ParseProgressTicker progress(options.encode_progress, input.size());
    for (std::size_t position = 0; position < input.size(); ++position) {
        progress.tick(position);
        // This slot last represented position - 1. No earlier position can
        // reach the newly exposed far edge, so clear it before relaxing edges
        // from the current position.
        if (position + max_transition <= input.size()) {
            costs[(position + max_transition) % costs.size()] = kInf;
        }
        const auto current_cost = costs[position % costs.size()];
        std::span<const Match> match_candidates;
        if (candidate_pipeline) {
            match_candidates = candidate_pipeline->next(position);
        }
        if (current_cost == kInf) {
            // Unreachable position: the match finder still needs it indexed.
            if (use_tree && !candidate_pipeline) {
                tree->advance(position, nullptr, 0);
            } else {
                if (!use_tree) {
                    insert_position(position);
                }
            }
            continue;
        }

        // Settle the rep state for this position from its recorded decision.
        if (position == 0) {
            reps_at[0] = {1, 2, 3, 4};
        } else {
            const auto& decision = decisions[position];
            auto state = reps_at[position - decision.length];
            if (decision.kind == ParseKind::match) {
                state = {decision.distance, state[0], state[1], state[2]};
            } else if (decision.kind == ParseKind::rep) {
                const auto chosen = state[decision.rep_index];
                for (std::size_t j = decision.rep_index; j > 0; --j) {
                    state[j] = state[j - 1];
                }
                state[0] = chosen;
            }
            reps_at[position] = state;
        }

        const auto literal_next = current_cost + cost_literal_model(model, input[position]);
        auto& literal_target = costs[(position + 1) % costs.size()];
        if (literal_next < literal_target) {
            literal_target = literal_next;
            decisions[position + 1] = ParseDecision{1, 0, ParseKind::literal, 0};
        }

        if (use_tree && !candidate_pipeline) {
            matches.clear();
            tree->advance(position, &matches, max_candidates);
            match_candidates = std::span<const Match>(matches.values.data(), matches.count);
        } else if (!use_tree) {
            matches.clear();
            find_matches_at(input,
                            hash_heads,
                            previous,
                            position,
                            window_size,
                            max_match,
                            max_chain_depth,
                            max_candidates,
                            matches);
            match_candidates = std::span<const Match>(matches.values.data(), matches.count);
        }

        // The parser evaluates a bounded set of useful lengths instead of every
        // possible prefix. That keeps the pass linear enough for large blocks.
        for (const auto& match : match_candidates) {
            std::size_t length_count = 0;
            const auto lengths = useful_lengths(match.length, length_count);

            for (std::size_t i = 0; i < length_count; ++i) {
                const auto length = static_cast<std::size_t>(lengths[i]);
                const auto target = position + length;
                const auto candidate_cost =
                    current_cost + cost_match_model(model, length, match.distance);

                auto& target_cost = costs[target % costs.size()];
                if (candidate_cost < target_cost) {
                    target_cost = candidate_cost;
                    decisions[target] = ParseDecision{
                        static_cast<std::uint16_t>(length),
                        match.distance,
                        ParseKind::match,
                        0,
                    };
                }
            }
        }

        // Repeat-offset edges: reuse one of this path's recent distances. They
        // cost no distance, so rep_cost beats match_cost at equal length and the
        // parser naturally prefers them when a recent distance still matches.
        const auto& reps = reps_at[position];
        const auto rep_limit = std::min(max_match, input.size() - position);
        for (std::size_t i = 0; i < kRepCount; ++i) {
            const auto rep_length = match_length_at(input, position, reps[i], rep_limit);
            if (rep_length < kMinMatch) {
                continue;
            }

            std::size_t length_count = 0;
            const auto lengths = useful_lengths(rep_length, length_count);
            for (std::size_t k = 0; k < length_count; ++k) {
                const auto length = static_cast<std::size_t>(lengths[k]);
                const auto target = position + length;
                const auto candidate_cost =
                    current_cost + cost_rep_model(model, length, i);

                auto& target_cost = costs[target % costs.size()];
                if (candidate_cost < target_cost) {
                    target_cost = candidate_cost;
                    decisions[target] = ParseDecision{
                        static_cast<std::uint16_t>(length),
                        0,
                        ParseKind::rep,
                        static_cast<std::uint8_t>(i),
                    };
                }
            }
        }

        if (!use_tree) {
            insert_position(position);  // tree->advance already indexed it
        }
    }

    if (candidate_pipeline) {
        candidate_pipeline->finish();
    }

    ByteVector output;
    output.reserve(input.size());

    std::vector<ParseDecision> path;
    for (std::size_t position = input.size(); position > 0;) {
        const auto decision = decisions[position];
        if (decision.length == 0 || decision.length > position) {
            throw FormatError("optimal parser failed to reconstruct a path");
        }

        path.push_back(decision);
        position -= decision.length;
    }
    std::reverse(path.begin(), path.end());

    std::size_t position = 0;
    std::size_t literal_start = 0;

    for (const auto& decision : path) {
        if (decision.kind == ParseKind::literal) {
            position += 1;
            continue;
        }

        write_literal_run(output, input, literal_start, position - literal_start);
        if (decision.kind == ParseKind::rep) {
            output.push_back(static_cast<std::uint8_t>(kRepTokenBase + decision.rep_index));
            write_varuint(output, decision.length);
        } else {
            output.push_back(kMatchToken);
            write_varuint(output, decision.length);
            write_varuint(output, decision.distance);
        }

        position += decision.length;
        literal_start = position;
    }

    write_literal_run(output, input, literal_start, input.size() - literal_start);
    return output;
}

template <std::size_t Count>
std::array<std::uint64_t, Count> scaled_symbol_costs(
    const std::array<std::uint64_t, Count>& histogram,
    std::uint64_t total,
    std::uint64_t fallback) {
    std::array<std::uint64_t, Count> costs{};
    if (total == 0) {
        costs.fill(fallback);
        return costs;
    }

    // A one-count prior keeps previously unseen symbols finite. The re-parse
    // can legitimately create a new length or slot byte, and the final encoder
    // will add it to its rANS table instead of treating it as impossible.
    const auto denominator = static_cast<double>(total + Count);
    for (std::size_t symbol = 0; symbol < Count; ++symbol) {
        const auto probability = static_cast<double>(histogram[symbol] + 1) / denominator;
        costs[symbol] = static_cast<std::uint64_t>(
            -std::log2(probability) * static_cast<double>(kCostScale) + 0.5);
    }
    return costs;
}

// Estimate the per-byte coded cost of every split stream from a completed parse.
// This tracks the representation the entropy layer really sees: command bytes,
// match-length varint bytes, and position slots. It is intentionally order-0;
// the final entropy bake-off may select clustered order-1 rANS, but an exact
// per-symbol estimate is still substantially more useful to the parser than a
// single average cost for all literals or all lengths.
ParseCosts measure_costs(std::span<const std::uint8_t> tokens) {
    std::array<std::uint64_t, 256> literal_hist{};
    std::array<std::uint64_t, 256> command_hist{};
    std::array<std::uint64_t, 256> match_length_hist{};
    std::array<std::uint64_t, 64> slot_hist{};
    std::uint64_t literal_total = 0;
    std::uint64_t command_total = 0;
    std::uint64_t match_length_total = 0;
    std::uint64_t slot_total = 0;

    std::size_t cursor = 0;
    while (cursor < tokens.size()) {
        const auto token = tokens[cursor++];
        ++command_hist[token];
        ++command_total;

        if (token == kLiteralToken) {
            const auto length = static_cast<std::size_t>(read_varuint(tokens, cursor));
            for (std::size_t i = 0; i < length; ++i) {
                ++literal_hist[tokens[cursor + i]];
            }
            literal_total += length;
            cursor += length;
        } else if (token == kMatchToken) {
            const auto length_start = cursor;
            (void)read_varuint(tokens, cursor);
            for (std::size_t i = length_start; i < cursor; ++i) {
                ++match_length_hist[tokens[i]];
                ++match_length_total;
            }
            const auto distance = static_cast<std::size_t>(read_varuint(tokens, cursor));
            ++slot_hist[distance_slot(distance)];
            ++slot_total;
        } else {
            const auto length_start = cursor;
            (void)read_varuint(tokens, cursor);
            for (std::size_t i = length_start; i < cursor; ++i) {
                ++match_length_hist[tokens[i]];
                ++match_length_total;
            }
        }
    }

    ParseCosts model;
    model.measured = true;
    model.literal = scaled_symbol_costs(literal_hist, literal_total, 8 * kCostScale);
    model.command = scaled_symbol_costs(command_hist, command_total, 2 * kCostScale);
    model.match_length =
        scaled_symbol_costs(match_length_hist, match_length_total, 6 * kCostScale);
    model.slot = scaled_symbol_costs(slot_hist, slot_total, 5 * kCostScale);
    return model;
}

struct CheckpointSeed {
    std::size_t position = 0;
    std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};
};

struct CheckpointCandidateTile {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::vector<std::uint8_t> counts;
    std::vector<Match> matches;
};

std::vector<CheckpointSeed> checkpoint_seeds_from_greedy(
    std::span<const std::uint8_t> tokens,
    std::size_t output_size,
    std::size_t target_tile_size) {
    std::vector<CheckpointSeed> seeds{{}};
    std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};
    std::size_t cursor = 0;
    std::size_t out = 0;
    std::size_t next_target = target_tile_size;

    while (cursor < tokens.size()) {
        const auto command = tokens[cursor++];
        if (command == kLiteralToken) {
            const auto length = static_cast<std::size_t>(read_varuint(tokens, cursor));
            if (length > tokens.size() - cursor || length > output_size - out) {
                throw FormatError("invalid greedy literal while choosing checkpoints");
            }
            cursor += length;
            out += length;
        } else if (command == kMatchToken) {
            const auto length = static_cast<std::size_t>(read_varuint(tokens, cursor));
            const auto distance = static_cast<std::size_t>(read_varuint(tokens, cursor));
            if (distance == 0 || distance > out || length > output_size - out) {
                throw FormatError("invalid greedy match while choosing checkpoints");
            }
            reps = {distance, reps[0], reps[1], reps[2]};
            out += length;
        } else if (command >= kRepTokenBase &&
                   command < kRepTokenBase + kRepCount) {
            const auto index = static_cast<std::size_t>(command - kRepTokenBase);
            const auto length = static_cast<std::size_t>(read_varuint(tokens, cursor));
            if (reps[index] == 0 || reps[index] > out || length > output_size - out) {
                throw FormatError("invalid greedy rep while choosing checkpoints");
            }
            const auto chosen = reps[index];
            for (std::size_t i = index; i > 0; --i) {
                reps[i] = reps[i - 1];
            }
            reps[0] = chosen;
            out += length;
        } else {
            throw FormatError("unknown greedy token while choosing checkpoints");
        }

        // Boundaries are token ends, so no match can straddle a checkpoint.
        if (out >= next_target && out < output_size) {
            seeds.push_back({out, reps});
            next_target = out + target_tile_size;
        }
    }
    if (out != output_size) {
        throw FormatError("greedy parse size does not match checkpoint input");
    }
    return seeds;
}

ByteVector parse_checkpoint_tile(std::span<const std::uint8_t> input,
                                 const CompressionOptions& options,
                                 const ParseCosts& model,
                                 const CheckpointSeed& seed,
                                 CheckpointCandidateTile candidates) {
    constexpr std::uint64_t kInf = std::numeric_limits<std::uint64_t>::max() / 4;
    const auto length = candidates.end - candidates.begin;
    const auto max_match = std::max<std::size_t>(kMinMatch, options.max_match);

    const auto max_transition = std::min(max_match, length);
    std::vector<std::uint64_t> costs(max_transition + 1, kInf);
    std::vector<ParseDecision> decisions(length + 1);
    using RepState = std::array<std::uint32_t, kRepCount>;
    std::vector<RepState> reps_at(length + 1);
    costs[0] = 0;
    std::transform(seed.reps.begin(), seed.reps.end(), reps_at[0].begin(),
                   [](std::size_t distance) {
                       return static_cast<std::uint32_t>(distance);
                   });

    std::size_t match_cursor = 0;
    for (std::size_t local = 0; local < length; ++local) {
        if (local + max_transition <= length) {
            costs[(local + max_transition) % costs.size()] = kInf;
        }
        const auto current_cost = costs[local % costs.size()];
        if (local != 0) {
            const auto& decision = decisions[local];
            if (decision.length == 0 || decision.length > local) {
                throw FormatError("checkpoint parser reached an invalid state");
            }
            auto state = reps_at[local - decision.length];
            if (decision.kind == ParseKind::match) {
                state = {decision.distance, state[0], state[1], state[2]};
            } else if (decision.kind == ParseKind::rep) {
                const auto chosen = state[decision.rep_index];
                for (std::size_t i = decision.rep_index; i > 0; --i) {
                    state[i] = state[i - 1];
                }
                state[0] = chosen;
            }
            reps_at[local] = state;
        }

        const auto position = candidates.begin + local;
        const auto literal_cost_value =
            current_cost + cost_literal_model(model, input[position]);
        auto& literal_target = costs[(local + 1) % costs.size()];
        if (literal_cost_value < literal_target) {
            literal_target = literal_cost_value;
            decisions[local + 1] = {1, 0, ParseKind::literal, 0};
        }

        if (local >= candidates.counts.size()) {
            throw FormatError("checkpoint candidate counts ended early");
        }
        const auto count = static_cast<std::size_t>(candidates.counts[local]);
        if (match_cursor > candidates.matches.size() ||
            count > candidates.matches.size() - match_cursor) {
            throw FormatError("checkpoint candidate framing is invalid");
        }
        for (std::size_t index = 0; index < count; ++index) {
            const auto& match = candidates.matches[match_cursor + index];
            const auto capped_length = std::min<std::size_t>(match.length, length - local);
            if (capped_length < kMinMatch) {
                continue;
            }
            std::size_t useful_count = 0;
            const auto useful = useful_lengths(capped_length, useful_count);
            for (std::size_t i = 0; i < useful_count; ++i) {
                const auto match_length = static_cast<std::size_t>(useful[i]);
                const auto target = local + match_length;
                const auto candidate_cost = current_cost +
                    cost_match_model(model, match_length, match.distance);
                auto& target_cost = costs[target % costs.size()];
                if (candidate_cost < target_cost) {
                    target_cost = candidate_cost;
                    decisions[target] = {
                        static_cast<std::uint16_t>(match_length), match.distance,
                        ParseKind::match, 0};
                }
            }
        }
        match_cursor += count;

        const auto& reps = reps_at[local];
        const auto rep_limit = std::min(max_match, length - local);
        for (std::size_t index = 0; index < kRepCount; ++index) {
            const auto rep_length =
                match_length_at(input, position, reps[index], rep_limit);
            if (rep_length < kMinMatch) {
                continue;
            }
            std::size_t useful_count = 0;
            const auto useful = useful_lengths(rep_length, useful_count);
            for (std::size_t i = 0; i < useful_count; ++i) {
                const auto match_length = static_cast<std::size_t>(useful[i]);
                const auto target = local + match_length;
                const auto candidate_cost =
                    current_cost + cost_rep_model(model, match_length, index);
                auto& target_cost = costs[target % costs.size()];
                if (candidate_cost < target_cost) {
                    target_cost = candidate_cost;
                    decisions[target] = {
                        static_cast<std::uint16_t>(match_length), 0,
                        ParseKind::rep, static_cast<std::uint8_t>(index)};
                }
            }
        }
    }
    if (match_cursor != candidates.matches.size()) {
        throw FormatError("checkpoint candidates were not consumed exactly");
    }

    std::vector<ParseDecision> path;
    for (std::size_t local = length; local > 0;) {
        const auto decision = decisions[local];
        if (decision.length == 0 || decision.length > local) {
            throw FormatError("checkpoint parser failed to reconstruct a path");
        }
        path.push_back(decision);
        local -= decision.length;
    }
    std::reverse(path.begin(), path.end());

    ByteVector output;
    output.reserve(length / 4);
    std::size_t position = candidates.begin;
    std::size_t literal_start = position;
    for (const auto& decision : path) {
        if (decision.kind == ParseKind::literal) {
            ++position;
            continue;
        }
        write_literal_run(output, input, literal_start, position - literal_start);
        if (decision.kind == ParseKind::rep) {
            output.push_back(static_cast<std::uint8_t>(
                kRepTokenBase + decision.rep_index));
            write_varuint(output, decision.length);
        } else {
            output.push_back(kMatchToken);
            write_varuint(output, decision.length);
            write_varuint(output, decision.distance);
        }
        position += decision.length;
        literal_start = position;
    }
    write_literal_run(output, input, literal_start, candidates.end - literal_start);
    return output;
}

std::optional<ByteVector> encode_lz77_optimal_checkpointed(
    std::span<const std::uint8_t> input,
    const CompressionOptions& options,
    const ByteVector& greedy_tokens) {
    constexpr std::size_t kTileSize = std::size_t{2} << 20;
    constexpr std::size_t kMinimumInput = kTileSize * 2;
    if (!options.swarm_parse || !options.use_tree_matcher ||
        !options.enable_optimal_parser || input.size() < kMinimumInput ||
        input.size() > options.optimal_parse_limit ||
        input.size() >= std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    auto* executor = core::TaskExecutor::current();
    if (executor == nullptr || executor->worker_count() <= 1) {
        return std::nullopt;
    }

    const auto seeds =
        checkpoint_seeds_from_greedy(greedy_tokens, input.size(), kTileSize);
    if (seeds.size() <= 1) {
        return std::nullopt;
    }

    const auto window_size = std::max<std::size_t>(kMinMatch, options.window_size);
    const auto max_match = std::max<std::size_t>(kMinMatch, options.max_match);
    const auto chain_depth = std::max<std::size_t>(1, options.optimal_chain_depth);
    const auto max_candidates =
        std::clamp<std::size_t>(options.max_parser_candidates, 1,
                                kParserCandidateLimit);
    const auto model = measure_costs(greedy_tokens);
    TreeMatchSource<std::uint32_t> tree(
        input, window_size, max_match, chain_depth);

    struct PendingTile {
        std::size_t index = 0;
        std::future<ByteVector> future;
    };
    std::deque<PendingTile> pending;
    std::vector<ByteVector> encoded_tiles(seeds.size());
    // There is no fixed CPU ceiling: work in flight follows the executor size
    // and is naturally bounded by the number of deterministic tiles in the
    // block. Larger inputs and multiple blocks therefore scale beyond 32
    // workers without changing archive bytes.
    const auto max_in_flight = executor->worker_count();

    const auto collect_front = [&] {
        auto item = std::move(pending.front());
        pending.pop_front();
        encoded_tiles[item.index] = executor->wait(item.future);
    };

    ParseProgressTicker progress(options.encode_progress, input.size());
    for (std::size_t tile_index = 0; tile_index < seeds.size(); ++tile_index) {
        CheckpointCandidateTile tile;
        tile.begin = seeds[tile_index].position;
        tile.end = tile_index + 1 < seeds.size()
            ? seeds[tile_index + 1].position
            : input.size();
        const auto tile_length = tile.end - tile.begin;
        tile.counts.resize(tile_length);
        tile.matches.reserve(tile_length * std::min<std::size_t>(2, max_candidates));
        for (std::size_t position = tile.begin; position < tile.end; ++position) {
            progress.tick(position);
            MatchList matches;
            tree.advance(position, &matches, max_candidates);
            const auto local = position - tile.begin;
            tile.counts[local] = static_cast<std::uint8_t>(matches.count);
            tile.matches.insert(
                tile.matches.end(), matches.values.begin(),
                matches.values.begin() + static_cast<std::ptrdiff_t>(matches.count));
        }

        const auto seed = seeds[tile_index];
        pending.push_back({
            tile_index,
            executor->submit([input, options, model, seed, tile = std::move(tile)]() mutable {
                return parse_checkpoint_tile(input, options, model, seed,
                                             std::move(tile));
            })});
        if (pending.size() >= max_in_flight) {
            collect_front();
        }
    }
    while (!pending.empty()) {
        collect_front();
    }

    ByteVector output;
    const auto token_bytes = std::accumulate(
        encoded_tiles.begin(), encoded_tiles.end(), std::size_t{0},
        [](std::size_t total, const ByteVector& tile) {
            return total + tile.size();
        });
    output.reserve(token_bytes + (seeds.size() - 1) * 9);
    std::array<std::size_t, kRepCount> decoder_reps{1, 2, 3, 4};
    std::size_t decoded_position = 0;
    const auto advance_state = [&](std::span<const std::uint8_t> tokens) {
        std::size_t cursor = 0;
        while (cursor < tokens.size()) {
            const auto command = tokens[cursor++];
            if (command == kLiteralToken) {
                const auto length =
                    static_cast<std::size_t>(read_varuint(tokens, cursor));
                if (length > tokens.size() - cursor ||
                    length > input.size() - decoded_position) {
                    throw FormatError("invalid checkpoint tile literal");
                }
                cursor += length;
                decoded_position += length;
            } else if (command == kMatchToken) {
                const auto length =
                    static_cast<std::size_t>(read_varuint(tokens, cursor));
                const auto distance =
                    static_cast<std::size_t>(read_varuint(tokens, cursor));
                if (distance == 0 || distance > decoded_position ||
                    length > input.size() - decoded_position) {
                    throw FormatError("invalid checkpoint tile match");
                }
                decoder_reps = {distance, decoder_reps[0], decoder_reps[1],
                                decoder_reps[2]};
                decoded_position += length;
            } else if (command >= kRepTokenBase &&
                       command < kRepTokenBase + kRepCount) {
                const auto index =
                    static_cast<std::size_t>(command - kRepTokenBase);
                const auto length =
                    static_cast<std::size_t>(read_varuint(tokens, cursor));
                if (decoder_reps[index] > decoded_position ||
                    length > input.size() - decoded_position) {
                    throw FormatError("invalid checkpoint tile rep");
                }
                const auto chosen = decoder_reps[index];
                for (std::size_t i = index; i > 0; --i) {
                    decoder_reps[i] = decoder_reps[i - 1];
                }
                decoder_reps[0] = chosen;
                decoded_position += length;
            } else {
                throw FormatError("unexpected token inside checkpoint tile");
            }
        }
    };
    for (std::size_t tile_index = 0; tile_index < encoded_tiles.size(); ++tile_index) {
        if (tile_index != 0) {
            output.push_back(kCheckpointToken);
            const auto old_reps = decoder_reps;
            for (const auto distance : seeds[tile_index].reps) {
                std::size_t old_index = kRepCount;
                for (std::size_t i = 0; i < kRepCount; ++i) {
                    if (old_reps[i] == distance) {
                        old_index = i;
                        break;
                    }
                }
                if (old_index < kRepCount) {
                    output.push_back(static_cast<std::uint8_t>(old_index));
                } else {
                    output.push_back(kCheckpointExplicit);
                    write_varuint(output, distance);
                }
            }
            decoder_reps = seeds[tile_index].reps;
        }
        output.insert(output.end(), encoded_tiles[tile_index].begin(),
                      encoded_tiles[tile_index].end());
        advance_state(encoded_tiles[tile_index]);
    }
    if (decoded_position != input.size()) {
        throw FormatError("checkpoint tiles do not cover the input exactly");
    }
    return output;
}

ByteVector encode_lz77_optimal(std::span<const std::uint8_t> input,
                               const CompressionOptions& options,
                               const ByteVector* greedy_tokens) {
    if (input.empty()) {
        return {};
    }

    if (input.size() > options.optimal_parse_limit ||
        input.size() >= std::numeric_limits<std::uint32_t>::max()) {
        return encode_lz77(input, options);
    }

    // Single-pass mode (the mid-effort presets): measure the symbol statistics
    // from a cheap greedy parse instead of a throwaway first DP pass, then run
    // the DP once with entropy-aligned costs. Roughly halves the optimal-parse
    // cost for most of its ratio.
    if (!options.optimal_two_pass) {
        if (greedy_tokens != nullptr) {
            return optimal_parse_with_costs(input, options, measure_costs(*greedy_tokens));
        }
        const auto greedy_options = scoped_progress_options(options, 0.0, 0.30);
        const auto greedy = encode_lz77(input, greedy_options);
        const auto parse_options = scoped_progress_options(options, 0.30, 1.0);
        return optimal_parse_with_costs(input, parse_options, measure_costs(greedy));
    }

    // Parse once with crude weights, measure the real symbol statistics, then
    // re-parse with entropy-aligned costs. The two DP passes split this call's
    // progress budget; the trailing entropy bake-off is short by comparison.
    const auto first_options = scoped_progress_options(options, 0.0, 0.47);
    auto first = optimal_parse_with_costs(input, first_options, ParseCosts{});
    const auto measured = measure_costs(first);
    const auto second_options = scoped_progress_options(options, 0.47, 0.94);
    auto second = optimal_parse_with_costs(input, second_options, measured);

    // The measured parse is meant to win after entropy coding even when it has
    // more raw tokens, so compare the actual coded size rather than token count.
    auto coded_size = [fast = options.fast_entropy](const ByteVector& tokens) {
        // Both layouts share commands, lengths, and literals. Building them
        // independently re-runs the clustered literal rANS encoder merely to
        // select between two already-compatible distance representations.
        // The paired encoder emits byte-identical candidates while doing that
        // common work once.
        auto payloads = encode_lz77_split_payloads(tokens, fast);
        auto best = payloads.split.size();
        if (payloads.slots) {
            best = std::min(best, payloads.slots->size());
        }
        if (payloads.contextual_slots) {
            best = std::min(best, payloads.contextual_slots->size());
        }
        return best;
    };

    return coded_size(second) <= coded_size(first) ? std::move(second) : std::move(first);
}

void decode_lz77_into(std::span<const std::uint8_t> encoded,
                      std::span<std::uint8_t> output) {
    const auto output_size = output.size();
    std::size_t out = 0;

    std::array<std::size_t, kRepCount> reps{1, 2, 3, 4};
    auto copy_match = [&](std::size_t length, std::size_t distance) {
        copy_lz_match(output, out, distance, length, "invalid match reference",
                      "match exceeds declared output size");
    };

    std::size_t cursor = 0;
    while (out < output_size) {
        if (cursor >= encoded.size()) {
            throw FormatError("truncated LZ77 token stream");
        }

        const auto token = encoded[cursor++];

        if (token == kLiteralToken) {
            const auto length = static_cast<std::size_t>(read_varuint(encoded, cursor));

            if (length > encoded.size() - cursor) {
                throw FormatError("literal run exceeds payload");
            }
            if (length > output_size - out) {
                throw FormatError("literal run exceeds declared output size");
            }

            std::memcpy(output.data() + out, encoded.data() + cursor, length);
            out += length;
            cursor += length;
        } else if (token == kMatchToken) {
            const auto length = static_cast<std::size_t>(read_varuint(encoded, cursor));
            const auto distance = static_cast<std::size_t>(read_varuint(encoded, cursor));

            copy_match(length, distance);
            reps = {distance, reps[0], reps[1], reps[2]};
        } else if (token >= kRepTokenBase && token < kRepTokenBase + kRepCount) {
            const auto index = static_cast<std::size_t>(token - kRepTokenBase);
            const auto length = static_cast<std::size_t>(read_varuint(encoded, cursor));
            const auto distance = reps[index];

            copy_match(length, distance);

            const auto chosen = reps[index];
            for (std::size_t j = index; j > 0; --j) {
                reps[j] = reps[j - 1];
            }
            reps[0] = chosen;
        } else {
            throw FormatError("unknown LZ77 token");
        }
    }

    if (cursor != encoded.size()) {
        throw FormatError("trailing bytes after LZ77 stream");
    }
}

ByteVector decode_lz77(std::span<const std::uint8_t> encoded,
                       std::size_t output_size) {
    ByteVector output(output_size);
    decode_lz77_into(encoded, output);

    return output;
}

}  // namespace axiom::codec
