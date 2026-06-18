#pragma once

#include "third_party/blake3/blake3.h"

#include <array>
#include <cstdint>
#include <span>

namespace axiom::core {

// 256-bit BLAKE3 digest — the strong per-file content hash (CRC-32 is retained for
// the cheap per-block check). BLAKE3 is collision-resistant, unlike CRC.
using Blake3Digest = std::array<std::uint8_t, BLAKE3_OUT_LEN>;

// Thin incremental wrapper over the vendored BLAKE3 (portable build). Feed file
// chunks through update(); finalize() yields the digest. Cheap to construct.
class Blake3 {
public:
    Blake3() { blake3_hasher_init(&hasher_); }

    void update(std::span<const std::uint8_t> data) {
        blake3_hasher_update(&hasher_, data.data(), data.size());
    }

    Blake3Digest finalize() const {
        Blake3Digest digest{};
        blake3_hasher_finalize(&hasher_, digest.data(), digest.size());
        return digest;
    }

    static Blake3Digest hash(std::span<const std::uint8_t> data) {
        Blake3 hasher;
        hasher.update(data);
        return hasher.finalize();
    }

private:
    blake3_hasher hasher_;
};

}  // namespace axiom::core
