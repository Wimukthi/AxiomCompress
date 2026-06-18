#pragma once

#include "axiom/axiom.hpp"

namespace axiom::codec {

// Reserve up to a requested size, but never more than a sane cap. Decode output
// sizes come from archive headers (i.e. untrusted input), and the decode loops
// already enforce the true size as they append, so pre-reserving the full
// declared size only risks a huge allocation on a malformed input. Capping keeps
// the common case fast while refusing to pre-commit gigabytes for a hostile one.
inline void bounded_reserve(ByteVector& buffer, std::size_t requested) {
    constexpr std::size_t kReserveCap = std::size_t{1} << 28;  // 256 MiB
    buffer.reserve(requested < kReserveCap ? requested : kReserveCap);
}

inline void write_varuint(ByteVector& output, std::uint64_t value) {
    while (value >= 0x80) {
        output.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }

    output.push_back(static_cast<std::uint8_t>(value));
}

inline std::uint64_t read_varuint(std::span<const std::uint8_t> input,
                                  std::size_t& cursor) {
    std::uint64_t value = 0;
    unsigned shift = 0;

    while (cursor < input.size()) {
        const auto byte = input[cursor++];
        value |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;

        if ((byte & 0x80u) == 0) {
            return value;
        }

        shift += 7;
        if (shift >= 64) {
            throw FormatError("varint is too large");
        }
    }

    throw FormatError("truncated varint");
}

}  // namespace axiom::codec
