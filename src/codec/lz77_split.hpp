#pragma once

#include "axiom/axiom.hpp"

#include <array>
#include <optional>
#include <utility>

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

// Produces both the plain-split and slot-split payloads in one pass, sharing the
// encoding of the streams they have in common (commands, lengths, and the
// literal stream, whose order-1 coding is the most expensive). The second value
// is nullopt when a distance does not fit the slot scheme. Byte-for-byte
// identical to calling the two encoders separately, but roughly half the work.
std::pair<ByteVector, std::optional<ByteVector>> encode_lz77_split_payloads(
    std::span<const std::uint8_t> lz77_payload, bool fast = false);

std::pair<ByteVector, std::optional<ByteVector>> encode_lz77_split_payloads(
    const Lz77SplitStreams& streams, bool fast = false);

}  // namespace axiom::codec
