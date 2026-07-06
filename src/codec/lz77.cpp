#include "codec/lz77.hpp"

#include "codec/lz77_split.hpp"
#include "codec/match_copy.hpp"
#include "codec/varint.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
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

constexpr std::uint8_t kLiteralToken = 0;
constexpr std::uint8_t kMatchToken = 1;
// Repeat-offset matches reuse one of the last kRepCount distances. Rep i is
// encoded as command byte (kRepTokenBase + i) followed by only a length: no
// distance is stored, which is the whole point of the optimization.
constexpr std::uint8_t kRepTokenBase = 2;
constexpr std::size_t kRepCount = 4;
constexpr std::size_t kMinMatch = 4;
constexpr std::size_t kHashBits = 20;
constexpr std::size_t kHashSize = 1u << kHashBits;
constexpr std::size_t kParserCandidateLimit = 64;

struct Match {
    std::uint16_t length = 0;
    std::uint32_t distance = 0;
};

enum class ParseKind : std::uint8_t { literal, match, rep };

struct ParseDecision {
    std::uint16_t length = 0;
    std::uint32_t distance = 0;       // only meaningful for a normal match
    ParseKind kind = ParseKind::literal;
    std::uint8_t rep_index = 0;       // only meaningful for a rep match
};

std::size_t hash4(std::span<const std::uint8_t> input, std::size_t position) {
    const auto value =
        static_cast<std::uint32_t>(input[position]) |
        (static_cast<std::uint32_t>(input[position + 1]) << 8) |
        (static_cast<std::uint32_t>(input[position + 2]) << 16) |
        (static_cast<std::uint32_t>(input[position + 3]) << 24);

    return (value * 2654435761u) >> (32 - kHashBits);
}

std::uint32_t load_u32_le(const std::uint8_t* data) {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

// Length of the common prefix of two byte runs, capped at `limit`. Compares eight
// bytes per step (the first mismatching byte is located with a single
// count-trailing-zeros) instead of one byte at a time. Both pointers read the
// original input buffer, so overlapping match sources (distance < 8) compare
// exactly as a byte-serial loop would.
std::size_t common_prefix(const std::uint8_t* a, const std::uint8_t* b, std::size_t limit) {
    std::size_t i = 0;
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
// model is built from the actual symbol statistics of a first parse.
constexpr std::uint64_t kCostScale = 16;

struct ParseCosts {
    bool measured = false;
    std::uint64_t literal = 0;   // scaled bits per literal byte
    std::uint64_t command = 0;   // scaled bits per command token
    std::uint64_t length = 0;    // scaled bits per match-length value
    std::uint64_t slot = 0;      // scaled bits per distance slot
};

std::uint64_t cost_literal_model(const ParseCosts& model) {
    return model.measured ? model.literal : static_cast<std::uint64_t>(literal_cost());
}

std::uint64_t cost_match_model(const ParseCosts& model, std::size_t length, std::size_t distance) {
    if (!model.measured) {
        return match_cost(length, distance);
    }
    return model.command + model.length + model.slot +
           static_cast<std::uint64_t>(distance_footer_bits(distance)) * kCostScale;
}

std::uint64_t cost_rep_model(const ParseCosts& model, std::size_t length) {
    if (!model.measured) {
        return rep_cost(length);
    }
    return model.command + model.length;
}

void add_match_candidate(std::vector<Match>& matches,
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

    for (auto& match : matches) {
        if (match.distance == narrowed_distance) {
            match.length = std::max(match.length, narrowed_length);
            return;
        }
    }

    if (matches.size() >= max_candidates) {
        const auto& worst = matches.back();
        if (narrowed_length < worst.length ||
            (narrowed_length == worst.length && narrowed_distance >= worst.distance)) {
            return;
        }
    }

    matches.push_back(Match{narrowed_length, narrowed_distance});

    std::sort(matches.begin(), matches.end(), [](const Match& left, const Match& right) {
        if (left.length != right.length) {
            return left.length > right.length;
        }

        return left.distance < right.distance;
    });

    if (matches.size() > max_candidates) {
        matches.resize(max_candidates);
    }
}

template <typename Index>
std::vector<Match> find_matches_at(std::span<const std::uint8_t> input,
                                   const std::vector<Index>& hash_heads,
                                   const std::vector<Index>& previous,
                                   std::size_t position,
                                   std::size_t window_size,
                                   std::size_t max_match,
                                   std::size_t max_chain_depth,
                                   std::size_t max_candidates) {
    constexpr Index kNoPos = std::numeric_limits<Index>::max();
    std::vector<Match> matches;
    matches.reserve(std::min(max_candidates, kParserCandidateLimit));
    if (position + kMinMatch > input.size()) {
        return matches;
    }

    auto candidate = hash_heads[hash4(input, position)];
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

        if (matches.size() >= max_candidates) {
            const auto threshold = static_cast<std::size_t>(matches.back().length);
            if (threshold > kMinMatch &&
                input[candidate + threshold - 1] != input[position + threshold - 1]) {
                candidate = next;
                ++depth;
                continue;
            }
        }

        if (load_u32_le(input.data() + candidate) != load_u32_le(input.data() + position)) {
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

    return matches;
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

        auto candidate = hash_heads[hash4(input, pos)];
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
                load_u32_le(input.data() + candidate) == load_u32_le(input.data() + pos)) {
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

    while (position < input.size()) {
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

    while (position < n) {
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
        const bool take_match = !take_rep && best_length >= kMinMatch;

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
    void advance(std::size_t p, std::vector<Match>* matches, std::size_t max_candidates) {
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

ByteVector encode_lz77(std::span<const std::uint8_t> input,
                       const CompressionOptions& options) {
    // 32-bit indices suffice (and halve match-finder memory) until the input
    // reaches the point where a position would collide with the sentinel.
    if (options.use_tree_matcher && input.size() < std::numeric_limits<std::uint32_t>::max()) {
        return encode_lz77_tree<std::uint32_t>(input, options);
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
    if (use_tree) {
        tree.emplace(input, options.window_size, max_match, max_chain_depth);
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
    costs.assign(input.size() + 1, kInf);
    decisions.assign(input.size() + 1, ParseDecision{});
    // The recent-distance list depends on the path taken, so each position keeps
    // the rep state of the lowest-cost path that reaches it. Because costs[pos]
    // is final once the loop reaches pos (every in-edge comes from an earlier
    // position), the recorded decision lets us settle reps_at[pos] deterministically.
    static thread_local std::vector<std::array<std::size_t, kRepCount>> reps_at;
    reps_at.assign(input.size() + 1, {});
    costs[0] = 0;

    auto insert_position = [&](std::size_t position) {
        if (position + kMinMatch <= input.size()) {
            const auto hash = hash4(input, position);
            previous[position] = hash_heads[hash];
            hash_heads[hash] = static_cast<Index>(position);
        }
    };

    for (std::size_t position = 0; position < input.size(); ++position) {
        if (costs[position] == kInf) {
            // Unreachable position: the match finder still needs it indexed.
            if (use_tree) {
                tree->advance(position, nullptr, 0);
            } else {
                insert_position(position);
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

        const auto literal_next = costs[position] + cost_literal_model(model);
        if (literal_next < costs[position + 1]) {
            costs[position + 1] = literal_next;
            decisions[position + 1] = ParseDecision{1, 0, ParseKind::literal, 0};
        }

        std::vector<Match> matches;
        if (use_tree) {
            matches.reserve(std::min(max_candidates, kParserCandidateLimit));
            tree->advance(position, &matches, max_candidates);
        } else {
            matches = find_matches_at(input,
                                      hash_heads,
                                      previous,
                                      position,
                                      window_size,
                                      max_match,
                                      max_chain_depth,
                                      max_candidates);
        }

        // The parser evaluates a bounded set of useful lengths instead of every
        // possible prefix. That keeps the pass linear enough for large blocks.
        for (const auto& match : matches) {
            std::size_t length_count = 0;
            const auto lengths = useful_lengths(match.length, length_count);

            for (std::size_t i = 0; i < length_count; ++i) {
                const auto length = static_cast<std::size_t>(lengths[i]);
                const auto target = position + length;
                const auto candidate_cost =
                    costs[position] + cost_match_model(model, length, match.distance);

                if (candidate_cost < costs[target]) {
                    costs[target] = candidate_cost;
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
                const auto candidate_cost = costs[position] + cost_rep_model(model, length);

                if (candidate_cost < costs[target]) {
                    costs[target] = candidate_cost;
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

std::uint64_t scaled_entropy(const std::vector<std::uint64_t>& histogram, std::uint64_t total) {
    if (total == 0) {
        return 0;
    }

    double bits = 0.0;
    for (const auto count : histogram) {
        if (count == 0) {
            continue;
        }
        const auto probability = static_cast<double>(count) / static_cast<double>(total);
        bits -= probability * std::log2(probability);
    }

    return static_cast<std::uint64_t>(bits * static_cast<double>(kCostScale) + 0.5);
}

// Estimate the per-symbol coded cost of each stream from a completed parse using
// its order-0 entropy. This is what the entropy stage approaches, so re-parsing
// with these costs aligns the parser's split decisions with reality.
ParseCosts measure_costs(std::span<const std::uint8_t> tokens) {
    std::vector<std::uint64_t> literal_hist(256, 0);
    std::vector<std::uint64_t> command_hist(256, 0);
    std::vector<std::uint64_t> length_hist(1u << 16, 0);
    std::vector<std::uint64_t> slot_hist(64, 0);
    std::uint64_t literal_total = 0;
    std::uint64_t command_total = 0;
    std::uint64_t length_total = 0;
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
            const auto length = static_cast<std::size_t>(read_varuint(tokens, cursor));
            const auto distance = static_cast<std::size_t>(read_varuint(tokens, cursor));
            if (length < length_hist.size()) {
                ++length_hist[length];
            }
            ++length_total;
            ++slot_hist[distance_slot(distance)];
            ++slot_total;
        } else {
            const auto length = static_cast<std::size_t>(read_varuint(tokens, cursor));
            if (length < length_hist.size()) {
                ++length_hist[length];
            }
            ++length_total;
        }
    }

    ParseCosts model;
    model.measured = true;
    model.literal = literal_total ? scaled_entropy(literal_hist, literal_total) : 8 * kCostScale;
    model.command = command_total ? scaled_entropy(command_hist, command_total) : 2 * kCostScale;
    model.length = length_total ? scaled_entropy(length_hist, length_total) : 6 * kCostScale;
    model.slot = slot_total ? scaled_entropy(slot_hist, slot_total) : 5 * kCostScale;
    return model;
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
        const auto greedy = encode_lz77(input, options);
        return optimal_parse_with_costs(input, options, measure_costs(greedy));
    }

    // Parse once with crude weights, measure the real symbol statistics, then
    // re-parse with entropy-aligned costs.
    auto first = optimal_parse_with_costs(input, options, ParseCosts{});
    const auto measured = measure_costs(first);
    auto second = optimal_parse_with_costs(input, options, measured);

    // The measured parse is meant to win after entropy coding even when it has
    // more raw tokens, so compare the actual coded size rather than token count.
    auto coded_size = [fast = options.fast_entropy](const ByteVector& tokens) {
        auto split = encode_lz77_split_streams(tokens, fast).size();
        if (auto slots = encode_lz77_split_streams_slots(tokens, fast)) {
            split = std::min(split, slots->size());
        }
        return split;
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
