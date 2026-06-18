#pragma once

#include "axiom/axiom.hpp"

namespace axiom::core {

std::uint32_t crc32(std::span<const std::uint8_t> input);

// Incremental CRC-32 for streaming over data that does not fit in memory at
// once. Start from crc32_init(), feed chunks through crc32_update(), then
// crc32_final() to obtain the same value crc32() would return over the whole.
std::uint32_t crc32_init();
std::uint32_t crc32_update(std::uint32_t state, std::span<const std::uint8_t> input);
std::uint32_t crc32_final(std::uint32_t state);
std::uint32_t crc32_combine(std::uint32_t first_crc,
                            std::uint32_t second_crc,
                            std::uint64_t second_size);

}  // namespace axiom::core
