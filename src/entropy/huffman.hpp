#pragma once

#include "axiom/axiom.hpp"

#include <optional>

namespace axiom::entropy {

std::optional<ByteVector> encode_huffman(std::span<const std::uint8_t> input);

ByteVector decode_huffman(std::span<const std::uint8_t> encoded,
                          std::size_t max_output_size);

}  // namespace axiom::entropy
