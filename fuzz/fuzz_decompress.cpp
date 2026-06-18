// libFuzzer target for the single-stream decoder. This is the largest untrusted
// surface: decompress() dispatches to the LZ77 / rep / slot / order-1 / range /
// Huffman / parallel-block decoders, all on attacker-controlled bytes.
//
// Build (MSVC):  cl /std:c++20 /O1 /Zi /fsanitize=fuzzer /fsanitize=address ...
// Build (clang): clang++ -std=c++20 -O1 -g -fsanitize=fuzzer,address ...

#include "axiom/axiom.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        // Cap output so a valid-looking bomb cannot exhaust the fuzzer's memory.
        axiom::DecompressionOptions options;
        options.max_output_size = std::size_t{16} << 20;
        (void)axiom::decompress(std::span<const std::uint8_t>(data, size), options);
    } catch (const std::exception&) {
        // Malformed input must be rejected with an exception, never a crash.
    }
    return 0;
}
