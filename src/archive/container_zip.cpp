// ZIP archive support: miniz-backed read/write, ZipCrypto and WinZip AES-256
// entry encryption, atomic archive rewrites, and the built-in ZIP
// ArchiveProvider. Extracted from container.cpp; the AXAR engine stays there.

#include "axiom/archive.hpp"

#include "archive/container_internal.hpp"
#include "core/checksum.hpp"
#include "core/crypto.hpp"
#include "core/file_meta.hpp"
#include "third_party/miniz/miniz.h"

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

std::string miniz_error(const mz_zip_archive& zip, std::string_view action) {
    std::string message(action);
    message += ": ";
    message += mz_zip_get_error_string(zip.m_last_error);
    return message;
}

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
        throw std::runtime_error("ZIP file is too large: " + path.string());
    }
    return static_cast<std::uint64_t>(size);
}

class ZipReader final {
public:
    explicit ZipReader(const fs::path& path)
        : path_(path), stream_(path, std::ios::binary), size_(checked_file_size(path)) {
        if (!stream_) {
            throw std::runtime_error("cannot open ZIP archive: " + path.string());
        }
        mz_zip_zero_struct(&zip_);
        zip_.m_pRead = &ZipReader::read_callback;
        zip_.m_pIO_opaque = this;
        if (!mz_zip_reader_init(&zip_, size_, 0)) {
            const auto message = miniz_error(zip_, "cannot open ZIP archive");
            mz_zip_reader_end(&zip_);
            throw FormatError(message);
        }
        open_ = true;
    }

    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    ~ZipReader() {
        if (open_) {
            mz_zip_reader_end(&zip_);
        }
    }

    mz_zip_archive& zip() { return zip_; }
    const mz_zip_archive& zip() const { return zip_; }
    const fs::path& path() const { return path_; }
    std::uint64_t size() const { return size_; }

    ByteVector read_bytes(std::uint64_t file_ofs, std::size_t count) {
        ByteVector bytes(count);
        if (count == 0) return bytes;
        const size_t read = read_callback(this, file_ofs, bytes.data(), count);
        if (read != count) {
            throw FormatError("ZIP archive is truncated");
        }
        return bytes;
    }

private:
    static size_t read_callback(void* opaque, mz_uint64 file_ofs, void* buffer, size_t count) {
        auto* self = static_cast<ZipReader*>(opaque);
        if (file_ofs >= self->size_) {
            return 0;
        }
        const auto remaining = self->size_ - file_ofs;
        const auto to_read = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, count));
        self->stream_.clear();
        self->stream_.seekg(static_cast<std::streamoff>(file_ofs), std::ios::beg);
        if (!self->stream_) {
            return 0;
        }
        self->stream_.read(static_cast<char*>(buffer), to_read);
        return static_cast<size_t>(self->stream_.gcount());
    }

    fs::path path_;
    std::ifstream stream_;
    std::uint64_t size_ = 0;
    mz_zip_archive zip_{};
    bool open_ = false;
};

class ZipWriter final {
public:
    explicit ZipWriter(const fs::path& path)
        : stream_(path, std::ios::binary | std::ios::trunc) {
        if (!stream_) {
            throw std::runtime_error("cannot create ZIP archive: " + path.string());
        }
        mz_zip_zero_struct(&zip_);
        zip_.m_pWrite = &ZipWriter::write_callback;
        zip_.m_pIO_opaque = this;
        if (!mz_zip_writer_init_v2(&zip_, 0, MZ_ZIP_FLAG_WRITE_ZIP64)) {
            const auto message = miniz_error(zip_, "cannot initialize ZIP writer");
            mz_zip_writer_end(&zip_);
            throw std::runtime_error(message);
        }
        open_ = true;
    }

    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    ~ZipWriter() {
        if (open_) {
            mz_zip_writer_end(&zip_);
        }
    }

    mz_zip_archive& zip() { return zip_; }
    const mz_zip_archive& zip() const { return zip_; }

    void finalize() {
        if (!mz_zip_writer_finalize_archive(&zip_)) {
            throw std::runtime_error(miniz_error(zip_, "cannot finalize ZIP archive"));
        }
        if (!stream_) {
            throw std::runtime_error("failed writing ZIP archive");
        }
    }

private:
    static size_t write_callback(void* opaque, mz_uint64 file_ofs,
                                 const void* buffer, size_t count) {
        auto* self = static_cast<ZipWriter*>(opaque);
        self->stream_.clear();
        self->stream_.seekp(static_cast<std::streamoff>(file_ofs), std::ios::beg);
        if (!self->stream_) {
            return 0;
        }
        self->stream_.write(static_cast<const char*>(buffer),
                            static_cast<std::streamsize>(count));
        return self->stream_ ? count : 0;
    }

    std::ofstream stream_;
    mz_zip_archive zip_{};
    bool open_ = false;
};

struct ZipEntryPlan {
    mz_uint index = 0;
    ArchiveEntry entry;
    mz_uint16 method = 0;
    mz_uint16 bit_flag = 0;
    mz_uint64 local_header_ofs = 0;
    mz_uint64 central_dir_ofs = 0;
    mz_uint64 compressed_size = 0;
    std::string comment;
    bool supported = false;
    bool encrypted = false;
    bool zipcrypto_supported = false;
    bool aes_supported = false;
    mz_uint16 aes_version = 0;
    mz_uint16 aes_actual_method = 0;
    std::uint8_t aes_strength = 0;
};

constexpr std::uint32_t kZipLocalHeaderSignature = 0x04034b50u;
constexpr std::uint32_t kZipCentralHeaderSignature = 0x02014b50u;
constexpr std::uint32_t kZipEndOfCentralDirSignature = 0x06054b50u;
constexpr std::uint16_t kZipFlagEncrypted = 0x0001u;
constexpr std::uint16_t kZipFlagDataDescriptor = 0x0008u;
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

std::uint32_t read_le32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) throw FormatError("ZIP structure is truncated");
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void write_le16(ByteVector& bytes, std::size_t offset, std::uint16_t value) {
    if (offset + 2 > bytes.size()) throw FormatError("ZIP structure is truncated");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void write_le32(ByteVector& bytes, std::size_t offset, std::uint32_t value) {
    if (offset + 4 > bytes.size()) throw FormatError("ZIP structure is truncated");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
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

    std::uint8_t encrypt(std::uint8_t value) {
        const std::uint8_t cipher = static_cast<std::uint8_t>(value ^ crypt_byte());
        update_keys(value);
        return cipher;
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

void zipcrypto_transform(ByteVector& bytes, std::string_view password, bool encrypt) {
    ZipCrypto crypto(password);
    for (auto& byte : bytes) {
        byte = encrypt ? crypto.encrypt(byte) : crypto.decrypt(byte);
    }
}

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

ByteVector read_zip_file_bytes(const fs::path& path);
std::size_t find_zip_eocd(std::span<const std::uint8_t> bytes);

ByteVector read_zip_central_extra(ZipReader& reader,
                                  const mz_zip_archive_file_stat& stat) {
    if (stat.m_central_dir_ofs + 46 <= reader.size()) {
        const ByteVector header = reader.read_bytes(stat.m_central_dir_ofs, 46);
        if (read_le32(header, 0) == kZipCentralHeaderSignature) {
            const std::uint16_t name_size = read_le16(header, 28);
            const std::uint16_t extra_size = read_le16(header, 30);
            return reader.read_bytes(stat.m_central_dir_ofs + 46u + name_size, extra_size);
        }
    }

    const ByteVector bytes = read_zip_file_bytes(reader.path());
    const std::size_t eocd_offset = find_zip_eocd(bytes);
    const std::uint32_t cd_size32 = read_le32(bytes, eocd_offset + 12);
    const std::uint32_t cd_offset32 = read_le32(bytes, eocd_offset + 16);
    if (cd_size32 == 0xffffffffu || cd_offset32 == 0xffffffffu) {
        throw FormatError("ZIP64 AES central-directory parsing is not supported yet");
    }
    const std::size_t cd_start = cd_offset32;
    const std::size_t cd_end = cd_start + cd_size32;
    if (cd_start > eocd_offset || cd_end > eocd_offset) {
        throw FormatError("ZIP central directory is invalid");
    }

    std::size_t pos = cd_start;
    while (pos < cd_end) {
        if (pos + 46 > cd_end ||
            read_le32(bytes, pos) != kZipCentralHeaderSignature) {
            throw FormatError("ZIP central directory is invalid");
        }
        const std::uint16_t name_size = read_le16(bytes, pos + 28);
        const std::uint16_t extra_size = read_le16(bytes, pos + 30);
        const std::uint16_t comment_size = read_le16(bytes, pos + 32);
        const std::uint32_t local_offset = read_le32(bytes, pos + 42);
        const std::size_t extra_offset = pos + 46u + name_size;
        const std::size_t entry_end = extra_offset + extra_size + comment_size;
        if (entry_end > cd_end) {
            throw FormatError("ZIP central directory is truncated");
        }
        if (local_offset == stat.m_local_header_ofs) {
            return ByteVector(bytes.begin() + static_cast<std::ptrdiff_t>(extra_offset),
                              bytes.begin() + static_cast<std::ptrdiff_t>(extra_offset + extra_size));
        }
        pos = entry_end;
    }
    throw FormatError("ZIP central directory entry was not found");
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

std::array<std::uint8_t, 20> hmac_sha1(std::span<const std::uint8_t> key,
                                       std::span<const std::uint8_t> message) {
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
    std::array<std::uint8_t, 64> opad{};
    for (std::size_t i = 0; i < key_block.size(); ++i) {
        ipad[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x36u);
        opad[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x5cu);
    }

    Sha1 inner;
    inner.update(ipad);
    inner.update(message);
    const auto inner_digest = inner.final();

    Sha1 outer;
    outer.update(opad);
    outer.update(inner_digest);
    return outer.final();
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

void aes256_ctr_transform(std::span<std::uint8_t> data,
                          std::span<const std::uint8_t, 32> key) {
    Aes256 aes(key);
    std::array<std::uint8_t, 16> counter{};
    std::array<std::uint8_t, 16> stream{};
    std::uint64_t block_index = 1;
    std::size_t offset = 0;
    while (offset < data.size()) {
        for (std::size_t i = 0; i < 8; ++i) {
            counter[i] = static_cast<std::uint8_t>(block_index >> (i * 8));
        }
        aes.encrypt_block(counter.data(), stream.data());
        const std::size_t take = std::min<std::size_t>(16, data.size() - offset);
        for (std::size_t i = 0; i < take; ++i) {
            data[offset + i] ^= stream[i];
        }
        offset += take;
        ++block_index;
    }
}

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

ArchiveEntry archive_entry_from_zip_stat(const mz_zip_archive_file_stat& stat) {
    ArchiveEntry entry;
    entry.path = normalize_zip_entry_path(stat.m_filename);
    entry.is_directory = stat.m_is_directory != 0;
    entry.size = entry.is_directory ? 0 : static_cast<std::uint64_t>(stat.m_uncomp_size);
    entry.packed_size = entry.is_directory
        ? std::optional<std::uint64_t>{}
        : std::optional<std::uint64_t>{static_cast<std::uint64_t>(stat.m_comp_size)};
    entry.crc32 = entry.is_directory ? 0 : static_cast<std::uint32_t>(stat.m_crc32);
    entry.has_crc32 = !entry.is_directory;
#ifndef MINIZ_NO_TIME
    entry.mtime = static_cast<std::int64_t>(stat.m_time);
#endif
    return entry;
}

std::vector<ZipEntryPlan> read_zip_entry_plans(ZipReader& reader) {
    mz_zip_archive& zip = reader.zip();
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    std::vector<ZipEntryPlan> result;
    result.reserve(count);
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat)) {
            throw FormatError(miniz_error(zip, "cannot read ZIP directory"));
        }
        ZipEntryPlan plan;
        plan.index = index;
        plan.entry = archive_entry_from_zip_stat(stat);
        plan.method = stat.m_method;
        plan.bit_flag = stat.m_bit_flag;
        plan.local_header_ofs = stat.m_local_header_ofs;
        plan.central_dir_ofs = stat.m_central_dir_ofs;
        plan.compressed_size = stat.m_comp_size;
        plan.comment.assign(stat.m_comment,
                            stat.m_comment + std::min<std::uint32_t>(
                                stat.m_comment_size,
                                MZ_ZIP_MAX_ARCHIVE_FILE_COMMENT_SIZE - 1));
        plan.supported = stat.m_is_supported != 0;
        plan.encrypted = stat.m_is_encrypted != 0;
        if (plan.encrypted && plan.method == kZipMethodAes) {
            const auto aes = parse_zip_aes_extra(read_zip_central_extra(reader, stat));
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

std::size_t checked_zip_offset(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw FormatError("ZIP archive is too large for this operation");
    }
    return static_cast<std::size_t>(value);
}

void append_zip_range(ByteVector& out,
                      const ByteVector& input,
                      std::size_t begin,
                      std::size_t end) {
    if (begin > end || end > input.size()) {
        throw FormatError("ZIP archive is truncated");
    }
    out.insert(out.end(),
               input.begin() + static_cast<std::ptrdiff_t>(begin),
               input.begin() + static_cast<std::ptrdiff_t>(end));
}

ByteVector read_zip_file_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read ZIP archive: " + path.string());
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < std::streamoff(0)) {
        throw std::runtime_error("cannot determine ZIP archive size: " + path.string());
    }
    ByteVector bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
            throw FormatError("ZIP archive is truncated");
        }
    }
    return bytes;
}

void write_zip_file_bytes(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write ZIP archive: " + path.string());
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    if (!output) throw std::runtime_error("cannot finish ZIP archive: " + path.string());
}

std::size_t find_zip_eocd(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 22) throw FormatError("ZIP end of central directory is missing");
    const std::size_t search_begin =
        bytes.size() > 65557 ? bytes.size() - 65557 : 0;
    for (std::size_t pos = bytes.size() - 22; pos + 1 > search_begin; --pos) {
        if (read_le32(bytes, pos) == kZipEndOfCentralDirSignature) {
            const std::uint16_t comment_size = read_le16(bytes, pos + 20);
            if (pos + 22u + comment_size == bytes.size()) {
                return pos;
            }
        }
        if (pos == 0) break;
    }
    throw FormatError("ZIP end of central directory is missing");
}

struct ZipLocalHeaderInfo {
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint16_t mod_time = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint16_t name_size = 0;
    std::uint16_t extra_size = 0;
    std::size_t data_offset = 0;
};

ZipLocalHeaderInfo read_zip_local_header(std::span<const std::uint8_t> bytes,
                                         std::size_t offset) {
    if (offset + 30 > bytes.size() ||
        read_le32(bytes, offset) != kZipLocalHeaderSignature) {
        throw FormatError("ZIP local header is invalid");
    }
    ZipLocalHeaderInfo info;
    info.flags = read_le16(bytes, offset + 6);
    info.method = read_le16(bytes, offset + 8);
    info.mod_time = read_le16(bytes, offset + 10);
    info.crc32 = read_le32(bytes, offset + 14);
    info.compressed_size = read_le32(bytes, offset + 18);
    info.uncompressed_size = read_le32(bytes, offset + 22);
    info.name_size = read_le16(bytes, offset + 26);
    info.extra_size = read_le16(bytes, offset + 28);
    info.data_offset = offset + 30u + info.name_size + info.extra_size;
    if (info.data_offset > bytes.size()) {
        throw FormatError("ZIP local header is truncated");
    }
    return info;
}

struct ZipRewriteInfo {
    std::uint64_t old_local_offset = 0;
    std::uint64_t new_local_offset = 0;
    std::uint64_t old_compressed_size = 0;
    std::uint64_t new_compressed_size = 0;
    std::uint16_t method = 0;
    bool encrypted = false;
};

void encrypt_zip_archive_in_place(const fs::path& path, const std::string& password) {
    if (password.empty()) return;

    ByteVector input = read_zip_file_bytes(path);
    const std::size_t eocd_offset = find_zip_eocd(input);
    const std::uint16_t disk_entries = read_le16(input, eocd_offset + 8);
    const std::uint16_t total_entries = read_le16(input, eocd_offset + 10);
    const std::uint32_t old_cd_size32 = read_le32(input, eocd_offset + 12);
    const std::uint32_t old_cd_offset32 = read_le32(input, eocd_offset + 16);
    if (disk_entries == 0xffffu || total_entries == 0xffffu ||
        old_cd_size32 == 0xffffffffu || old_cd_offset32 == 0xffffffffu) {
        throw std::runtime_error("encrypted ZIP writing does not support ZIP64 yet");
    }
    const std::size_t old_cd_start = old_cd_offset32;
    const std::size_t old_cd_size = old_cd_size32;
    if (old_cd_start > eocd_offset || old_cd_size > eocd_offset - old_cd_start) {
        throw FormatError("ZIP central directory is invalid");
    }

    std::vector<ZipEntryPlan> plans;
    {
        ZipReader reader(path);
        plans = read_zip_entry_plans(reader);
    }
    std::sort(plans.begin(), plans.end(), [](const auto& left, const auto& right) {
        return left.local_header_ofs < right.local_header_ofs;
    });

    ByteVector output;
    output.reserve(input.size() + plans.size() *
        (kZipAes256Overhead + zip_aes_extra_field(kZipMethodDeflate).size() * 2));
    std::size_t cursor = 0;
    std::unordered_map<std::uint64_t, ZipRewriteInfo> rewrites;
    rewrites.reserve(plans.size());

    for (std::size_t plan_index = 0; plan_index < plans.size(); ++plan_index) {
        const auto& plan = plans[plan_index];
        if (plan.encrypted) {
            throw std::runtime_error("cannot re-encrypt an already encrypted ZIP archive");
        }
        if (plan.method != kZipMethodStore && plan.method != kZipMethodDeflate) {
            throw std::runtime_error("ZIP AES-256 encryption supports only stored and deflated entries");
        }
        const std::size_t local_offset = checked_zip_offset(plan.local_header_ofs);
        if (local_offset < cursor || local_offset >= old_cd_start) {
            throw FormatError("ZIP local headers are not ordered as expected");
        }
        const std::size_t old_record_end = plan_index + 1 < plans.size()
            ? checked_zip_offset(plans[plan_index + 1].local_header_ofs)
            : old_cd_start;
        if (old_record_end < local_offset || old_record_end > old_cd_start) {
            throw FormatError("ZIP local record is invalid");
        }
        append_zip_range(output, input, cursor, local_offset);

        ZipLocalHeaderInfo local = read_zip_local_header(input, local_offset);
        if (((local.flags & kZipFlagDataDescriptor) == 0 &&
             (local.compressed_size == 0xffffffffu ||
              local.uncompressed_size == 0xffffffffu)) ||
            plan.compressed_size > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("encrypted ZIP writing does not support ZIP64 yet");
        }
        if (plan.entry.size > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("encrypted ZIP writing does not support ZIP64 yet");
        }
        if (local.method != plan.method ||
            ((local.flags & kZipFlagDataDescriptor) == 0 &&
             static_cast<std::uint64_t>(local.compressed_size) != plan.compressed_size)) {
            throw FormatError("ZIP local and central directory entries disagree");
        }

        const bool encrypt_entry = !plan.entry.is_directory;
        ZipRewriteInfo rewrite;
        rewrite.old_local_offset = plan.local_header_ofs;
        rewrite.new_local_offset = output.size();
        rewrite.old_compressed_size = plan.compressed_size;
        rewrite.new_compressed_size = plan.compressed_size +
            (encrypt_entry ? kZipAes256Overhead : 0);
        rewrite.method = plan.method;
        rewrite.encrypted = encrypt_entry;
        if (rewrite.new_local_offset > std::numeric_limits<std::uint32_t>::max() ||
            rewrite.new_compressed_size > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("ZIP AES-256 writing does not support ZIP64 yet");
        }
        rewrites.emplace(plan.local_header_ofs, rewrite);

        if (!encrypt_entry) {
            append_zip_range(output, input, local_offset, old_record_end);
            cursor = old_record_end;
            continue;
        }

        ByteVector local_header(input.begin() + static_cast<std::ptrdiff_t>(local_offset),
                                input.begin() + static_cast<std::ptrdiff_t>(local.data_offset));
        const ByteVector aes_extra = zip_aes_extra_field(plan.method);
        if (local.extra_size >
            std::numeric_limits<std::uint16_t>::max() - aes_extra.size()) {
            throw std::runtime_error("ZIP local extra field is too large for AES metadata");
        }
        write_le16(local_header, 6, static_cast<std::uint16_t>(
            (local.flags | kZipFlagEncrypted) & ~kZipFlagDataDescriptor));
        write_le16(local_header, 8, kZipMethodAes);
        write_le32(local_header, 14, 0);
        write_le32(local_header, 18, static_cast<std::uint32_t>(rewrite.new_compressed_size));
        write_le32(local_header, 22, static_cast<std::uint32_t>(plan.entry.size));
        write_le16(local_header, 28,
                   static_cast<std::uint16_t>(local.extra_size + aes_extra.size()));
        local_header.insert(local_header.end(), aes_extra.begin(), aes_extra.end());
        output.insert(output.end(), local_header.begin(), local_header.end());

        const std::size_t data_end = local.data_offset + checked_zip_offset(plan.compressed_size);
        if (data_end > old_record_end || data_end > input.size()) {
            throw FormatError("ZIP compressed payload is truncated");
        }
        std::array<std::uint8_t, kZipAes256SaltSize> salt{};
        core::random_bytes(salt);
        ZipAesKeyMaterial keys = zip_aes256_key_material(password, salt);
        ByteVector ciphertext(input.begin() + static_cast<std::ptrdiff_t>(local.data_offset),
                              input.begin() + static_cast<std::ptrdiff_t>(data_end));
        aes256_ctr_transform(ciphertext, keys.encryption_key);
        const auto auth = hmac_sha1(keys.authentication_key, ciphertext);

        output.insert(output.end(), salt.begin(), salt.end());
        output.insert(output.end(), keys.password_verifier.begin(),
                      keys.password_verifier.end());
        output.insert(output.end(), ciphertext.begin(), ciphertext.end());
        output.insert(output.end(), auth.begin(),
                      auth.begin() + static_cast<std::ptrdiff_t>(kZipAesAuthCodeSize));
        cursor = old_record_end;
    }
    append_zip_range(output, input, cursor, old_cd_start);

    const std::size_t new_cd_start = output.size();
    if (new_cd_start > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("encrypted ZIP writing does not support ZIP64 yet");
    }

    const ByteVector central(input.begin() + static_cast<std::ptrdiff_t>(old_cd_start),
                             input.begin() + static_cast<std::ptrdiff_t>(old_cd_start + old_cd_size));
    ByteVector central_out;
    central_out.reserve(central.size() +
                        rewrites.size() * zip_aes_extra_field(kZipMethodDeflate).size());
    std::size_t pos = 0;
    while (pos < central.size()) {
        if (pos + 46 > central.size() ||
            read_le32(central, pos) != kZipCentralHeaderSignature) {
            throw FormatError("ZIP central directory is invalid");
        }
        const std::uint16_t name_size = read_le16(central, pos + 28);
        const std::uint16_t extra_size = read_le16(central, pos + 30);
        const std::uint16_t comment_size = read_le16(central, pos + 32);
        const std::uint32_t old_local_offset = read_le32(central, pos + 42);
        const auto found = rewrites.find(old_local_offset);
        if (found == rewrites.end()) {
            throw FormatError("ZIP central directory references an unknown local header");
        }
        const ZipRewriteInfo& rewrite = found->second;
        const std::size_t entry_end =
            pos + 46u + name_size + extra_size + comment_size;
        if (entry_end > central.size()) {
            throw FormatError("ZIP central directory is truncated");
        }
        ByteVector entry(central.begin() + static_cast<std::ptrdiff_t>(pos),
                         central.begin() + static_cast<std::ptrdiff_t>(entry_end));
        write_le32(entry, 42, static_cast<std::uint32_t>(rewrite.new_local_offset));
        if (rewrite.encrypted) {
            const ByteVector aes_extra = zip_aes_extra_field(rewrite.method);
            if (extra_size >
                std::numeric_limits<std::uint16_t>::max() - aes_extra.size()) {
                throw std::runtime_error("ZIP central extra field is too large for AES metadata");
            }
            const std::uint16_t flags = read_le16(entry, 8);
            write_le16(entry, 8, static_cast<std::uint16_t>(
                (flags | kZipFlagEncrypted) & ~kZipFlagDataDescriptor));
            write_le16(entry, 10, kZipMethodAes);
            write_le32(entry, 16, 0);
            write_le32(entry, 20, static_cast<std::uint32_t>(rewrite.new_compressed_size));
            write_le16(entry, 30,
                       static_cast<std::uint16_t>(extra_size + aes_extra.size()));
            const std::size_t insert_pos = 46u + name_size + extra_size;
            entry.insert(entry.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                         aes_extra.begin(), aes_extra.end());
        }
        central_out.insert(central_out.end(), entry.begin(), entry.end());
        pos = entry_end;
    }

    if (central_out.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("ZIP AES-256 writing does not support ZIP64 yet");
    }
    output.insert(output.end(), central_out.begin(), central_out.end());

    ByteVector tail(input.begin() + static_cast<std::ptrdiff_t>(eocd_offset),
                    input.end());
    write_le32(tail, 12, static_cast<std::uint32_t>(central_out.size()));
    write_le32(tail, 16, static_cast<std::uint32_t>(new_cd_start));
    output.insert(output.end(), tail.begin(), tail.end());

    fs::path encrypted_path = path;
    encrypted_path += ".enc";
    TempFileGuard encrypted_guard(encrypted_path);
    write_zip_file_bytes(encrypted_path, output);
    replace_archive_file(encrypted_path, path);
    encrypted_guard.dismiss();
}

ArchiveCapabilities zip_capabilities_for_plans(const std::vector<ZipEntryPlan>& entries,
                                               bool has_password) {
    ArchiveCapabilities result;
    result.list = true;
    result.create = true;
    result.encryption = true;
    result.metadata = true;
    result.packed_sizes = true;

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
    if (options.force_store) {
        return MZ_NO_COMPRESSION;
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

struct ZipFileReadContext {
    std::ifstream input;
    std::shared_ptr<OperationControl> operation;
    std::uint64_t* completed_bytes = nullptr;
    std::uint64_t total_bytes = 0;
    std::uint64_t completed_items = 0;
    std::uint64_t total_items = 0;
    std::uint64_t reported_until = 0;
    std::string current_path;
    bool cancelled = false;
    bool failed = false;
    std::string error;
};

size_t zip_file_read_callback(void* opaque, mz_uint64 file_ofs, void* buffer, size_t count) {
    auto* context = static_cast<ZipFileReadContext*>(opaque);
    try {
        operation_checkpoint(context->operation);
        context->input.clear();
        context->input.seekg(static_cast<std::streamoff>(file_ofs), std::ios::beg);
        if (!context->input) {
            throw std::runtime_error("failed seeking ZIP input: " + context->current_path);
        }
        context->input.read(static_cast<char*>(buffer), static_cast<std::streamsize>(count));
        const auto read = static_cast<size_t>(context->input.gcount());
        const std::uint64_t end_offset = file_ofs + read;
        if (context->completed_bytes != nullptr && end_offset > context->reported_until) {
            *context->completed_bytes += end_offset - context->reported_until;
            context->reported_until = end_offset;
            report_operation(context->operation, OperationStage::compressing,
                             *context->completed_bytes, context->total_bytes,
                             context->completed_items, context->total_items,
                             context->current_path);
        }
        return read;
    } catch (const OperationCancelled&) {
        context->cancelled = true;
    } catch (const std::exception& error) {
        context->failed = true;
        context->error = error.what();
    } catch (...) {
        context->failed = true;
        context->error = "unknown ZIP input read error";
    }
    return 0;
}

void throw_zip_file_read_failure(const ZipFileReadContext& context,
                                 const mz_zip_archive& zip,
                                 std::string_view action) {
    if (context.cancelled) {
        throw OperationCancelled();
    }
    if (context.failed) {
        throw std::runtime_error(context.error);
    }
    throw std::runtime_error(miniz_error(zip, action));
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
                       std::uint64_t total_items) {
    operation_checkpoint(options.operation);
    const std::string archive_path = zip_writer_path(item);
    MZ_TIME_T modified = static_cast<MZ_TIME_T>(scan_item_mtime(item));
    if (item.is_directory) {
        const char empty = 0;
        if (!mz_zip_writer_add_mem_ex_v2(&writer.zip(), archive_path.c_str(), &empty, 0,
                                         nullptr, 0, MZ_NO_COMPRESSION, 0, 0,
                                         &modified, nullptr, 0, nullptr, 0)) {
            throw std::runtime_error(miniz_error(writer.zip(), "cannot add ZIP directory"));
        }
        ++completed_items;
        report_operation(options.operation, OperationStage::compressing,
                         completed_bytes, total_bytes, completed_items, total_items,
                         item.archive_path);
        return;
    }
    if (item.is_symlink) {
        throw std::runtime_error("ZIP writing does not support symbolic links yet: " +
                                 item.archive_path);
    }

    ZipFileReadContext context;
    context.input.open(item.absolute, std::ios::binary);
    if (!context.input) {
        throw std::runtime_error("cannot read ZIP input: " + item.absolute.string());
    }
    context.operation = options.operation;
    context.completed_bytes = &completed_bytes;
    context.total_bytes = total_bytes;
    context.completed_items = completed_items;
    context.total_items = total_items;
    context.current_path = item.archive_path;
    const auto size = static_cast<mz_uint64>(fs::file_size(item.absolute));
    if (!mz_zip_writer_add_read_buf_callback(
            &writer.zip(), archive_path.c_str(), &zip_file_read_callback,
            &context, size, &modified, nullptr, 0,
            static_cast<mz_uint>(zip_compression_level(options)),
            nullptr, 0, nullptr, 0)) {
        throw_zip_file_read_failure(context, writer.zip(), "cannot add ZIP file");
    }
    if (context.reported_until < size) {
        completed_bytes += size - context.reported_until;
    }
    ++completed_items;
    report_operation(options.operation, OperationStage::compressing,
                     completed_bytes, total_bytes, completed_items, total_items,
                     item.archive_path);
}

template <typename KeepExisting>
void rebuild_zip_archive(const fs::path& archive_path,
                         const std::vector<ScanItem>& additions,
                         const CompressionOptions& options,
                         KeepExisting keep_existing,
                         bool preserve_existing = true) {
    reject_zip_write_options(options);
    const auto replacements = zip_replacement_paths(additions);
    const bool existing_archive = preserve_existing && fs::exists(archive_path);
    std::uint64_t total_bytes = scanned_file_bytes(additions);
    std::uint64_t total_items = additions.size();

    fs::path temp_path = archive_path;
    temp_path += ".tmp";
    TempFileGuard temp_guard(temp_path);

    if (existing_archive) {
        ZipReader reader(archive_path);
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
            ZipWriter writer(temp_path);
            for (const auto& plan : plans) {
                operation_checkpoint(options.operation);
                if (!keep_existing(plan) ||
                    replacements.find(plan.entry.path) != replacements.end()) {
                    continue;
                }
                if (!mz_zip_writer_add_from_zip_reader(&writer.zip(), &reader.zip(),
                                                       plan.index)) {
                    throw std::runtime_error(miniz_error(
                        writer.zip(), "cannot preserve existing ZIP entry"));
                }
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
                                  completed_items, total_items);
            }
            report_operation(options.operation, OperationStage::finalizing,
                             completed_bytes, total_bytes, completed_items, total_items);
            writer.finalize();
        }
    } else {
        validate_zip_items(additions, {}, options.operation);
        std::uint64_t completed_bytes = 0;
        std::uint64_t completed_items = 0;
        report_operation(options.operation, OperationStage::compressing,
                         completed_bytes, total_bytes, completed_items, total_items);
        {
            ZipWriter writer(temp_path);
            for (const auto& item : additions) {
                add_zip_scan_item(writer, item, options, completed_bytes, total_bytes,
                                  completed_items, total_items);
            }
            report_operation(options.operation, OperationStage::finalizing,
                             completed_bytes, total_bytes, completed_items, total_items);
            writer.finalize();
        }
    }

    encrypt_zip_archive_in_place(temp_path, options.password);
    replace_archive_file(temp_path, archive_path);
    temp_guard.dismiss();
    report_operation(options.operation, OperationStage::finalizing,
                     total_bytes, total_bytes, total_items, total_items);
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
    if (plan.encrypted || !plan.supported) {
        throw FormatError("cannot move encrypted or unsupported ZIP entries yet: " +
                          plan.entry.path);
    }

    const std::string archive_path = zip_writer_path(destination_path, plan.entry.is_directory);
    MZ_TIME_T modified = static_cast<MZ_TIME_T>(plan.entry.mtime);
    const void* comment = plan.comment.empty() ? nullptr : plan.comment.data();
    const auto comment_size = static_cast<mz_uint16>(
        std::min<std::size_t>(plan.comment.size(), std::numeric_limits<mz_uint16>::max()));

    if (plan.entry.is_directory) {
        const char empty = 0;
        if (!mz_zip_writer_add_mem_ex_v2(&writer.zip(), archive_path.c_str(), &empty, 0,
                                         comment, comment_size, MZ_NO_COMPRESSION, 0, 0,
                                         &modified, nullptr, 0, nullptr, 0)) {
            throw std::runtime_error(miniz_error(writer.zip(), "cannot move ZIP directory"));
        }
        ++completed_items;
        report_operation(options.operation, OperationStage::compressing,
                         completed_bytes, total_bytes, completed_items, total_items,
                         destination_path);
        return;
    }

    if (plan.method != 0 && plan.method != MZ_DEFLATED) {
        throw FormatError("cannot move unsupported ZIP compression method: " +
                          plan.entry.path);
    }
    size_t payload_size = 0;
    mz_uint writer_flags = static_cast<mz_uint>(zip_compression_level(options));
    if (plan.method == 0) {
        writer_flags = MZ_NO_COMPRESSION;
    }

    void* payload = mz_zip_reader_extract_to_heap(&reader.zip(), plan.index,
                                                  &payload_size, 0);
    if (payload == nullptr && payload_size != 0) {
        throw std::runtime_error(miniz_error(reader.zip(), "cannot read moved ZIP entry"));
    }
    std::unique_ptr<void, decltype(&mz_free)> payload_guard(payload, &mz_free);

    if (!mz_zip_writer_add_mem_ex_v2(
            &writer.zip(), archive_path.c_str(), payload, payload_size,
            comment, comment_size, writer_flags,
            0, 0,
            &modified, nullptr, 0, nullptr, 0)) {
        throw std::runtime_error(miniz_error(writer.zip(), "cannot write moved ZIP entry"));
    }

    completed_bytes += plan.entry.size;
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
        ZipReader reader(archive_path);
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
            ZipWriter writer(temp_path);
            for (const auto& plan : plans) {
                operation_checkpoint(options.operation);
                const std::string destination = moved_path(plan.entry.path);
                if (destination != plan.entry.path) {
                    continue;
                }
                if (!mz_zip_writer_add_from_zip_reader(&writer.zip(), &reader.zip(),
                                                       plan.index)) {
                    throw std::runtime_error(miniz_error(
                        writer.zip(), "cannot preserve existing ZIP entry"));
                }
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
    bool cancelled = false;
    bool failed = false;
    std::string error;
};

size_t zip_extract_write_callback(void* opaque, mz_uint64 file_ofs, const void* buffer,
                                  size_t count) {
    auto* context = static_cast<ZipExtractCallbackContext*>(opaque);
    try {
        operation_checkpoint(context->operation);
        if (context->output != nullptr) {
            context->output->seekp(static_cast<std::streamoff>(file_ofs), std::ios::beg);
            context->output->write(static_cast<const char*>(buffer),
                                   static_cast<std::streamsize>(count));
            if (!*context->output) {
                throw std::runtime_error("failed writing file: " + context->current_path);
            }
        }
        if (context->completed_bytes != nullptr) {
            *context->completed_bytes += count;
            report_operation(context->operation, context->stage,
                             *context->completed_bytes, context->total_bytes,
                             context->completed_items, context->total_items,
                             context->current_path);
        }
        return count;
    } catch (const OperationCancelled&) {
        context->cancelled = true;
    } catch (const std::exception& error) {
        context->failed = true;
        context->error = error.what();
    } catch (...) {
        context->failed = true;
        context->error = "unknown ZIP extraction error";
    }
    return 0;
}

void throw_zip_callback_failure(const ZipExtractCallbackContext& context,
                                const mz_zip_archive& zip, std::string_view action) {
    if (context.cancelled) {
        throw OperationCancelled();
    }
    if (context.failed) {
        throw std::runtime_error(context.error);
    }
    throw FormatError(miniz_error(zip, action));
}

ByteVector read_zip_file_range(const fs::path& path, std::uint64_t offset, std::uint64_t size) {
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw FormatError("ZIP entry is too large");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read ZIP archive: " + path.string());
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    ByteVector bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
            throw FormatError("ZIP entry payload is truncated");
        }
    }
    return bytes;
}

ZipLocalHeaderInfo read_zip_local_header_from_file(const fs::path& path,
                                                   std::uint64_t offset) {
    ByteVector header = read_zip_file_range(path, offset, 30);
    if (read_le32(header, 0) != kZipLocalHeaderSignature) {
        throw FormatError("ZIP local header is invalid");
    }
    ZipLocalHeaderInfo info;
    info.flags = read_le16(header, 6);
    info.method = read_le16(header, 8);
    info.mod_time = read_le16(header, 10);
    info.crc32 = read_le32(header, 14);
    info.compressed_size = read_le32(header, 18);
    info.uncompressed_size = read_le32(header, 22);
    info.name_size = read_le16(header, 26);
    info.extra_size = read_le16(header, 28);
    const std::uint64_t data_offset = offset + 30u + info.name_size + info.extra_size;
    info.data_offset = checked_zip_offset(data_offset);
    return info;
}

struct ZipCryptoInflateContext {
    ZipExtractCallbackContext* extract = nullptr;
    std::uint64_t output_offset = 0;
    std::uint32_t crc = core::crc32_init();
};

bool zipcrypto_emit_plain(ZipCryptoInflateContext& context,
                          const void* data,
                          std::size_t size) {
    if (size == 0) return true;
    const auto* first = static_cast<const std::uint8_t*>(data);
    context.crc = core::crc32_update(context.crc, std::span<const std::uint8_t>(first, size));
    const size_t written = zip_extract_write_callback(
        context.extract, context.output_offset, data, size);
    if (written != size) return false;
    context.output_offset += size;
    return true;
}

int zipcrypto_tinfl_put_buf(const void* data, int len, void* user) {
    auto* context = static_cast<ZipCryptoInflateContext*>(user);
    if (len < 0) return 0;
    return zipcrypto_emit_plain(*context, data, static_cast<std::size_t>(len)) ? 1 : 0;
}

void throw_zipcrypto_context_failure(const ZipExtractCallbackContext& context,
                                     std::string_view action) {
    if (context.cancelled) {
        throw OperationCancelled();
    }
    if (context.failed) {
        throw std::runtime_error(context.error);
    }
    throw FormatError(std::string(action));
}

void extract_zipcrypto_entry(const fs::path& archive_path,
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

    const ZipLocalHeaderInfo local =
        read_zip_local_header_from_file(archive_path, plan.local_header_ofs);
    ByteVector encrypted = read_zip_file_range(
        archive_path, local.data_offset, plan.compressed_size);
    zipcrypto_transform(encrypted, password, false);

    const std::uint8_t expected_check = (plan.bit_flag & kZipFlagDataDescriptor) != 0
        ? static_cast<std::uint8_t>(local.mod_time >> 8)
        : static_cast<std::uint8_t>(plan.entry.crc32 >> 24);
    if (encrypted.size() < kZipEncryptionHeaderSize ||
        encrypted[kZipEncryptionHeaderSize - 1] != expected_check) {
        throw std::runtime_error("wrong password for encrypted ZIP archive");
    }

    const auto* compressed = encrypted.data() + kZipEncryptionHeaderSize;
    const std::size_t compressed_size = encrypted.size() - kZipEncryptionHeaderSize;

    ZipCryptoInflateContext inflate_context;
    inflate_context.extract = &context;
    if (plan.method == kZipMethodStore) {
        if (!zipcrypto_emit_plain(inflate_context, compressed, compressed_size)) {
            throw_zipcrypto_context_failure(context, "ZIP extraction failed");
        }
    } else if (plan.method == kZipMethodDeflate) {
        std::size_t input_size = compressed_size;
        const int ok = tinfl_decompress_mem_to_callback(
            compressed, &input_size, &zipcrypto_tinfl_put_buf, &inflate_context, 0);
        if (!ok || input_size != compressed_size) {
            throw_zipcrypto_context_failure(context, "ZIP decompression failed");
        }
    } else {
        throw FormatError("ZIP entry uses unsupported compression method: " + plan.entry.path);
    }

    if (inflate_context.output_offset != plan.entry.size) {
        throw FormatError("ZIP entry decompressed to an unexpected size: " + plan.entry.path);
    }
    if (core::crc32_final(inflate_context.crc) != plan.entry.crc32) {
        throw FormatError("ZIP entry CRC check failed: " + plan.entry.path);
    }
}

void extract_zip_aes_entry(const fs::path& archive_path,
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

    const ZipLocalHeaderInfo local =
        read_zip_local_header_from_file(archive_path, plan.local_header_ofs);
    ByteVector encrypted = read_zip_file_range(
        archive_path, local.data_offset, plan.compressed_size);
    const auto salt = std::span<const std::uint8_t>(
        encrypted.data(), kZipAes256SaltSize);
    const auto verifier = std::span<const std::uint8_t>(
        encrypted.data() + kZipAes256SaltSize, kZipAesPasswordVerifierSize);
    const std::size_t ciphertext_size =
        encrypted.size() - kZipAes256SaltSize - kZipAesPasswordVerifierSize -
        kZipAesAuthCodeSize;
    auto ciphertext = std::span<std::uint8_t>(
        encrypted.data() + kZipAes256SaltSize + kZipAesPasswordVerifierSize,
        ciphertext_size);
    const auto stored_auth = std::span<const std::uint8_t>(
        encrypted.data() + encrypted.size() - kZipAesAuthCodeSize,
        kZipAesAuthCodeSize);

    ZipAesKeyMaterial keys = zip_aes256_key_material(password, salt);
    if (!constant_time_equal(keys.password_verifier, verifier)) {
        throw std::runtime_error("wrong password for encrypted ZIP archive");
    }
    const auto auth = hmac_sha1(keys.authentication_key, ciphertext);
    if (!constant_time_equal(
            std::span<const std::uint8_t>(auth.data(), kZipAesAuthCodeSize),
            stored_auth)) {
        throw FormatError("ZIP AES authentication failed: " + plan.entry.path);
    }
    aes256_ctr_transform(ciphertext, keys.encryption_key);

    ZipCryptoInflateContext inflate_context;
    inflate_context.extract = &context;
    const std::uint16_t method = zip_effective_method(plan);
    if (method == kZipMethodStore) {
        if (!zipcrypto_emit_plain(inflate_context, ciphertext.data(), ciphertext.size())) {
            throw_zipcrypto_context_failure(context, "ZIP extraction failed");
        }
    } else if (method == kZipMethodDeflate) {
        std::size_t input_size = ciphertext.size();
        const int ok = tinfl_decompress_mem_to_callback(
            ciphertext.data(), &input_size, &zipcrypto_tinfl_put_buf, &inflate_context, 0);
        if (!ok || input_size != ciphertext.size()) {
            throw_zipcrypto_context_failure(context, "ZIP decompression failed");
        }
    } else {
        throw FormatError("ZIP entry uses unsupported compression method: " + plan.entry.path);
    }

    if (inflate_context.output_offset != plan.entry.size) {
        throw FormatError("ZIP entry decompressed to an unexpected size: " + plan.entry.path);
    }
    if (plan.entry.has_crc32 &&
        core::crc32_final(inflate_context.crc) != plan.entry.crc32) {
        throw FormatError("ZIP entry CRC check failed: " + plan.entry.path);
    }
}

class ZipArchiveProvider final : public ArchiveProvider {
public:
    const ArchiveFormatInfo& info() const override {
        return kArchiveFormats[kZipFormatIndex];
    }

    bool matches_path(const std::filesystem::path& path) const override {
        return lower_ascii(path.extension().wstring()) == L".zip";
    }

    ArchiveCapabilities capabilities(const std::filesystem::path& archive_path,
                                     const std::string& password) const override {
        // A capability query must not throw: the path may be a new archive that
        // does not exist yet, or an unreadable file. Fall back to the format's
        // static capabilities (empty entry list) and let the actual operation
        // report the precise error.
        try {
            ZipReader reader(archive_path);
            return zip_capabilities_for_plans(read_zip_entry_plans(reader), !password.empty());
        } catch (...) {
            return zip_capabilities_for_plans({}, !password.empty());
        }
    }

    std::vector<ArchiveEntry> list(const std::filesystem::path& archive_path,
                                   const std::string&) const override {
        ZipReader reader(archive_path);
        auto plans = read_zip_entry_plans(reader);
        std::vector<ArchiveEntry> result;
        result.reserve(plans.size());
        for (auto& plan : plans) {
            result.push_back(std::move(plan.entry));
        }
        return result;
    }

    void test(const std::filesystem::path& archive_path,
              const DecompressionOptions& options) const override {
        ZipReader reader(archive_path);
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
            if (plan.encrypted) {
                if (plan.aes_supported) {
                    extract_zip_aes_entry(archive_path, plan, options.password, context);
                } else {
                    extract_zipcrypto_entry(archive_path, plan, options.password, context);
                }
            } else {
                if (!mz_zip_reader_extract_to_callback(&reader.zip(), plan.index,
                                                       &zip_extract_write_callback,
                                                       &context, 0)) {
                    throw_zip_callback_failure(context, reader.zip(),
                                               "ZIP integrity test failed");
                }
            }
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

    void add(const std::vector<std::filesystem::path>& inputs,
             const std::filesystem::path& archive_path,
             const CompressionOptions& options) const override {
        auto items = scan_zip_inputs(inputs, options.operation);
        rebuild_zip_archive(archive_path, items, options,
                            [](const ZipEntryPlan&) { return true; });
    }

    void add_mapped(const std::vector<ArchiveInput>& inputs,
                    const std::filesystem::path& archive_path,
                    const CompressionOptions& options) const override {
        auto items = scan_zip_inputs(inputs, options.operation);
        rebuild_zip_archive(archive_path, items, options,
                            [](const ZipEntryPlan&) { return true; });
    }

    void update(const std::vector<std::filesystem::path>& inputs,
                const std::filesystem::path& archive_path,
                const CompressionOptions& options,
                bool fresh_only) const override {
        auto items = scan_zip_inputs(inputs, options.operation);
        if (!fs::exists(archive_path)) {
            if (!fresh_only) {
                rebuild_zip_archive(archive_path, items, options,
                                    [](const ZipEntryPlan&) { return false; },
                                    false);
            }
            return;
        }

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

    void sync(const std::vector<std::filesystem::path>& inputs,
              const std::filesystem::path& archive_path,
              const CompressionOptions& options) const override {
        auto items = scan_zip_inputs(inputs, options.operation);
        if (!fs::exists(archive_path)) {
            rebuild_zip_archive(archive_path, items, options,
                                [](const ZipEntryPlan&) { return false; },
                                false);
            return;
        }

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

    void delete_entries(const std::filesystem::path& archive_path,
                        const std::vector<std::string>& paths,
                        const CompressionOptions& options) const override {
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
        move_zip_entries(archive_path, moves, options);
    }

private:
    void extract_matching(const std::filesystem::path& archive_path,
                          const std::vector<std::string>& wanted,
                          const std::filesystem::path& dest_dir,
                          const ExtractOptions& options) const {
        ZipReader reader(archive_path);
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
            const fs::path target = (dest_dir / fs::path(entry.path)).lexically_normal();
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
                    throw std::runtime_error("target already exists: " + target.string());
                }
                if (fs::is_directory(target, ec)) {
                    throw std::runtime_error("target is a directory: " + target.string());
                }
                fs::remove(target, ec);
            }

            fs::path temp_target = target;
            temp_target += ".axtmp";
            TempFileGuard temp_guard(temp_target);
            {
                std::ofstream output(temp_target, std::ios::binary | std::ios::trunc);
                if (!output) {
                    throw std::runtime_error("cannot write file: " + temp_target.string());
                }
                ZipExtractCallbackContext context;
                context.output = &output;
                context.operation = options.operation;
                context.completed_bytes = &completed_bytes;
                context.total_bytes = total_bytes;
                context.completed_items = completed_items;
                context.total_items = total_items;
                context.current_path = entry.path;
                if (plan.encrypted) {
                    if (plan.aes_supported) {
                        extract_zip_aes_entry(archive_path, plan, options.password, context);
                    } else {
                        extract_zipcrypto_entry(archive_path, plan, options.password, context);
                    }
                } else {
                    if (!mz_zip_reader_extract_to_callback(&reader.zip(), plan.index,
                                                           &zip_extract_write_callback,
                                                           &context, 0)) {
                        throw_zip_callback_failure(context, reader.zip(),
                                                   "ZIP extraction failed");
                    }
                }
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
};

}  // namespace

const ArchiveProvider& zip_archive_provider() {
    static const ZipArchiveProvider provider;
    return provider;
}

}  // namespace axiom
