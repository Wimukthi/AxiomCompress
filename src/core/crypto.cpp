#include "core/crypto.hpp"

#include "third_party/monocypher/monocypher.h"

#include <cstring>
#include <memory>
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
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/random.h>
#endif
#endif

namespace axiom::core {

#if !defined(_WIN32)
namespace {

// Reads from /dev/urandom with plain descriptors: a stream would buffer a few
// KiB of the kernel's output for every handful of bytes asked for.
bool fill_from_urandom(std::uint8_t* cursor, std::size_t remaining) {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = ::open("/dev/urandom", flags);
    if (fd < 0) return false;
    bool ok = true;
    while (remaining != 0) {
        const ssize_t got = ::read(fd, cursor, remaining);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            ok = false;
            break;
        }
        cursor += got;
        remaining -= static_cast<std::size_t>(got);
    }
    ::close(fd);
    return ok;
}

}  // namespace
#endif

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
    std::uint8_t* cursor = out.data();
    std::size_t remaining = out.size();
#if defined(__linux__) || defined(__APPLE__)
    // One system call and no file descriptor. getentropy() takes at most 256
    // bytes per call, so ask in pieces of that size on every platform.
    while (remaining != 0) {
        const std::size_t want = std::min<std::size_t>(remaining, 256);
#if defined(__linux__)
        const ssize_t got = ::getrandom(cursor, want, 0);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) continue;
            break;  // ENOSYS on an old kernel, or no entropy source: try the device
        }
        const auto produced = static_cast<std::size_t>(got);
#else
        if (::getentropy(cursor, want) != 0) break;
        const std::size_t produced = want;
#endif
        cursor += produced;
        remaining -= produced;
    }
    if (remaining == 0) {
        return;
    }
#endif
    if (!fill_from_urandom(cursor, remaining)) {
        throw std::runtime_error("secure random generation failed");
    }
#endif
}

CryptoKey derive_key(const std::string& password, const KdfParams& params) {
    // The parameters may come from an untrusted archive. Argon2 itself checks
    // nothing: too few blocks for the lanes would make it index outside its work area.
    if (!kdf_parameters_valid(params)) {
        throw std::invalid_argument("key-derivation parameters are outside the supported range");
    }
    CryptoKey key{};
    // Argon2 needs a scratch area of nb_blocks * 1 KiB. It writes every block before
    // reading it, so the area is left uninitialized rather than zero-filled first.
    const std::size_t work_area_size = static_cast<std::size_t>(params.mem_blocks) * 1024;
    const auto work_area = std::make_unique_for_overwrite<std::uint8_t[]>(work_area_size);

    const crypto_argon2_config config{params.algorithm, params.mem_blocks, params.passes,
                                      params.lanes};
    const crypto_argon2_inputs inputs{reinterpret_cast<const std::uint8_t*>(password.data()),
                                      params.salt.data(),
                                      static_cast<std::uint32_t>(password.size()),
                                      static_cast<std::uint32_t>(params.salt.size())};

    crypto_argon2(key.data(), static_cast<std::uint32_t>(key.size()), work_area.get(), config,
                  inputs, crypto_argon2_no_extras);
    crypto_wipe(work_area.get(), work_area_size);
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

SigningKeyPair generate_signing_key() {
    std::array<std::uint8_t, 32> seed{};
    random_bytes(seed);
    SigningKeyPair pair;
    crypto_eddsa_key_pair(pair.secret_key.data(), pair.public_key.data(), seed.data());
    secure_wipe(seed);
    return pair;
}

std::array<std::uint8_t, 64> sign_message(
    const std::array<std::uint8_t, 64>& secret_key,
    std::span<const std::uint8_t> message) {
    std::array<std::uint8_t, 64> signature{};
    crypto_eddsa_sign(signature.data(), secret_key.data(),
                      message.empty() ? nullptr : message.data(), message.size());
    return signature;
}

bool verify_message(const std::array<std::uint8_t, 32>& public_key,
                    const std::array<std::uint8_t, 64>& signature,
                    std::span<const std::uint8_t> message) {
    return crypto_eddsa_check(signature.data(), public_key.data(),
                              message.empty() ? nullptr : message.data(), message.size()) == 0;
}

}  // namespace axiom::core
