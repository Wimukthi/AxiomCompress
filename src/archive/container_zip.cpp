// ZIP archive support: minizip-ng container I/O, miniz-backed Deflate,
// streaming ZipCrypto and WinZip AES-256, atomic archive rewrites, and the
// built-in ZIP ArchiveProvider. The AXAR engine stays in container.cpp.

#include "axiom/archive.hpp"

#include "archive/container_internal.hpp"
#include "archive/zip_split_backend.hpp"
#include "core/checksum.hpp"
#include "core/crypto.hpp"
#include "core/file_replace.hpp"
#include "core/file_meta.hpp"
#include "core/path_text.hpp"
// miniz declares its full static helper set in the header, so every
// translation unit that includes it leaves most of them unused. The
// vendored file is dependency-locked and cannot carry the suppression.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "third_party/miniz/miniz.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace axiom {
namespace {

namespace fs = std::filesystem;

class TempDirectoryGuard final {
public:
    explicit TempDirectoryGuard(fs::path path) : path_(std::move(path)) {}
    ~TempDirectoryGuard() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

private:
    fs::path path_;
};

std::string normalize_zip_entry_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    return normalize_archive_path(std::move(path), "ZIP entry path");
}

std::uint64_t checked_file_size(const fs::path& path) {
    const auto size = fs::file_size(path);
    if (size > std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("ZIP file is too large: " + core::path_to_utf8(path));
    }
    return static_cast<std::uint64_t>(size);
}

bool is_numbered_zip_volume(const fs::path& path) {
    const auto extension = path.extension().wstring();
    return extension.size() >= 3 && (extension[1] == L'z' || extension[1] == L'Z') &&
        std::all_of(extension.begin() + 2, extension.end(),
                    [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
}

fs::path zip_final_volume_path(const fs::path& path) {
    if (is_numbered_zip_volume(path)) {
        fs::path final = path;
        final.replace_extension(L".zip");
        std::error_code error;
        if (fs::exists(final, error)) return final;
    }
    return path;
}

bool zip_is_multidisk(const fs::path& path) {
    std::ifstream input(zip_final_volume_path(path), std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < std::streamoff(22)) return false;
    const auto tail_size = static_cast<std::size_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(end), 65557));
    ByteVector tail(tail_size);
    input.seekg(end - static_cast<std::streamoff>(tail_size), std::ios::beg);
    input.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    if (!input) return false;
    for (std::size_t pos = tail.size() - 22;; --pos) {
        if (tail[pos] == 'P' && tail[pos + 1] == 'K' && tail[pos + 2] == 5 &&
            tail[pos + 3] == 6) {
            const auto disk = static_cast<std::uint16_t>(tail[pos + 4]) |
                              (static_cast<std::uint16_t>(tail[pos + 5]) << 8);
            const auto central_disk = static_cast<std::uint16_t>(tail[pos + 6]) |
                                      (static_cast<std::uint16_t>(tail[pos + 7]) << 8);
            return disk != 0 || central_disk != 0;
        }
        if (pos == 0) break;
    }
    return false;
}

void reject_split_zip_edit(const fs::path& path) {
    if (zip_is_multidisk(path)) {
        throw std::runtime_error(
            "editing a split ZIP in place is not supported; extract or recreate the set");
    }
}

class ZipReader final {
public:
    explicit ZipReader(const fs::path& path,
                       const std::shared_ptr<OperationControl>& operation = nullptr,
                       const SfxZipPayloadRange& payload_range = std::nullopt)
        : path_(payload_range ? path : zip_final_volume_path(path)) {
        if (!payload_range && is_numbered_zip_volume(path) && path_ == path) {
            fs::path final = path;
            final.replace_extension(L".zip");
            throw FormatError("split ZIP is incomplete; final volume is missing: " +
                              core::path_to_utf8(final));
        }
        backend_ = std::make_unique<ZipBackendReader>(path_, operation, payload_range);
    }

    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    ~ZipReader() = default;

    const fs::path& path() const { return path_; }
    bool is_split() const { return backend_->is_split(); }
    const std::vector<ZipBackendEntryInfo>& entries() const {
        return backend_->entries();
    }
    void read_raw_entry(std::size_t index, const ZipRawChunkCallback& callback) {
        backend_->read_raw_entry(index, callback);
    }
    ZipBackendReader& backend() { return *backend_; }

private:
    fs::path path_;
    std::unique_ptr<ZipBackendReader> backend_;
};

class ZipWriter final {
public:
    explicit ZipWriter(const fs::path& path, std::uint64_t volume_size = 0,
                       const std::shared_ptr<OperationControl>& operation = nullptr)
        : backend_(path, volume_size, operation) {}

    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    ~ZipWriter() = default;

    ZipBackendWriter& backend() { return backend_; }
    std::uint64_t size() const { return backend_.size(); }
    void finalize() { backend_.finalize(); }

private:
    ZipBackendWriter backend_;
};

struct ZipEntryPlan {
    std::size_t index = 0;
    ArchiveEntry entry;
    std::uint16_t method = 0;
    std::uint16_t bit_flag = 0;
    std::uint64_t compressed_size = 0;
    std::string comment;
    bool supported = false;
    bool encrypted = false;
    bool zipcrypto_supported = false;
    bool aes_supported = false;
    std::uint16_t aes_version = 0;
    std::uint16_t aes_actual_method = 0;
    std::uint16_t zipcrypto_verifier = 0;
    std::uint8_t aes_strength = 0;
};

constexpr std::uint16_t kZipFlagEncrypted = 0x0001u;
constexpr std::uint16_t kZipFlagStrongEncryption = 0x0040u;
constexpr std::uint16_t kZipMethodStore = 0;
constexpr std::uint16_t kZipMethodDeflate = 8;
constexpr std::uint16_t kZipMethodAes = 99;
constexpr std::size_t kZipEncryptionHeaderSize = 12;
constexpr std::uint16_t kZipAesExtraFieldId = 0x9901u;
constexpr std::uint16_t kZipAesVendorVersionAe2 = 0x0002u;
constexpr std::uint8_t kZipAesStrength256 = 3;
constexpr std::size_t kZipAes256SaltSize = 16;
constexpr std::size_t kZipAesPasswordVerifierSize = 2;
constexpr std::size_t kZipAesAuthCodeSize = 10;
constexpr std::size_t kZipAes256Overhead =
    kZipAes256SaltSize + kZipAesPasswordVerifierSize + kZipAesAuthCodeSize;

std::uint16_t read_le16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset + 2 > bytes.size()) throw FormatError("ZIP structure is truncated");
    return static_cast<std::uint16_t>(bytes[offset] |
                                      (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

bool zip_entry_uses_classic_crypto(const ZipEntryPlan& plan) {
    return plan.encrypted &&
           (plan.bit_flag & kZipFlagEncrypted) != 0 &&
           (plan.bit_flag & kZipFlagStrongEncryption) == 0 &&
           (plan.method == kZipMethodStore || plan.method == kZipMethodDeflate);
}

class ZipCrypto {
public:
    explicit ZipCrypto(std::string_view password) {
        for (unsigned char ch : password) {
            update_keys(ch);
        }
    }

    std::uint8_t decrypt(std::uint8_t value) {
        const std::uint8_t plain = static_cast<std::uint8_t>(value ^ crypt_byte());
        update_keys(plain);
        return plain;
    }

private:
    static const std::array<std::uint32_t, 256>& crc_table() {
        static const auto table = [] {
            std::array<std::uint32_t, 256> values{};
            for (std::uint32_t i = 0; i < values.size(); ++i) {
                std::uint32_t crc = i;
                for (int bit = 0; bit < 8; ++bit) {
                    crc = (crc & 1u) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
                }
                values[i] = crc;
            }
            return values;
        }();
        return table;
    }

    static std::uint32_t crc32_byte(std::uint32_t crc, std::uint8_t value) {
        const auto& table = crc_table();
        return table[(crc ^ value) & 0xffu] ^ (crc >> 8);
    }

    std::uint8_t crypt_byte() const {
        const std::uint16_t temp = static_cast<std::uint16_t>(key2_ | 2u);
        return static_cast<std::uint8_t>((temp * (temp ^ 1u)) >> 8);
    }

    void update_keys(std::uint8_t value) {
        key0_ = crc32_byte(key0_, value);
        key1_ = key1_ + (key0_ & 0xffu);
        key1_ = key1_ * 134775813u + 1u;
        key2_ = crc32_byte(key2_, static_cast<std::uint8_t>(key1_ >> 24));
    }

    std::uint32_t key0_ = 305419896u;
    std::uint32_t key1_ = 591751049u;
    std::uint32_t key2_ = 878082192u;
};

struct ZipAesExtra {
    std::uint16_t version = 0;
    std::uint8_t strength = 0;
    std::uint16_t actual_method = 0;
};

std::optional<ZipAesExtra> parse_zip_aes_extra(std::span<const std::uint8_t> extra) {
    std::size_t pos = 0;
    while (pos + 4 <= extra.size()) {
        const std::uint16_t id = read_le16(extra, pos);
        const std::uint16_t size = read_le16(extra, pos + 2);
        pos += 4;
        if (pos + size > extra.size()) {
            throw FormatError("ZIP AES extra field is truncated");
        }
        if (id == kZipAesExtraFieldId) {
            if (size < 7) {
                throw FormatError("ZIP AES extra field is invalid");
            }
            if (extra[pos + 2] != 'A' || extra[pos + 3] != 'E') {
                throw FormatError("ZIP AES vendor is unsupported");
            }
            ZipAesExtra parsed;
            parsed.version = read_le16(extra, pos);
            parsed.strength = extra[pos + 4];
            parsed.actual_method = read_le16(extra, pos + 5);
            return parsed;
        }
        pos += size;
    }
    return std::nullopt;
}

ByteVector zip_aes_extra_field(std::uint16_t actual_method) {
    ByteVector extra;
    extra.reserve(11);
    put_u16(extra, kZipAesExtraFieldId);
    put_u16(extra, 7);
    put_u16(extra, kZipAesVendorVersionAe2);
    extra.push_back(static_cast<std::uint8_t>('A'));
    extra.push_back(static_cast<std::uint8_t>('E'));
    extra.push_back(kZipAesStrength256);
    put_u16(extra, actual_method);
    return extra;
}

std::uint16_t zip_effective_method(const ZipEntryPlan& plan) {
    return plan.aes_supported ? plan.aes_actual_method : plan.method;
}

bool zip_entry_extractable(const ZipEntryPlan& plan, bool has_password) {
    if (plan.entry.is_directory) return true;
    if (!plan.encrypted) return plan.supported;
    return has_password && (plan.zipcrypto_supported || plan.aes_supported);
}

class Sha1 {
public:
    void update(std::span<const std::uint8_t> input) {
        total_size_ += input.size();
        std::size_t offset = 0;
        if (buffer_size_ != 0) {
            const std::size_t take = std::min<std::size_t>(input.size(), 64 - buffer_size_);
            std::copy_n(input.data(), take, buffer_.data() + buffer_size_);
            buffer_size_ += take;
            offset += take;
            if (buffer_size_ == 64) {
                transform(buffer_.data());
                buffer_size_ = 0;
            }
        }
        while (offset + 64 <= input.size()) {
            transform(input.data() + offset);
            offset += 64;
        }
        if (offset < input.size()) {
            buffer_size_ = input.size() - offset;
            std::copy_n(input.data() + offset, buffer_size_, buffer_.data());
        }
    }

    std::array<std::uint8_t, 20> final() {
        const std::uint64_t bit_size = static_cast<std::uint64_t>(total_size_) * 8u;
        buffer_[buffer_size_++] = 0x80;
        if (buffer_size_ > 56) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
                      buffer_.end(), std::uint8_t{0});
            transform(buffer_.data());
            buffer_size_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
                  buffer_.begin() + 56, std::uint8_t{0});
        for (int i = 0; i < 8; ++i) {
            buffer_[56 + i] = static_cast<std::uint8_t>(bit_size >> ((7 - i) * 8));
        }
        transform(buffer_.data());

        std::array<std::uint8_t, 20> digest{};
        for (std::size_t i = 0; i < state_.size(); ++i) {
            digest[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24);
            digest[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
            digest[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
            digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
        }
        return digest;
    }

private:
    static std::uint32_t rol(std::uint32_t value, int bits) {
        return (value << bits) | (value >> (32 - bits));
    }

    static std::uint32_t be32(const std::uint8_t* in) {
        return (static_cast<std::uint32_t>(in[0]) << 24) |
               (static_cast<std::uint32_t>(in[1]) << 16) |
               (static_cast<std::uint32_t>(in[2]) << 8) |
               static_cast<std::uint32_t>(in[3]);
    }

    void transform(const std::uint8_t block[64]) {
        std::array<std::uint32_t, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = be32(block + i * 4);
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }

    std::array<std::uint32_t, 5> state_{
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t total_size_ = 0;
};

class HmacSha1 {
public:
    explicit HmacSha1(std::span<const std::uint8_t> key) {
        std::array<std::uint8_t, 64> key_block{};
        if (key.size() > key_block.size()) {
            Sha1 sha;
            sha.update(key);
            const auto digest = sha.final();
            std::copy(digest.begin(), digest.end(), key_block.begin());
        } else if (!key.empty()) {
            std::copy(key.begin(), key.end(), key_block.begin());
        }
        std::array<std::uint8_t, 64> ipad{};
        for (std::size_t i = 0; i < key_block.size(); ++i) {
            ipad[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x36u);
            opad_[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x5cu);
        }
        inner_.update(ipad);
    }

    void update(std::span<const std::uint8_t> input) { inner_.update(input); }

    std::array<std::uint8_t, 20> final() {
        const auto inner_digest = inner_.final();
        Sha1 outer;
        outer.update(opad_);
        outer.update(inner_digest);
        return outer.final();
    }

private:
    Sha1 inner_;
    std::array<std::uint8_t, 64> opad_{};
};

std::array<std::uint8_t, 20> hmac_sha1(std::span<const std::uint8_t> key,
                                       std::span<const std::uint8_t> message) {
    HmacSha1 hmac(key);
    hmac.update(message);
    return hmac.final();
}

bool constant_time_equal(std::span<const std::uint8_t> left,
                         std::span<const std::uint8_t> right) {
    if (left.size() != right.size()) return false;
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<std::uint8_t>(left[i] ^ right[i]);
    }
    return diff == 0;
}

ByteVector pbkdf2_hmac_sha1(std::string_view password,
                            std::span<const std::uint8_t> salt,
                            std::uint32_t iterations,
                            std::size_t output_size) {
    const auto* password_bytes =
        reinterpret_cast<const std::uint8_t*>(password.data());
    const std::span<const std::uint8_t> key(password_bytes, password.size());
    ByteVector output;
    output.reserve(output_size);
    ByteVector block_input(salt.begin(), salt.end());
    block_input.resize(salt.size() + 4);

    for (std::uint32_t block_index = 1; output.size() < output_size; ++block_index) {
        block_input[salt.size()] = static_cast<std::uint8_t>(block_index >> 24);
        block_input[salt.size() + 1] = static_cast<std::uint8_t>(block_index >> 16);
        block_input[salt.size() + 2] = static_cast<std::uint8_t>(block_index >> 8);
        block_input[salt.size() + 3] = static_cast<std::uint8_t>(block_index);
        auto u = hmac_sha1(key, block_input);
        auto t = u;
        for (std::uint32_t i = 1; i < iterations; ++i) {
            u = hmac_sha1(key, u);
            for (std::size_t j = 0; j < t.size(); ++j) {
                t[j] ^= u[j];
            }
        }
        const std::size_t take = std::min<std::size_t>(t.size(), output_size - output.size());
        output.insert(output.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return output;
}

class Aes256 {
public:
    explicit Aes256(std::span<const std::uint8_t, 32> key) {
        expand_key(key);
    }

    void encrypt_block(const std::uint8_t input[16], std::uint8_t output[16]) const {
        std::array<std::uint8_t, 16> state{};
        std::copy_n(input, state.size(), state.begin());
        add_round_key(state, 0);
        for (std::size_t round = 1; round < 14; ++round) {
            sub_bytes(state);
            shift_rows(state);
            mix_columns(state);
            add_round_key(state, round);
        }
        sub_bytes(state);
        shift_rows(state);
        add_round_key(state, 14);
        std::copy(state.begin(), state.end(), output);
    }

private:
    static constexpr std::array<std::uint8_t, 256> kSbox{
        0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
        0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
        0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
        0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
        0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
        0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
        0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
        0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
        0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
        0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
        0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
        0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
        0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
        0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
        0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
        0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

    static constexpr std::array<std::uint8_t, 15> kRcon{
        0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36,0x6c,0xd8,0xab,0x4d};

    static std::uint8_t xtime(std::uint8_t value) {
        return static_cast<std::uint8_t>((value << 1) ^ ((value & 0x80u) ? 0x1bu : 0));
    }

    static std::uint8_t mul2(std::uint8_t value) { return xtime(value); }
    static std::uint8_t mul3(std::uint8_t value) {
        return static_cast<std::uint8_t>(xtime(value) ^ value);
    }

    void expand_key(std::span<const std::uint8_t, 32> key) {
        std::copy(key.begin(), key.end(), round_keys_.begin());
        std::size_t bytes = 32;
        std::uint8_t rcon_index = 1;
        std::array<std::uint8_t, 4> temp{};
        while (bytes < round_keys_.size()) {
            std::copy_n(round_keys_.begin() + static_cast<std::ptrdiff_t>(bytes - 4),
                        4, temp.begin());
            if (bytes % 32 == 0) {
                const std::uint8_t first = temp[0];
                temp[0] = static_cast<std::uint8_t>(kSbox[temp[1]] ^ kRcon[rcon_index++]);
                temp[1] = kSbox[temp[2]];
                temp[2] = kSbox[temp[3]];
                temp[3] = kSbox[first];
            } else if (bytes % 32 == 16) {
                for (auto& item : temp) item = kSbox[item];
            }
            for (std::size_t i = 0; i < 4; ++i) {
                round_keys_[bytes] =
                    static_cast<std::uint8_t>(round_keys_[bytes - 32] ^ temp[i]);
                ++bytes;
            }
        }
    }

    void add_round_key(std::array<std::uint8_t, 16>& state, std::size_t round) const {
        const std::size_t base = round * 16;
        for (std::size_t i = 0; i < state.size(); ++i) {
            state[i] ^= round_keys_[base + i];
        }
    }

    static void sub_bytes(std::array<std::uint8_t, 16>& state) {
        for (auto& byte : state) byte = kSbox[byte];
    }

    static void shift_rows(std::array<std::uint8_t, 16>& s) {
        const auto old = s;
        s[1] = old[5];  s[5] = old[9];  s[9] = old[13]; s[13] = old[1];
        s[2] = old[10]; s[6] = old[14]; s[10] = old[2]; s[14] = old[6];
        s[3] = old[15]; s[7] = old[3];  s[11] = old[7]; s[15] = old[11];
    }

    static void mix_columns(std::array<std::uint8_t, 16>& s) {
        for (std::size_t c = 0; c < 4; ++c) {
            const std::size_t i = c * 4;
            const std::uint8_t a0 = s[i];
            const std::uint8_t a1 = s[i + 1];
            const std::uint8_t a2 = s[i + 2];
            const std::uint8_t a3 = s[i + 3];
            s[i] = static_cast<std::uint8_t>(mul2(a0) ^ mul3(a1) ^ a2 ^ a3);
            s[i + 1] = static_cast<std::uint8_t>(a0 ^ mul2(a1) ^ mul3(a2) ^ a3);
            s[i + 2] = static_cast<std::uint8_t>(a0 ^ a1 ^ mul2(a2) ^ mul3(a3));
            s[i + 3] = static_cast<std::uint8_t>(mul3(a0) ^ a1 ^ a2 ^ mul2(a3));
        }
    }

    std::array<std::uint8_t, 240> round_keys_{};
};

class Aes256Ctr {
public:
    explicit Aes256Ctr(std::span<const std::uint8_t, 32> key) : aes_(key) {}

    void update(std::span<std::uint8_t> bytes) {
        for (auto& byte : bytes) {
            if (stream_offset_ == stream_.size()) {
                std::array<std::uint8_t, 16> counter{};
                for (std::size_t i = 0; i < 8; ++i) {
                    counter[i] = static_cast<std::uint8_t>(block_index_ >> (i * 8));
                }
                aes_.encrypt_block(counter.data(), stream_.data());
                ++block_index_;
                stream_offset_ = 0;
            }
            byte ^= stream_[stream_offset_++];
        }
    }

private:
    Aes256 aes_;
    std::array<std::uint8_t, 16> stream_{};
    std::size_t stream_offset_ = 16;
    std::uint64_t block_index_ = 1;
};

struct ZipAesKeyMaterial {
    std::array<std::uint8_t, 32> encryption_key{};
    std::array<std::uint8_t, 32> authentication_key{};
    std::array<std::uint8_t, 2> password_verifier{};
};

ZipAesKeyMaterial zip_aes256_key_material(std::string_view password,
                                          std::span<const std::uint8_t> salt) {
    if (salt.size() != kZipAes256SaltSize) {
        throw FormatError("ZIP AES-256 salt is invalid");
    }
    ByteVector derived = pbkdf2_hmac_sha1(password, salt, 1000, 66);
    ZipAesKeyMaterial keys;
    std::copy_n(derived.begin(), 32, keys.encryption_key.begin());
    std::copy_n(derived.begin() + 32, 32, keys.authentication_key.begin());
    keys.password_verifier[0] = derived[64];
    keys.password_verifier[1] = derived[65];
    return keys;
}

std::vector<ZipEntryPlan> read_zip_entry_plans(ZipReader& reader) {
    const auto& entries = reader.entries();
    std::vector<ZipEntryPlan> result;
    result.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& source = entries[index];
        ZipEntryPlan plan;
        plan.index = index;
        plan.entry.path = normalize_zip_entry_path(source.path);
        plan.entry.is_directory = source.directory;
        plan.entry.size = source.directory ? 0 : source.uncompressed_size;
        plan.entry.packed_size = source.directory
            ? std::optional<std::uint64_t>{}
            : std::optional<std::uint64_t>{source.compressed_size};
        plan.entry.crc32 = source.directory ? 0 : source.crc32;
        plan.entry.has_crc32 = !source.directory;
        plan.entry.mtime = source.modified_time;
        plan.method = source.method;
        plan.bit_flag = source.flags;
        plan.compressed_size = source.compressed_size;
        plan.comment = source.comment;
        plan.supported = source.directory || plan.method == kZipMethodStore ||
                         plan.method == kZipMethodDeflate;
        plan.encrypted = (plan.bit_flag & kZipFlagEncrypted) != 0;
        plan.zipcrypto_verifier = source.zipcrypto_verifier;
        if (plan.encrypted && plan.method == kZipMethodAes) {
            const auto aes = parse_zip_aes_extra(source.extra);
            if (aes.has_value()) {
                plan.aes_version = aes->version;
                plan.aes_strength = aes->strength;
                plan.aes_actual_method = aes->actual_method;
                plan.aes_supported =
                    (aes->version == 1 || aes->version == kZipAesVendorVersionAe2) &&
                    aes->strength == kZipAesStrength256 &&
                    (aes->actual_method == kZipMethodStore ||
                     aes->actual_method == kZipMethodDeflate);
                if (aes->version == kZipAesVendorVersionAe2) {
                    plan.entry.has_crc32 = false;
                }
            }
        }
        plan.zipcrypto_supported = zip_entry_uses_classic_crypto(plan);
        result.push_back(std::move(plan));
    }
    return result;
}

ArchiveCapabilities zip_capabilities_for_plans(const std::vector<ZipEntryPlan>& entries,
                                               bool has_password) {
    ArchiveCapabilities result;
    result.list = true;
    result.create = true;
    result.encryption = true;
    result.metadata = true;
    result.packed_sizes = true;
    result.sfx = true;
    result.can_create_volumes = true;

    bool all_extractable = true;
    bool any_extractable = false;
    bool any_encrypted = false;
    for (const auto& item : entries) {
        any_encrypted = any_encrypted || item.encrypted;
        const bool extractable = zip_entry_extractable(item, has_password);
        any_extractable = any_extractable || extractable;
        if (!extractable) {
            all_extractable = false;
        }
    }
    result.extract = all_extractable;
    result.test = all_extractable;
    result.selective_extract = any_extractable;
    result.encrypted = any_encrypted;
    result.update = !any_encrypted;
    result.delete_entries = !any_encrypted;
    result.move_entries = !any_encrypted;
    return result;
}

bool zip_plan_selected(const ZipEntryPlan& plan, const std::vector<std::string>& wanted) {
    if (wanted.empty()) {
        return true;
    }
    for (const auto& item : wanted) {
        if (plan.entry.path == item || is_same_or_child(plan.entry.path, item)) {
            return true;
        }
    }
    return false;
}

int zip_compression_level(const CompressionOptions& options) {
    if (options.force_store || options.method == CompressionMethod::store) {
        return MZ_NO_COMPRESSION;
    }
    if (options.method == CompressionMethod::deflate) {
        return options.codec_level == kAutomaticCodecLevel
            ? std::clamp(options.level, 0, 9)
            : std::clamp(options.codec_level, 0, 9);
    }
    if (options.use_fast_lz || options.max_chain_depth <= 16 || options.fast_entropy) {
        return MZ_BEST_SPEED;
    }
    if (options.use_tree_matcher || options.max_chain_depth >= 256 ||
        options.enable_optimal_parser) {
        return MZ_BEST_COMPRESSION;
    }
    return MZ_DEFAULT_COMPRESSION;
}

void reject_zip_write_options(const CompressionOptions& options) {
    if (options.method == CompressionMethod::zstandard ||
        options.method == CompressionMethod::lzma2) {
        throw std::runtime_error(
            "ZIP creation supports Deflate or Store; choose AXAR for this compression method");
    }
    if (options.encrypt_header) {
        throw std::runtime_error(
            "ZIP AES-256 encrypts file data only; use AXAR for encrypted file names");
    }
    if (options.recovery_percent != 0) {
        throw std::runtime_error("ZIP writing does not support Axiom recovery records");
    }
}

void reject_unwritable_zip_entries(const std::vector<ZipEntryPlan>& plans) {
    for (const auto& plan : plans) {
        if (plan.encrypted) {
            throw FormatError("cannot update encrypted ZIP archives yet");
        }
    }
}

std::vector<ScanItem> scan_zip_inputs(const std::vector<fs::path>& inputs,
                                      const std::shared_ptr<OperationControl>& operation) {
    report_operation(operation, OperationStage::scanning, 0, 0, 0, inputs.size());
    std::vector<ScanItem> items;
    for (const auto& input : inputs) {
        operation_checkpoint(operation);
        scan_input(input, items);
    }
    return items;
}

std::vector<ScanItem> scan_zip_inputs(const std::vector<ArchiveInput>& inputs,
                                      const std::shared_ptr<OperationControl>& operation) {
    report_operation(operation, OperationStage::scanning, 0, 0, 0, inputs.size());
    std::vector<ScanItem> items;
    for (const auto& input : inputs) {
        operation_checkpoint(operation);
        scan_input_at(input, items, operation);
    }
    return items;
}

std::string zip_writer_path(const ScanItem& item) {
    std::string path = normalize_archive_path(item.archive_path, "ZIP entry path");
    if (item.is_directory && !path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    return path;
}

std::unordered_set<std::string> zip_replacement_paths(const std::vector<ScanItem>& items) {
    std::unordered_set<std::string> result;
    result.reserve(items.size());
    for (const auto& item : items) {
        result.insert(normalize_archive_path(item.archive_path, "ZIP entry path"));
    }
    return result;
}

void validate_zip_items(const std::vector<ScanItem>& items,
                        const std::vector<ZipEntryPlan>& existing,
                        const std::shared_ptr<OperationControl>& operation) {
    std::unordered_map<std::string, bool> incoming;
    incoming.reserve(items.size());
    for (const auto& item : items) {
        operation_checkpoint(operation);
        if (item.is_symlink) {
            throw std::runtime_error("ZIP writing does not support symbolic links yet: " +
                                     item.archive_path);
        }
        const std::string path = normalize_archive_path(item.archive_path, "ZIP entry path");
        const auto [it, inserted] = incoming.emplace(path, item.is_directory);
        if (!inserted) {
            throw std::invalid_argument("duplicate ZIP destination: " + path);
        }
    }

    for (const auto& [path, is_directory] : incoming) {
        if (is_directory) {
            continue;
        }
        for (const auto& [other, ignored] : incoming) {
            (void)ignored;
            if (other.size() > path.size() && is_same_or_child(other, path)) {
                throw std::invalid_argument("non-directory ZIP destination has children: " +
                                            path);
            }
        }
    }

    for (const auto& plan : existing) {
        const auto& old_path = plan.entry.path;
        for (const auto& [path, is_directory] : incoming) {
            if (!is_directory && old_path.size() > path.size() &&
                is_same_or_child(old_path, path)) {
                throw std::invalid_argument(
                    "non-directory ZIP destination has existing children: " + path);
            }
            if (!plan.entry.is_directory && path.size() > old_path.size() &&
                is_same_or_child(path, old_path) && incoming.find(old_path) == incoming.end()) {
                throw std::invalid_argument("cannot add a ZIP entry below an existing file: " +
                                            path);
            }
        }
    }
}

class ZipEntryOutput {
public:
    ZipEntryOutput(ZipBackendWriter& writer, std::string_view password)
        : writer_(writer) {
        if (password.empty()) return;
        std::array<std::uint8_t, kZipAes256SaltSize> salt{};
        core::random_bytes(salt);
        const auto keys = zip_aes256_key_material(password, salt);
        ctr_ = std::make_unique<Aes256Ctr>(keys.encryption_key);
        hmac_ = std::make_unique<HmacSha1>(keys.authentication_key);
        writer_.write_raw(salt);
        writer_.write_raw(keys.password_verifier);
    }

    void write(std::span<std::uint8_t> compressed) {
        if (ctr_) {
            ctr_->update(compressed);
            hmac_->update(compressed);
        }
        writer_.write_raw(compressed);
    }

    void finish() {
        if (!hmac_) return;
        const auto auth = hmac_->final();
        writer_.write_raw(std::span<const std::uint8_t>(auth.data(),
                                                       kZipAesAuthCodeSize));
    }

private:
    ZipBackendWriter& writer_;
    std::unique_ptr<Aes256Ctr> ctr_;
    std::unique_ptr<HmacSha1> hmac_;
};

struct ZipWriteResult {
    std::uint32_t crc32 = 0;
    std::uint64_t size = 0;
};

ZipWriteResult write_zip_input(
    ZipEntryOutput& output, std::ifstream& input, bool store, int level,
    const CompressionOptions& options, ZipWriter& writer,
    std::uint64_t& completed_bytes, std::uint64_t total_bytes,
    std::uint64_t completed_items, std::uint64_t total_items,
    const std::string& current_path, std::uint64_t expected_size) {
    std::array<std::uint8_t, 64u << 10> input_buffer{};
    std::array<std::uint8_t, 64u << 10> output_buffer{};
    ZipWriteResult result;
    mz_stream stream{};
    bool deflate_open = false;
    if (!store) {
        const int status = mz_deflateInit2(
            &stream, level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9,
            MZ_DEFAULT_STRATEGY);
        if (status != MZ_OK) {
            throw std::runtime_error("cannot initialize ZIP Deflate encoder");
        }
        deflate_open = true;
    }

    try {
        for (;;) {
            operation_checkpoint(options.operation);
            input.read(reinterpret_cast<char*>(input_buffer.data()),
                       static_cast<std::streamsize>(input_buffer.size()));
            const auto count = input.gcount();
            if (count < 0) throw std::runtime_error("cannot read ZIP input");
            if (count == 0) {
                if (!input.eof()) throw std::runtime_error("cannot read ZIP input");
                break;
            }
            const auto amount = static_cast<std::size_t>(count);
            result.crc32 = static_cast<std::uint32_t>(mz_crc32(
                result.crc32, input_buffer.data(), amount));
            result.size += amount;
            if (store) {
                output.write(std::span<std::uint8_t>(input_buffer.data(), amount));
            } else {
                stream.next_in = input_buffer.data();
                stream.avail_in = static_cast<unsigned int>(amount);
                while (stream.avail_in != 0) {
                    stream.next_out = output_buffer.data();
                    stream.avail_out = static_cast<unsigned int>(output_buffer.size());
                    const int status = mz_deflate(&stream, MZ_NO_FLUSH);
                    if (status != MZ_OK) {
                        throw std::runtime_error("ZIP Deflate compression failed");
                    }
                    const auto produced = output_buffer.size() - stream.avail_out;
                    if (produced != 0) {
                        output.write(std::span<std::uint8_t>(output_buffer.data(), produced));
                    }
                }
            }
            completed_bytes += amount;
            report_operation(options.operation, OperationStage::compressing,
                             completed_bytes, total_bytes, completed_items,
                             total_items, current_path, result.size, expected_size,
                             completed_bytes, writer.size(), completed_bytes);
        }

        if (!store) {
            int status = MZ_OK;
            do {
                stream.next_out = output_buffer.data();
                stream.avail_out = static_cast<unsigned int>(output_buffer.size());
                status = mz_deflate(&stream, MZ_FINISH);
                if (status != MZ_OK && status != MZ_STREAM_END) {
                    throw std::runtime_error("ZIP Deflate finalization failed");
                }
                const auto produced = output_buffer.size() - stream.avail_out;
                if (produced != 0) {
                    output.write(std::span<std::uint8_t>(output_buffer.data(), produced));
                }
            } while (status != MZ_STREAM_END);
        }
        output.finish();
    } catch (...) {
        if (deflate_open) mz_deflateEnd(&stream);
        throw;
    }
    if (deflate_open) mz_deflateEnd(&stream);
    return result;
}

std::int64_t scan_item_mtime(const ScanItem& item) {
    std::error_code ec;
    const auto stamp = fs::last_write_time(item.absolute, ec);
    if (ec) {
        return 0;
    }
    try {
        return to_unix_seconds(stamp);
    } catch (...) {
        return 0;
    }
}

void add_zip_scan_item(ZipWriter& writer, const ScanItem& item,
                       const CompressionOptions& options,
                       std::uint64_t& completed_bytes,
                       std::uint64_t total_bytes,
                       std::uint64_t& completed_items,
                       std::uint64_t total_items,
                       bool allow_unreadable_skips) {
    operation_checkpoint(options.operation);
    const std::string archive_path = zip_writer_path(item);
    const std::int64_t modified = scan_item_mtime(item);
    if (item.is_directory) {
        ZipBackendWriteInfo info;
        info.path = archive_path;
        info.method = kZipMethodStore;
        info.flags = 0x0800u;
        info.modified_time = modified;
        info.external_attributes = 0x10u;
        info.directory = true;
        writer.backend().begin_raw_entry(info);
        writer.backend().finish_raw_entry(0, 0);
        ++completed_items;
        report_operation(options.operation, OperationStage::compressing,
                         completed_bytes, total_bytes, completed_items, total_items,
                         item.archive_path, 0, 0, completed_bytes,
                         writer.size(), completed_bytes);
        return;
    }
    if (item.is_symlink) {
        throw std::runtime_error("ZIP writing does not support symbolic links yet: " +
                                 item.archive_path);
    }

    std::ifstream input;
    if (!open_input_with_retry(input, item.absolute,
                               options.input_open_retries, options.operation)) {
        if (!allow_unreadable_skips) {
            throw std::runtime_error("cannot read ZIP input: " +
                                     core::path_to_utf8(item.absolute));
        }
        report_skipped_input(item, options.operation);
        ++completed_items;
        report_operation(options.operation, OperationStage::compressing,
                         completed_bytes, total_bytes, completed_items,
                         total_items, item.archive_path);
        return;
    }
    const auto size = checked_file_size(item.absolute);
    const int level = zip_compression_level(options);
    const bool store = level == MZ_NO_COMPRESSION;
    const bool encrypted = !options.password.empty();
    ZipBackendWriteInfo info;
    info.path = archive_path;
    info.method = encrypted ? kZipMethodAes
                            : store ? kZipMethodStore : kZipMethodDeflate;
    info.flags = static_cast<std::uint16_t>(0x0800u |
        (encrypted ? kZipFlagEncrypted : 0));
    info.modified_time = modified;
    info.external_attributes = 0x20u;
    info.uncompressed_size = size;
    if (encrypted) {
        info.extra = zip_aes_extra_field(store ? kZipMethodStore : kZipMethodDeflate);
    }
    writer.backend().begin_raw_entry(info);
    ZipEntryOutput output(writer.backend(), options.password);
    const auto written = write_zip_input(
        output, input, store, level, options, writer, completed_bytes,
        total_bytes, completed_items, total_items, item.archive_path, size);
    writer.backend().finish_raw_entry(encrypted ? 0 : written.crc32,
                                      written.size);
    ++completed_items;
    report_operation(options.operation, OperationStage::compressing,
                      completed_bytes, total_bytes, completed_items, total_items,
                      item.archive_path, size, size, completed_bytes,
                      writer.size(), completed_bytes);
}

template <typename KeepExisting>
void rebuild_zip_archive(const fs::path& archive_path,
                         const std::vector<ScanItem>& additions,
                         const CompressionOptions& options,
                         KeepExisting keep_existing,
                         bool preserve_existing = true,
                         std::uint64_t volume_size = 0) {
    reject_zip_write_options(options);
    if (volume_size != 0 && preserve_existing) {
        throw std::invalid_argument(
            "direct split ZIP output is only valid for archive creation");
    }
    const auto replacements = zip_replacement_paths(additions);
    const bool existing_archive = preserve_existing && fs::exists(archive_path);
    std::uint64_t total_bytes = scanned_file_bytes(additions);
    std::uint64_t total_items = additions.size();

    fs::path temp_path;
    std::unique_ptr<TempDirectoryGuard> staging_guard;
    std::unique_ptr<TempFileGuard> temp_guard;
    if (volume_size != 0) {
        const auto staging = core::unique_sibling_path(archive_path, L"split-stage");
        fs::create_directory(staging);
        staging_guard = std::make_unique<TempDirectoryGuard>(staging);
        temp_path = staging / archive_path.filename();
    } else {
        temp_path = archive_path;
        temp_path += ".tmp";
        temp_guard = std::make_unique<TempFileGuard>(temp_path);
    }

    if (existing_archive) {
        ZipReader reader(archive_path, options.operation);
        auto plans = read_zip_entry_plans(reader);
        reject_unwritable_zip_entries(plans);
        validate_zip_items(additions, plans, options.operation);
        for (const auto& plan : plans) {
            if (keep_existing(plan) &&
                replacements.find(plan.entry.path) == replacements.end()) {
                ++total_items;
                if (!plan.entry.is_directory) {
                    total_bytes += plan.entry.size;
                }
            }
        }

        std::uint64_t completed_bytes = 0;
        std::uint64_t completed_items = 0;
        report_operation(options.operation, OperationStage::compressing,
                         completed_bytes, total_bytes, completed_items, total_items);
        {
            ZipWriter writer(temp_path, volume_size, options.operation);
            for (const auto& plan : plans) {
                operation_checkpoint(options.operation);
                if (!keep_existing(plan) ||
                    replacements.find(plan.entry.path) != replacements.end()) {
                    continue;
                }
                writer.backend().copy_entry(reader.backend(), plan.index);
                if (!plan.entry.is_directory) {
                    completed_bytes += plan.entry.size;
                }
                ++completed_items;
                report_operation(options.operation, OperationStage::compressing,
                                 completed_bytes, total_bytes, completed_items,
                                 total_items, plan.entry.path);
            }
            for (const auto& item : additions) {
                add_zip_scan_item(writer, item, options, completed_bytes, total_bytes,
                                  completed_items, total_items, false);
            }
            report_operation(options.operation, OperationStage::finalizing,
                             completed_bytes, total_bytes, completed_items, total_items,
                             {}, 0, 0, completed_bytes, writer.size(), completed_bytes);
            writer.finalize();
        }
    } else {
        validate_zip_items(additions, {}, options.operation);
        std::uint64_t completed_bytes = 0;
        std::uint64_t completed_items = 0;
        report_operation(options.operation, OperationStage::compressing,
                         completed_bytes, total_bytes, completed_items, total_items);
        {
            ZipWriter writer(temp_path, volume_size, options.operation);
            for (const auto& item : additions) {
                add_zip_scan_item(writer, item, options, completed_bytes, total_bytes,
                                  completed_items, total_items,
                                  options.skip_unreadable_files);
            }
            report_operation(options.operation, OperationStage::finalizing,
                             completed_bytes, total_bytes, completed_items, total_items,
                             {}, 0, 0, completed_bytes, writer.size(), completed_bytes);
            writer.finalize();
        }
    }

    if (volume_size != 0) {
        const auto staged_size = zip_volume_set_size(temp_path);
        if (staged_size <= volume_size) {
            throw std::invalid_argument(
                "split volume size must be smaller than the completed ZIP archive (" +
                std::to_string(staged_size) + " bytes)");
        }
        {
            ZipBackendReader staged_reader(temp_path, options.operation);
            if (!staged_reader.is_split()) {
                throw std::invalid_argument(
                    "split volume size must be smaller than the completed ZIP archive");
            }
        }
        install_zip_volume_set(temp_path, archive_path, options.operation);
    } else {
        replace_archive_file(temp_path, archive_path);
        temp_guard->dismiss();
    }
    report_operation(options.operation, OperationStage::finalizing,
                     total_bytes, total_bytes, total_items, total_items,
                     {}, 0, 0, total_bytes,
                     volume_size == 0 ? checked_file_size(archive_path)
                                      : zip_volume_set_size(archive_path),
                     total_bytes);
}

std::string zip_writer_path(std::string path, bool is_directory) {
    path = normalize_archive_path(std::move(path), "ZIP entry path");
    if (is_directory && path.back() != '/') {
        path.push_back('/');
    }
    return path;
}

void add_moved_zip_entry(ZipWriter& writer,
                         ZipReader& reader,
                         const ZipEntryPlan& plan,
                         const std::string& destination_path,
                         const CompressionOptions& options,
                         std::uint64_t& completed_bytes,
                         std::uint64_t total_bytes,
                         std::uint64_t& completed_items,
                         std::uint64_t total_items) {
    operation_checkpoint(options.operation);
    if (plan.encrypted) {
        throw FormatError("cannot move encrypted ZIP entries yet: " +
                          plan.entry.path);
    }

    const std::string archive_path = zip_writer_path(destination_path, plan.entry.is_directory);
    writer.backend().copy_entry(reader.backend(), plan.index, archive_path);
    if (!plan.entry.is_directory) completed_bytes += plan.entry.size;
    ++completed_items;
    report_operation(options.operation, OperationStage::compressing,
                     completed_bytes, total_bytes, completed_items, total_items,
                     destination_path);
}

std::vector<ArchiveMove> normalize_archive_moves(const std::vector<ArchiveMove>& moves,
                                                 const char* source_field,
                                                 const char* destination_field,
                                                 const std::shared_ptr<OperationControl>& operation) {
    std::vector<ArchiveMove> normalized;
    normalized.reserve(moves.size());
    std::unordered_set<std::string> sources;
    std::unordered_set<std::string> destinations;
    for (const auto& move : moves) {
        operation_checkpoint(operation);
        ArchiveMove item{
            normalize_archive_path(move.source_path, source_field),
            normalize_archive_path(move.destination_path, destination_field)};
        if (item.source_path == item.destination_path) {
            throw std::invalid_argument("archive move source and destination are identical: " +
                                        item.source_path);
        }
        if (!sources.insert(item.source_path).second) {
            throw std::invalid_argument("duplicate archive move source: " + item.source_path);
        }
        if (!destinations.insert(item.destination_path).second) {
            throw std::invalid_argument("duplicate archive move destination: " +
                                        item.destination_path);
        }
        normalized.push_back(std::move(item));
    }
    return normalized;
}

void validate_zip_moves(const std::vector<ArchiveMove>& moves,
                        const std::vector<ZipEntryPlan>& plans,
                        const std::shared_ptr<OperationControl>& operation) {
    std::unordered_map<std::string, bool> original_types;
    original_types.reserve(plans.size());
    for (const auto& plan : plans) {
        operation_checkpoint(operation);
        original_types.emplace(plan.entry.path, plan.entry.is_directory);
    }

    for (std::size_t i = 0; i < moves.size(); ++i) {
        operation_checkpoint(operation);
        const auto& move = moves[i];
        if (is_same_or_child(move.destination_path, move.source_path)) {
            throw std::invalid_argument("cannot move a ZIP entry into its own subtree: " +
                                        move.source_path);
        }
        bool found_source = original_types.find(move.source_path) != original_types.end();
        if (!found_source) {
            for (const auto& [path, ignored] : original_types) {
                (void)ignored;
                if (is_same_or_child(path, move.source_path)) {
                    found_source = true;
                    break;
                }
            }
        }
        if (!found_source) {
            throw std::invalid_argument("ZIP move source does not exist: " + move.source_path);
        }
        for (std::size_t j = 0; j < moves.size(); ++j) {
            if (i == j) {
                continue;
            }
            if (is_same_or_child(move.source_path, moves[j].source_path)) {
                throw std::invalid_argument("ZIP move sources overlap: " + move.source_path);
            }
            if (is_same_or_child(move.destination_path, moves[j].source_path)) {
                throw std::invalid_argument(
                    "ZIP move destination lies in another moved subtree: " +
                    move.destination_path);
            }
        }
    }

    auto moved_path = [&moves](const std::string& path) {
        for (const auto& move : moves) {
            if (path == move.source_path) {
                return move.destination_path;
            }
            if (is_same_or_child(path, move.source_path)) {
                return move.destination_path + path.substr(move.source_path.size());
            }
        }
        return path;
    };

    std::unordered_map<std::string, bool> final_types;
    final_types.reserve(plans.size());
    for (const auto& plan : plans) {
        const std::string path = moved_path(plan.entry.path);
        if (!final_types.emplace(path, plan.entry.is_directory).second) {
            throw std::invalid_argument("ZIP move destination already exists: " + path);
        }
    }

    for (const auto& [path, is_directory] : final_types) {
        operation_checkpoint(operation);
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos) {
            const std::string parent = path.substr(0, slash);
            const auto found = final_types.find(parent);
            if (found != final_types.end() && !found->second) {
                throw std::invalid_argument("ZIP move destination parent is not a directory: " +
                                            parent);
            }
        }
        if (!is_directory) {
            for (const auto& [other, ignored] : final_types) {
                (void)ignored;
                if (other.size() > path.size() && is_same_or_child(other, path)) {
                    throw std::invalid_argument("non-directory ZIP entry has children: " + path);
                }
            }
        }
    }
}

void move_zip_entries(const fs::path& archive_path,
                      const std::vector<ArchiveMove>& moves,
                      const CompressionOptions& options) {
    if (moves.empty()) {
        return;
    }
    reject_zip_write_options(options);
    if (!options.password.empty()) {
        throw std::runtime_error("ZIP move does not support applying encryption");
    }
    auto normalized = normalize_archive_moves(moves, "ZIP move source",
                                              "ZIP move destination", options.operation);

    std::uint64_t total_bytes = 0;
    std::uint64_t total_items = 0;

    fs::path temp_path = archive_path;
    temp_path += ".tmp";
    TempFileGuard temp_guard(temp_path);
    {
        ZipReader reader(archive_path, options.operation);
        auto plans = read_zip_entry_plans(reader);
        reject_unwritable_zip_entries(plans);
        validate_zip_moves(normalized, plans, options.operation);

        auto moved_path = [&normalized](const std::string& path) {
            for (const auto& move : normalized) {
                if (path == move.source_path) {
                    return move.destination_path;
                }
                if (is_same_or_child(path, move.source_path)) {
                    return move.destination_path + path.substr(move.source_path.size());
                }
            }
            return path;
        };

        total_items = plans.size();
        for (const auto& plan : plans) {
            if (!plan.entry.is_directory) {
                total_bytes += plan.entry.size;
            }
        }
        std::uint64_t completed_bytes = 0;
        std::uint64_t completed_items = 0;
        report_operation(options.operation, OperationStage::compressing,
                         completed_bytes, total_bytes, completed_items, total_items);

        {
            ZipWriter writer(temp_path, 0, options.operation);
            for (const auto& plan : plans) {
                operation_checkpoint(options.operation);
                const std::string destination = moved_path(plan.entry.path);
                if (destination != plan.entry.path) {
                    continue;
                }
                writer.backend().copy_entry(reader.backend(), plan.index);
                if (!plan.entry.is_directory) {
                    completed_bytes += plan.entry.size;
                }
                ++completed_items;
                report_operation(options.operation, OperationStage::compressing,
                                 completed_bytes, total_bytes, completed_items,
                                 total_items, plan.entry.path);
            }
            for (const auto& plan : plans) {
                operation_checkpoint(options.operation);
                const std::string destination = moved_path(plan.entry.path);
                if (destination == plan.entry.path) {
                    continue;
                }
                add_moved_zip_entry(writer, reader, plan, destination, options,
                                    completed_bytes, total_bytes, completed_items,
                                    total_items);
            }
            report_operation(options.operation, OperationStage::finalizing,
                             completed_bytes, total_bytes, completed_items, total_items);
            writer.finalize();
        }
    }

    replace_archive_file(temp_path, archive_path);
    temp_guard.dismiss();
    report_operation(options.operation, OperationStage::finalizing,
                     total_bytes, total_bytes, total_items, total_items);
}

struct ZipExtractCallbackContext {
    std::ofstream* output = nullptr;
    std::shared_ptr<OperationControl> operation;
    OperationStage stage = OperationStage::extracting;
    std::uint64_t* completed_bytes = nullptr;
    std::uint64_t total_bytes = 0;
    std::uint64_t completed_items = 0;
    std::uint64_t total_items = 0;
    std::string current_path;
    std::uint64_t current_file_bytes = 0;
    std::uint64_t current_file_total = 0;
};

void zip_emit_plain(ZipExtractCallbackContext& context,
                    std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return;
    operation_checkpoint(context.operation);
    if (context.output != nullptr) {
        context.output->write(reinterpret_cast<const char*>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()));
        if (!*context.output) {
            throw std::runtime_error("failed writing file: " + context.current_path);
        }
    }
    if (context.completed_bytes != nullptr) {
        *context.completed_bytes += bytes.size();
        context.current_file_bytes += bytes.size();
        report_operation(context.operation, context.stage,
                         *context.completed_bytes, context.total_bytes,
                         context.completed_items, context.total_items,
                         context.current_path, context.current_file_bytes,
                         context.current_file_total);
    }
}

class ZipPayloadDecoder {
public:
    ZipPayloadDecoder(std::uint16_t method, const ZipEntryPlan& plan,
                      ZipExtractCallbackContext& context)
        : method_(method), plan_(plan), context_(context) {
        if (method_ == kZipMethodDeflate) {
            if (mz_inflateInit2(&stream_, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
                throw std::runtime_error("cannot initialize ZIP Deflate decoder");
            }
            inflate_open_ = true;
        } else if (method_ != kZipMethodStore) {
            throw FormatError("ZIP entry uses unsupported compression method: " +
                              plan_.entry.path);
        }
    }

    ~ZipPayloadDecoder() {
        if (inflate_open_) mz_inflateEnd(&stream_);
    }

    void write(std::span<const std::uint8_t> compressed) {
        if (finished_) throw FormatError("ZIP entry has trailing compressed data");
        if (method_ == kZipMethodStore) {
            emit(compressed);
            return;
        }
        stream_.next_in = compressed.data();
        stream_.avail_in = static_cast<unsigned int>(compressed.size());
        while (stream_.avail_in != 0) {
            stream_.next_out = output_.data();
            stream_.avail_out = static_cast<unsigned int>(output_.size());
            const auto before_in = stream_.avail_in;
            const int status = mz_inflate(&stream_, MZ_NO_FLUSH);
            const auto produced = output_.size() - stream_.avail_out;
            if (produced != 0) {
                emit(std::span<const std::uint8_t>(output_.data(), produced));
            }
            if (status == MZ_STREAM_END) {
                finished_ = true;
                if (stream_.avail_in != 0) {
                    throw FormatError("ZIP entry has trailing compressed data");
                }
                break;
            }
            if (status != MZ_OK ||
                (before_in == stream_.avail_in && produced == 0)) {
                throw FormatError("ZIP decompression failed: " + plan_.entry.path);
            }
        }
    }

    void finish() {
        if (method_ == kZipMethodDeflate && !finished_) {
            stream_.next_in = nullptr;
            stream_.avail_in = 0;
            for (;;) {
                stream_.next_out = output_.data();
                stream_.avail_out = static_cast<unsigned int>(output_.size());
                const int status = mz_inflate(&stream_, MZ_FINISH);
                const auto produced = output_.size() - stream_.avail_out;
                if (produced != 0) {
                    emit(std::span<const std::uint8_t>(output_.data(), produced));
                }
                if (status == MZ_STREAM_END) {
                    finished_ = true;
                    break;
                }
                if (status != MZ_OK || produced == 0) {
                    throw FormatError("ZIP compressed entry is truncated: " +
                                      plan_.entry.path);
                }
            }
        }
        if (output_size_ != plan_.entry.size) {
            throw FormatError("ZIP entry decompressed to an unexpected size: " +
                              plan_.entry.path);
        }
        if (plan_.entry.has_crc32 &&
            core::crc32_final(crc_) != plan_.entry.crc32) {
            throw FormatError("ZIP entry CRC check failed: " + plan_.entry.path);
        }
    }

private:
    void emit(std::span<const std::uint8_t> bytes) {
        crc_ = core::crc32_update(crc_, bytes);
        output_size_ += bytes.size();
        if (output_size_ > plan_.entry.size) {
            throw FormatError("ZIP entry expands beyond its declared size: " +
                              plan_.entry.path);
        }
        zip_emit_plain(context_, bytes);
    }

    std::uint16_t method_ = 0;
    const ZipEntryPlan& plan_;
    ZipExtractCallbackContext& context_;
    mz_stream stream_{};
    std::array<std::uint8_t, 64u << 10> output_{};
    std::uint64_t output_size_ = 0;
    std::uint32_t crc_ = core::crc32_init();
    bool inflate_open_ = false;
    bool finished_ = false;
};

void extract_zipcrypto_entry(ZipReader& reader,
                             const ZipEntryPlan& plan,
                             const std::string& password,
                             ZipExtractCallbackContext& context) {
    if (password.empty()) {
        throw std::runtime_error("ZIP archive is encrypted; a password is required");
    }
    if (!plan.zipcrypto_supported) {
        throw FormatError("ZIP entry uses unsupported encryption: " + plan.entry.path);
    }
    if (plan.compressed_size < kZipEncryptionHeaderSize) {
        throw FormatError("ZIP encrypted entry is truncated: " + plan.entry.path);
    }

    ZipCrypto crypto(password);
    std::array<std::uint8_t, kZipEncryptionHeaderSize> header{};
    std::size_t header_size = 0;
    bool password_verified = false;
    ZipPayloadDecoder decoder(plan.method, plan, context);
    reader.read_raw_entry(plan.index, [&](std::span<const std::uint8_t> chunk) {
        ByteVector plain(chunk.begin(), chunk.end());
        for (auto& byte : plain) byte = crypto.decrypt(byte);
        std::size_t offset = 0;
        if (header_size < header.size()) {
            const auto take = (std::min)(header.size() - header_size, plain.size());
            std::copy_n(plain.data(), take, header.data() + header_size);
            header_size += take;
            offset += take;
            if (header_size == header.size()) {
                const auto expected_check =
                    static_cast<std::uint8_t>(plan.zipcrypto_verifier);
                if (header.back() != expected_check) {
                    throw std::runtime_error(
                        "wrong password for encrypted ZIP archive");
                }
                password_verified = true;
            }
        }
        if (offset < plain.size()) {
            decoder.write(std::span<const std::uint8_t>(
                plain.data() + offset, plain.size() - offset));
        }
    });
    if (!password_verified) {
        throw FormatError("ZIP encrypted entry is truncated: " + plan.entry.path);
    }
    decoder.finish();
}

void extract_zip_aes_entry(ZipReader& reader,
                           const ZipEntryPlan& plan,
                           const std::string& password,
                           ZipExtractCallbackContext& context) {
    if (password.empty()) {
        throw std::runtime_error("ZIP archive is encrypted; a password is required");
    }
    if (!plan.aes_supported) {
        throw FormatError("ZIP entry uses unsupported AES parameters: " + plan.entry.path);
    }
    if (plan.compressed_size < kZipAes256Overhead) {
        throw FormatError("ZIP AES entry is truncated: " + plan.entry.path);
    }

    constexpr std::size_t header_length =
        kZipAes256SaltSize + kZipAesPasswordVerifierSize;
    ByteVector header;
    header.reserve(header_length);
    ByteVector tail;
    tail.reserve((64u << 10) + kZipAesAuthCodeSize);
    std::unique_ptr<Aes256Ctr> ctr;
    std::unique_ptr<HmacSha1> hmac;
    ZipPayloadDecoder decoder(zip_effective_method(plan), plan, context);

    reader.read_raw_entry(plan.index, [&](std::span<const std::uint8_t> chunk) {
        std::size_t offset = 0;
        if (header.size() < header_length) {
            const auto take = (std::min)(header_length - header.size(), chunk.size());
            header.insert(header.end(), chunk.begin(),
                          chunk.begin() + static_cast<std::ptrdiff_t>(take));
            offset += take;
            if (header.size() == header_length) {
                const auto salt = std::span<const std::uint8_t>(
                    header.data(), kZipAes256SaltSize);
                const auto keys = zip_aes256_key_material(password, salt);
                const auto verifier = std::span<const std::uint8_t>(
                    header.data() + kZipAes256SaltSize,
                    kZipAesPasswordVerifierSize);
                if (!constant_time_equal(keys.password_verifier, verifier)) {
                    throw std::runtime_error("wrong password for encrypted ZIP archive");
                }
                ctr = std::make_unique<Aes256Ctr>(keys.encryption_key);
                hmac = std::make_unique<HmacSha1>(keys.authentication_key);
            }
        }
        if (offset < chunk.size()) {
            tail.insert(tail.end(),
                        chunk.begin() + static_cast<std::ptrdiff_t>(offset),
                        chunk.end());
        }
        if (tail.size() > kZipAesAuthCodeSize) {
            const auto process_size = tail.size() - kZipAesAuthCodeSize;
            hmac->update(std::span<const std::uint8_t>(tail.data(), process_size));
            ctr->update(std::span<std::uint8_t>(tail.data(), process_size));
            decoder.write(std::span<const std::uint8_t>(tail.data(), process_size));
            tail.erase(tail.begin(),
                       tail.begin() + static_cast<std::ptrdiff_t>(process_size));
        }
    });
    if (header.size() != header_length || tail.size() != kZipAesAuthCodeSize ||
        !hmac || !ctr) {
        throw FormatError("ZIP AES entry is truncated: " + plan.entry.path);
    }
    const auto auth = hmac->final();
    if (!constant_time_equal(
            std::span<const std::uint8_t>(auth.data(), kZipAesAuthCodeSize), tail)) {
        throw FormatError("ZIP AES authentication failed: " + plan.entry.path);
    }
    decoder.finish();
}

void extract_zip_entry(ZipReader& reader,
                       const ZipEntryPlan& plan,
                       const std::string& password,
                       ZipExtractCallbackContext& context,
                       std::string_view failure) {
    if (plan.encrypted) {
        if (plan.aes_supported) {
            extract_zip_aes_entry(reader, plan, password, context);
        } else {
            extract_zipcrypto_entry(reader, plan, password, context);
        }
        return;
    }
    (void)failure;
    ZipPayloadDecoder decoder(plan.method, plan, context);
    reader.read_raw_entry(plan.index, [&](std::span<const std::uint8_t> chunk) {
        decoder.write(chunk);
    });
    decoder.finish();
}

void update_zip_items(const std::vector<ScanItem>& items,
                      const fs::path& archive_path,
                      const CompressionOptions& options,
                      bool fresh_only) {
    std::unordered_map<std::string, std::int64_t> existing_mtime;
    {
        ZipReader reader(archive_path);
        auto plans = read_zip_entry_plans(reader);
        reject_unwritable_zip_entries(plans);
        for (const auto& plan : plans) {
            existing_mtime.emplace(plan.entry.path, plan.entry.mtime);
        }
    }

    std::vector<ScanItem> selected;
    for (const auto& item : items) {
        operation_checkpoint(options.operation);
        const auto found = existing_mtime.find(item.archive_path);
        const bool in_archive = found != existing_mtime.end();
        if (item.is_directory || item.is_symlink) {
            if (!in_archive && !fresh_only) {
                selected.push_back(item);
            }
            continue;
        }
        const std::int64_t disk_mtime = scan_item_mtime(item);
        if (in_archive) {
            if (disk_mtime > found->second) {
                selected.push_back(item);
            }
        } else if (!fresh_only) {
            selected.push_back(item);
        }
    }
    if (selected.empty()) {
        return;
    }
    rebuild_zip_archive(archive_path, selected, options,
                        [](const ZipEntryPlan&) { return true; });
}

void sync_zip_items(const std::vector<ScanItem>& items,
                    const fs::path& archive_path,
                    const CompressionOptions& options) {
    std::unordered_map<std::string, std::int64_t> existing_mtime;
    {
        ZipReader reader(archive_path);
        auto plans = read_zip_entry_plans(reader);
        reject_unwritable_zip_entries(plans);
        for (const auto& plan : plans) {
            existing_mtime.emplace(plan.entry.path, plan.entry.mtime);
        }
    }

    std::unordered_set<std::string> wanted;
    wanted.reserve(items.size());
    std::vector<ScanItem> selected;
    for (const auto& item : items) {
        operation_checkpoint(options.operation);
        wanted.insert(item.archive_path);
        const auto found = existing_mtime.find(item.archive_path);
        if (found == existing_mtime.end()) {
            selected.push_back(item);
            continue;
        }
        if (!item.is_directory && !item.is_symlink &&
            scan_item_mtime(item) > found->second) {
            selected.push_back(item);
        }
    }
    rebuild_zip_archive(archive_path, selected, options,
                        [&wanted](const ZipEntryPlan& plan) {
                            return wanted.find(plan.entry.path) != wanted.end();
                        });
}

class ZipArchiveProvider final : public ArchiveProvider {
public:
    const ArchiveFormatInfo& info() const override {
        return kArchiveFormats[kZipFormatIndex];
    }

    bool matches_path(const std::filesystem::path& path) const override {
        const auto extension = lower_ascii(path.extension().wstring());
        return extension == L".zip" ||
               (extension.size() >= 3 && extension[1] == L'z' &&
                std::all_of(extension.begin() + 2, extension.end(),
                            [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }));
    }

    ArchiveCapabilities capabilities(const std::filesystem::path& archive_path,
                                     const std::string& password) const override {
        // A capability query must not throw: the path may be a new archive that
        // does not exist yet, or an unreadable file. Fall back to the format's
        // static capabilities (empty entry list) and let the actual operation
        // report the precise error.
        // A numbered member is path-level evidence of a volume set even when
        // the final .zip member is missing or unreadable. Keep it read-only.
        try {
            return open(archive_path, password).capabilities;
        } catch (...) {
            auto result = zip_capabilities_for_plans({}, !password.empty());
            const bool multidisk = is_numbered_zip_volume(archive_path) ||
                                   zip_is_multidisk(archive_path);
            if (multidisk) {
                make_split_read_only(result);
            }
            return result;
        }
    }

    std::vector<ArchiveEntry> list(const std::filesystem::path& archive_path,
                                   const std::string& password) const override {
        return open(archive_path, password).entries;
    }

    ArchiveContents open(const std::filesystem::path& archive_path,
                         const std::string& password) const override {
        ZipReader reader(archive_path);
        auto plans = read_zip_entry_plans(reader);
        auto capabilities = zip_capabilities_for_plans(plans, !password.empty());
        if (reader.is_split() || is_numbered_zip_volume(archive_path)) {
            make_split_read_only(capabilities);
        }
        std::vector<ArchiveEntry> result;
        result.reserve(plans.size());
        for (auto& plan : plans) {
            result.push_back(std::move(plan.entry));
        }
        return ArchiveContents{std::move(capabilities), std::move(result)};
    }

    void test(const std::filesystem::path& archive_path,
              const DecompressionOptions& options) const override {
        ZipReader reader(archive_path, options.operation);
        auto plans = read_zip_entry_plans(reader);
        const auto capabilities = zip_capabilities_for_plans(plans, !options.password.empty());
        if (!capabilities.test) {
            throw FormatError("ZIP archive uses encryption or compression methods not supported yet");
        }

        std::uint64_t total_bytes = 0;
        std::uint64_t total_items = 0;
        for (const auto& plan : plans) {
            if (!plan.entry.is_directory) {
                total_bytes += plan.entry.size;
            }
            ++total_items;
        }
        std::uint64_t completed_bytes = 0;
        std::uint64_t completed_items = 0;
        report_operation(options.operation, OperationStage::testing, completed_bytes,
                         total_bytes, completed_items, total_items);

        for (const auto& plan : plans) {
            operation_checkpoint(options.operation);
            if (plan.entry.is_directory) {
                ++completed_items;
                report_operation(options.operation, OperationStage::testing, completed_bytes,
                                 total_bytes, completed_items, total_items, plan.entry.path);
                continue;
            }

            ZipExtractCallbackContext context;
            context.stage = OperationStage::testing;
            context.operation = options.operation;
            context.completed_bytes = &completed_bytes;
            context.total_bytes = total_bytes;
            context.completed_items = completed_items;
            context.total_items = total_items;
            context.current_path = plan.entry.path;
            context.current_file_total = plan.entry.size;
            extract_zip_entry(reader, plan, options.password, context,
                              "ZIP integrity test failed");
            ++completed_items;
            report_operation(options.operation, OperationStage::testing, completed_bytes,
                             total_bytes, completed_items, total_items, plan.entry.path);
        }
    }

    void extract_all(const std::filesystem::path& archive_path,
                     const std::filesystem::path& dest_dir,
                     const ExtractOptions& options) const override {
        extract_matching(archive_path, {}, dest_dir, options);
    }

    void extract_selected(const std::filesystem::path& archive_path,
                          const std::vector<std::string>& entries,
                          const std::filesystem::path& dest_dir,
                          const ExtractOptions& options) const override {
        std::vector<std::string> normalized;
        normalized.reserve(entries.size());
        for (auto entry : entries) {
            normalized.push_back(normalize_archive_path(std::move(entry), "ZIP selected entry"));
        }
        extract_matching(archive_path, normalized, dest_dir, options);
    }

    void create(const std::vector<std::filesystem::path>& inputs,
                const std::filesystem::path& archive_path,
                const CompressionOptions& options) const override {
        auto items = scan_zip_inputs(inputs, options.operation);
        rebuild_zip_archive(archive_path, items, options,
                            [](const ZipEntryPlan&) { return false; },
                            false);
    }

    void create(const ArchiveCreateRequest& request) const override {
        auto items = scan_zip_inputs(request.inputs, request.options.operation);
        rebuild_zip_archive(request.archive_path, items, request.options,
                            [](const ZipEntryPlan&) { return false; },
                            false, request.output.volume_size);
    }

    void add(const std::vector<std::filesystem::path>& inputs,
             const std::filesystem::path& archive_path,
             const CompressionOptions& options) const override {
        reject_split_zip_edit(archive_path);
        auto items = scan_zip_inputs(inputs, options.operation);
        rebuild_zip_archive(archive_path, items, options,
                            [](const ZipEntryPlan&) { return true; });
    }

    void add_mapped(const std::vector<ArchiveInput>& inputs,
                    const std::filesystem::path& archive_path,
                    const CompressionOptions& options) const override {
        reject_split_zip_edit(archive_path);
        auto items = scan_zip_inputs(inputs, options.operation);
        rebuild_zip_archive(archive_path, items, options,
                            [](const ZipEntryPlan&) { return true; });
    }

    void update(const std::vector<std::filesystem::path>& inputs,
                const std::filesystem::path& archive_path,
                const CompressionOptions& options,
                bool fresh_only) const override {
        reject_split_zip_edit(archive_path);
        auto items = scan_zip_inputs(inputs, options.operation);
        if (!fs::exists(archive_path)) {
            if (!fresh_only) {
                rebuild_zip_archive(archive_path, items, options,
                                    [](const ZipEntryPlan&) { return false; },
                                    false);
            }
            return;
        }
        update_zip_items(items, archive_path, options, fresh_only);
    }

    void update_mapped(const std::vector<ArchiveInput>& inputs,
                       const std::filesystem::path& archive_path,
                       const CompressionOptions& options,
                       bool fresh_only) const override {
        reject_split_zip_edit(archive_path);
        auto items = scan_zip_inputs(inputs, options.operation);
        if (!fs::exists(archive_path)) {
            if (fresh_only) return;
            throw std::runtime_error("path-aware ZIP update requires an existing archive");
        }
        update_zip_items(items, archive_path, options, fresh_only);
    }

    void sync(const std::vector<std::filesystem::path>& inputs,
              const std::filesystem::path& archive_path,
              const CompressionOptions& options) const override {
        reject_split_zip_edit(archive_path);
        auto items = scan_zip_inputs(inputs, options.operation);
        if (!fs::exists(archive_path)) {
            rebuild_zip_archive(archive_path, items, options,
                                [](const ZipEntryPlan&) { return false; },
                                false);
            return;
        }
        sync_zip_items(items, archive_path, options);
    }

    void sync_mapped(const std::vector<ArchiveInput>& inputs,
                     const std::filesystem::path& archive_path,
                     const CompressionOptions& options) const override {
        reject_split_zip_edit(archive_path);
        auto items = scan_zip_inputs(inputs, options.operation);
        if (!fs::exists(archive_path)) {
            throw std::runtime_error(
                "path-aware ZIP synchronization requires an existing archive");
        }
        sync_zip_items(items, archive_path, options);
    }

    void delete_entries(const std::filesystem::path& archive_path,
                        const std::vector<std::string>& paths,
                        const CompressionOptions& options) const override {
        reject_split_zip_edit(archive_path);
        std::vector<std::string> targets;
        targets.reserve(paths.size());
        for (auto path : paths) {
            std::replace(path.begin(), path.end(), '\\', '/');
            while (path.size() > 1 && path.back() == '/') {
                path.pop_back();
            }
            if (!path.empty()) {
                targets.push_back(normalize_archive_path(std::move(path), "ZIP delete path"));
            }
        }
        if (targets.empty()) {
            return;
        }
        rebuild_zip_archive(archive_path, {}, options,
                            [&targets](const ZipEntryPlan& plan) {
                                for (const auto& target : targets) {
                                    if (plan.entry.path == target ||
                                        is_same_or_child(plan.entry.path, target)) {
                                        return false;
                                    }
                                }
                                return true;
                            });
    }

    void move_entries(const std::filesystem::path& archive_path,
                      const std::vector<ArchiveMove>& moves,
                      const CompressionOptions& options) const override {
        reject_split_zip_edit(archive_path);
        move_zip_entries(archive_path, moves, options);
    }

public:
    // This is static so the decode-only SFX library can reuse the hardened ZIP
    // extraction path without constructing the full mutation provider.
    static void extract_matching(const std::filesystem::path& archive_path,
                                 const std::vector<std::string>& wanted,
                                 const std::filesystem::path& dest_dir,
                                 const ExtractOptions& options,
                                 const SfxZipPayloadRange& payload_range = std::nullopt) {
        ZipReader reader(archive_path, options.operation, payload_range);
        auto plans = read_zip_entry_plans(reader);

        std::vector<const ZipEntryPlan*> selected;
        selected.reserve(plans.size());
        std::uint64_t total_bytes = 0;
        for (const auto& plan : plans) {
            if (!zip_plan_selected(plan, wanted)) {
                continue;
            }
            selected.push_back(&plan);
            if (!plan.entry.is_directory) {
                total_bytes += plan.entry.size;
            }
        }
        if (selected.empty() && !wanted.empty()) {
            throw std::runtime_error("selected ZIP entries were not found");
        }
        for (const ZipEntryPlan* plan : selected) {
            if (plan->encrypted) {
                if (!plan->zipcrypto_supported && !plan->aes_supported) {
                    throw FormatError("selected ZIP entries use unsupported encryption");
                }
                if (options.password.empty()) {
                    throw std::runtime_error(
                        "ZIP archive is encrypted; a password is required");
                }
            } else if (!plan->entry.is_directory && !plan->supported) {
                throw FormatError(
                    wanted.empty()
                        ? "ZIP archive uses compression methods not supported yet"
                        : "selected ZIP entries use compression methods not supported yet");
            }
        }

        std::uint64_t completed_bytes = 0;
        std::uint64_t completed_items = 0;
        const std::uint64_t total_items = selected.size();
        report_operation(options.operation, OperationStage::extracting, completed_bytes,
                         total_bytes, completed_items, total_items);

        std::error_code ec;
        fs::create_directories(dest_dir, ec);
        const fs::path dest_norm = dest_dir.lexically_normal();

        struct DeferredDir {
            fs::path target;
            std::int64_t mtime = 0;
        };
        std::vector<DeferredDir> deferred_dirs;

        for (const auto* selected_plan : selected) {
            const auto& plan = *selected_plan;
            const auto& entry = plan.entry;
            operation_checkpoint(options.operation);
            if (!is_safe_relative(entry.path)) {
                throw FormatError("ZIP archive contains an unsafe path: " + entry.path);
            }
            const fs::path target =
                (dest_dir / core::path_from_utf8(entry.path)).lexically_normal();
            if (!is_within(dest_norm, target)) {
                throw FormatError("ZIP archive path escapes the destination: " + entry.path);
            }
            reject_symlinked_ancestor(dest_norm, target);

            if (entry.is_directory) {
                if (core::is_reparse_point(target)) {
                    throw FormatError("refusing to restore a directory over a symlink: " +
                                      entry.path);
                }
                fs::create_directories(target, ec);
                deferred_dirs.push_back({target, entry.mtime});
                ++completed_items;
                report_operation(options.operation, OperationStage::extracting,
                                 completed_bytes, total_bytes, completed_items, total_items,
                                 entry.path);
                continue;
            }

            fs::create_directories(target.parent_path(), ec);
            if (fs::exists(target, ec)) {
                if (options.overwrite == ExtractOptions::Overwrite::skip) {
                    completed_bytes += entry.size;
                    ++completed_items;
                    report_operation(options.operation, OperationStage::extracting,
                                     completed_bytes, total_bytes, completed_items, total_items,
                                     entry.path);
                    continue;
                }
                if (options.overwrite == ExtractOptions::Overwrite::fail) {
                    throw std::runtime_error("target already exists: " +
                                             core::path_to_utf8(target));
                }
                if (fs::is_directory(target, ec)) {
                    throw std::runtime_error("target is a directory: " +
                                             core::path_to_utf8(target));
                }
                fs::remove(target, ec);
            }

            fs::path temp_target = target;
            temp_target += ".axtmp";
            TempFileGuard temp_guard(temp_target);
            {
                std::ofstream output(temp_target, std::ios::binary | std::ios::trunc);
                if (!output) {
                    throw std::runtime_error("cannot write file: " +
                                             core::path_to_utf8(temp_target));
                }
                ZipExtractCallbackContext context;
                context.output = &output;
                context.operation = options.operation;
                context.completed_bytes = &completed_bytes;
                context.total_bytes = total_bytes;
                context.completed_items = completed_items;
                context.total_items = total_items;
                context.current_path = entry.path;
                context.current_file_total = entry.size;
                extract_zip_entry(reader, plan, options.password, context,
                                  "ZIP extraction failed");
            }

            fs::rename(temp_target, target, ec);
            if (ec) {
                fs::remove(target, ec);
                fs::rename(temp_target, target, ec);
                if (ec) {
                    throw std::runtime_error("failed to move extracted ZIP file into place: " +
                                             ec.message());
                }
            }
            temp_guard.dismiss();
            if (options.restore_mtime && entry.mtime != 0) {
                try {
                    fs::last_write_time(target, from_unix_seconds(entry.mtime), ec);
                } catch (...) {
                    // best effort
                }
            }
            ++completed_items;
            report_operation(options.operation, OperationStage::extracting, completed_bytes,
                             total_bytes, completed_items, total_items, entry.path);
        }

        std::sort(deferred_dirs.begin(), deferred_dirs.end(),
                  [](const DeferredDir& left, const DeferredDir& right) {
                      return left.target.native().size() > right.target.native().size();
                  });
        for (const auto& dir : deferred_dirs) {
            if (options.restore_mtime && dir.mtime != 0) {
                try {
                    fs::last_write_time(dir.target, from_unix_seconds(dir.mtime), ec);
                } catch (...) {
                    // best effort
                }
            }
        }
    }

private:
    static void make_split_read_only(ArchiveCapabilities& capabilities) {
        capabilities.can_create_volumes = false;
        capabilities.is_multi_volume = true;
        capabilities.update = false;
        capabilities.delete_entries = false;
        capabilities.move_entries = false;
    }
};

}  // namespace

ArchiveCapabilities sfx_zip_capabilities(
    const std::filesystem::path& archive_path,
    const std::string& password,
    const SfxZipPayloadRange& payload_range) {
    const bool multidisk = !payload_range &&
                           (is_numbered_zip_volume(archive_path) ||
                            zip_is_multidisk(archive_path));
    try {
        ZipReader reader(archive_path, nullptr, payload_range);
        auto result = zip_capabilities_for_plans(
            read_zip_entry_plans(reader), !password.empty());
        if (multidisk) {
            result.can_create_volumes = false;
            result.is_multi_volume = true;
            result.update = false;
            result.delete_entries = false;
            result.move_entries = false;
        }
        return result;
    } catch (...) {
        auto result = zip_capabilities_for_plans({}, !password.empty());
        if (multidisk) {
            result.can_create_volumes = false;
            result.is_multi_volume = true;
            result.update = false;
            result.delete_entries = false;
            result.move_entries = false;
        }
        return result;
    }
}

std::vector<ArchiveEntry> sfx_zip_list(
    const std::filesystem::path& archive_path,
    const std::string& password,
    const SfxZipPayloadRange& payload_range) {
    (void)password;
    ZipReader reader(archive_path, nullptr, payload_range);
    auto plans = read_zip_entry_plans(reader);
    std::vector<ArchiveEntry> result;
    result.reserve(plans.size());
    for (auto& plan : plans) {
        result.push_back(std::move(plan.entry));
    }
    return result;
}

void sfx_zip_test(const std::filesystem::path& archive_path,
                  const DecompressionOptions& options,
                  const SfxZipPayloadRange& payload_range) {
    ZipReader reader(archive_path, options.operation, payload_range);
    auto plans = read_zip_entry_plans(reader);
    const auto capabilities = zip_capabilities_for_plans(plans, !options.password.empty());
    if (!capabilities.test) {
        throw FormatError("ZIP archive uses encryption or compression methods not supported yet");
    }

    std::uint64_t total_bytes = 0;
    std::uint64_t total_items = 0;
    for (const auto& plan : plans) {
        if (!plan.entry.is_directory) {
            total_bytes += plan.entry.size;
        }
        ++total_items;
    }
    std::uint64_t completed_bytes = 0;
    std::uint64_t completed_items = 0;
    report_operation(options.operation, OperationStage::testing, completed_bytes,
                     total_bytes, completed_items, total_items);

    for (const auto& plan : plans) {
        operation_checkpoint(options.operation);
        if (plan.entry.is_directory) {
            ++completed_items;
            report_operation(options.operation, OperationStage::testing, completed_bytes,
                             total_bytes, completed_items, total_items, plan.entry.path);
            continue;
        }

        ZipExtractCallbackContext context;
        context.stage = OperationStage::testing;
        context.operation = options.operation;
        context.completed_bytes = &completed_bytes;
        context.total_bytes = total_bytes;
        context.completed_items = completed_items;
        context.total_items = total_items;
        context.current_path = plan.entry.path;
        context.current_file_total = plan.entry.size;
        extract_zip_entry(reader, plan, options.password, context,
                          "ZIP integrity test failed");
        ++completed_items;
        report_operation(options.operation, OperationStage::testing, completed_bytes,
                         total_bytes, completed_items, total_items, plan.entry.path);
    }
}

void sfx_zip_extract_all(const std::filesystem::path& archive_path,
                         const std::filesystem::path& dest_dir,
                         const ExtractOptions& options,
                         const SfxZipPayloadRange& payload_range) {
    ZipArchiveProvider::extract_matching(archive_path, {}, dest_dir, options,
                                         payload_range);
}

void sfx_zip_extract_selected(const std::filesystem::path& archive_path,
                              const std::vector<std::string>& entries,
                              const std::filesystem::path& dest_dir,
                              const ExtractOptions& options,
                              const SfxZipPayloadRange& payload_range) {
    std::vector<std::string> normalized;
    normalized.reserve(entries.size());
    for (auto entry : entries) {
        normalized.push_back(normalize_archive_path(std::move(entry),
                                                    "ZIP selected entry"));
    }
    ZipArchiveProvider::extract_matching(archive_path, normalized, dest_dir, options,
                                         payload_range);
}

const ArchiveProvider& zip_archive_provider() {
    static const ZipArchiveProvider provider;
    return provider;
}

}  // namespace axiom
