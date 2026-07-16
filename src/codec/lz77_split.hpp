#pragma once

#include "axiom/axiom.hpp"

#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace axiom::core {
class TaskExecutor;
}

namespace axiom::codec {

struct Lz77SplitStreams {
    using Histogram = std::array<std::uint64_t, 256>;

    ByteVector commands;
    ByteVector literal_lengths;
    ByteVector match_lengths;
    ByteVector distances;
    ByteVector literals;
    bool has_histograms = false;
    Histogram commands_hist{};
    Histogram literal_lengths_hist{};
    Histogram match_lengths_hist{};
    Histogram distances_hist{};
    Histogram literals_hist{};
};

struct Lz77PayloadCandidates {
    std::optional<ByteVector> split;
    std::optional<ByteVector> slots;
    std::optional<ByteVector> contextual_slots;
    std::optional<ByteVector> sequence;
};

struct Lz77SplitPayloads {
    ByteVector split;
    std::optional<ByteVector> slots;
    std::optional<ByteVector> contextual_slots;
};

struct Lz77ContextSplitPayloads {
    std::optional<ByteVector> slots;
    std::optional<ByteVector> contextual_slots;
};

// When `fast` is set, each substream's entropy coder is chosen from a cheap
// order-0 (and, for literals, sampled order-1) entropy estimate instead of
// trial-encoding every coder. The resulting archive decodes identically; only
// the encoder's coder choice (and its cost) changes.
ByteVector encode_lz77_split_streams(std::span<const std::uint8_t> lz77_payload,
                                     bool fast = false);

ByteVector decode_lz77_split_streams(std::span<const std::uint8_t> encoded,
                                     std::size_t output_size);

void decode_lz77_split_streams_into(std::span<const std::uint8_t> encoded,
                                    std::span<std::uint8_t> output);

// Variant that codes match distances as LZMA-style position slots plus packed
// footer bits instead of raw varints. Returns nullopt if a distance does not
// fit the 32-bit slot scheme (so the caller falls back to the plain split).
std::optional<ByteVector> encode_lz77_split_streams_slots(
    std::span<const std::uint8_t> lz77_payload, bool fast = false);

ByteVector decode_lz77_split_streams_slots(std::span<const std::uint8_t> encoded,
                                           std::size_t output_size);

void decode_lz77_split_streams_slots_into(std::span<const std::uint8_t> encoded,
                                          std::span<std::uint8_t> output);

// AXC v7 hybrid layout: retain the proven split command/length/distance
// streams, but partition literals by the preceding decoded byte and optionally
// XOR them with the current rep0 prediction. This lets the stronger literal
// model compete without forcing the v6 sequence representation for matches.
Lz77ContextSplitPayloads encode_lz77_context_split_streams(
    std::span<const std::uint8_t> input,
    std::span<const std::uint8_t> lz77_payload,
    std::span<const std::uint8_t> slot_payload,
    std::span<const std::uint8_t> contextual_slot_payload = {},
    core::TaskExecutor* executor = nullptr);

void decode_lz77_context_split_streams_into(
    std::span<const std::uint8_t> encoded,
    std::span<std::uint8_t> output);

void decode_lz77_contextual_slot_streams_into(
    std::span<const std::uint8_t> encoded,
    std::span<std::uint8_t> output);

void decode_lz77_contextual_slot_context_split_streams_into(
    std::span<const std::uint8_t> encoded,
    std::span<std::uint8_t> output);

// AXC v6 sequence representation. Literal-run, match-length, and offset values
// are converted to compact code alphabets plus raw extra bits. Literals are
// partitioned by the preceding decoded byte; the encoder also trials an XOR
// residual against the current rep0 prediction and keeps the smaller mode.
std::optional<ByteVector> encode_lz77_sequence_streams(
    std::span<const std::uint8_t> input,
    std::span<const std::uint8_t> lz77_payload,
    bool fast = false,
    std::size_t maximum_useful_size = std::numeric_limits<std::size_t>::max(),
    core::TaskExecutor* executor = nullptr);

// Parses the token stream once while building the legacy split streams and v6
// sequence metadata. Small blocks build both literal modes in one cache-hot
// pass; large blocks retain compact run metadata and materialize only the mode
// selected by the entropy estimate.
Lz77PayloadCandidates encode_lz77_payload_candidates(
    std::span<const std::uint8_t> input,
    std::span<const std::uint8_t> lz77_payload,
    bool fast = false,
    core::TaskExecutor* executor = nullptr);

ByteVector decode_lz77_sequence_streams(std::span<const std::uint8_t> encoded,
                                        std::size_t output_size);

void decode_lz77_sequence_streams_into(std::span<const std::uint8_t> encoded,
                                       std::span<std::uint8_t> output);

// Produces both the plain-split and slot-split payloads in one pass, sharing the
// encoding of the streams they have in common (commands, lengths, and the
// literal stream, whose order-1 coding is the most expensive). The second value
// is nullopt when a distance does not fit the slot scheme. Byte-for-byte
// identical to calling the two encoders separately, but roughly half the work.
Lz77SplitPayloads encode_lz77_split_payloads(
    std::span<const std::uint8_t> lz77_payload,
    bool fast = false,
    core::TaskExecutor* executor = nullptr);

Lz77SplitPayloads encode_lz77_split_payloads(
    const Lz77SplitStreams& streams,
    bool fast = false,
    core::TaskExecutor* executor = nullptr);

}  // namespace axiom::codec
