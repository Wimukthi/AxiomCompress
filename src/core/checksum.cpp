#include "core/checksum.hpp"

#include "core/cpu.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define AXIOM_CHECKSUM_X86 1
#endif

namespace axiom::core {

#if defined(AXIOM_CHECKSUM_X86)
namespace detail {
// Defined in checksum_clmul.cpp; requires size >= 64, size % 16 == 0, and a
// CPU with PCLMUL + SSE4.1 (checked by the caller).
std::uint32_t crc32_update_pclmul(std::uint32_t state,
                                  const std::uint8_t* data,
                                  std::size_t size);
}  // namespace detail
#endif

namespace {

// Standard reflected CRC-32 (polynomial 0xEDB88320, as used by zlib/PNG/gzip).
// table[0] is the classic byte table; table[1..7] extend it so the scalar path
// can consume eight bytes per step (the "slice-by-8" method).
using CrcTables = std::array<std::array<std::uint32_t, 256>, 8>;

CrcTables make_crc_tables() {
    CrcTables tables{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
        }
        tables[0][i] = value;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t value = tables[0][i];
        for (std::size_t slice = 1; slice < 8; ++slice) {
            value = tables[0][value & 0xFFu] ^ (value >> 8);
            tables[slice][i] = value;
        }
    }
    return tables;
}

const CrcTables& crc_tables() {
    static const CrcTables tables = make_crc_tables();
    return tables;
}

// Little-endian-valued 32-bit load, built from bytes so the result is
// independent of host endianness (the slice-by-8 tables assume LE word order).
// On a little-endian target the compiler folds this back into a single load.
std::uint32_t load_u32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint32_t gf2_matrix_times(const std::array<std::uint32_t, 32>& matrix,
                               std::uint32_t vector) {
    std::uint32_t sum = 0;
    std::size_t index = 0;
    while (vector != 0) {
        if ((vector & 1u) != 0) {
            sum ^= matrix[index];
        }
        vector >>= 1;
        ++index;
    }
    return sum;
}

std::array<std::uint32_t, 32> gf2_matrix_square(const std::array<std::uint32_t, 32>& matrix) {
    std::array<std::uint32_t, 32> square{};
    for (std::size_t i = 0; i < square.size(); ++i) {
        square[i] = gf2_matrix_times(matrix, matrix[i]);
    }
    return square;
}

}  // namespace

std::uint32_t crc32_init() {
    return 0xFFFFFFFFu;
}

// Reflected CRC-32 update. Large inputs go through the PCLMULQDQ folding
// kernel when the CPU supports it (an order of magnitude faster than the
// table method); the remainder and small inputs use slice-by-8, which stays
// the guaranteed-correct portable reference.
std::uint32_t crc32_update(std::uint32_t state, std::span<const std::uint8_t> input) {
    const auto& tables = crc_tables();
    const std::uint8_t* data = input.data();
    std::size_t size = input.size();

#if defined(AXIOM_CHECKSUM_X86)
    if (size >= 64) {
        const auto& cpu = cpu_features();
        if (cpu.pclmul && cpu.sse41) {
            const std::size_t folded = size & ~std::size_t{15};
            state = detail::crc32_update_pclmul(state, data, folded);
            data += folded;
            size -= folded;
        }
    }
#endif

    while (size >= 8) {
        const std::uint32_t low = state ^ load_u32_le(data);
        const std::uint32_t high = load_u32_le(data + 4);
        state = tables[7][low & 0xFFu] ^ tables[6][(low >> 8) & 0xFFu] ^
                tables[5][(low >> 16) & 0xFFu] ^ tables[4][(low >> 24) & 0xFFu] ^
                tables[3][high & 0xFFu] ^ tables[2][(high >> 8) & 0xFFu] ^
                tables[1][(high >> 16) & 0xFFu] ^ tables[0][(high >> 24) & 0xFFu];
        data += 8;
        size -= 8;
    }

    for (std::size_t i = 0; i < size; ++i) {
        state = tables[0][(state ^ data[i]) & 0xFFu] ^ (state >> 8);
    }
    return state;
}

std::uint32_t crc32_final(std::uint32_t state) {
    return state ^ 0xFFFFFFFFu;
}

std::uint32_t crc32(std::span<const std::uint8_t> input) {
    return crc32_final(crc32_update(crc32_init(), input));
}

std::uint32_t crc32_combine(std::uint32_t first_crc,
                            std::uint32_t second_crc,
                            std::uint64_t second_size) {
    if (second_size == 0) {
        return first_crc;
    }

    std::array<std::uint32_t, 32> odd{};
    odd[0] = 0xEDB88320u;
    std::uint32_t row = 1;
    for (std::size_t i = 1; i < odd.size(); ++i) {
        odd[i] = row;
        row <<= 1;
    }

    auto even = gf2_matrix_square(odd);
    odd = gf2_matrix_square(even);

    auto crc = first_crc;
    do {
        even = gf2_matrix_square(odd);
        if ((second_size & 1u) != 0) {
            crc = gf2_matrix_times(even, crc);
        }
        second_size >>= 1;
        if (second_size == 0) {
            break;
        }

        odd = gf2_matrix_square(even);
        if ((second_size & 1u) != 0) {
            crc = gf2_matrix_times(odd, crc);
        }
        second_size >>= 1;
    } while (second_size != 0);

    return crc ^ second_crc;
}

}  // namespace axiom::core
