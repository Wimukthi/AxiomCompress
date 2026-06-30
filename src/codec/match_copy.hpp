#pragma once

#include "axiom/axiom.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace axiom::codec {

inline void copy_repeated_pattern(std::span<std::uint8_t> output,
                                  std::size_t& out,
                                  std::size_t& length,
                                  std::uint64_t pattern) {
    while (length >= sizeof(pattern)) {
        std::memcpy(output.data() + out, &pattern, sizeof(pattern));
        out += sizeof(pattern);
        length -= sizeof(pattern);
    }
}

inline void copy_lz_match(std::span<std::uint8_t> output,
                          std::size_t& out,
                          std::size_t distance,
                          std::size_t length,
                          const char* invalid_reference_error,
                          const char* output_overflow_error) {
    if (length == 0 || distance == 0 || distance > out) {
        throw FormatError(invalid_reference_error);
    }
    if (length > output.size() - out) {
        throw FormatError(output_overflow_error);
    }

    if (distance == 1) {
        std::fill_n(output.begin() + static_cast<std::ptrdiff_t>(out), length,
                    output[out - 1]);
        out += length;
        return;
    }

    if (distance >= length) {
        std::memcpy(output.data() + out, output.data() + out - distance, length);
        out += length;
        return;
    }

    // Common small repeated patterns are frequent in text, zero-filled data, and
    // structured binary streams. Expanding them by an 8-byte replicated pattern
    // avoids the byte loop while preserving the exact LZ overlap semantics.
    if (distance == 2) {
        std::uint16_t unit = 0;
        std::memcpy(&unit, output.data() + out - 2, sizeof(unit));
        const std::uint64_t pattern = static_cast<std::uint64_t>(unit) |
            (static_cast<std::uint64_t>(unit) << 16) |
            (static_cast<std::uint64_t>(unit) << 32) |
            (static_cast<std::uint64_t>(unit) << 48);
        copy_repeated_pattern(output, out, length, pattern);
    } else if (distance == 4) {
        std::uint32_t unit = 0;
        std::memcpy(&unit, output.data() + out - 4, sizeof(unit));
        const std::uint64_t pattern = static_cast<std::uint64_t>(unit) |
            (static_cast<std::uint64_t>(unit) << 32);
        copy_repeated_pattern(output, out, length, pattern);
    } else if (distance >= 16) {
        // For non-overlapping chunks inside an overlapping match, copying 16
        // bytes at a time is safe because the source chunk ends before the
        // destination chunk starts. Later chunks may intentionally read bytes
        // produced by earlier chunks, which is the required LZ behavior.
        while (length >= 16) {
            std::memcpy(output.data() + out, output.data() + out - distance, 16);
            out += 16;
            length -= 16;
        }
    }

    while (length >= 8 && distance >= 8) {
        std::memcpy(output.data() + out, output.data() + out - distance, 8);
        out += 8;
        length -= 8;
    }
    while (length > 0) {
        output[out] = output[out - distance];
        ++out;
        --length;
    }
}

}  // namespace axiom::codec
