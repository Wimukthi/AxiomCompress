#include "core/checksum.hpp"

#include <cstddef>
#include <cstdint>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#define AXIOM_CHECKSUM_X86 1
#endif

#if defined(AXIOM_CHECKSUM_X86)

#include <emmintrin.h>
#include <smmintrin.h>
#include <wmmintrin.h>

namespace axiom::core::detail {

// PCLMULQDQ-folded reflected CRC-32 (polynomial 0xEDB88320), after the Intel
// "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ" paper; the
// folding structure and bit-reflected constants match zlib's crc32_simd.c.
// Four 128-bit lanes fold 64 bytes per iteration, then reduce 4 -> 1 lane,
// fold remaining 16-byte blocks, and Barrett-reduce to 32 bits.
//
// Caller contract: size >= 64 and size % 16 == 0; `state` is the running
// pre-conditioned CRC (initialised to 0xFFFFFFFF, final xor applied by the
// caller), exactly as crc32_update maintains it. Only call when the CPU
// reports PCLMUL and SSE4.1 support.
std::uint32_t crc32_update_pclmul(std::uint32_t state,
                                  const std::uint8_t* data,
                                  std::size_t size) {
    alignas(16) static const std::uint64_t k1k2[2] = {0x0154442bd4, 0x01c6e41596};
    alignas(16) static const std::uint64_t k3k4[2] = {0x01751997d0, 0x00ccaa009e};
    alignas(16) static const std::uint64_t k5k0[2] = {0x0163cd6124, 0x0000000000};
    alignas(16) static const std::uint64_t poly[2] = {0x01db710641, 0x01f7011641};

    __m128i x0, x1, x2, x3, x4, x5, x6, x7, x8, y5, y6, y7, y8;

    x1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 0x00));
    x2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 0x10));
    x3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 0x20));
    x4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 0x30));

    x1 = _mm_xor_si128(x1, _mm_cvtsi32_si128(static_cast<int>(state)));

    x0 = _mm_load_si128(reinterpret_cast<const __m128i*>(k1k2));

    data += 64;
    size -= 64;

    while (size >= 64) {
        x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
        x6 = _mm_clmulepi64_si128(x2, x0, 0x00);
        x7 = _mm_clmulepi64_si128(x3, x0, 0x00);
        x8 = _mm_clmulepi64_si128(x4, x0, 0x00);

        x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
        x2 = _mm_clmulepi64_si128(x2, x0, 0x11);
        x3 = _mm_clmulepi64_si128(x3, x0, 0x11);
        x4 = _mm_clmulepi64_si128(x4, x0, 0x11);

        y5 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 0x00));
        y6 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 0x10));
        y7 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 0x20));
        y8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 0x30));

        x1 = _mm_xor_si128(x1, x5);
        x2 = _mm_xor_si128(x2, x6);
        x3 = _mm_xor_si128(x3, x7);
        x4 = _mm_xor_si128(x4, x8);

        x1 = _mm_xor_si128(x1, y5);
        x2 = _mm_xor_si128(x2, y6);
        x3 = _mm_xor_si128(x3, y7);
        x4 = _mm_xor_si128(x4, y8);

        data += 64;
        size -= 64;
    }

    // Reduce the four lanes to one.
    x0 = _mm_load_si128(reinterpret_cast<const __m128i*>(k3k4));

    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(x1, x2);
    x1 = _mm_xor_si128(x1, x5);

    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(x1, x3);
    x1 = _mm_xor_si128(x1, x5);

    x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
    x1 = _mm_xor_si128(x1, x4);
    x1 = _mm_xor_si128(x1, x5);

    // Fold any remaining 16-byte blocks.
    while (size >= 16) {
        x2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));

        x5 = _mm_clmulepi64_si128(x1, x0, 0x00);
        x1 = _mm_clmulepi64_si128(x1, x0, 0x11);
        x1 = _mm_xor_si128(x1, x2);
        x1 = _mm_xor_si128(x1, x5);

        data += 16;
        size -= 16;
    }

    // Fold 128 bits to 64 bits.
    x2 = _mm_clmulepi64_si128(x1, x0, 0x10);
    x3 = _mm_setr_epi32(~0, 0, ~0, 0);
    x1 = _mm_srli_si128(x1, 8);
    x1 = _mm_xor_si128(x1, x2);

    x0 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(k5k0));

    x2 = _mm_srli_si128(x1, 4);
    x1 = _mm_and_si128(x1, x3);
    x1 = _mm_clmulepi64_si128(x1, x0, 0x00);
    x1 = _mm_xor_si128(x1, x2);

    // Barrett reduction to 32 bits.
    x0 = _mm_load_si128(reinterpret_cast<const __m128i*>(poly));

    x2 = _mm_and_si128(x1, x3);
    x2 = _mm_clmulepi64_si128(x2, x0, 0x10);
    x2 = _mm_and_si128(x2, x3);
    x2 = _mm_clmulepi64_si128(x2, x0, 0x00);
    x1 = _mm_xor_si128(x1, x2);

    return static_cast<std::uint32_t>(_mm_extract_epi32(x1, 1));
}

}  // namespace axiom::core::detail

#endif  // AXIOM_CHECKSUM_X86
