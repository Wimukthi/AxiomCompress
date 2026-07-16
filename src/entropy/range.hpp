#pragma once

#include "axiom/axiom.hpp"

#include <array>
#include <optional>

namespace axiom::entropy {

// Adaptive, order-1 (previous-byte context) range coder. No frequency table is
// transmitted: the encoder and decoder evolve identical per-context models as
// they process symbols. This captures the order-1 structure that bytewise
// order-0 coders cannot, and is the strongest gain on literal streams.
std::optional<ByteVector> encode_order1(std::span<const std::uint8_t> input);

ByteVector decode_order1(std::span<const std::uint8_t> encoded,
                         std::size_t max_output_size);

// Static, byte-oriented order-0 rANS coder. The payload stores four interleaved
// states so decode has independent dependency chains while retaining O(1) slot
// table lookup per byte.
std::optional<ByteVector> encode_rans(std::span<const std::uint8_t> input);

std::optional<ByteVector> encode_rans(
    std::span<const std::uint8_t> input,
    const std::array<std::uint64_t, 256>& counts);

ByteVector decode_rans(std::span<const std::uint8_t> encoded,
                       std::size_t max_output_size);

// Static contextual rANS for cases where the decoder already knows one small
// context id per symbol (for example, an LZ distance slot). Only the used
// context tables are transmitted; decoding remains a four-lane table lookup.
std::optional<ByteVector> encode_rans_contextual(
    std::span<const std::uint8_t> input,
    std::span<const std::uint8_t> contexts,
    std::size_t context_count,
    std::size_t symbol_count);

ByteVector decode_rans_contextual(
    std::span<const std::uint8_t> encoded,
    std::span<const std::uint8_t> contexts,
    std::size_t context_count,
    std::size_t symbol_count);

// Static order-1 rANS: previous-byte contexts are clustered into a small set
// of transmitted frequency tables (plus a 256-entry context map), keeping the
// interleaved table-lookup decode of the order-0 coder while capturing most of
// the order-1 modelling gain. Returns nullopt for inputs too small for the
// per-context statistics to pay for the headers.
std::optional<ByteVector> encode_rans_order1(std::span<const std::uint8_t> input);

ByteVector decode_rans_order1(std::span<const std::uint8_t> encoded,
                              std::size_t max_output_size);

}  // namespace axiom::entropy
