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

// Fill `out` with cryptographically secure random bytes. Throws on RNG failure.
void random_bytes(std::span<std::uint8_t> out);

// Derive a 32-byte key from `password` using Argon2id with `params`. Expensive by
// design (memory-hard); call once per archive, not per block.
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
