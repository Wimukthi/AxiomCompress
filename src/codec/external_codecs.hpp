#pragma once

#include "axiom/axiom.hpp"

#include <cstdint>
#include <span>

namespace axiom::codec {

ByteVector encode_external_codec(std::span<const std::uint8_t> input,
                                 CompressionMethod method,
                                 const CompressionOptions& options);

ByteVector decode_external_codec(std::span<const std::uint8_t> payload,
                                 CompressionMethod method,
                                 std::size_t expected_size,
                                 const DecompressionOptions& options);

}  // namespace axiom::codec
