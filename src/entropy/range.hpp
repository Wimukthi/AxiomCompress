#pragma once

#include "axiom/axiom.hpp"

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

ByteVector decode_rans(std::span<const std::uint8_t> encoded,
                       std::size_t max_output_size);

}  // namespace axiom::entropy
