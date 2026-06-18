#pragma once

#include <cstdint>
#include <span>

namespace axiom::detail {

// Parses an archive container from memory and decodes every block, exercising
// the directory parser and block decoder on untrusted bytes. Used by the fuzz
// targets so the container can be driven in-memory without a temp file per
// input. Throws on malformed input; must never crash or read out of bounds.
void fuzz_read_archive(std::span<const std::uint8_t> bytes);

}  // namespace axiom::detail
