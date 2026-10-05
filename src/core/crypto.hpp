#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace axiom::core {

// A derived 256-bit symmetric key.
using CryptoKey = std::array<std::uint8_t, 32>;

struct SigningKeyPair {
    std::array<std::uint8_t, 64> secret_key{};
    std::array<std::uint8_t, 32> public_key{};
};

// Password key-derivation parameters (Argon2id). The salt and the cost parameters
// are stored in the archive so the same key can be re-derived on decrypt.
struct KdfParams {
    std::uint32_t algorithm = 2;          // CRYPTO_ARGON2_ID
    std::uint32_t mem_blocks = 1u << 16;  // memory in KiB blocks (64 MiB)
    std::uint32_t passes = 3;             // time cost
    std::uint32_t lanes = 1;              // parallelism (single-threaded here)
    std::array<std::uint8_t, 16> salt{};
};

// The cost parameters are read from the archive, so a reader must bound them
// before it spends the time and memory they ask for. Axiom itself writes 64 MiB,
// 3 passes, 1 lane (196,608 block passes, about a quarter of a second).
inline constexpr std::uint32_t kMaxKdfMemBlocks = 1u << 21;  // 2 GiB of 1 KiB blocks
inline constexpr std::uint32_t kMaxKdfPasses = 64;
inline constexpr std::uint32_t kMaxKdfLanes = 16;
// Memory blocks x passes: the work of one derivation, about 3 us per block pass at
// worst. Together with the memory limit this allows 2 GiB for one pass, 64 MiB for
// 32, and everything between.
inline constexpr std::uint64_t kMaxKdfWorkBlocks = std::uint64_t{1} << 21;
// The same measure summed over all the password slots of one archive, all of which
// a wrong password has to work through.
inline constexpr std::uint64_t kMaxKdfArchiveWorkBlocks = std::uint64_t{1} << 22;

constexpr std::uint64_t kdf_work_blocks(const KdfParams& params) noexcept {
    return std::uint64_t{params.mem_blocks} * params.passes;
}

// Whether `params` are inside the limits above and satisfy Argon2's own floor of 8
// blocks per lane. Computed in 64 bits: a 32-bit `8 * lanes` wraps to zero.
constexpr bool kdf_parameters_valid(const KdfParams& params) noexcept {
    return params.algorithm <= 2 &&
           params.lanes >= 1 && params.lanes <= kMaxKdfLanes &&
           params.passes >= 1 && params.passes <= kMaxKdfPasses &&
           params.mem_blocks <= kMaxKdfMemBlocks &&
           std::uint64_t{params.mem_blocks} >= std::uint64_t{8} * params.lanes &&
           kdf_work_blocks(params) <= kMaxKdfWorkBlocks;
}

// Fill `out` with cryptographically secure random bytes. Throws on RNG failure.
void random_bytes(std::span<std::uint8_t> out);

// Derive a 32-byte key from `password` using Argon2id with `params`. Expensive by
// design (memory-hard); call once per archive, not per block. Throws
// std::invalid_argument when `params` fail kdf_parameters_valid().
CryptoKey derive_key(const std::string& password, const KdfParams& params);

// AEAD seal (XChaCha20-Poly1305): returns nonce(24) || mac(16) || ciphertext. `ad`
// is authenticated but not encrypted — bind context (e.g. the block index) so blocks
// cannot be reordered or swapped between archives.
std::vector<std::uint8_t> aead_seal(const CryptoKey& key,
                                    std::span<const std::uint8_t> plaintext,
                                    std::span<const std::uint8_t> ad);

// AEAD open: verify and decrypt a blob produced by aead_seal. Returns false on any
// authentication failure (wrong key/password or tampering); `out` is then untouched.
bool aead_open(const CryptoKey& key, std::span<const std::uint8_t> sealed,
               std::span<const std::uint8_t> ad, std::vector<std::uint8_t>& out);

// Best-effort wipe of a secret buffer (not optimized away).
void secure_wipe(std::span<std::uint8_t> buffer);

SigningKeyPair generate_signing_key();
std::array<std::uint8_t, 64> sign_message(
    const std::array<std::uint8_t, 64>& secret_key,
    std::span<const std::uint8_t> message);
bool verify_message(const std::array<std::uint8_t, 32>& public_key,
                    const std::array<std::uint8_t, 64>& signature,
                    std::span<const std::uint8_t> message);

// Bytes prepended to a sealed blob: 24-byte nonce + 16-byte Poly1305 tag.
constexpr std::size_t kAeadOverhead = 24 + 16;

}  // namespace axiom::core
