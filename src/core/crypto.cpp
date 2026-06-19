#include "core/crypto.hpp"

#include "third_party/monocypher/monocypher.h"

#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#if defined(_MSC_VER)
#pragma comment(lib, "bcrypt")  // auto-link; the g++ build passes -lbcrypt instead
#endif
#else
#include <fstream>
#endif

namespace axiom::core {

void random_bytes(std::span<std::uint8_t> out) {
    if (out.empty()) {
        return;
    }
#if defined(_WIN32)
    const NTSTATUS status = BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        throw std::runtime_error("secure random generation failed");
    }
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom ||
        !urandom.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()))) {
        throw std::runtime_error("secure random generation failed");
    }
#endif
}

CryptoKey derive_key(const std::string& password, const KdfParams& params) {
    CryptoKey key{};
    // Argon2 needs a scratch area of nb_blocks * 1 KiB.
    std::vector<std::uint8_t> work_area(static_cast<std::size_t>(params.mem_blocks) * 1024);

    const crypto_argon2_config config{params.algorithm, params.mem_blocks, params.passes,
                                      params.lanes};
    const crypto_argon2_inputs inputs{reinterpret_cast<const std::uint8_t*>(password.data()),
                                      params.salt.data(),
                                      static_cast<std::uint32_t>(password.size()),
                                      static_cast<std::uint32_t>(params.salt.size())};

    crypto_argon2(key.data(), static_cast<std::uint32_t>(key.size()), work_area.data(), config,
                  inputs, crypto_argon2_no_extras);
    crypto_wipe(work_area.data(), work_area.size());
    return key;
}

std::vector<std::uint8_t> aead_seal(const CryptoKey& key,
                                    std::span<const std::uint8_t> plaintext,
                                    std::span<const std::uint8_t> ad) {
    std::vector<std::uint8_t> sealed(kAeadOverhead + plaintext.size());
    std::uint8_t* nonce = sealed.data();
    std::uint8_t* mac = sealed.data() + 24;
    std::uint8_t* cipher = sealed.data() + kAeadOverhead;
    random_bytes({nonce, 24});
    crypto_aead_lock(cipher, mac, key.data(), nonce, ad.empty() ? nullptr : ad.data(), ad.size(),
                     plaintext.empty() ? nullptr : plaintext.data(), plaintext.size());
    return sealed;
}

bool aead_open(const CryptoKey& key, std::span<const std::uint8_t> sealed,
               std::span<const std::uint8_t> ad, std::vector<std::uint8_t>& out) {
    if (sealed.size() < kAeadOverhead) {
        return false;
    }
    const std::uint8_t* nonce = sealed.data();
    const std::uint8_t* mac = sealed.data() + 24;
    const std::uint8_t* cipher = sealed.data() + kAeadOverhead;
    const std::size_t cipher_size = sealed.size() - kAeadOverhead;

    std::vector<std::uint8_t> plaintext(cipher_size);
    const int result = crypto_aead_unlock(plaintext.empty() ? nullptr : plaintext.data(), mac,
                                          key.data(), nonce, ad.empty() ? nullptr : ad.data(),
                                          ad.size(), cipher_size == 0 ? nullptr : cipher,
                                          cipher_size);
    if (result != 0) {
        return false;  // forged or wrong key
    }
    out = std::move(plaintext);
    return true;
}

void secure_wipe(std::span<std::uint8_t> buffer) {
    if (!buffer.empty()) {
        crypto_wipe(buffer.data(), buffer.size());
    }
}

}  // namespace axiom::core
