// libFuzzer target for the multi-file archive container parser. Drives the
// directory/block-table parser and block decoder in memory on untrusted bytes.

#include "archive/fuzz_support.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        axiom::detail::fuzz_read_archive(std::span<const std::uint8_t>(data, size));
    } catch (const std::exception&) {
        // Malformed archives must be rejected with an exception, never a crash.
    }
    return 0;
}
