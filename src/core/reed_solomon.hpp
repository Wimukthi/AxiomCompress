#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace axiom::core {

// Systematic Reed-Solomon erasure coding over GF(2^8). `data_shards` original shards
// are protected by `parity_shards` computed shards; any up to `parity_shards` lost or
// corrupted shards can be reconstructed, provided the caller knows which are missing.
//
// This is a portable scalar implementation (correctness first); a SIMD backend can be
// slotted in later without changing the on-disk recovery format.
class ReedSolomon {
public:
    ReedSolomon(int data_shards, int parity_shards);

    int data_shards() const { return data_shards_; }
    int parity_shards() const { return parity_shards_; }
    int total_shards() const { return data_shards_ + parity_shards_; }

    // Compute the parity shards from the data shards. `data` has `data_shards` entries
    // and `parity` has `parity_shards` entries, all of identical length.
    void encode(const std::vector<std::span<const std::uint8_t>>& data,
                const std::vector<std::span<std::uint8_t>>& parity) const;

    // Reconstruct missing shards in place. `shards` holds all total_shards() shards
    // (equal length); `present[i]` is false for a missing/corrupt shard, whose buffer
    // is overwritten with the recovered bytes. Returns false if fewer than
    // data_shards present shards remain (unrecoverable).
    bool reconstruct(std::vector<std::vector<std::uint8_t>>& shards,
                     const std::vector<bool>& present) const;

private:
    int data_shards_;
    int parity_shards_;
    std::vector<std::uint8_t> matrix_;  // total_shards × data_shards, row-major
};

}  // namespace axiom::core
