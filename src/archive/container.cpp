#include "axiom/archive.hpp"

#include "archive/fuzz_support.hpp"
#include "core/checksum.hpp"
#include "core/crypto.hpp"
#include "core/file_meta.hpp"
#include "core/hash.hpp"
#include "core/reed_solomon.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace axiom {
namespace {

namespace fs = std::filesystem;

constexpr std::array<std::uint8_t, 8> kArchiveMagic = {'A', 'X', 'I', 'O', 'M', 'A', 'R', '\0'};
constexpr std::uint16_t kArchiveVersion = 4;
constexpr std::size_t kHeaderSize = 16;
// Header flag: the central directory is sealed and the encryption parameters live in
// a plaintext preamble after the header (rather than in the directory's TLV). Old
// readers reject this flag, so it doubles as the compatibility gate.
constexpr std::uint16_t kFlagEncryptedDirectory = 0x0001;
// AEAD associated data tag for a sealed directory (distinct from any block index).
constexpr std::array<std::uint8_t, 8> kDirectoryAd = {'A', 'X', 'D', 'I', 'R', 0, 0, 0};
constexpr std::size_t kFooterSize = 24;
constexpr std::array<std::uint8_t, 8> kSfxMagic = {'A', 'X', 'I', 'O', 'M', 'S', 'F', 'X'};
constexpr std::array<std::uint8_t, 8> kRecoveryMagic = {'A', 'X', 'I', 'O', 'M', 'R', 'R', 0};
constexpr std::array<std::uint8_t, 8> kVolumeMagic = {'A', 'X', 'I', 'O', 'M', 'V', 'L', 0};
constexpr std::uint16_t kRecoveryVersion = 1;
constexpr std::uint16_t kVolumeVersion = 1;
constexpr std::size_t kVolumeHeaderSize = 80;
constexpr std::size_t kRecoveryTailSize = 24;
constexpr std::uint8_t kEntryFile = 0;
constexpr std::uint8_t kEntryDir = 1;
constexpr std::uint8_t kEntrySymlink = 2;
constexpr std::uint8_t kEntryHardlink = 3;  // link_target = archive path of the shared file
constexpr std::size_t kFileChunk = 1u << 16;

// Per-entry "extra area" record types (TLV). The directory stores each entry as a
// length-prefixed record: a small typed core followed by zero or more of these
// optional records. New metadata (strong hashes, high-precision times, link
// targets, attributes, owner…) is added as new types here without touching the
// record layout — readers consume the types they know and skip the rest by length.
constexpr std::uint64_t kExtraMtime = 1;     // file mtime, i64 unix seconds (8-byte LE)
constexpr std::uint64_t kExtraCrc32 = 2;     // file CRC-32 (4-byte LE)
constexpr std::uint64_t kExtraBlake3 = 3;    // file BLAKE3-256 digest (32 bytes)
constexpr std::uint64_t kExtraWinAttrs = 4;  // Windows file attributes (u32 LE)
constexpr std::uint64_t kExtraWinTimes = 5;  // Windows creation/access/write FILETIMEs (3 × u64 LE)
constexpr std::uint64_t kExtraAdsStream = 6; // one NTFS named stream: vint name_len, name, bytes
constexpr std::uint64_t kExtraPosix = 7;     // mode, uid, gid (3 x u32 LE)

// Archive-level extra record types (the TLV area after the entry list). Separate
// numbering from the per-entry extras above; unknown types are skipped by length.
constexpr std::uint64_t kArchiveComment = 1;  // UTF-8 archive comment (payload = the text)
constexpr std::uint64_t kArchiveLock = 2;     // presence marks the archive read-only (no payload)
constexpr std::uint64_t kArchiveEncryption = 3;  // KDF params + salt + password key-check token
constexpr std::uint64_t kArchiveSignature = 4;   // public key (32) + EdDSA signature (64)

// ---- little-endian serialization -------------------------------------------

void put_u16(ByteVector& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_u32(ByteVector& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void put_u64(ByteVector& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

// LEB128 unsigned varint: 7 bits/byte, high bit = "more bytes follow". Compact for
// the small counts/sizes/offsets that dominate the directory, and length-agnostic
// so fields can widen later without a format change.
void put_vint(ByteVector& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value) | 0x80u);
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data, std::size_t cursor = 0)
        : data_(data), cursor_(cursor) {}

    std::uint16_t u16() {
        need(2);
        const auto value = static_cast<std::uint16_t>(data_[cursor_] |
                                                      (data_[cursor_ + 1] << 8));
        cursor_ += 2;
        return value;
    }

    std::uint32_t u32() {
        need(4);
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(data_[cursor_++]) << shift;
        }
        return value;
    }

    std::uint64_t u64() {
        need(8);
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value |= static_cast<std::uint64_t>(data_[cursor_++]) << shift;
        }
        return value;
    }

    std::uint64_t vint() {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            need(1);
            const auto byte = data_[cursor_++];
            value |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
            if ((byte & 0x80u) == 0) {
                return value;
            }
        }
        throw FormatError("archive varint is too long");
    }

    std::string str(std::size_t length) {
        need(length);
        std::string value(reinterpret_cast<const char*>(data_.data() + cursor_), length);
        cursor_ += length;
        return value;
    }

    // Borrow `length` bytes as a sub-span (e.g. an entry record body or a TLV
    // payload) and advance past them; the result must not outlive the backing data.
    std::span<const std::uint8_t> take(std::size_t length) {
        need(length);
        const auto sub = data_.subspan(cursor_, length);
        cursor_ += length;
        return sub;
    }

    bool has_more() const { return cursor_ < data_.size(); }

    std::size_t remaining() const { return data_.size() - cursor_; }

private:
    void need(std::size_t count) const {
        if (cursor_ + count > data_.size()) {
            throw FormatError("archive directory is truncated");
        }
    }

    std::span<const std::uint8_t> data_;
    std::size_t cursor_;
};

// ---- timestamps ------------------------------------------------------------

std::int64_t to_unix_seconds(fs::file_time_type stamp) {
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(stamp);
    return std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
}

fs::file_time_type from_unix_seconds(std::int64_t seconds) {
    const std::chrono::system_clock::time_point system{std::chrono::seconds{seconds}};
    return std::chrono::clock_cast<fs::file_time_type::clock>(system);
}

// ---- path safety -----------------------------------------------------------

bool is_safe_relative(const std::string& path) {
    if (path.empty()) {
        return false;
    }

    const fs::path candidate(path);
    if (candidate.is_absolute() || candidate.has_root_name() || candidate.has_root_directory()) {
        return false;
    }

    for (const auto& part : candidate) {
        if (part == "..") {
            return false;
        }
    }

    return true;
}

std::string normalize_archive_path(std::string path, const char* field_name) {
    if (path.empty() || path.find('\0') != std::string::npos) {
        throw std::invalid_argument(std::string(field_name) + " must not be empty");
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.front() == '/' || path.back() == '/' || path.find("//") != std::string::npos ||
        (path.size() >= 2 && path[1] == ':')) {
        throw std::invalid_argument(std::string(field_name) +
                                    " must be a normalized relative archive path");
    }
    std::size_t begin = 0;
    while (begin < path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string_view part(path.data() + begin,
                                    (end == std::string::npos ? path.size() : end) - begin);
        if (part.empty() || part == "." || part == "..") {
            throw std::invalid_argument(std::string(field_name) +
                                        " contains an invalid path component");
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    if (!is_safe_relative(path)) {
        throw std::invalid_argument(std::string(field_name) + " is not a safe relative path");
    }
    return path;
}

bool is_same_or_child(std::string_view candidate, std::string_view parent) {
    return candidate == parent ||
           (candidate.size() > parent.size() &&
            candidate.compare(0, parent.size(), parent) == 0 &&
            candidate[parent.size()] == '/');
}

std::string join_archive_path(std::string_view parent, std::string_view child) {
    if (child.empty()) {
        return std::string(parent);
    }
    std::string result(parent);
    result.push_back('/');
    result.append(child);
    return result;
}

bool is_within(const fs::path& base, const fs::path& target) {
    auto b = base.begin();
    auto t = target.begin();
    for (; b != base.end(); ++b, ++t) {
        if (t == target.end() || *b != *t) {
            return false;
        }
    }
    return true;
}

// Symlink-safe extraction: lexical containment (is_within) only proves a path *spells*
// no escape — a symlink among its real directory components could still redirect a
// write outside the destination. Before materializing any entry we require every
// existing component from the destination root down to the entry's parent to be a
// real directory, never a symlink. Combined with in-order extraction this also stops
// an archive that plants a symlink and then writes through it: the later entry's
// parent chain now contains that symlink and is rejected. `dest_norm` is trusted.
void reject_symlinked_ancestor(const fs::path& dest_norm, const fs::path& target) {
    std::vector<fs::path> chain;  // target's parent up to (excluding) dest_norm
    for (fs::path p = target.parent_path(); p != dest_norm; p = p.parent_path()) {
        chain.push_back(p);
        if (p == p.parent_path()) {
            break;  // hit a filesystem root without meeting dest (post-containment: unreachable)
        }
    }
    // Check outermost-first so the message names the shallowest offending link.
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (core::is_reparse_point(*it)) {
            throw FormatError("refusing to extract through a symlinked directory: " +
                              it->lexically_relative(dest_norm).generic_string());
        }
    }
}

// ---- archive model ---------------------------------------------------------

struct BlockRec {
    std::uint64_t compressed_offset = 0;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
};

struct EntryRec {
    std::string path;
    std::uint8_t type = kEntryFile;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::uint32_t crc = 0;
    std::uint64_t first_block = 0;
    std::uint64_t offset = 0;
    bool has_blake3 = false;
    core::Blake3Digest blake3{};
    core::FileMetadata meta;
    std::string link_target;  // symlink target (verbatim) for kEntrySymlink
    std::vector<core::AdsStream> ads;  // NTFS alternate data streams (kEntryFile)
};

// Password-encryption parameters recorded in the directory (present only when the
// archive is encrypted). The salt + cost parameters let a reader re-derive the key;
// the key-check token is a sealed constant used to reject a wrong password up front.
struct EncryptionInfo {
    bool enabled = false;
    bool encrypt_directory = false;  // directory sealed + params in the header preamble
    core::KdfParams kdf;
    std::vector<std::uint8_t> key_check;
};

// Archive-wide metadata stored in the directory's trailing TLV area.
struct ArchiveMeta {
    std::string comment;     // free-form UTF-8 comment (empty = none)
    bool locked = false;     // read-only: edit operations refuse to modify the archive
    EncryptionInfo encryption;
    bool has_signature = false;
    std::array<std::uint8_t, 32> signature_public_key{};
    std::array<std::uint8_t, 64> signature{};
};

// Fixed plaintext sealed under the archive key to verify a password without touching
// any block; its AEAD also binds the KDF salt as associated data.
constexpr std::array<std::uint8_t, 16> kKeyCheckPlaintext = {
    'a', 'x', 'i', 'o', 'm', '-', 'k', 'e', 'y', 'c', 'h', 'e', 'c', 'k', '0', '1'};

struct ArchiveIndex {
    std::vector<BlockRec> blocks;
    std::vector<EntryRec> entries;
    ArchiveMeta meta;
};

struct ScanItem {
    fs::path absolute;
    std::string archive_path;
    bool is_directory = false;
    bool is_symlink = false;
    std::string symlink_target{};  // verbatim target when is_symlink
};

void operation_checkpoint(const std::shared_ptr<OperationControl>& operation);

void scan_input(const fs::path& input, std::vector<ScanItem>& items) {
    std::error_code ec;

    const fs::path base = input.has_parent_path() ? input.parent_path() : fs::path(".");
    auto relative_path = [&](const fs::path& path) {
        auto rel = fs::relative(path, base, ec).generic_string();
        if (ec || rel.empty()) {
            rel = path.filename().generic_string();
        }
        return rel;
    };
    auto symlink_item = [&](const fs::path& path) {
        return ScanItem{path, relative_path(path), false, true,
                        fs::read_symlink(path, ec).generic_string()};
    };

    // A symlink given directly is archived as a symlink, not its target — check
    // before fs::exists (which follows links and rejects dangling ones).
    if (fs::is_symlink(fs::symlink_status(input, ec))) {
        items.push_back(symlink_item(input));
        return;
    }

    if (!fs::exists(input, ec)) {
        throw std::runtime_error("input does not exist: " + input.string());
    }

    if (fs::is_regular_file(input, ec)) {
        items.push_back({input, relative_path(input), false});
        return;
    }

    if (fs::is_directory(input, ec)) {
        items.push_back({input, relative_path(input), true});
        // recursive_directory_iterator does not descend into symlinked dirs by
        // default, so recording the symlink here cannot cause an infinite walk.
        for (fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec;
             it.increment(ec)) {
            const auto& entry = *it;
            if (entry.is_symlink(ec)) {
                items.push_back(symlink_item(entry.path()));
            } else if (entry.is_directory(ec)) {
                items.push_back({entry.path(), relative_path(entry.path()), true});
            } else if (entry.is_regular_file(ec)) {
                items.push_back({entry.path(), relative_path(entry.path()), false});
            }
        }
        return;
    }

    throw std::runtime_error("unsupported input type: " + input.string());
}

void scan_input_at(const ArchiveInput& input, std::vector<ScanItem>& items,
                   const std::shared_ptr<OperationControl>& operation) {
    const std::string destination =
        normalize_archive_path(input.destination_path, "archive destination");
    std::error_code ec;
    auto archive_path_for = [&](const fs::path& path) {
        if (path == input.source) {
            return destination;
        }
        const fs::path relative = path.lexically_relative(input.source);
        if (relative.empty() || relative.native().empty()) {
            throw std::runtime_error("cannot map input beneath archive destination: " +
                                     path.string());
        }
        return join_archive_path(destination, relative.generic_string());
    };
    auto add_symlink = [&](const fs::path& path) {
        const fs::path target = fs::read_symlink(path, ec);
        if (ec) {
            throw std::runtime_error("cannot read symbolic link: " + path.string());
        }
        items.push_back({path, archive_path_for(path), false, true, target.generic_string()});
    };

    const fs::file_status status = fs::symlink_status(input.source, ec);
    if (ec) {
        throw std::runtime_error("cannot inspect input: " + input.source.string());
    }
    if (fs::is_symlink(status)) {
        add_symlink(input.source);
        return;
    }
    if (!fs::exists(status)) {
        throw std::runtime_error("input does not exist: " + input.source.string());
    }
    if (fs::is_regular_file(status)) {
        items.push_back({input.source, destination, false});
        return;
    }
    if (!fs::is_directory(status)) {
        throw std::runtime_error("unsupported input type: " + input.source.string());
    }

    items.push_back({input.source, destination, true});
    for (fs::recursive_directory_iterator it(
             input.source, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end && !ec; it.increment(ec)) {
        operation_checkpoint(operation);
        const auto& entry = *it;
        if (entry.is_symlink(ec)) {
            add_symlink(entry.path());
        } else if (entry.is_directory(ec)) {
            items.push_back({entry.path(), archive_path_for(entry.path()), true});
        } else if (entry.is_regular_file(ec)) {
            items.push_back({entry.path(), archive_path_for(entry.path()), false});
        }
    }
    if (ec) {
        throw std::runtime_error("failed while scanning input: " + input.source.string());
    }
}

std::uint8_t scan_item_type(const ScanItem& item) {
    if (item.is_directory) {
        return kEntryDir;
    }
    return item.is_symlink ? kEntrySymlink : kEntryFile;
}

void validate_mapped_items(const std::vector<ScanItem>& items,
                           const ArchiveIndex& existing,
                           const std::shared_ptr<OperationControl>& operation) {
    std::unordered_map<std::string, std::uint8_t> incoming;
    incoming.reserve(items.size());
    for (const auto& item : items) {
        operation_checkpoint(operation);
        const auto [it, inserted] = incoming.emplace(item.archive_path, scan_item_type(item));
        if (!inserted) {
            throw std::invalid_argument("duplicate archive destination: " + item.archive_path);
        }
    }

    auto validate_tree = [](const auto& paths, const char* message) {
        for (const auto& [path, type] : paths) {
            if (type == kEntryDir) {
                continue;
            }
            const std::string prefix = path + "/";
            for (const auto& [other, ignored] : paths) {
                (void)ignored;
                if (other.size() > path.size() && other.compare(0, prefix.size(), prefix) == 0) {
                    throw std::invalid_argument(std::string(message) + path);
                }
            }
        }
    };
    validate_tree(incoming, "non-directory archive destination has children: ");

    std::unordered_map<std::string, std::uint8_t> current;
    current.reserve(existing.entries.size());
    for (const auto& entry : existing.entries) {
        current.emplace(entry.path, entry.type);
    }
    for (const auto& [path, type] : incoming) {
        operation_checkpoint(operation);
        if (const auto found = current.find(path);
            found != current.end() && found->second != type) {
            throw std::invalid_argument("archive destination changes entry type: " + path);
        }
        for (const auto& [old_path, old_type] : current) {
            if (old_path == path) {
                continue;
            }
            if (old_type != kEntryDir && is_same_or_child(path, old_path)) {
                throw std::invalid_argument("archive destination is beneath a non-directory: " +
                                            old_path);
            }
            if (type != kEntryDir && is_same_or_child(old_path, path)) {
                throw std::invalid_argument("non-directory archive destination has children: " +
                                            path);
            }
        }
    }
}

std::uint64_t scanned_file_bytes(const std::vector<ScanItem>& items) {
    std::uint64_t total = 0;
    for (const auto& item : items) {
        if (item.is_directory) {
            continue;
        }
        std::error_code ec;
        const auto size = fs::file_size(item.absolute, ec);
        if (!ec) {
            total += static_cast<std::uint64_t>(size);
        }
    }
    return total;
}

std::uint64_t archive_file_bytes(const ArchiveIndex& index) {
    std::uint64_t total = 0;
    for (const auto& entry : index.entries) {
        if (entry.type == kEntryFile) {
            total += entry.size;
        }
    }
    return total;
}

void report_operation(const std::shared_ptr<OperationControl>& operation,
                      OperationStage stage,
                      std::uint64_t completed_bytes,
                      std::uint64_t total_bytes,
                      std::uint64_t completed_items,
                      std::uint64_t total_items,
                      std::string current_path = {}) {
    if (operation) {
        operation->report(OperationProgress{
            stage,
            completed_bytes,
            total_bytes,
            completed_items,
            total_items,
            std::move(current_path),
        });
    }
}

void operation_checkpoint(const std::shared_ptr<OperationControl>& operation) {
    if (operation) {
        operation->checkpoint();
    }
}

class TempFileGuard {
public:
    explicit TempFileGuard(fs::path path) : path_(std::move(path)) {}
    ~TempFileGuard() {
        if (active_) {
            std::error_code ec;
            fs::remove(path_, ec);
        }
    }

    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;

    void dismiss() {
        active_ = false;
    }

private:
    fs::path path_;
    bool active_ = true;
};

// ---- reading ---------------------------------------------------------------

// Random-access byte source for an archive, backed by either a file (so large
// archives are read on demand, not loaded whole) or an in-memory span (used by
// tests and fuzzers). Every read is bounds-checked.
class ByteSource {
public:
    ByteSource(std::ifstream& stream, std::uint64_t size) : stream_(&stream), size_(size) {}
    explicit ByteSource(std::span<const std::uint8_t> data)
        : data_(data), size_(data.size()) {}

    std::uint64_t size() const { return size_; }

    ByteVector read(std::uint64_t offset, std::uint64_t length) const {
        if (length > size_ || offset > size_ - length) {
            throw FormatError("archive is truncated");
        }
        if (stream_ != nullptr) {
            stream_->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            ByteVector buffer(static_cast<std::size_t>(length));
            if (length > 0) {
                stream_->read(reinterpret_cast<char*>(buffer.data()),
                              static_cast<std::streamsize>(length));
                if (static_cast<std::uint64_t>(stream_->gcount()) != length) {
                    throw FormatError("archive is truncated");
                }
            }
            return buffer;
        }
        return ByteVector(data_.begin() + static_cast<std::ptrdiff_t>(offset),
                          data_.begin() + static_cast<std::ptrdiff_t>(offset + length));
    }

private:
    std::ifstream* stream_ = nullptr;
    std::span<const std::uint8_t> data_{};
    std::uint64_t size_ = 0;
};

std::ifstream open_archive(const fs::path& archive_path, std::uint64_t& file_size);

// Where an archive's fixed structure points: directory location, plus the header
// flags and (if the directory is encrypted) the plaintext preamble's extent.
struct ArchiveLayout {
    std::uint16_t flags = 0;
    std::uint64_t directory_offset = 0;
    std::uint64_t directory_size = 0;
    std::uint64_t preamble_offset = 0;  // start of the encryption preamble (if any)
    std::uint64_t preamble_size = 0;    // length of the preamble's parameter bytes
};

struct RecoveryService {
    unsigned percent = 0;
    std::uint16_t data_shards = 0;
    std::uint16_t parity_shards = 0;
    std::uint64_t shard_size = 0;
    std::uint64_t protected_size = 0;
    std::uint64_t directory_offset = 0;
    std::uint64_t directory_size = 0;
    std::uint64_t service_offset = 0;
    std::uint64_t service_size = 0;
    std::vector<std::uint32_t> checksums;
    ByteVector parity;
};

std::uint32_t recovery_crc(std::span<const std::uint8_t> bytes) {
    auto crc = core::crc32_init();
    crc = core::crc32_update(crc, bytes);
    return core::crc32_final(crc);
}

std::optional<RecoveryService> read_recovery_service(const ByteSource& source) {
    if (source.size() < kFooterSize + kRecoveryTailSize) return std::nullopt;
    const std::uint64_t tail_offset = source.size() - kFooterSize - kRecoveryTailSize;
    const auto tail = source.read(tail_offset, kRecoveryTailSize);
    if (!std::equal(kRecoveryMagic.begin(), kRecoveryMagic.end(), tail.begin() + 16)) {
        return std::nullopt;
    }
    Reader tail_reader(tail);
    RecoveryService service;
    service.service_offset = tail_reader.u64();
    service.service_size = tail_reader.u64();
    if (service.service_size > tail_offset ||
        service.service_offset != tail_offset - service.service_size) {
        throw FormatError("invalid recovery service location");
    }
    const auto body = source.read(service.service_offset, service.service_size);
    if (body.size() < 48 ||
        !std::equal(kRecoveryMagic.begin(), kRecoveryMagic.end(), body.begin())) {
        throw FormatError("invalid recovery service header");
    }
    Reader reader(body, kRecoveryMagic.size());
    if (reader.u16() != kRecoveryVersion) throw FormatError("unsupported recovery service version");
    service.percent = reader.u16();
    service.data_shards = reader.u16();
    service.parity_shards = reader.u16();
    service.shard_size = reader.u64();
    service.protected_size = reader.u64();
    service.directory_offset = reader.u64();
    service.directory_size = reader.u64();
    if (service.percent == 0 || service.percent > 100 || service.data_shards == 0 ||
        service.parity_shards == 0 ||
        service.data_shards + service.parity_shards > 255 || service.shard_size == 0 ||
        service.protected_size > service.service_offset ||
        service.directory_offset + service.directory_size > service.protected_size) {
        throw FormatError("invalid recovery service parameters");
    }
    const std::size_t checksum_count =
        static_cast<std::size_t>(service.data_shards + service.parity_shards);
    service.checksums.reserve(checksum_count);
    for (std::size_t i = 0; i < checksum_count; ++i) service.checksums.push_back(reader.u32());
    const std::uint64_t parity_bytes =
        static_cast<std::uint64_t>(service.parity_shards) * service.shard_size;
    if (parity_bytes > reader.remaining()) throw FormatError("recovery service is truncated");
    const auto parity = reader.take(static_cast<std::size_t>(parity_bytes));
    service.parity.assign(parity.begin(), parity.end());
    if (reader.has_more()) throw FormatError("recovery service has trailing data");
    return service;
}

ArchiveLayout read_layout(const ByteSource& source) {
    const auto file_size = source.size();
    if (file_size < kHeaderSize + kFooterSize) {
        throw FormatError("archive is smaller than its fixed structure");
    }
    const auto header = source.read(0, kHeaderSize);
    if (!std::equal(kArchiveMagic.begin(), kArchiveMagic.end(), header.begin())) {
        throw FormatError("not an Axiom archive");
    }
    Reader header_reader(header, kArchiveMagic.size());
    if (header_reader.u16() != kArchiveVersion) {
        throw FormatError("unsupported archive version");
    }
    ArchiveLayout layout;
    layout.flags = header_reader.u16();
    if ((layout.flags & ~kFlagEncryptedDirectory) != 0) {
        throw FormatError("archive uses features this build does not support");
    }

    const auto footer = source.read(file_size - kFooterSize, kFooterSize);
    if (!std::equal(kArchiveMagic.begin(), kArchiveMagic.end(), footer.begin() + 16)) {
        throw FormatError("invalid archive footer");
    }
    Reader footer_reader(footer);
    layout.directory_offset = footer_reader.u64();
    layout.directory_size = footer_reader.u64();
    if (layout.directory_offset < kHeaderSize ||
        layout.directory_offset + layout.directory_size > file_size - kFooterSize) {
        throw FormatError("invalid directory location");
    }
    if ((layout.flags & kFlagEncryptedDirectory) != 0) {
        // The preamble is a fixed u32 length right after the header, then that many
        // plaintext parameter bytes, before the (encrypted) block region.
        const auto len_bytes = source.read(kHeaderSize, 4);
        Reader len_reader(len_bytes);
        layout.preamble_size = len_reader.u32();
        layout.preamble_offset = kHeaderSize + 4;
        if (layout.preamble_offset + layout.preamble_size > layout.directory_offset) {
            throw FormatError("invalid encryption preamble");
        }
    }
    return layout;
}

ByteVector archive_footer_bytes(std::uint64_t directory_offset,
                                std::uint64_t directory_size) {
    ByteVector footer;
    put_u64(footer, directory_offset);
    put_u64(footer, directory_size);
    footer.insert(footer.end(), kArchiveMagic.begin(), kArchiveMagic.end());
    return footer;
}

struct EncodedRecoveryService {
    ByteVector body;
    ByteVector tail;
};

EncodedRecoveryService encode_recovery_service(
    const ByteSource& source, std::uint64_t protected_size,
    std::uint64_t directory_offset, std::uint64_t directory_size,
    unsigned percent, const std::shared_ptr<OperationControl>& operation) {
    if (percent < 1 || percent > 100) {
        throw std::invalid_argument("recovery percentage must be between 1 and 100");
    }
    constexpr std::uint64_t target_shard_size = 1u << 20;
    const std::uint64_t desired_data = std::max<std::uint64_t>(
        1, (protected_size + target_shard_size - 1) / target_shard_size);
    const std::uint64_t max_data = std::max<std::uint64_t>(
        1, (255u * 100u) / (100u + percent));
    const auto data_count = static_cast<int>(std::min(desired_data, max_data));
    const auto parity_count = static_cast<int>(std::max<std::uint64_t>(
        1, std::min<std::uint64_t>(255 - data_count,
            (static_cast<std::uint64_t>(data_count) * percent + 99) / 100)));
    const std::uint64_t shard_size =
        std::max<std::uint64_t>(1, (protected_size + data_count - 1) / data_count);
    if (shard_size > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("archive is too large for recovery processing");
    }

    std::vector<std::vector<std::uint8_t>> data(
        static_cast<std::size_t>(data_count),
        std::vector<std::uint8_t>(static_cast<std::size_t>(shard_size), 0));
    std::uint64_t completed = 0;
    for (int i = 0; i < data_count; ++i) {
        operation_checkpoint(operation);
        const std::uint64_t offset = static_cast<std::uint64_t>(i) * shard_size;
        const std::uint64_t count = offset < protected_size
            ? std::min(shard_size, protected_size - offset) : 0;
        if (count != 0) {
            auto bytes = source.read(offset, count);
            std::copy(bytes.begin(), bytes.end(), data[static_cast<std::size_t>(i)].begin());
            completed += count;
        }
        report_operation(operation, OperationStage::finalizing, completed, protected_size,
                         static_cast<std::uint64_t>(i + 1),
                         static_cast<std::uint64_t>(data_count + parity_count),
                         "Building recovery record");
    }

    std::vector<std::vector<std::uint8_t>> parity(
        static_cast<std::size_t>(parity_count),
        std::vector<std::uint8_t>(static_cast<std::size_t>(shard_size), 0));
    std::vector<std::span<const std::uint8_t>> data_spans;
    std::vector<std::span<std::uint8_t>> parity_spans;
    data_spans.reserve(data.size());
    parity_spans.reserve(parity.size());
    for (const auto& shard : data) data_spans.emplace_back(shard);
    for (auto& shard : parity) parity_spans.emplace_back(shard);
    core::ReedSolomon(data_count, parity_count).encode(data_spans, parity_spans);

    ByteVector body;
    body.insert(body.end(), kRecoveryMagic.begin(), kRecoveryMagic.end());
    put_u16(body, kRecoveryVersion);
    put_u16(body, static_cast<std::uint16_t>(percent));
    put_u16(body, static_cast<std::uint16_t>(data_count));
    put_u16(body, static_cast<std::uint16_t>(parity_count));
    put_u64(body, shard_size);
    put_u64(body, protected_size);
    put_u64(body, directory_offset);
    put_u64(body, directory_size);
    for (const auto& shard : data) put_u32(body, recovery_crc(shard));
    for (const auto& shard : parity) put_u32(body, recovery_crc(shard));
    for (const auto& shard : parity) body.insert(body.end(), shard.begin(), shard.end());

    ByteVector tail;
    put_u64(tail, protected_size);
    put_u64(tail, body.size());
    tail.insert(tail.end(), kRecoveryMagic.begin(), kRecoveryMagic.end());
    return {std::move(body), std::move(tail)};
}

void replace_archive_file(const fs::path& temporary, const fs::path& destination) {
    std::error_code error;
    fs::rename(temporary, destination, error);
    if (!error) return;
    fs::remove(destination, error);
    fs::rename(temporary, destination, error);
    if (error) throw std::runtime_error("failed to install archive: " + error.message());
}

void rewrite_recovery_service(const fs::path& archive_path, unsigned percent,
                              const std::shared_ptr<OperationControl>& operation) {
    std::uint64_t file_size = 0;
    auto input = open_archive(archive_path, file_size);
    const ByteSource source(input, file_size);
    const ArchiveLayout layout = read_layout(source);
    const std::uint64_t protected_size = layout.directory_offset + layout.directory_size;
    EncodedRecoveryService recovery;
    if (percent != 0) {
        recovery = encode_recovery_service(source, protected_size, layout.directory_offset,
                                           layout.directory_size, percent, operation);
    }
    const auto footer = archive_footer_bytes(layout.directory_offset, layout.directory_size);

    fs::path temporary = archive_path;
    temporary += L".recovery.tmp";
    TempFileGuard guard(temporary);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create recovery output");
    std::uint64_t copied = 0;
    while (copied < protected_size) {
        operation_checkpoint(operation);
        const auto count = std::min<std::uint64_t>(kFileChunk, protected_size - copied);
        const auto bytes = source.read(copied, count);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        copied += count;
    }
    if (percent != 0) {
        output.write(reinterpret_cast<const char*>(recovery.body.data()),
                     static_cast<std::streamsize>(recovery.body.size()));
        output.write(reinterpret_cast<const char*>(recovery.tail.data()),
                     static_cast<std::streamsize>(recovery.tail.size()));
    }
    output.write(reinterpret_cast<const char*>(footer.data()),
                 static_cast<std::streamsize>(footer.size()));
    output.close();
    if (!output) throw std::runtime_error("failed to finalize recovery output");
    input.close();
    replace_archive_file(temporary, archive_path);
    guard.dismiss();
}

// Parse the plaintext encryption parameters carried in the header preamble.
EncryptionInfo parse_encryption_preamble(const ByteVector& bytes) {
    EncryptionInfo enc;
    enc.enabled = true;
    enc.encrypt_directory = true;
    Reader r(bytes);
    enc.kdf.algorithm = static_cast<std::uint32_t>(r.vint());
    enc.kdf.mem_blocks = static_cast<std::uint32_t>(r.vint());
    enc.kdf.passes = static_cast<std::uint32_t>(r.vint());
    enc.kdf.lanes = static_cast<std::uint32_t>(r.vint());
    const auto salt_len = static_cast<std::size_t>(r.vint());
    const auto salt = r.take(salt_len);
    std::copy_n(salt.begin(), std::min(salt_len, enc.kdf.salt.size()), enc.kdf.salt.begin());
    const auto check_len = static_cast<std::size_t>(r.vint());
    const auto check = r.take(check_len);
    enc.key_check.assign(check.begin(), check.end());
    return enc;
}

// Parse a (decrypted) directory image into the block table, entries, and metadata.
ArchiveIndex parse_directory(const ByteVector& directory, std::uint64_t directory_offset) {
    Reader reader(directory);

    ArchiveIndex index;
    const auto block_count = reader.vint();
    index.blocks.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(block_count, 1u << 16)));
    for (std::uint64_t i = 0; i < block_count; ++i) {
        BlockRec block;
        block.compressed_offset = reader.vint();
        block.compressed_size = reader.vint();
        block.uncompressed_size = reader.vint();
        (void)reader.take(static_cast<std::size_t>(reader.vint()));  // reserved block extra
        if (block.compressed_offset < kHeaderSize ||
            block.compressed_size > directory_offset ||
            block.compressed_offset > directory_offset - block.compressed_size) {
            throw FormatError("block record points outside the archive");
        }
        // Reject an absurd declared block size before it can drive a huge
        // allocation when the block is later decoded.
        constexpr std::uint64_t kMaxBlockUncompressed = std::uint64_t{4} << 30;  // 4 GiB
        if (block.uncompressed_size > kMaxBlockUncompressed) {
            throw FormatError("block declares an implausible uncompressed size");
        }
        index.blocks.push_back(block);
    }

    const auto entry_count = reader.vint();
    index.entries.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(entry_count, 1u << 16)));
    for (std::uint64_t i = 0; i < entry_count; ++i) {
        // Each entry is a length-prefixed record: a typed core followed by TLV
        // extra records. Parsing inside the borrowed body means a malformed inner
        // length can never read past this entry, and unknown extra records are
        // skipped by their declared length.
        Reader body(reader.take(static_cast<std::size_t>(reader.vint())));
        EntryRec entry;
        entry.type = static_cast<std::uint8_t>(body.vint());
        if (entry.type != kEntryFile && entry.type != kEntryDir &&
            entry.type != kEntrySymlink && entry.type != kEntryHardlink) {
            throw FormatError("unknown archive entry type");
        }
        entry.path = body.str(static_cast<std::size_t>(body.vint()));
        if (entry.type == kEntryFile) {
            entry.size = body.vint();
            entry.first_block = body.vint();
            entry.offset = body.vint();
        } else if (entry.type == kEntrySymlink || entry.type == kEntryHardlink) {
            entry.link_target = body.str(static_cast<std::size_t>(body.vint()));
        }
        while (body.has_more()) {
            const auto record_type = body.vint();
            Reader payload(body.take(static_cast<std::size_t>(body.vint())));
            if (record_type == kExtraMtime) {
                entry.mtime = static_cast<std::int64_t>(payload.u64());
            } else if (record_type == kExtraCrc32) {
                entry.crc = payload.u32();
            } else if (record_type == kExtraBlake3) {
                const auto digest = payload.take(entry.blake3.size());
                std::copy(digest.begin(), digest.end(), entry.blake3.begin());
                entry.has_blake3 = true;
            } else if (record_type == kExtraWinAttrs) {
                entry.meta.has_windows_attributes = true;
                entry.meta.windows_attributes = payload.u32();
            } else if (record_type == kExtraWinTimes) {
                entry.meta.has_windows_times = true;
                entry.meta.windows_creation_time = payload.u64();
                entry.meta.windows_access_time = payload.u64();
                entry.meta.windows_write_time = payload.u64();
            } else if (record_type == kExtraAdsStream) {
                core::AdsStream stream;
                stream.name = payload.str(static_cast<std::size_t>(payload.vint()));
                const auto data = payload.take(payload.remaining());
                stream.data.assign(data.begin(), data.end());
                entry.ads.push_back(std::move(stream));
            } else if (record_type == kExtraPosix) {
                entry.meta.has_posix = true;
                entry.meta.posix_mode = payload.u32();
                entry.meta.posix_uid = payload.u32();
                entry.meta.posix_gid = payload.u32();
            }
            // Unknown extra records are intentionally skipped (consumed by length).
        }
        index.entries.push_back(std::move(entry));
    }

    // Archive-level extra records (comment, lock, …); unknown ones skipped by length.
    const auto archive_extra_count = reader.vint();
    for (std::uint64_t i = 0; i < archive_extra_count; ++i) {
        const auto record_type = reader.vint();
        Reader payload(reader.take(static_cast<std::size_t>(reader.vint())));
        if (record_type == kArchiveComment) {
            index.meta.comment = payload.str(payload.remaining());
        } else if (record_type == kArchiveLock) {
            index.meta.locked = true;
        } else if (record_type == kArchiveEncryption) {
            auto& enc = index.meta.encryption;
            enc.enabled = true;
            enc.kdf.algorithm = static_cast<std::uint32_t>(payload.vint());
            enc.kdf.mem_blocks = static_cast<std::uint32_t>(payload.vint());
            enc.kdf.passes = static_cast<std::uint32_t>(payload.vint());
            enc.kdf.lanes = static_cast<std::uint32_t>(payload.vint());
            const auto salt_len = static_cast<std::size_t>(payload.vint());
            const auto salt = payload.take(salt_len);
            std::copy_n(salt.begin(), std::min(salt_len, enc.kdf.salt.size()), enc.kdf.salt.begin());
            const auto check_len = static_cast<std::size_t>(payload.vint());
            const auto check = payload.take(check_len);
            enc.key_check.assign(check.begin(), check.end());
        } else if (record_type == kArchiveSignature) {
            if (payload.remaining() != index.meta.signature_public_key.size() +
                                           index.meta.signature.size()) {
                throw FormatError("invalid archive signature record");
            }
            const auto public_key = payload.take(index.meta.signature_public_key.size());
            std::copy(public_key.begin(), public_key.end(),
                      index.meta.signature_public_key.begin());
            const auto signature = payload.take(index.meta.signature.size());
            std::copy(signature.begin(), signature.end(), index.meta.signature.begin());
            index.meta.has_signature = true;
        }
    }

    return index;
}

// Read the directory of a non-directory-encrypted archive (plaintext directory).
// A directory-encrypted archive needs the password — callers use load_index instead.
ArchiveIndex read_index(const ByteSource& source) {
    const auto layout = read_layout(source);
    if ((layout.flags & kFlagEncryptedDirectory) != 0) {
        throw std::runtime_error("archive directory is encrypted; a password is required");
    }
    const auto directory = source.read(layout.directory_offset, layout.directory_size);
    return parse_directory(directory, layout.directory_offset);
}

// Decodes solid blocks on demand, caching the most recently used one so the many
// small files sharing a block are not decoded repeatedly.

// Associated data for a block's AEAD: its index, little-endian. Binding the index
// makes a sealed block valid only at its own position, defeating block reordering.
inline std::array<std::uint8_t, 8> block_associated_data(std::uint64_t block_index) {
    std::array<std::uint8_t, 8> ad{};
    for (int i = 0; i < 8; ++i) {
        ad[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((block_index >> (8 * i)) & 0xFFu);
    }
    return ad;
}

class BlockSource {
public:
    BlockSource(const ByteSource& source,
                const ArchiveIndex& index,
                std::size_t thread_count,
                std::shared_ptr<OperationControl> operation,
                std::optional<core::CryptoKey> key = std::nullopt)
        : source_(source),
          index_(index),
          thread_count_(thread_count),
          operation_(std::move(operation)),
          key_(std::move(key)) {}

    const ByteVector& block(std::uint64_t block_index) {
        if (block_index != cached_index_) {
            operation_checkpoint(operation_);
            if (block_index >= index_.blocks.size()) {
                throw FormatError("block index out of range");
            }
            const auto& record = index_.blocks[block_index];
            auto compressed = source_.read(record.compressed_offset, record.compressed_size);
            if (key_) {
                // Verify + decrypt before decompressing; the index is the AEAD's AD.
                std::vector<std::uint8_t> plaintext;
                if (!core::aead_open(*key_, compressed, block_associated_data(block_index),
                                     plaintext)) {
                    throw FormatError(
                        "block authentication failed (wrong password or corrupt archive)");
                }
                compressed = std::move(plaintext);
            }
            // Bound the decode to this block's declared size; the equality check
            // below then confirms the block produced exactly what the directory
            // promised.
            cached_ = decompress(compressed,
                                  DecompressionOptions{
                                      static_cast<std::size_t>(record.uncompressed_size),
                                      thread_count_,
                                      operation_,
                                  });
            if (cached_.size() != record.uncompressed_size) {
                throw FormatError("block expands to an unexpected size");
            }
            cached_index_ = block_index;
        }
        return cached_;
    }

private:
    const ByteSource& source_;
    const ArchiveIndex& index_;
    std::size_t thread_count_ = 0;
    std::shared_ptr<OperationControl> operation_;
    std::optional<core::CryptoKey> key_;
    std::uint64_t cached_index_ = std::numeric_limits<std::uint64_t>::max();
    ByteVector cached_;
};

void read_file_bytes(BlockSource& source,
                     std::size_t block_count,
                     const EntryRec& entry,
                     const std::shared_ptr<OperationControl>& operation,
                     const std::function<void(std::span<const std::uint8_t>)>& sink) {
    std::uint64_t remaining = entry.size;
    std::uint64_t block_index = entry.first_block;
    std::uint64_t within = entry.offset;

    while (remaining > 0) {
        operation_checkpoint(operation);
        if (block_index >= block_count) {
            throw FormatError("file extends past the last block");
        }
        const auto& bytes = source.block(block_index);
        if (within > bytes.size()) {
            throw FormatError("file offset lies past its block");
        }
        const auto available = static_cast<std::uint64_t>(bytes.size()) - within;
        const auto take = std::min<std::uint64_t>(available, remaining);
        sink(std::span<const std::uint8_t>(bytes.data() + within, static_cast<std::size_t>(take)));
        remaining -= take;
        within = 0;
        ++block_index;
    }
}

std::ifstream open_archive(const fs::path& archive_path, std::uint64_t& file_size) {
    std::ifstream stream(archive_path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open archive: " + archive_path.string());
    }
    stream.seekg(0, std::ios::end);
    file_size = static_cast<std::uint64_t>(stream.tellg());
    return stream;
}

// Compress a list of scanned items into solid blocks, appending to `out` and to the
// `blocks`/`entries` vectors and advancing `written`. New blocks are numbered from
// blocks.size(), so seeding `blocks`/`entries`/`written` with an existing archive's
// contents (and having pre-copied its block bytes into `out`) appends to it.
void compress_items_into(std::ofstream& out, std::uint64_t& written,
                         std::vector<BlockRec>& blocks, std::vector<EntryRec>& entries,
                         const std::vector<ScanItem>& items, const CompressionOptions& options,
                         std::size_t block_size,
                         const std::shared_ptr<OperationControl>& operation,
                         std::uint64_t total_bytes, std::uint64_t total_items,
                         std::uint64_t& completed_bytes, std::uint64_t& completed_items,
                         const core::CryptoKey* key = nullptr) {
    ByteVector buffer;
    std::uint64_t current_block = blocks.size();

    // Maps a file's on-disk identity to the archive path under which its bytes were
    // first stored; later paths sharing that identity become hardlink entries.
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, std::string> hardlinks;

    auto flush_block = [&](std::string current_path = {}) {
        if (buffer.empty()) {
            return;
        }
        report_operation(operation, OperationStage::compressing, completed_bytes, total_bytes,
                         completed_items, total_items, current_path);
        auto compressed = compress(buffer, options);
        if (key != nullptr) {
            // Seal the block, binding its index as associated data so blocks cannot
            // be reordered or transplanted between archives.
            const auto ad = block_associated_data(blocks.size());
            compressed = core::aead_seal(*key, compressed, ad);
        }
        operation_checkpoint(operation);
        blocks.push_back({written, static_cast<std::uint64_t>(compressed.size()),
                          static_cast<std::uint64_t>(buffer.size())});
        out.write(reinterpret_cast<const char*>(compressed.data()),
                  static_cast<std::streamsize>(compressed.size()));
        if (!out) {
            throw std::runtime_error("failed while writing archive blocks");
        }
        written += compressed.size();
        ++current_block;
        buffer.clear();
        report_operation(operation, OperationStage::writing, completed_bytes, total_bytes,
                         completed_items, total_items, std::move(current_path));
    };

    for (const auto& item : items) {
        operation_checkpoint(operation);
        if (item.archive_path.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("path too long to archive: " + item.archive_path);
        }

        EntryRec entry;
        entry.path = item.archive_path;
        entry.meta = core::capture_metadata(item.absolute);

        if (item.is_symlink) {
            entry.type = kEntrySymlink;
            entry.link_target = item.symlink_target;
            entries.push_back(std::move(entry));
            ++completed_items;
            report_operation(operation, OperationStage::reading, completed_bytes, total_bytes,
                             completed_items, total_items, item.archive_path);
            continue;
        }

        std::error_code ec;
        const auto stamp = fs::last_write_time(item.absolute, ec);
        if (!ec) {
            try {
                entry.mtime = to_unix_seconds(stamp);
            } catch (...) {
                entry.mtime = 0;
            }
        }
        if (item.is_directory) {
            entry.type = kEntryDir;
            entries.push_back(std::move(entry));
            ++completed_items;
            report_operation(operation, OperationStage::reading, completed_bytes, total_bytes,
                             completed_items, total_items, item.archive_path);
            continue;
        }

        // A regular file that shares its identity with one already stored is a hard
        // link: record a reference to the first path instead of duplicating bytes.
        if (auto id = core::hardlink_identity(item.absolute)) {
            const auto identity = std::make_tuple(id->volume, id->index_high, id->index_low);
            const auto found = hardlinks.find(identity);
            if (found != hardlinks.end()) {
                entry.type = kEntryHardlink;
                entry.link_target = found->second;
                entry.mtime = 0;          // shared with the canonical file's inode
                entry.meta = {};
                entries.push_back(std::move(entry));
                ++completed_items;
                report_operation(operation, OperationStage::reading, completed_bytes, total_bytes,
                                 completed_items, total_items, item.archive_path);
                continue;
            }
            hardlinks.emplace(identity, item.archive_path);  // canonical copy follows below
        }

        entry.type = kEntryFile;
        entry.first_block = current_block;
        entry.offset = buffer.size();

        std::ifstream in(item.absolute, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot read input file: " + item.absolute.string());
        }

        auto crc = core::crc32_init();
        core::Blake3 hasher;
        std::uint64_t total = 0;
        std::array<char, kFileChunk> chunk{};
        report_operation(operation, OperationStage::reading, completed_bytes, total_bytes,
                         completed_items, total_items, item.archive_path);
        while (in) {
            operation_checkpoint(operation);
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto got = in.gcount();
            if (got <= 0) {
                break;
            }
            const std::span<const std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(chunk.data()), static_cast<std::size_t>(got));
            crc = core::crc32_update(crc, bytes);
            hasher.update(bytes);
            total += static_cast<std::uint64_t>(got);
            completed_bytes += static_cast<std::uint64_t>(got);
            buffer.insert(buffer.end(), bytes.begin(), bytes.end());
            report_operation(operation, OperationStage::reading, completed_bytes, total_bytes,
                             completed_items, total_items, item.archive_path);
            if (buffer.size() >= block_size) {
                flush_block(item.archive_path);
            }
        }

        entry.size = total;
        entry.crc = core::crc32_final(crc);
        entry.blake3 = hasher.finalize();
        entry.has_blake3 = true;
        entry.ads = core::capture_ads(item.absolute);  // NTFS named streams (Win32 only)
        entries.push_back(std::move(entry));
        ++completed_items;
        report_operation(operation, OperationStage::reading, completed_bytes, total_bytes,
                         completed_items, total_items, item.archive_path);
    }
    flush_block();
}

// Build encryption parameters for a new archive: random salt, default Argon2id cost,
// a freshly derived key, and a key-check token (the fixed plaintext sealed under the
// key, with the salt as associated data). Returns the metadata and the live key.
std::pair<EncryptionInfo, core::CryptoKey> make_encryption(const std::string& password) {
    EncryptionInfo enc;
    enc.enabled = true;
    enc.kdf = core::KdfParams{};
    core::random_bytes(enc.kdf.salt);
    core::CryptoKey key = core::derive_key(password, enc.kdf);
    const std::span<const std::uint8_t> ad(enc.kdf.salt.data(), enc.kdf.salt.size());
    enc.key_check = core::aead_seal(key, kKeyCheckPlaintext, ad);
    return {std::move(enc), key};
}

// Serialize the plaintext encryption preamble (a fixed u32 length then the Argon2
// parameters, salt, and key-check) that precedes the blocks of a sealed-directory
// archive.
ByteVector serialize_encryption_preamble(const EncryptionInfo& enc) {
    ByteVector params;
    put_vint(params, enc.kdf.algorithm);
    put_vint(params, enc.kdf.mem_blocks);
    put_vint(params, enc.kdf.passes);
    put_vint(params, enc.kdf.lanes);
    put_vint(params, enc.kdf.salt.size());
    params.insert(params.end(), enc.kdf.salt.begin(), enc.kdf.salt.end());
    put_vint(params, enc.key_check.size());
    params.insert(params.end(), enc.key_check.begin(), enc.key_check.end());
    ByteVector out;
    put_u32(out, static_cast<std::uint32_t>(params.size()));
    out.insert(out.end(), params.begin(), params.end());
    return out;
}

// Re-derive an encrypted archive's key from the password and verify it against the
// stored key-check token. Throws on a missing or wrong password before any block is
// touched, so failures are fast and unambiguous.
core::CryptoKey derive_archive_key(const EncryptionInfo& enc, const std::string& password) {
    if (password.empty()) {
        throw std::runtime_error("archive is encrypted; a password is required");
    }
    // The KDF parameters come from the untrusted header; a hostile archive could ask
    // for terabytes of Argon2 memory to OOM whoever supplies a password. Bound them
    // (and enforce Argon2's own minimums) before allocating.
    constexpr std::uint32_t kMaxKdfMemBlocks = 1u << 21;  // 2 GiB of 1 KiB blocks
    constexpr std::uint32_t kMaxKdfPasses = 64;
    if (enc.kdf.lanes < 1 || enc.kdf.passes < 1 || enc.kdf.passes > kMaxKdfPasses ||
        enc.kdf.mem_blocks < 8 * enc.kdf.lanes || enc.kdf.mem_blocks > kMaxKdfMemBlocks) {
        throw std::runtime_error("encrypted archive has implausible KDF parameters");
    }
    core::CryptoKey key = core::derive_key(password, enc.kdf);
    const std::span<const std::uint8_t> ad(enc.kdf.salt.data(), enc.kdf.salt.size());
    std::vector<std::uint8_t> check;
    const bool ok = core::aead_open(key, enc.key_check, ad, check) &&
                    check.size() == kKeyCheckPlaintext.size() &&
                    std::equal(check.begin(), check.end(), kKeyCheckPlaintext.begin());
    if (!ok) {
        core::secure_wipe(key);
        throw std::runtime_error("wrong password for encrypted archive");
    }
    return key;
}

// An archive's directory plus, when encrypted, the derived key (for reading blocks).
struct LoadedArchive {
    ArchiveIndex index;
    std::optional<core::CryptoKey> key;
};

// Read an archive's directory, transparently handling encryption. For a
// directory-encrypted archive the preamble carries the parameters, the directory is
// decrypted before parsing, and a password is mandatory; for a block-only encrypted
// archive the directory is plaintext and the password is used only for the blocks.
LoadedArchive load_index(const ByteSource& source, const std::string& password) {
    const auto layout = read_layout(source);
    LoadedArchive loaded;
    if ((layout.flags & kFlagEncryptedDirectory) != 0) {
        const auto preamble = source.read(layout.preamble_offset, layout.preamble_size);
        const EncryptionInfo enc = parse_encryption_preamble(preamble);
        core::CryptoKey key = derive_archive_key(enc, password);
        const auto sealed = source.read(layout.directory_offset, layout.directory_size);
        ByteVector plain;
        const std::span<const std::uint8_t> ad(kDirectoryAd.data(), kDirectoryAd.size());
        if (!core::aead_open(key, sealed, ad, plain)) {
            core::secure_wipe(key);
            throw std::runtime_error(
                "archive directory failed to decrypt (wrong password or corrupt)");
        }
        loaded.index = parse_directory(plain, layout.directory_offset);
        loaded.index.meta.encryption = enc;
        loaded.key = key;
    } else {
        const auto directory = source.read(layout.directory_offset, layout.directory_size);
        loaded.index = parse_directory(directory, layout.directory_offset);
        // Block-only encryption keeps the directory plaintext, so list/comment work
        // without a password; the key is derived only when one is supplied (to read
        // blocks). Callers that need block data check for the key and demand it.
        if (loaded.index.meta.encryption.enabled && !password.empty()) {
            loaded.key = derive_archive_key(loaded.index.meta.encryption, password);
        }
    }
    return loaded;
}

core::Blake3Digest archive_signature_digest(const ByteSource& source,
                                            const ArchiveLayout& layout,
                                            const ArchiveIndex& index) {
    core::Blake3 hasher;
    constexpr std::array<std::uint8_t, 18> domain = {
        'A', 'X', 'I', 'O', 'M', '-', 'S', 'I', 'G', 'N', 'A', 'T', 'U', 'R', 'E', '-', '0', '1'};
    hasher.update(domain);

    // Cover the exact header, encryption preamble, and compressed/encrypted block
    // bytes. The directory is covered below as a canonical semantic manifest with
    // the signature record omitted, avoiding a self-referential byte stream.
    std::uint64_t offset = 0;
    while (offset < layout.directory_offset) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(kFileChunk, layout.directory_offset - offset));
        const auto chunk = source.read(offset, count);
        hasher.update(chunk);
        offset += count;
    }

    ByteVector manifest;
    put_vint(manifest, index.blocks.size());
    for (const auto& block : index.blocks) {
        put_u64(manifest, block.compressed_offset);
        put_u64(manifest, block.compressed_size);
        put_u64(manifest, block.uncompressed_size);
    }
    put_vint(manifest, index.entries.size());
    for (const auto& entry : index.entries) {
        put_vint(manifest, entry.type);
        put_vint(manifest, entry.path.size());
        manifest.insert(manifest.end(), entry.path.begin(), entry.path.end());
        put_u64(manifest, entry.size);
        put_u64(manifest, static_cast<std::uint64_t>(entry.mtime));
        put_u32(manifest, entry.crc);
        put_u64(manifest, entry.first_block);
        put_u64(manifest, entry.offset);
        manifest.push_back(entry.has_blake3 ? 1 : 0);
        if (entry.has_blake3) {
            manifest.insert(manifest.end(), entry.blake3.begin(), entry.blake3.end());
        }
        put_vint(manifest, entry.link_target.size());
        manifest.insert(manifest.end(), entry.link_target.begin(), entry.link_target.end());
        manifest.push_back(entry.meta.has_windows_attributes ? 1 : 0);
        put_u32(manifest, entry.meta.windows_attributes);
        manifest.push_back(entry.meta.has_windows_times ? 1 : 0);
        put_u64(manifest, entry.meta.windows_creation_time);
        put_u64(manifest, entry.meta.windows_access_time);
        put_u64(manifest, entry.meta.windows_write_time);
        manifest.push_back(entry.meta.has_posix ? 1 : 0);
        put_u32(manifest, entry.meta.posix_mode);
        put_u32(manifest, entry.meta.posix_uid);
        put_u32(manifest, entry.meta.posix_gid);
        put_vint(manifest, entry.ads.size());
        for (const auto& stream : entry.ads) {
            put_vint(manifest, stream.name.size());
            manifest.insert(manifest.end(), stream.name.begin(), stream.name.end());
            put_vint(manifest, stream.data.size());
            manifest.insert(manifest.end(), stream.data.begin(), stream.data.end());
        }
    }
    put_vint(manifest, index.meta.comment.size());
    manifest.insert(manifest.end(), index.meta.comment.begin(), index.meta.comment.end());
    manifest.push_back(index.meta.locked ? 1 : 0);
    manifest.push_back(index.meta.encryption.enabled ? 1 : 0);
    manifest.push_back(index.meta.encryption.encrypt_directory ? 1 : 0);
    if (index.meta.encryption.enabled) {
        const auto& kdf = index.meta.encryption.kdf;
        put_u32(manifest, kdf.algorithm);
        put_u32(manifest, kdf.mem_blocks);
        put_u32(manifest, kdf.passes);
        put_u32(manifest, kdf.lanes);
        manifest.insert(manifest.end(), kdf.salt.begin(), kdf.salt.end());
        put_vint(manifest, index.meta.encryption.key_check.size());
        manifest.insert(manifest.end(), index.meta.encryption.key_check.begin(),
                        index.meta.encryption.key_check.end());
    }
    hasher.update(manifest);
    return hasher.finalize();
}

// Serialize the central directory and footer for the given blocks/entries to `out`
// (already positioned at `written`) and close the stream. When `dir_key` is given the
// directory is sealed before writing (its parameters live in the header preamble, not
// in the directory's own TLV).
void write_directory_and_footer(std::ofstream& out, std::uint64_t written,
                                const std::vector<BlockRec>& blocks,
                                const std::vector<EntryRec>& entries,
                                const ArchiveMeta& meta,
                                const core::CryptoKey* dir_key = nullptr) {
    ByteVector directory;
    put_vint(directory, blocks.size());
    for (const auto& block : blocks) {
        put_vint(directory, block.compressed_offset);
        put_vint(directory, block.compressed_size);
        put_vint(directory, block.uncompressed_size);
        put_vint(directory, 0);  // block extra length (reserved for later)
    }
    put_vint(directory, entries.size());
    for (const auto& entry : entries) {
        // Build the entry record body, then length-prefix it so a reader can
        // bound (and, for unknown future types, skip) it.
        ByteVector body;
        put_vint(body, entry.type);
        put_vint(body, entry.path.size());
        body.insert(body.end(), entry.path.begin(), entry.path.end());
        if (entry.type == kEntryFile) {
            put_vint(body, entry.size);
            put_vint(body, entry.first_block);
            put_vint(body, entry.offset);
            put_vint(body, kExtraCrc32);
            put_vint(body, 4);
            put_u32(body, entry.crc);
            if (entry.has_blake3) {
                put_vint(body, kExtraBlake3);
                put_vint(body, entry.blake3.size());
                body.insert(body.end(), entry.blake3.begin(), entry.blake3.end());
            }
        }
        if (entry.type == kEntrySymlink || entry.type == kEntryHardlink) {
            put_vint(body, entry.link_target.size());
            body.insert(body.end(), entry.link_target.begin(), entry.link_target.end());
        }
        if (entry.mtime != 0) {
            put_vint(body, kExtraMtime);
            put_vint(body, 8);
            put_u64(body, static_cast<std::uint64_t>(entry.mtime));
        }
        if (entry.meta.has_windows_attributes) {
            put_vint(body, kExtraWinAttrs);
            put_vint(body, 4);
            put_u32(body, entry.meta.windows_attributes);
        }
        if (entry.meta.has_windows_times) {
            put_vint(body, kExtraWinTimes);
            put_vint(body, 24);
            put_u64(body, entry.meta.windows_creation_time);
            put_u64(body, entry.meta.windows_access_time);
            put_u64(body, entry.meta.windows_write_time);
        }
        if (entry.meta.has_posix) {
            put_vint(body, kExtraPosix);
            put_vint(body, 12);
            put_u32(body, entry.meta.posix_mode);
            put_u32(body, entry.meta.posix_uid);
            put_u32(body, entry.meta.posix_gid);
        }
        for (const auto& stream : entry.ads) {
            ByteVector payload;
            put_vint(payload, stream.name.size());
            payload.insert(payload.end(), stream.name.begin(), stream.name.end());
            payload.insert(payload.end(), stream.data.begin(), stream.data.end());
            put_vint(body, kExtraAdsStream);
            put_vint(body, payload.size());
            body.insert(body.end(), payload.begin(), payload.end());
        }
        put_vint(directory, body.size());
        directory.insert(directory.end(), body.begin(), body.end());
    }

    // Archive-level extra records (TLV): comment, lock, and (later) recovery,
    // encryption, and volume parameters. Each is type + length + payload.
    ByteVector archive_extras;
    std::uint64_t archive_extra_count = 0;
    if (!meta.comment.empty()) {
        put_vint(archive_extras, kArchiveComment);
        put_vint(archive_extras, meta.comment.size());
        archive_extras.insert(archive_extras.end(), meta.comment.begin(), meta.comment.end());
        ++archive_extra_count;
    }
    if (meta.locked) {
        put_vint(archive_extras, kArchiveLock);
        put_vint(archive_extras, 0);  // presence is the signal; no payload
        ++archive_extra_count;
    }
    // Block-only encryption records its parameters here; for a sealed directory the
    // parameters are in the plaintext preamble instead (dir_key != nullptr).
    if (meta.encryption.enabled && dir_key == nullptr) {
        const auto& enc = meta.encryption;
        ByteVector payload;
        put_vint(payload, enc.kdf.algorithm);
        put_vint(payload, enc.kdf.mem_blocks);
        put_vint(payload, enc.kdf.passes);
        put_vint(payload, enc.kdf.lanes);
        put_vint(payload, enc.kdf.salt.size());
        payload.insert(payload.end(), enc.kdf.salt.begin(), enc.kdf.salt.end());
        put_vint(payload, enc.key_check.size());
        payload.insert(payload.end(), enc.key_check.begin(), enc.key_check.end());
        put_vint(archive_extras, kArchiveEncryption);
        put_vint(archive_extras, payload.size());
        archive_extras.insert(archive_extras.end(), payload.begin(), payload.end());
        ++archive_extra_count;
    }
    if (meta.has_signature) {
        put_vint(archive_extras, kArchiveSignature);
        put_vint(archive_extras,
                 meta.signature_public_key.size() + meta.signature.size());
        archive_extras.insert(archive_extras.end(), meta.signature_public_key.begin(),
                              meta.signature_public_key.end());
        archive_extras.insert(archive_extras.end(), meta.signature.begin(), meta.signature.end());
        ++archive_extra_count;
    }
    put_vint(directory, archive_extra_count);
    directory.insert(directory.end(), archive_extras.begin(), archive_extras.end());

    if (dir_key != nullptr) {
        const std::span<const std::uint8_t> ad(kDirectoryAd.data(), kDirectoryAd.size());
        directory = core::aead_seal(*dir_key, directory, ad);
    }

    const std::uint64_t directory_offset = written;
    out.write(reinterpret_cast<const char*>(directory.data()),
              static_cast<std::streamsize>(directory.size()));

    ByteVector footer;
    put_u64(footer, directory_offset);
    put_u64(footer, directory.size());
    footer.insert(footer.end(), kArchiveMagic.begin(), kArchiveMagic.end());
    out.write(reinterpret_cast<const char*>(footer.data()), static_cast<std::streamsize>(footer.size()));

    out.flush();
    if (!out) {
        throw std::runtime_error("failed to finalize archive");
    }
    out.close();
}

// Rewrite an archive keeping only the entries for which `keep` returns true,
// decoding each kept file's bytes from the old blocks into fresh solid blocks. This
// reclaims dead space (e.g. data left behind by replaced/removed entries) and is the
// engine behind both `delete` (keep = not-deleted) and `repack` (keep = all).
void rebuild_archive_keeping(const fs::path& archive_path,
                             const std::function<bool(const EntryRec&)>& keep,
                             const CompressionOptions& options) {
    const auto operation = options.operation;

    std::uint64_t file_size = 0;
    auto in = open_archive(archive_path, file_size);
    const ByteSource bytes(in, file_size);
    const auto existing_recovery = read_recovery_service(bytes);
    auto index = read_index(bytes);
    if (index.meta.locked) {
        throw std::runtime_error("archive is locked (read-only)");
    }
    index.meta.has_signature = false;
    // For an encrypted archive, derive the key once: it both decrypts the surviving
    // blocks (via BlockSource) and re-seals the freshly written ones.
    std::optional<core::CryptoKey> key;
    if (index.meta.encryption.enabled) {
        key = derive_archive_key(index.meta.encryption, options.password);
    }
    BlockSource source(bytes, index, 0, operation, key);

    // Paths surviving the filter — used to drop hardlinks whose target is removed.
    std::unordered_set<std::string> kept_paths;
    std::uint64_t total_bytes = 0;
    std::uint64_t total_items = 0;
    for (const auto& entry : index.entries) {
        if (keep(entry)) {
            kept_paths.insert(entry.path);
            ++total_items;
            if (entry.type == kEntryFile) {
                total_bytes += entry.size;
            }
        }
    }

    const auto block_size = std::max<std::size_t>(1, options.block_size);

    fs::path temp_path = archive_path;
    temp_path += ".tmp";
    TempFileGuard temp_guard(temp_path);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot create archive: " + temp_path.string());
    }

    ByteVector header;
    header.insert(header.end(), kArchiveMagic.begin(), kArchiveMagic.end());
    put_u16(header, kArchiveVersion);
    put_u16(header, 0);  // flags
    put_u32(header, 0);  // reserved
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    std::uint64_t written = header.size();

    std::vector<BlockRec> new_blocks;
    std::vector<EntryRec> new_entries;
    ByteVector buffer;
    std::uint64_t current_block = 0;
    std::uint64_t completed_bytes = 0;
    std::uint64_t completed_items = 0;

    auto flush_block = [&]() {
        if (buffer.empty()) {
            return;
        }
        report_operation(operation, OperationStage::compressing, completed_bytes, total_bytes,
                         completed_items, total_items);
        auto compressed = compress(buffer, options);
        if (key) {
            compressed = core::aead_seal(*key, compressed, block_associated_data(new_blocks.size()));
        }
        operation_checkpoint(operation);
        new_blocks.push_back({written, static_cast<std::uint64_t>(compressed.size()),
                              static_cast<std::uint64_t>(buffer.size())});
        out.write(reinterpret_cast<const char*>(compressed.data()),
                  static_cast<std::streamsize>(compressed.size()));
        if (!out) {
            throw std::runtime_error("failed while writing archive blocks");
        }
        written += compressed.size();
        ++current_block;
        buffer.clear();
    };

    for (const auto& entry : index.entries) {
        operation_checkpoint(operation);
        if (!keep(entry)) {
            continue;
        }
        if (entry.type == kEntryHardlink &&
            kept_paths.find(entry.link_target) == kept_paths.end()) {
            continue;  // its target was removed; drop the now-dangling hard link
        }

        EntryRec out_entry = entry;  // carries metadata, crc/blake3, link target, ads
        if (entry.type == kEntryFile) {
            out_entry.first_block = current_block;
            out_entry.offset = buffer.size();
            read_file_bytes(source, index.blocks.size(), entry, operation,
                            [&](std::span<const std::uint8_t> chunk) {
                                buffer.insert(buffer.end(), chunk.begin(), chunk.end());
                                completed_bytes += chunk.size();
                                if (buffer.size() >= block_size) {
                                    flush_block();
                                }
                            });
        }
        new_entries.push_back(std::move(out_entry));
        ++completed_items;
        report_operation(operation, OperationStage::writing, completed_bytes, total_bytes,
                         completed_items, total_items, entry.path);
    }
    flush_block();
    in.close();  // release the source handle so the rename can replace it (Windows)

    report_operation(operation, OperationStage::finalizing, completed_bytes, total_bytes,
                     completed_items, total_items);
    write_directory_and_footer(out, written, new_blocks, new_entries, index.meta);

    std::error_code ec;
    fs::rename(temp_path, archive_path, ec);
    if (ec) {
        fs::remove(archive_path, ec);
        fs::rename(temp_path, archive_path, ec);
        if (ec) {
            throw std::runtime_error("failed to move archive into place: " + ec.message());
        }
    }
    temp_guard.dismiss();
    if (key) {
        core::secure_wipe(*key);
    }
    const unsigned recovery_percent = options.recovery_percent != 0
        ? options.recovery_percent
        : (existing_recovery ? existing_recovery->percent : 0);
    if (recovery_percent != 0) {
        rewrite_recovery_service(archive_path, recovery_percent, operation);
    }
    report_operation(operation, OperationStage::finalizing, total_bytes, total_bytes,
                     total_items, total_items);
}

// Append already-scanned items to an existing archive: existing block bytes are
// copied verbatim and the items become new blocks, with same-path items replacing
// the existing entry. Shared by add/update/sync. `meta_override`, when non-null,
// replaces the archive metadata (used to set the comment / lock flag); otherwise the
// existing metadata is preserved.
void append_items_to_archive(const fs::path& archive_path, const std::vector<ScanItem>& items,
                             const CompressionOptions& options,
                             const ArchiveMeta* meta_override = nullptr) {
    const auto operation = options.operation;

    // Read the existing archive's directory; new solid blocks are appended after its
    // block region, so existing block indices (and entry references) stay valid.
    ArchiveIndex existing;
    unsigned existing_recovery_percent = 0;
    {
        std::uint64_t file_size = 0;
        auto in = open_archive(archive_path, file_size);
        const ByteSource source(in, file_size);
        if (const auto recovery = read_recovery_service(source)) {
            existing_recovery_percent = recovery->percent;
        }
        existing = read_index(source);
    }
    if (existing.meta.locked) {
        throw std::runtime_error("archive is locked (read-only)");
    }
    // Encrypted archives: existing blocks are copied verbatim (still sealed), so only
    // a change that writes *new* blocks needs the key. Derive it then, sealing new
    // blocks with the same key; a comment/lock-only rewrite (no items) needs no key.
    std::optional<core::CryptoKey> key;
    if (existing.meta.encryption.enabled && !items.empty()) {
        key = derive_archive_key(existing.meta.encryption, options.password);
    }
    const core::CryptoKey* key_ptr = key ? &*key : nullptr;
    ArchiveMeta result_meta = meta_override ? *meta_override : existing.meta;
    result_meta.has_signature = false;
    std::uint64_t block_region_end = kHeaderSize;
    if (!existing.blocks.empty()) {
        const auto& last = existing.blocks.back();
        block_region_end = last.compressed_offset + last.compressed_size;
    }

    // Added paths replace any existing entry with the same path; the replaced data
    // stays in its solid block as dead space until a repack reclaims it.
    std::unordered_set<std::string> new_paths;
    for (const auto& item : items) {
        new_paths.insert(item.archive_path);
    }
    std::vector<EntryRec> entries;
    entries.reserve(existing.entries.size() + items.size());
    for (auto& existing_entry : existing.entries) {
        if (new_paths.find(existing_entry.path) == new_paths.end()) {
            entries.push_back(std::move(existing_entry));
        }
    }
    std::vector<BlockRec> blocks = std::move(existing.blocks);

    const auto total_bytes = scanned_file_bytes(items);
    const auto total_items = static_cast<std::uint64_t>(items.size());
    std::uint64_t completed_bytes = 0;
    std::uint64_t completed_items = 0;
    report_operation(operation, OperationStage::reading, completed_bytes, total_bytes,
                     completed_items, total_items);

    const auto block_size = std::max<std::size_t>(1, options.block_size);

    fs::path temp_path = archive_path;
    temp_path += ".tmp";
    TempFileGuard temp_guard(temp_path);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot create archive: " + temp_path.string());
    }

    // Copy the existing header + block region verbatim — the existing files are not
    // recompressed, only the directory is rebuilt with the merged entry list.
    {
        std::ifstream src(archive_path, std::ios::binary);
        if (!src) {
            throw std::runtime_error("cannot open archive: " + archive_path.string());
        }
        std::uint64_t remaining = block_region_end;
        std::array<char, kFileChunk> chunk{};
        while (remaining > 0) {
            operation_checkpoint(operation);
            const auto want = static_cast<std::streamsize>(
                std::min<std::uint64_t>(remaining, chunk.size()));
            src.read(chunk.data(), want);
            const auto got = src.gcount();
            if (got <= 0) {
                throw std::runtime_error("archive truncated while copying blocks");
            }
            out.write(chunk.data(), got);
            remaining -= static_cast<std::uint64_t>(got);
        }
    }
    std::uint64_t written = block_region_end;

    compress_items_into(out, written, blocks, entries, items, options, block_size, operation,
                        total_bytes, total_items, completed_bytes, completed_items, key_ptr);
    if (key) {
        core::secure_wipe(*key);
    }

    report_operation(operation, OperationStage::finalizing, completed_bytes, total_bytes,
                     completed_items, total_items);
    write_directory_and_footer(out, written, blocks, entries, result_meta);

    std::error_code ec;
    fs::rename(temp_path, archive_path, ec);
    if (ec) {
        fs::remove(archive_path, ec);
        fs::rename(temp_path, archive_path, ec);
        if (ec) {
            throw std::runtime_error("failed to move archive into place: " + ec.message());
        }
    }
    temp_guard.dismiss();
    const unsigned recovery_percent = options.recovery_percent != 0
        ? options.recovery_percent : existing_recovery_percent;
    if (recovery_percent != 0) {
        rewrite_recovery_service(archive_path, recovery_percent, operation);
    }
    report_operation(operation, OperationStage::finalizing, total_bytes, total_bytes,
                     total_items, total_items);
}

void rewrite_archive_directory(const fs::path& archive_path, ArchiveIndex index,
                               const CompressionOptions& options) {
    const auto operation = options.operation;
    std::uint64_t file_size = 0;
    auto in = open_archive(archive_path, file_size);
    const ByteSource source(in, file_size);
    const auto existing_recovery = read_recovery_service(source);
    const ArchiveLayout layout = read_layout(source);
    if ((layout.flags & kFlagEncryptedDirectory) != 0) {
        throw std::runtime_error("editing an archive with an encrypted directory is not supported");
    }
    if (index.meta.locked) {
        throw std::runtime_error("archive is locked (read-only)");
    }
    if (index.meta.encryption.enabled) {
        auto key = derive_archive_key(index.meta.encryption, options.password);
        core::secure_wipe(key);
    }

    std::uint64_t block_region_end = kHeaderSize;
    if (!index.blocks.empty()) {
        const auto& last = index.blocks.back();
        block_region_end = last.compressed_offset + last.compressed_size;
    }

    fs::path temp_path = archive_path;
    temp_path += ".tmp";
    TempFileGuard temp_guard(temp_path);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot create archive: " + temp_path.string());
    }

    std::uint64_t copied = 0;
    const std::uint64_t total_items = index.entries.size();
    report_operation(operation, OperationStage::writing, 0, block_region_end, 0, total_items);
    in.clear();
    in.seekg(0, std::ios::beg);
    std::array<char, kFileChunk> chunk{};
    while (copied < block_region_end) {
        operation_checkpoint(operation);
        const auto want = static_cast<std::streamsize>(
            std::min<std::uint64_t>(block_region_end - copied, chunk.size()));
        in.read(chunk.data(), want);
        const auto got = in.gcount();
        if (got <= 0) {
            throw std::runtime_error("archive truncated while copying blocks");
        }
        out.write(chunk.data(), got);
        if (!out) {
            throw std::runtime_error("failed while copying archive blocks");
        }
        copied += static_cast<std::uint64_t>(got);
        report_operation(operation, OperationStage::writing, copied, block_region_end,
                         0, total_items);
    }
    in.close();

    report_operation(operation, OperationStage::finalizing, copied, block_region_end,
                     total_items, total_items);
    write_directory_and_footer(out, block_region_end, index.blocks, index.entries, index.meta);

    std::error_code ec;
    fs::rename(temp_path, archive_path, ec);
    if (ec) {
        fs::remove(archive_path, ec);
        fs::rename(temp_path, archive_path, ec);
        if (ec) {
            throw std::runtime_error("failed to move archive into place: " + ec.message());
        }
    }
    temp_guard.dismiss();
    const unsigned recovery_percent = options.recovery_percent != 0
        ? options.recovery_percent
        : (existing_recovery ? existing_recovery->percent : 0);
    if (recovery_percent != 0) {
        rewrite_recovery_service(archive_path, recovery_percent, operation);
    }
    report_operation(operation, OperationStage::finalizing, block_region_end, block_region_end,
                     total_items, total_items);
}

}  // namespace

void create_archive(const std::vector<std::filesystem::path>& inputs,
                    const std::filesystem::path& archive_path,
                    const CompressionOptions& options) {
    const auto operation = options.operation;
    report_operation(operation, OperationStage::scanning, 0, 0, 0, inputs.size());

    std::vector<ScanItem> items;
    for (const auto& input : inputs) {
        operation_checkpoint(operation);
        scan_input(input, items);
    }

    const auto total_bytes = scanned_file_bytes(items);
    const auto total_items = static_cast<std::uint64_t>(items.size());
    std::uint64_t completed_bytes = 0;
    std::uint64_t completed_items = 0;
    report_operation(operation, OperationStage::reading, completed_bytes, total_bytes,
                     completed_items, total_items);

    const auto block_size = std::max<std::size_t>(1, options.block_size);

    fs::path temp_path = archive_path;
    temp_path += ".tmp";
    TempFileGuard temp_guard(temp_path);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot create archive: " + temp_path.string());
    }

    // Encryption (optional). Derive the key + key-check first so we can flag the
    // header and, for a sealed directory, emit the plaintext parameter preamble.
    ArchiveMeta meta;
    core::CryptoKey key{};
    const core::CryptoKey* key_ptr = nullptr;
    const bool encrypt_dir = !options.password.empty() && options.encrypt_header;
    if (!options.password.empty()) {
        auto [enc, derived] = make_encryption(options.password);
        enc.encrypt_directory = encrypt_dir;
        meta.encryption = std::move(enc);
        key = derived;
        key_ptr = &key;
    }

    ByteVector header;
    header.insert(header.end(), kArchiveMagic.begin(), kArchiveMagic.end());
    put_u16(header, kArchiveVersion);
    put_u16(header, encrypt_dir ? kFlagEncryptedDirectory : static_cast<std::uint16_t>(0));
    put_u32(header, 0);  // reserved
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    std::uint64_t written = header.size();

    if (encrypt_dir) {
        const auto preamble = serialize_encryption_preamble(meta.encryption);
        out.write(reinterpret_cast<const char*>(preamble.data()),
                  static_cast<std::streamsize>(preamble.size()));
        written += preamble.size();
    }

    std::vector<BlockRec> blocks;
    std::vector<EntryRec> entries;
    compress_items_into(out, written, blocks, entries, items, options, block_size, operation,
                        total_bytes, total_items, completed_bytes, completed_items, key_ptr);

    report_operation(operation, OperationStage::finalizing, completed_bytes, total_bytes,
                     completed_items, total_items);
    write_directory_and_footer(out, written, blocks, entries, meta,
                               encrypt_dir ? key_ptr : nullptr);
    core::secure_wipe(key);

    std::error_code ec;
    fs::rename(temp_path, archive_path, ec);
    if (ec) {
        fs::remove(archive_path, ec);
        fs::rename(temp_path, archive_path, ec);
        if (ec) {
            throw std::runtime_error("failed to move archive into place: " + ec.message());
        }
    }
    temp_guard.dismiss();
    if (options.recovery_percent != 0) {
        rewrite_recovery_service(archive_path, options.recovery_percent, operation);
    }
    report_operation(operation, OperationStage::finalizing, total_bytes, total_bytes,
                     total_items, total_items);
}

void add_to_archive(const std::vector<std::filesystem::path>& inputs,
                    const std::filesystem::path& archive_path,
                    const CompressionOptions& options) {
    const auto operation = options.operation;
    report_operation(operation, OperationStage::scanning, 0, 0, 0, inputs.size());

    std::vector<ScanItem> items;
    for (const auto& input : inputs) {
        operation_checkpoint(operation);
        scan_input(input, items);
    }
    append_items_to_archive(archive_path, items, options);
}

void add_to_archive(const std::vector<ArchiveInput>& inputs,
                    const std::filesystem::path& archive_path,
                    const CompressionOptions& options) {
    const auto operation = options.operation;
    report_operation(operation, OperationStage::scanning, 0, 0, 0, inputs.size());

    std::vector<ScanItem> items;
    for (const auto& input : inputs) {
        operation_checkpoint(operation);
        scan_input_at(input, items, operation);
    }

    ArchiveIndex existing;
    {
        std::uint64_t file_size = 0;
        auto in = open_archive(archive_path, file_size);
        const ByteSource source(in, file_size);
        existing = read_index(source);
    }
    validate_mapped_items(items, existing, operation);
    append_items_to_archive(archive_path, items, options);
}

void delete_from_archive(const std::filesystem::path& archive_path,
                         const std::vector<std::string>& paths,
                         const CompressionOptions& options) {
    // Normalize the targets the way archive paths are stored: '/'-separated, no
    // trailing slash. A directory target removes its whole subtree.
    std::vector<std::string> targets;
    targets.reserve(paths.size());
    for (auto path : paths) {
        std::replace(path.begin(), path.end(), '\\', '/');
        while (path.size() > 1 && path.back() == '/') {
            path.pop_back();
        }
        if (!path.empty()) {
            targets.push_back(std::move(path));
        }
    }
    auto is_deleted = [&](const std::string& candidate) {
        for (const auto& target : targets) {
            if (candidate == target) {
                return true;
            }
            if (candidate.size() > target.size() &&
                candidate.compare(0, target.size(), target) == 0 &&
                candidate[target.size()] == '/') {
                return true;  // entry lives under a deleted directory
            }
        }
        return false;
    };
    rebuild_archive_keeping(
        archive_path, [&](const EntryRec& entry) { return !is_deleted(entry.path); }, options);
}

void repack_archive(const std::filesystem::path& archive_path,
                    const CompressionOptions& options) {
    rebuild_archive_keeping(archive_path, [](const EntryRec&) { return true; }, options);
}

void update_archive(const std::vector<std::filesystem::path>& inputs,
                    const std::filesystem::path& archive_path, const CompressionOptions& options,
                    bool fresh_only) {
    const auto operation = options.operation;
    report_operation(operation, OperationStage::scanning, 0, 0, 0, inputs.size());

    std::vector<ScanItem> items;
    for (const auto& input : inputs) {
        operation_checkpoint(operation);
        scan_input(input, items);
    }

    if (!fs::exists(archive_path)) {
        // Nothing to refresh against. `update` seeds a new archive; `fresh` is a no-op.
        if (!fresh_only) {
            create_archive(inputs, archive_path, options);
        }
        return;
    }

    // Map each existing entry's path to its stored mtime so we can compare ages.
    std::unordered_map<std::string, std::int64_t> existing_mtime;
    {
        std::uint64_t file_size = 0;
        auto in = open_archive(archive_path, file_size);
        const ByteSource source(in, file_size);
        const auto index = read_index(source);
        for (const auto& entry : index.entries) {
            existing_mtime.emplace(entry.path, entry.mtime);
        }
    }

    // Keep only the items that should be written: a file when it is newer than the
    // archived copy (or, for `update`, brand new); a directory/symlink only when it
    // is new (and `update`, not `fresh`).
    std::vector<ScanItem> selected;
    std::error_code ec;
    for (const auto& item : items) {
        const auto found = existing_mtime.find(item.archive_path);
        const bool in_archive = found != existing_mtime.end();
        if (item.is_directory || item.is_symlink) {
            if (!in_archive && !fresh_only) {
                selected.push_back(item);
            }
            continue;
        }
        std::int64_t disk_mtime = 0;
        const auto stamp = fs::last_write_time(item.absolute, ec);
        if (!ec) {
            try {
                disk_mtime = to_unix_seconds(stamp);
            } catch (...) {
                disk_mtime = 0;
            }
        }
        if (in_archive) {
            if (disk_mtime > found->second) {
                selected.push_back(item);  // newer on disk → replace
            }
        } else if (!fresh_only) {
            selected.push_back(item);  // new file → add (update only)
        }
    }

    append_items_to_archive(archive_path, selected, options);
}

void sync_archive(const std::vector<std::filesystem::path>& inputs,
                  const std::filesystem::path& archive_path, const CompressionOptions& options) {
    // First bring in new and newer files (creating the archive if it is missing).
    update_archive(inputs, archive_path, options, /*fresh_only=*/false);

    // Then mirror deletions: drop any archived path no longer present in the inputs.
    std::vector<ScanItem> items;
    for (const auto& input : inputs) {
        scan_input(input, items);
    }
    std::unordered_set<std::string> wanted;
    for (const auto& item : items) {
        wanted.insert(item.archive_path);
    }

    std::vector<std::string> stale;
    {
        std::uint64_t file_size = 0;
        auto in = open_archive(archive_path, file_size);
        const ByteSource source(in, file_size);
        const auto index = read_index(source);
        for (const auto& entry : index.entries) {
            if (wanted.find(entry.path) == wanted.end()) {
                stale.push_back(entry.path);
            }
        }
    }
    if (!stale.empty()) {
        delete_from_archive(archive_path, stale, options);
    }
}

void move_archive_entries(const std::filesystem::path& archive_path,
                          const std::vector<ArchiveMove>& moves,
                          const CompressionOptions& options) {
    if (moves.empty()) {
        return;
    }

    const auto operation = options.operation;
    std::vector<ArchiveMove> normalized;
    normalized.reserve(moves.size());
    std::unordered_set<std::string> sources;
    std::unordered_set<std::string> destinations;
    for (const auto& move : moves) {
        operation_checkpoint(operation);
        ArchiveMove item{
            normalize_archive_path(move.source_path, "archive move source"),
            normalize_archive_path(move.destination_path, "archive move destination")};
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

    for (std::size_t i = 0; i < normalized.size(); ++i) {
        operation_checkpoint(operation);
        const auto& move = normalized[i];
        if (is_same_or_child(move.destination_path, move.source_path)) {
            throw std::invalid_argument("cannot move an archive entry into its own subtree: " +
                                        move.source_path);
        }
        for (std::size_t j = 0; j < normalized.size(); ++j) {
            if (i == j) {
                continue;
            }
            if (is_same_or_child(move.source_path, normalized[j].source_path)) {
                throw std::invalid_argument("archive move sources overlap: " + move.source_path);
            }
            if (is_same_or_child(move.destination_path, normalized[j].source_path)) {
                throw std::invalid_argument(
                    "archive move destination lies in another moved subtree: " +
                    move.destination_path);
            }
        }
    }

    ArchiveIndex index;
    {
        std::uint64_t file_size = 0;
        auto in = open_archive(archive_path, file_size);
        const ByteSource source(in, file_size);
        index = read_index(source);
    }

    std::unordered_map<std::string, std::uint8_t> original_types;
    original_types.reserve(index.entries.size());
    for (const auto& entry : index.entries) {
        operation_checkpoint(operation);
        original_types.emplace(entry.path, entry.type);
    }
    for (const auto& move : normalized) {
        bool found_source = original_types.find(move.source_path) != original_types.end();
        if (!found_source) {
            for (const auto& [path, ignored] : original_types) {
                (void)ignored;
                if (is_same_or_child(path, move.source_path)) {
                    found_source = true;  // implicit directory represented by descendants
                    break;
                }
            }
        }
        if (!found_source) {
            throw std::invalid_argument("archive move source does not exist: " +
                                        move.source_path);
        }
    }

    auto moved_path = [&](const std::string& path) {
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

    std::unordered_map<std::string, std::uint8_t> final_types;
    final_types.reserve(index.entries.size());
    for (auto& entry : index.entries) {
        operation_checkpoint(operation);
        entry.path = moved_path(entry.path);
        if (entry.type == kEntryHardlink) {
            entry.link_target = moved_path(entry.link_target);
        }
        if (!final_types.emplace(entry.path, entry.type).second) {
            throw std::invalid_argument("archive move destination already exists: " + entry.path);
        }
    }

    for (const auto& [path, type] : final_types) {
        operation_checkpoint(operation);
        std::size_t slash = path.rfind('/');
        if (slash != std::string::npos) {
            const std::string parent = path.substr(0, slash);
            const auto found = final_types.find(parent);
            if (found != final_types.end() && found->second != kEntryDir) {
                throw std::invalid_argument("archive move destination parent is not a directory: " +
                                            parent);
            }
        }
        if (type != kEntryDir) {
            const std::string prefix = path + "/";
            for (const auto& [other, ignored] : final_types) {
                (void)ignored;
                if (other.size() > path.size() &&
                    other.compare(0, prefix.size(), prefix) == 0) {
                    throw std::invalid_argument("non-directory archive entry has children: " +
                                                path);
                }
            }
        }
    }
    for (const auto& entry : index.entries) {
        operation_checkpoint(operation);
        if (entry.type == kEntryHardlink) {
            const auto target = final_types.find(entry.link_target);
            if (target == final_types.end() || target->second != kEntryFile) {
                throw std::invalid_argument("archive move leaves a dangling hard link: " +
                                            entry.path);
            }
        }
    }

    index.meta.has_signature = false;
    rewrite_archive_directory(archive_path, std::move(index), options);
}

void set_archive_comment(const std::filesystem::path& archive_path, const std::string& comment,
                         const CompressionOptions& options) {
    // Start from the existing metadata so the encryption record (and anything else)
    // is preserved; only the comment changes.
    ArchiveMeta meta;
    {
        std::uint64_t file_size = 0;
        auto in = open_archive(archive_path, file_size);
        const ByteSource source(in, file_size);
        meta = read_index(source).meta;
    }
    meta.comment = comment;
    meta.has_signature = false;
    append_items_to_archive(archive_path, {}, options, &meta);
}

void lock_archive(const std::filesystem::path& archive_path, const CompressionOptions& options) {
    ArchiveMeta meta;
    {
        std::uint64_t file_size = 0;
        auto in = open_archive(archive_path, file_size);
        const ByteSource source(in, file_size);
        meta = read_index(source).meta;  // preserve any existing comment
    }
    meta.locked = true;
    meta.has_signature = false;
    append_items_to_archive(archive_path, {}, options, &meta);
}

std::string archive_comment(const std::filesystem::path& archive_path,
                            const std::string& password) {
    std::uint64_t file_size = 0;
    auto in = open_archive(archive_path, file_size);
    const ByteSource source(in, file_size);
    return load_index(source, password).index.meta.comment;
}

bool archive_is_locked(const std::filesystem::path& archive_path, const std::string& password) {
    std::uint64_t file_size = 0;
    auto in = open_archive(archive_path, file_size);
    const ByteSource source(in, file_size);
    return load_index(source, password).index.meta.locked;
}

ArchiveEncryptionMode archive_encryption_mode(const std::filesystem::path& archive_path) {
    std::uint64_t file_size = 0;
    auto in = open_archive(archive_path, file_size);
    const ByteSource source(in, file_size);
    // The header flag identifies a sealed-directory archive without a password; a
    // block-only encrypted archive is identified from its (plaintext) directory.
    const auto layout = read_layout(source);
    if ((layout.flags & kFlagEncryptedDirectory) != 0) {
        return ArchiveEncryptionMode::data_and_directory;
    }
    return read_index(source).meta.encryption.enabled
        ? ArchiveEncryptionMode::data_only
        : ArchiveEncryptionMode::none;
}

bool archive_is_encrypted(const std::filesystem::path& archive_path) {
    return archive_encryption_mode(archive_path) != ArchiveEncryptionMode::none;
}

namespace {

struct VolumeHeader {
    bool recovery = false;
    std::uint32_t index = 0;
    std::uint32_t data_count = 0;
    std::uint32_t parity_count = 0;
    std::uint64_t shard_size = 0;
    std::uint64_t archive_size = 0;
    core::Blake3Digest digest{};
    std::uint32_t payload_crc = 0;
};

ByteVector serialize_volume_header(const VolumeHeader& header) {
    ByteVector bytes;
    bytes.insert(bytes.end(), kVolumeMagic.begin(), kVolumeMagic.end());
    put_u16(bytes, kVolumeVersion);
    put_u16(bytes, header.recovery ? 1 : 0);
    put_u32(bytes, header.index);
    put_u32(bytes, header.data_count);
    put_u32(bytes, header.parity_count);
    put_u64(bytes, header.shard_size);
    put_u64(bytes, header.archive_size);
    bytes.insert(bytes.end(), header.digest.begin(), header.digest.end());
    put_u32(bytes, header.payload_crc);
    put_u32(bytes, 0);
    return bytes;
}

VolumeHeader read_volume_header(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open archive volume: " + path.string());
    ByteVector bytes(kVolumeHeaderSize);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()) ||
        !std::equal(kVolumeMagic.begin(), kVolumeMagic.end(), bytes.begin())) {
        throw FormatError("invalid archive volume header");
    }
    Reader reader(bytes, kVolumeMagic.size());
    if (reader.u16() != kVolumeVersion) throw FormatError("unsupported archive volume version");
    VolumeHeader header;
    const auto kind = reader.u16();
    if (kind > 1) throw FormatError("invalid archive volume kind");
    header.recovery = kind == 1;
    header.index = reader.u32();
    header.data_count = reader.u32();
    header.parity_count = reader.u32();
    header.shard_size = reader.u64();
    header.archive_size = reader.u64();
    const auto digest = reader.take(header.digest.size());
    std::copy(digest.begin(), digest.end(), header.digest.begin());
    header.payload_crc = reader.u32();
    (void)reader.u32();
    if (header.data_count == 0 || header.data_count + header.parity_count > 255 ||
        header.shard_size == 0 || header.index >=
            (header.recovery ? header.parity_count : header.data_count)) {
        throw FormatError("invalid archive volume parameters");
    }
    return header;
}

std::wstring three_digit(unsigned value) {
    std::wostringstream text;
    text << std::setw(3) << std::setfill(L'0') << value;
    return text.str();
}

fs::path volume_root(const fs::path& any_volume) {
    std::wstring name = any_volume.filename().wstring();
    const auto part = name.rfind(L".part");
    const auto rev = name.rfind(L".rev");
    std::size_t cut = std::wstring::npos;
    if (part != std::wstring::npos) cut = part;
    if (rev != std::wstring::npos && (cut == std::wstring::npos || rev > cut)) cut = rev;
    if (cut == std::wstring::npos) throw std::invalid_argument("not a numbered Axiom volume");
    return any_volume.parent_path() / name.substr(0, cut);
}

fs::path data_volume_path(const fs::path& root, unsigned index) {
    return fs::path(root.wstring() + L".part" + three_digit(index + 1) + L".axar");
}

fs::path recovery_volume_path(const fs::path& root, unsigned index) {
    return fs::path(root.wstring() + L".rev" + three_digit(index + 1));
}

core::Blake3Digest hash_file(const fs::path& path,
                             const std::shared_ptr<OperationControl>& operation = nullptr) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open archive: " + path.string());
    core::Blake3 hash;
    std::array<std::uint8_t, kFileChunk> buffer{};
    while (input) {
        operation_checkpoint(operation);
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) hash.update(std::span<const std::uint8_t>(
            buffer.data(), static_cast<std::size_t>(count)));
    }
    return hash.finalize();
}

void write_volume(const fs::path& path, const VolumeHeader& header,
                  std::span<const std::uint8_t> payload) {
    const auto bytes = serialize_volume_header(header);
    fs::path temporary = path;
    temporary += L".tmp";
    TempFileGuard guard(temporary);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.write(reinterpret_cast<const char*>(payload.data()),
                 static_cast<std::streamsize>(payload.size()));
    output.close();
    if (!output) throw std::runtime_error("failed to write archive volume");
    replace_archive_file(temporary, path);
    guard.dismiss();
}

}  // namespace

ArchiveRecoveryInfo archive_recovery_info(const std::filesystem::path& archive_path) {
    std::uint64_t file_size = 0;
    auto stream = open_archive(archive_path, file_size);
    const ByteSource source(stream, file_size);
    const auto service = read_recovery_service(source);
    if (!service) return {};
    return {true, service->percent, service->data_shards, service->parity_shards,
            service->protected_size};
}

void set_archive_recovery(const std::filesystem::path& archive_path, unsigned percent,
                          const std::shared_ptr<OperationControl>& operation) {
    if (percent > 100) throw std::invalid_argument("recovery percentage must be 0..100");
    rewrite_recovery_service(archive_path, percent, operation);
}

bool repair_archive(const std::filesystem::path& archive_path,
                    const std::shared_ptr<OperationControl>& operation) {
    std::uint64_t file_size = 0;
    auto input = open_archive(archive_path, file_size);
    const ByteSource source(input, file_size);
    const auto service_optional = read_recovery_service(source);
    if (!service_optional) return false;
    const auto& service = *service_optional;
    const int data_count = service.data_shards;
    const int parity_count = service.parity_shards;
    const int total_count = data_count + parity_count;
    if (service.shard_size > std::numeric_limits<std::size_t>::max()) {
        throw FormatError("recovery shard is too large");
    }
    const auto shard_size = static_cast<std::size_t>(service.shard_size);
    std::vector<std::vector<std::uint8_t>> shards(
        static_cast<std::size_t>(total_count), std::vector<std::uint8_t>(shard_size, 0));
    std::vector<bool> present(static_cast<std::size_t>(total_count), true);
    int damaged = 0;
    for (int index = 0; index < data_count; ++index) {
        operation_checkpoint(operation);
        const std::uint64_t offset = static_cast<std::uint64_t>(index) * service.shard_size;
        const std::uint64_t count = offset < service.protected_size
            ? std::min(service.shard_size, service.protected_size - offset) : 0;
        if (count != 0) {
            const auto bytes = source.read(offset, count);
            std::copy(bytes.begin(), bytes.end(), shards[static_cast<std::size_t>(index)].begin());
        }
        if (recovery_crc(shards[static_cast<std::size_t>(index)]) !=
            service.checksums[static_cast<std::size_t>(index)]) {
            present[static_cast<std::size_t>(index)] = false;
            ++damaged;
        }
        report_operation(operation, OperationStage::testing,
                         std::min(service.protected_size, offset + count),
                         service.protected_size, index + 1, total_count,
                         "Checking recovery shards");
    }
    for (int index = 0; index < parity_count; ++index) {
        const auto begin = service.parity.begin() +
            static_cast<std::ptrdiff_t>(static_cast<std::uint64_t>(index) * service.shard_size);
        std::copy_n(begin, shard_size,
                    shards[static_cast<std::size_t>(data_count + index)].begin());
        const auto shard_index = static_cast<std::size_t>(data_count + index);
        if (recovery_crc(shards[shard_index]) != service.checksums[shard_index]) {
            present[shard_index] = false;
            ++damaged;
        }
    }
    if (damaged > parity_count ||
        !core::ReedSolomon(data_count, parity_count).reconstruct(shards, present)) {
        throw FormatError("archive damage exceeds the recovery record capacity");
    }

    ByteVector protected_bytes;
    protected_bytes.reserve(static_cast<std::size_t>(service.protected_size));
    for (int index = 0; index < data_count &&
                        protected_bytes.size() < service.protected_size; ++index) {
        const auto remaining = static_cast<std::size_t>(
            service.protected_size - protected_bytes.size());
        const auto count = std::min(shard_size, remaining);
        protected_bytes.insert(protected_bytes.end(), shards[static_cast<std::size_t>(index)].begin(),
                               shards[static_cast<std::size_t>(index)].begin() +
                                   static_cast<std::ptrdiff_t>(count));
    }
    const ByteSource repaired_source{std::span<const std::uint8_t>(protected_bytes)};
    auto rebuilt = encode_recovery_service(
        repaired_source, service.protected_size, service.directory_offset,
        service.directory_size, service.percent, operation);
    const auto footer = archive_footer_bytes(service.directory_offset, service.directory_size);

    fs::path temporary = archive_path;
    temporary += L".repair.tmp";
    TempFileGuard guard(temporary);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(protected_bytes.data()),
                 static_cast<std::streamsize>(protected_bytes.size()));
    output.write(reinterpret_cast<const char*>(rebuilt.body.data()),
                 static_cast<std::streamsize>(rebuilt.body.size()));
    output.write(reinterpret_cast<const char*>(rebuilt.tail.data()),
                 static_cast<std::streamsize>(rebuilt.tail.size()));
    output.write(reinterpret_cast<const char*>(footer.data()),
                 static_cast<std::streamsize>(footer.size()));
    output.close();
    if (!output) throw std::runtime_error("failed to write repaired archive");
    input.close();
    replace_archive_file(temporary, archive_path);
    guard.dismiss();
    return true;
}

ArchiveVolumeSetInfo create_archive_volumes(
    const std::filesystem::path& archive_path, std::uint64_t volume_size,
    unsigned recovery_volume_count,
    const std::shared_ptr<OperationControl>& operation) {
    if (volume_size == 0 || volume_size > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("volume size is out of range");
    }
    std::error_code error;
    const std::uint64_t archive_size = fs::file_size(archive_path, error);
    if (error) throw std::runtime_error("cannot size archive: " + error.message());
    const std::uint64_t count64 = std::max<std::uint64_t>(
        1, (archive_size + volume_size - 1) / volume_size);
    if (count64 > 254 || recovery_volume_count > 255 - count64) {
        throw std::invalid_argument("volume set exceeds the 255-shard Reed-Solomon limit");
    }
    const auto data_count = static_cast<int>(count64);
    const auto parity_count = static_cast<int>(recovery_volume_count);
    const auto shard_size = static_cast<std::size_t>(volume_size);
    const auto digest = hash_file(archive_path, operation);

    std::vector<std::vector<std::uint8_t>> data(
        static_cast<std::size_t>(data_count), std::vector<std::uint8_t>(shard_size, 0));
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open archive for splitting");
    for (int index = 0; index < data_count; ++index) {
        operation_checkpoint(operation);
        const auto offset = static_cast<std::uint64_t>(index) * volume_size;
        const auto actual = static_cast<std::size_t>(
            std::min<std::uint64_t>(volume_size, archive_size - offset));
        input.read(reinterpret_cast<char*>(data[static_cast<std::size_t>(index)].data()),
                   static_cast<std::streamsize>(actual));
        if (input.gcount() != static_cast<std::streamsize>(actual)) {
            throw std::runtime_error("archive changed while creating volumes");
        }
    }

    std::vector<std::vector<std::uint8_t>> parity(
        static_cast<std::size_t>(parity_count), std::vector<std::uint8_t>(shard_size, 0));
    if (parity_count != 0) {
        std::vector<std::span<const std::uint8_t>> data_spans;
        std::vector<std::span<std::uint8_t>> parity_spans;
        for (const auto& shard : data) data_spans.emplace_back(shard);
        for (auto& shard : parity) parity_spans.emplace_back(shard);
        core::ReedSolomon(data_count, parity_count).encode(data_spans, parity_spans);
    }

    fs::path root = archive_path;
    if (root.extension() == fs::path(".axar")) root.replace_extension();
    std::vector<fs::path> created;
    try {
        for (int index = 0; index < data_count; ++index) {
            operation_checkpoint(operation);
            const auto offset = static_cast<std::uint64_t>(index) * volume_size;
            const auto actual = static_cast<std::size_t>(
                std::min<std::uint64_t>(volume_size, archive_size - offset));
            VolumeHeader header{false, static_cast<std::uint32_t>(index),
                static_cast<std::uint32_t>(data_count), static_cast<std::uint32_t>(parity_count),
                volume_size, archive_size, digest,
                recovery_crc(data[static_cast<std::size_t>(index)])};
            const auto path = data_volume_path(root, static_cast<unsigned>(index));
            write_volume(path, header, std::span<const std::uint8_t>(
                data[static_cast<std::size_t>(index)].data(), actual));
            created.push_back(path);
            report_operation(operation, OperationStage::writing,
                             std::min(archive_size, offset + actual), archive_size,
                             index + 1, data_count + parity_count, path.filename().string());
        }
        for (int index = 0; index < parity_count; ++index) {
            VolumeHeader header{true, static_cast<std::uint32_t>(index),
                static_cast<std::uint32_t>(data_count), static_cast<std::uint32_t>(parity_count),
                volume_size, archive_size, digest,
                recovery_crc(parity[static_cast<std::size_t>(index)])};
            const auto path = recovery_volume_path(root, static_cast<unsigned>(index));
            write_volume(path, header, parity[static_cast<std::size_t>(index)]);
            created.push_back(path);
        }
    } catch (...) {
        for (const auto& path : created) fs::remove(path, error);
        throw;
    }
    return {static_cast<std::uint32_t>(data_count), static_cast<std::uint32_t>(parity_count),
            volume_size, archive_size};
}

ArchiveVolumeSetInfo archive_volume_set_info(const std::filesystem::path& any_volume) {
    const auto header = read_volume_header(any_volume);
    return {header.data_count, header.parity_count, header.shard_size, header.archive_size};
}

void join_archive_volumes(const std::filesystem::path& any_volume,
                          const std::filesystem::path& output_archive,
                          const std::shared_ptr<OperationControl>& operation) {
    const auto seed = read_volume_header(any_volume);
    const fs::path root = volume_root(any_volume);
    const int data_count = static_cast<int>(seed.data_count);
    const int parity_count = static_cast<int>(seed.parity_count);
    const int total_count = data_count + parity_count;
    if (seed.shard_size > std::numeric_limits<std::size_t>::max()) {
        throw FormatError("archive volume shard is too large");
    }
    const auto shard_size = static_cast<std::size_t>(seed.shard_size);
    std::vector<std::vector<std::uint8_t>> shards(
        static_cast<std::size_t>(total_count), std::vector<std::uint8_t>(shard_size, 0));
    std::vector<bool> present(static_cast<std::size_t>(total_count), false);

    auto compatible = [&](const VolumeHeader& header, bool recovery, int index) {
        return header.recovery == recovery && header.index == static_cast<std::uint32_t>(index) &&
               header.data_count == seed.data_count && header.parity_count == seed.parity_count &&
               header.shard_size == seed.shard_size && header.archive_size == seed.archive_size &&
               header.digest == seed.digest;
    };
    auto read_one = [&](const fs::path& path, bool recovery, int index,
                        std::vector<std::uint8_t>& shard) {
        std::error_code exists_error;
        if (!fs::exists(path, exists_error)) return false;
        try {
            const auto header = read_volume_header(path);
            if (!compatible(header, recovery, index)) {
                throw FormatError("archive volume belongs to a different set");
            }
            const auto expected = recovery ? shard_size : static_cast<std::size_t>(
                std::min<std::uint64_t>(seed.shard_size,
                    seed.archive_size - static_cast<std::uint64_t>(index) * seed.shard_size));
            std::ifstream input(path, std::ios::binary);
            input.seekg(static_cast<std::streamoff>(kVolumeHeaderSize));
            input.read(reinterpret_cast<char*>(shard.data()), static_cast<std::streamsize>(expected));
            if (input.gcount() != static_cast<std::streamsize>(expected)) return false;
            return recovery_crc(shard) == header.payload_crc;
        } catch (const FormatError&) {
            throw;
        } catch (...) {
            return false;
        }
    };

    for (int index = 0; index < data_count; ++index) {
        present[static_cast<std::size_t>(index)] = read_one(
            data_volume_path(root, static_cast<unsigned>(index)), false, index,
            shards[static_cast<std::size_t>(index)]);
    }
    for (int index = 0; index < parity_count; ++index) {
        const auto shard_index = static_cast<std::size_t>(data_count + index);
        present[shard_index] = read_one(
            recovery_volume_path(root, static_cast<unsigned>(index)), true, index,
            shards[shard_index]);
    }
    const auto available = std::count(present.begin(), present.end(), true);
    if (available < data_count || (available != total_count &&
        !core::ReedSolomon(data_count, parity_count).reconstruct(shards, present))) {
        throw FormatError("not enough archive/recovery volumes to reconstruct the archive");
    }

    fs::path temporary = output_archive;
    temporary += L".join.tmp";
    TempFileGuard guard(temporary);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    core::Blake3 hash;
    std::uint64_t written = 0;
    for (int index = 0; index < data_count && written < seed.archive_size; ++index) {
        operation_checkpoint(operation);
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(seed.shard_size, seed.archive_size - written));
        output.write(reinterpret_cast<const char*>(shards[static_cast<std::size_t>(index)].data()),
                     static_cast<std::streamsize>(count));
        hash.update(std::span<const std::uint8_t>(
            shards[static_cast<std::size_t>(index)].data(), count));
        written += count;
        report_operation(operation, OperationStage::writing, written, seed.archive_size,
                         index + 1, data_count, output_archive.filename().string());
    }
    output.close();
    if (!output) throw std::runtime_error("failed to join archive volumes");
    if (hash.finalize() != seed.digest) throw FormatError("joined archive hash mismatch");
    replace_archive_file(temporary, output_archive);
    guard.dismiss();
}

ArchiveSigningKey generate_archive_signing_key() {
    const auto generated = core::generate_signing_key();
    return {generated.secret_key, generated.public_key};
}

void sign_archive(const std::filesystem::path& archive_path,
                  const ArchiveSigningKey& key,
                  const CompressionOptions& options) {
    std::uint64_t file_size = 0;
    auto stream = open_archive(archive_path, file_size);
    const ByteSource source(stream, file_size);
    const ArchiveLayout layout = read_layout(source);
    if ((layout.flags & kFlagEncryptedDirectory) != 0) {
        throw std::runtime_error(
            "signing an archive with an encrypted directory is not supported");
    }
    auto loaded = load_index(source, options.password);
    if (loaded.index.meta.locked) {
        throw std::runtime_error("archive is locked (read-only)");
    }
    const auto digest = archive_signature_digest(source, layout, loaded.index);
    const auto signature = core::sign_message(key.secret_key, digest);
    if (!core::verify_message(key.public_key, signature, digest)) {
        throw std::invalid_argument("signing key public and secret components do not match");
    }
    loaded.index.meta.has_signature = true;
    loaded.index.meta.signature_public_key = key.public_key;
    loaded.index.meta.signature = signature;
    stream.close();
    rewrite_archive_directory(archive_path, std::move(loaded.index), options);
}

ArchiveSignatureInfo verify_archive_signature(
    const std::filesystem::path& archive_path,
    const std::string& password,
    const std::optional<std::array<std::uint8_t, 32>>& trusted_key) {
    std::uint64_t file_size = 0;
    auto stream = open_archive(archive_path, file_size);
    const ByteSource source(stream, file_size);
    const ArchiveLayout layout = read_layout(source);
    const auto loaded = load_index(source, password);
    ArchiveSignatureInfo info;
    info.present = loaded.index.meta.has_signature;
    if (!info.present) return info;
    info.public_key = loaded.index.meta.signature_public_key;
    const auto digest = archive_signature_digest(source, layout, loaded.index);
    info.valid = core::verify_message(info.public_key, loaded.index.meta.signature, digest);
    info.trusted_key = trusted_key.has_value() && *trusted_key == info.public_key;
    return info;
}

void create_sfx_archive(const std::filesystem::path& archive_path,
                        const std::filesystem::path& stub_executable,
                        const std::filesystem::path& output_executable,
                        const std::shared_ptr<OperationControl>& operation) {
    const auto normalized_output = fs::absolute(output_executable).lexically_normal();
    if (fs::absolute(archive_path).lexically_normal() == normalized_output ||
        fs::absolute(stub_executable).lexically_normal() == normalized_output) {
        throw std::invalid_argument("SFX output must differ from its archive and stub");
    }
    std::error_code error;
    const std::uint64_t stub_size = fs::file_size(stub_executable, error);
    if (error) throw std::runtime_error("cannot read SFX stub: " + error.message());
    const std::uint64_t archive_size = fs::file_size(archive_path, error);
    if (error) throw std::runtime_error("cannot read SFX archive: " + error.message());

    fs::path temporary = output_executable;
    temporary += ".tmp";
    TempFileGuard guard(temporary);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create SFX output");

    std::uint64_t completed = 0;
    const std::uint64_t total = stub_size + archive_size;
    auto copy = [&](const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot read SFX input: " + path.string());
        std::array<char, kFileChunk> chunk{};
        while (input) {
            operation_checkpoint(operation);
            input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto count = input.gcount();
            if (count <= 0) break;
            output.write(chunk.data(), count);
            if (!output) throw std::runtime_error("failed while writing SFX output");
            completed += static_cast<std::uint64_t>(count);
            report_operation(operation, OperationStage::writing, completed, total, 0, 2,
                             path.filename().string());
        }
    };
    copy(stub_executable);
    copy(archive_path);
    output.write(reinterpret_cast<const char*>(kSfxMagic.data()),
                 static_cast<std::streamsize>(kSfxMagic.size()));
    ByteVector size;
    put_u64(size, archive_size);
    output.write(reinterpret_cast<const char*>(size.data()),
                 static_cast<std::streamsize>(size.size()));
    output.close();
    if (!output) throw std::runtime_error("failed to finalize SFX output");
    fs::rename(temporary, output_executable, error);
    if (error) {
        fs::remove(output_executable, error);
        fs::rename(temporary, output_executable, error);
        if (error) throw std::runtime_error("cannot install SFX output: " + error.message());
    }
    guard.dismiss();
}

std::vector<ArchiveEntry> list_archive(const std::filesystem::path& archive_path,
                                       const std::string& password) {
    std::uint64_t file_size = 0;
    auto stream = open_archive(archive_path, file_size);
    const ByteSource source(stream, file_size);
    const auto loaded = load_index(source, password);
    const auto& index = loaded.index;

    std::vector<ArchiveEntry> result;
    result.reserve(index.entries.size());
    for (const auto& entry : index.entries) {
        ArchiveEntry out;
        out.path = entry.path;
        out.is_directory = entry.type == kEntryDir;
        out.is_symlink = entry.type == kEntrySymlink;
        out.is_hardlink = entry.type == kEntryHardlink;
        out.link_target = entry.link_target;
        out.size = entry.size;
        out.mtime = entry.mtime;
        out.crc32 = entry.crc;
        out.has_blake3 = entry.has_blake3;
        out.blake3 = entry.blake3;
        result.push_back(std::move(out));
    }
    return result;
}

void test_archive(const std::filesystem::path& archive_path,
                  const DecompressionOptions& options) {
    const auto operation = options.operation;
    std::uint64_t file_size = 0;
    auto stream = open_archive(archive_path, file_size);
    const ByteSource bytes(stream, file_size);
    auto loaded = load_index(bytes, options.password);
    const auto& index = loaded.index;
    if (index.meta.has_signature) {
        const auto layout = read_layout(bytes);
        const auto digest = archive_signature_digest(bytes, layout, index);
        if (!core::verify_message(index.meta.signature_public_key,
                                  index.meta.signature, digest)) {
            throw FormatError("archive authenticity signature is invalid");
        }
    }
    if (index.meta.encryption.enabled && !loaded.key) {
        throw std::runtime_error("archive is encrypted; a password is required");
    }
    BlockSource source(bytes, index, options.thread_count, operation, loaded.key);

    const auto total_bytes = archive_file_bytes(index);
    const auto total_items = static_cast<std::uint64_t>(index.entries.size());
    std::uint64_t completed_bytes = 0;
    std::uint64_t completed_items = 0;
    report_operation(operation, OperationStage::testing, completed_bytes, total_bytes,
                     completed_items, total_items);

    for (const auto& entry : index.entries) {
        operation_checkpoint(operation);
        if (entry.type == kEntryDir || entry.type == kEntrySymlink ||
            entry.type == kEntryHardlink) {
            // No block content to verify (links carry only a target, not bytes).
            ++completed_items;
            report_operation(operation, OperationStage::testing, completed_bytes, total_bytes,
                             completed_items, total_items, entry.path);
            continue;
        }
        auto crc = core::crc32_init();
        core::Blake3 hasher;
        read_file_bytes(source, index.blocks.size(), entry, operation,
                        [&](std::span<const std::uint8_t> bytes) {
                            crc = core::crc32_update(crc, bytes);
                            hasher.update(bytes);
                            completed_bytes += bytes.size();
                            report_operation(operation, OperationStage::testing,
                                             completed_bytes, total_bytes,
                                             completed_items, total_items, entry.path);
                        });
        if (core::crc32_final(crc) != entry.crc) {
            throw FormatError("checksum mismatch for archived file: " + entry.path);
        }
        if (entry.has_blake3 && hasher.finalize() != entry.blake3) {
            throw FormatError("BLAKE3 mismatch for archived file: " + entry.path);
        }
        ++completed_items;
        report_operation(operation, OperationStage::testing, completed_bytes, total_bytes,
                         completed_items, total_items, entry.path);
    }
}

namespace {

void extract_entries_impl(const std::filesystem::path& archive_path,
                          const std::vector<std::string>* requested_entries,
                          const std::filesystem::path& dest_dir,
                          const ExtractOptions& options) {
    const auto operation = options.operation;
    std::uint64_t file_size = 0;
    auto stream = open_archive(archive_path, file_size);
    const ByteSource bytes(stream, file_size);
    auto loaded = load_index(bytes, options.password);
    const auto& index = loaded.index;
    if (index.meta.encryption.enabled && !loaded.key) {
        throw std::runtime_error("archive is encrypted; a password is required");
    }
    BlockSource source(bytes, index, options.thread_count, operation, loaded.key);

    std::unordered_map<std::string, std::size_t> entry_by_path;
    entry_by_path.reserve(index.entries.size());
    for (std::size_t i = 0; i < index.entries.size(); ++i) {
        operation_checkpoint(operation);
        entry_by_path.emplace(index.entries[i].path, i);
    }
    std::vector<bool> selected(index.entries.size(), requested_entries == nullptr);
    if (requested_entries != nullptr) {
        std::unordered_set<std::string> unique_requests;
        for (const auto& requested : *requested_entries) {
            operation_checkpoint(operation);
            const std::string path = normalize_archive_path(requested, "archive entry");
            if (!unique_requests.insert(path).second) {
                continue;
            }
            const auto found = entry_by_path.find(path);
            bool matched = false;
            if (found != entry_by_path.end()) {
                selected[found->second] = true;
                matched = true;
            }
            if (found == entry_by_path.end() || index.entries[found->second].type == kEntryDir) {
                for (std::size_t i = 0; i < index.entries.size(); ++i) {
                    if (is_same_or_child(index.entries[i].path, path) &&
                        index.entries[i].path != path) {
                        selected[i] = true;
                        matched = true;
                    }
                }
            }
            if (!matched) {
                throw std::invalid_argument("archive entry does not exist: " + path);
            }
        }
    }

    std::uint64_t total_bytes = 0;
    std::uint64_t total_items = 0;
    for (std::size_t i = 0; i < index.entries.size(); ++i) {
        operation_checkpoint(operation);
        if (!selected[i]) {
            continue;
        }
        ++total_items;
        const auto& entry = index.entries[i];
        if (entry.type == kEntryFile) {
            total_bytes += entry.size;
        } else if (entry.type == kEntryHardlink) {
            const auto target = entry_by_path.find(entry.link_target);
            if (target == entry_by_path.end() || index.entries[target->second].type != kEntryFile) {
                throw FormatError("archive contains a dangling hard link: " + entry.path);
            }
            if (!selected[target->second]) {
                total_bytes += index.entries[target->second].size;
            }
        }
    }
    std::uint64_t completed_bytes = 0;
    std::uint64_t completed_items = 0;
    report_operation(operation, OperationStage::extracting, completed_bytes, total_bytes,
                     completed_items, total_items);

    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    const fs::path dest_norm = dest_dir.lexically_normal();

    // Directory timestamps are applied after every file lands, so extracting files
    // into a directory does not overwrite its restored mtime; applied deepest-first.
    struct DeferredDir {
        fs::path target;
        core::FileMetadata meta;
        std::int64_t mtime = 0;
    };
    std::vector<DeferredDir> deferred_dirs;

    for (std::size_t entry_index = 0; entry_index < index.entries.size(); ++entry_index) {
        if (!selected[entry_index]) {
            continue;
        }
        const auto& entry = index.entries[entry_index];
        operation_checkpoint(operation);
        if (!is_safe_relative(entry.path)) {
            throw FormatError("archive contains an unsafe path: " + entry.path);
        }
        const fs::path target = (dest_dir / fs::path(entry.path)).lexically_normal();
        if (!is_within(dest_norm, target)) {
            throw FormatError("archive path escapes the destination: " + entry.path);
        }
        // Real directory components must not be symlinks, or a write could be
        // redirected outside the destination through one of them.
        reject_symlinked_ancestor(dest_norm, target);

        if (entry.type == kEntrySymlink) {
            fs::create_directories(target.parent_path(), ec);
            if (fs::exists(fs::symlink_status(target, ec))) {
                if (options.overwrite == ExtractOptions::Overwrite::skip) {
                    ++completed_items;
                    report_operation(operation, OperationStage::extracting, completed_bytes,
                                     total_bytes, completed_items, total_items, entry.path);
                    continue;
                }
                if (options.overwrite == ExtractOptions::Overwrite::fail) {
                    throw std::runtime_error("target already exists: " + target.string());
                }
                fs::remove(target, ec);
            }
            const fs::path link_to(entry.link_target);
            const fs::path resolved =
                link_to.is_absolute() ? link_to : (target.parent_path() / link_to);
            std::error_code link_ec;
            if (fs::is_directory(resolved, ec)) {
                fs::create_directory_symlink(link_to, target, link_ec);
            } else {
                fs::create_symlink(link_to, target, link_ec);
            }
            // Best effort: creating a symlink can require privilege (Windows
            // without Developer Mode); on failure the rest of the archive still
            // extracts rather than aborting.
            ++completed_items;
            report_operation(operation, OperationStage::extracting, completed_bytes, total_bytes,
                             completed_items, total_items, entry.path);
            continue;
        }

        const EntryRec* file_entry = &entry;
        if (entry.type == kEntryHardlink) {
            const auto canonical = entry_by_path.find(entry.link_target);
            if (canonical == entry_by_path.end() ||
                index.entries[canonical->second].type != kEntryFile) {
                throw FormatError("archive contains a dangling hard link: " + entry.path);
            }
            if (!selected[canonical->second]) {
                // A selected hardlink whose canonical file is outside the selection
                // is materialized directly from the canonical entry. This keeps the
                // requested output self-contained without exposing unrelated paths.
                file_entry = &index.entries[canonical->second];
            } else {
            // The canonical file precedes its hard links in the directory, so its
            // target already exists on disk by the time we reach this entry.
            const fs::path link_to = (dest_dir / fs::path(entry.link_target)).lexically_normal();
            if (!is_within(dest_norm, link_to)) {
                throw FormatError("hardlink target escapes the destination: " +
                                  entry.link_target);
            }
            fs::create_directories(target.parent_path(), ec);
            if (fs::exists(fs::symlink_status(target, ec))) {
                if (options.overwrite == ExtractOptions::Overwrite::skip) {
                    ++completed_items;
                    report_operation(operation, OperationStage::extracting, completed_bytes,
                                     total_bytes, completed_items, total_items, entry.path);
                    continue;
                }
                if (options.overwrite == ExtractOptions::Overwrite::fail) {
                    throw std::runtime_error("target already exists: " + target.string());
                }
                fs::remove(target, ec);
            }
            std::error_code link_ec;
            fs::create_hard_link(link_to, target, link_ec);
            if (link_ec) {
                // Cross-device or unsupported FS: fall back to an independent copy
                // so the file still appears, even if it no longer shares an inode.
                fs::copy_file(link_to, target, fs::copy_options::overwrite_existing, link_ec);
            }
            ++completed_items;
            report_operation(operation, OperationStage::extracting, completed_bytes, total_bytes,
                             completed_items, total_items, entry.path);
            continue;
            }
        }

        if (entry.type == kEntryDir) {
            if (core::is_reparse_point(target)) {
                throw FormatError("refusing to restore a directory over a symlink: " + entry.path);
            }
            fs::create_directories(target, ec);
            // Attributes now (harmless to later child writes); timestamps deferred.
            core::apply_metadata(target, entry.meta, /*restore_times=*/false);
            deferred_dirs.push_back({target, entry.meta, entry.mtime});
            ++completed_items;
            report_operation(operation, OperationStage::extracting, completed_bytes, total_bytes,
                             completed_items, total_items, entry.path);
            continue;
        }

        fs::create_directories(target.parent_path(), ec);
        if (fs::exists(target, ec)) {
            if (options.overwrite == ExtractOptions::Overwrite::skip) {
                completed_bytes += file_entry->size;
                ++completed_items;
                report_operation(operation, OperationStage::extracting, completed_bytes, total_bytes,
                                 completed_items, total_items, entry.path);
                continue;
            }
            if (options.overwrite == ExtractOptions::Overwrite::fail) {
                throw std::runtime_error("target already exists: " + target.string());
            }
        }

        fs::path temp_target = target;
        temp_target += ".axtmp";
        TempFileGuard temp_guard(temp_target);
        {
            std::ofstream file_out(temp_target, std::ios::binary | std::ios::trunc);
            if (!file_out) {
                throw std::runtime_error("cannot write file: " + temp_target.string());
            }
            read_file_bytes(source, index.blocks.size(), *file_entry, operation,
                            [&](std::span<const std::uint8_t> bytes) {
                                operation_checkpoint(operation);
                                file_out.write(reinterpret_cast<const char*>(bytes.data()),
                                               static_cast<std::streamsize>(bytes.size()));
                                if (!file_out) {
                                    throw std::runtime_error("failed writing file: " + temp_target.string());
                                }
                                completed_bytes += bytes.size();
                                report_operation(operation, OperationStage::extracting,
                                                 completed_bytes, total_bytes,
                                                 completed_items, total_items, entry.path);
                            });
        }

        fs::rename(temp_target, target, ec);
        if (ec) {
            fs::remove(target, ec);
            fs::rename(temp_target, target, ec);
            if (ec) {
                throw std::runtime_error("failed to move extracted file into place: " + ec.message());
            }
        }
        temp_guard.dismiss();

        // NTFS named streams are written before timestamps so restoring the write
        // time isn't disturbed by the stream writes that follow it.
        core::apply_ads(target, file_entry->ads);
        // High-precision Windows times (when present) supersede the seconds mtime.
        core::apply_metadata(target, file_entry->meta, options.restore_mtime);
        if (options.restore_mtime && !file_entry->meta.has_windows_times &&
            file_entry->mtime != 0) {
            try {
                fs::last_write_time(target, from_unix_seconds(file_entry->mtime), ec);
            } catch (...) {
                // best effort
            }
        }
        ++completed_items;
        report_operation(operation, OperationStage::extracting, completed_bytes, total_bytes,
                         completed_items, total_items, entry.path);
    }

    // Restore directory timestamps last (deepest-first) so nothing written into a
    // directory afterward disturbs its restored time.
    for (auto it = deferred_dirs.rbegin(); it != deferred_dirs.rend(); ++it) {
        core::apply_metadata(it->target, it->meta, options.restore_mtime);
        if (options.restore_mtime && !it->meta.has_windows_times && it->mtime != 0) {
            try {
                fs::last_write_time(it->target, from_unix_seconds(it->mtime), ec);
            } catch (...) {
                // best effort
            }
        }
    }
}

}  // namespace

void extract_archive(const std::filesystem::path& archive_path,
                     const std::filesystem::path& dest_dir,
                     const ExtractOptions& options) {
    extract_entries_impl(archive_path, nullptr, dest_dir, options);
}

void extract_entries(const std::filesystem::path& archive_path,
                     const std::vector<std::string>& entries,
                     const std::filesystem::path& dest_dir,
                     const ExtractOptions& options) {
    extract_entries_impl(archive_path, &entries, dest_dir, options);
}

namespace detail {

void fuzz_read_archive(std::span<const std::uint8_t> bytes) {
    const ByteSource source(bytes);
    const auto index = read_index(source);
    BlockSource blocks(source, index, 0, nullptr);
    for (const auto& entry : index.entries) {
        if (entry.type == kEntryDir) {
            continue;
        }
        read_file_bytes(blocks, index.blocks.size(), entry, nullptr,
                        [](std::span<const std::uint8_t>) {});
    }
}

}  // namespace detail

}  // namespace axiom
