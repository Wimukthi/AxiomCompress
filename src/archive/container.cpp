#include "axiom/archive.hpp"

#include "archive/container_internal.hpp"
#include "archive/sfx_image.hpp"
#include "codec/block.hpp"
#include "codec/external_codecs.hpp"
#include "codec/transform.hpp"
#include "archive/fuzz_support.hpp"
#include "core/checksum.hpp"
#include "core/archive.hpp"
#include "core/cpu.hpp"
#include "core/crypto.hpp"
#include "core/file_meta.hpp"
#include "core/file_replace.hpp"
#include "core/path_text.hpp"
#include "core/hash.hpp"
#include "core/reed_solomon.hpp"
#include "core/task_executor.hpp"
#include "archive/system_provider.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace axiom {

bool open_input_with_retry(std::ifstream& input, const std::filesystem::path& path,
                           unsigned retries,
                           const std::shared_ptr<OperationControl>& operation);

namespace {

int compression_type_class(const ScanItem& item) {
    if (item.is_directory || item.is_symlink || item.is_reparse_point) return 0;
    const auto extension = lower_ascii(item.absolute.extension().wstring());
    static constexpr std::array<std::wstring_view, 17> text{
        L".txt", L".md", L".csv", L".tsv", L".json", L".xml", L".html",
        L".htm", L".css", L".js", L".ts", L".cpp", L".c", L".hpp",
        L".h", L".py", L".log"};
    static constexpr std::array<std::wstring_view, 8> executable{
        L".exe", L".dll", L".sys", L".ocx", L".cpl", L".scr", L".com", L".efi"};
    static constexpr std::array<std::wstring_view, 4> raw_media{
        L".wav", L".bmp", L".dib", L".tiff"};
    static constexpr std::array<std::wstring_view, 24> compressed{
        L".7z", L".zip", L".rar", L".gz", L".xz", L".bz2", L".zst",
        L".jpg", L".jpeg", L".png", L".gif", L".webp", L".mp3", L".aac",
        L".flac", L".mp4", L".mkv", L".avi", L".pdf", L".docx", L".xlsx",
        L".pptx", L".epub", L".msi"};
    const auto in = [&](const auto& values) {
        return std::find(values.begin(), values.end(), extension) != values.end();
    };
    if (in(text)) return 1;
    if (in(executable)) return 2;
    if (in(raw_media)) return 3;
    if (in(compressed)) return 5;
    return 4;
}

std::vector<const ScanItem*> compression_order(const std::vector<ScanItem>& items) {
    std::vector<const ScanItem*> ordered;
    ordered.reserve(items.size());
    for (const auto& item : items) ordered.push_back(&item);
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        const auto left_class = compression_type_class(*left);
        const auto right_class = compression_type_class(*right);
        if (left_class != right_class) return left_class < right_class;
        if (left_class == 0) return false;
        return lower_ascii(left->absolute.extension().wstring()) <
               lower_ascii(right->absolute.extension().wstring());
    });
    return ordered;
}

namespace fs = std::filesystem;

// kArchiveMagic lives in container_internal.hpp (container_formats.cpp sniffs it too).
constexpr std::uint16_t kArchiveVersion4 = 4;
constexpr std::uint16_t kArchiveVersion5 = 5;
constexpr std::size_t kHeaderSize = 16;
// Header flag: the central directory is sealed and the encryption parameters live in
// a plaintext preamble after the header (rather than in the directory's TLV). Old
// readers reject this flag, so it doubles as the compatibility gate.
constexpr std::uint16_t kFlagEncryptedDirectory = 0x0001;
// AXAR v5 required-feature flags. They are deliberately explicit so a reader
// can reject a v5 archive whose fidelity metadata it cannot safely apply.
constexpr std::uint16_t kFlagSparseEntries = 0x0002;
constexpr std::uint16_t kFlagCaptureReport = 0x0004;
constexpr std::uint16_t kFlagExtendedMetadata = 0x0008;
// AXAR v5 encryption profile: a random archive data key is wrapped by one or
// more independently salted password slots. This is a required feature flag so
// older readers cannot mistake the v2 preamble/TLV for legacy KDF metadata.
constexpr std::uint16_t kFlagEncryptionV2 = 0x0010;
// Snapshot repositories use non-contiguous chunk references in their entry
// manifests. Older readers must reject this required feature instead of using
// the legacy first_block/offset range as if all chunks were adjacent.
constexpr std::uint16_t kFlagChunkTable = 0x0020;
// Large solid blocks are staged and read as bounded AXC external-codec
// subframes. Older readers must reject this profile because their block-size
// safety limit is intentionally lower and they do not know the streaming path.
constexpr std::uint16_t kFlagLargeSolidBlocks = 0x0040;
// Ordinary deduplicated archives use the same chunk table and entry references
// as snapshot repositories, but have one mutable live directory and no history.
// Keeping a distinct required bit lets old readers reject the non-contiguous
// addressing without conflating it with snapshot-history semantics.
constexpr std::uint16_t kFlagLiveDedup = 0x0080;
constexpr std::uint16_t kKnownArchiveFlags = kFlagEncryptedDirectory |
                                               kFlagSparseEntries |
                                               kFlagCaptureReport |
                                               kFlagExtendedMetadata |
                                               kFlagEncryptionV2 |
                                               kFlagChunkTable |
                                               kFlagLargeSolidBlocks |
                                               kFlagLiveDedup;

// ---- Frozen format constants ------------------------------------------------
// These values are the published compatibility contract: see the Compatibility
// section of FORMAT.md and docs/VERSIONING.md. Every archive already written
// carries them, so changing one is a deliberate format revision, never a
// refactor. The golden fixtures under tests/fixtures/ pin the same values as
// recorded bytes, and format_freeze_golden_profiles checks them.
static_assert(kArchiveMagic == std::array<std::uint8_t, 8>{'A', 'X', 'I', 'O',
                                                          'M', 'A', 'R', 0},
              "the AXAR magic prefix is frozen");
static_assert(kHeaderSize == 16, "the AXAR header is frozen at 16 bytes");
static_assert(kArchiveVersion4 == 4, "AXAR v4 is the frozen read baseline");
static_assert(kArchiveVersion5 == 5, "AXAR v5 is the frozen default write version");
static_assert(kFlagEncryptedDirectory == 0x0001, "frozen required-flag bit");
static_assert(kFlagSparseEntries == 0x0002, "frozen required-flag bit");
static_assert(kFlagCaptureReport == 0x0004, "frozen required-flag bit");
static_assert(kFlagExtendedMetadata == 0x0008, "frozen required-flag bit");
static_assert(kFlagEncryptionV2 == 0x0010, "frozen required-flag bit");
static_assert(kFlagChunkTable == 0x0020, "frozen required-flag bit");
static_assert(kFlagLargeSolidBlocks == 0x0040, "frozen required-flag bit");
static_assert(kFlagLiveDedup == 0x0080, "frozen required-flag bit");
// Bits 0x0100 and up are unassigned. A new required flag may only come from
// that range, and only for a profile that stays off by default, so an archive
// written with default options keeps opening on every 1.x reader.
static_assert(kKnownArchiveFlags == 0x00FF, "the assigned required-flag set is frozen");

// Optional block-extra profile. Older AXAR readers already skip the reserved
// block-extra byte range, so this remains additive and does not require a new
// AXAR header version or feature flag.
constexpr std::uint64_t kBlockExtraSubframeMap = 1;
constexpr std::uint64_t kSubframeStore = 1;
constexpr std::uint64_t kSubframeParallelBlock = 2;
constexpr std::uint64_t kSubframeExternalChunk = 3;
constexpr std::uint64_t kSubframeMapVersion = 1;
constexpr std::uint64_t kMaxSubframesPerBlock = 1u << 20;
// AEAD associated data tag for a sealed directory (distinct from any block index).
constexpr std::array<std::uint8_t, 8> kDirectoryAd = {'A', 'X', 'D', 'I', 'R', 0, 0, 0};
constexpr std::array<std::uint8_t, 8> kEncryptionV2Magic = {
    'A', 'X', 'I', 'O', 'M', 'E', '2', 0};
constexpr std::uint16_t kEncryptionV2Version = 2;
constexpr std::size_t kEncryptionKeyIdSize = 16;
constexpr std::uint64_t kMaxEncryptionSlots = 16;
constexpr std::size_t kWrappedEncryptionKeySize =
    sizeof(core::CryptoKey) + core::kAeadOverhead;
constexpr std::array<std::uint8_t, 8> kEncryptionSlotAd = {
    'A', 'X', 'I', 'O', 'M', '-', 'S', 'L'};
constexpr std::size_t kFooterSize = 24;
// Generation metadata deliberately precedes the optional recovery body and the
// legacy 24-byte footer. Keeping the legacy footer as the final suffix means
// existing v4/v5 readers still open the newest complete generation.
constexpr std::size_t kGenerationExtensionSize = 64;
constexpr std::array<std::uint8_t, 8> kGenerationMagic = {
    'A', 'X', 'I', 'O', 'M', 'G', 'F', 0};
constexpr std::uint16_t kGenerationVersion = 1;
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
constexpr std::size_t kDefaultIoBufferSize = 1u << 20;
constexpr std::size_t kMaxIoBufferSize = 64u << 20;
constexpr std::size_t kMinAutoCodecBlockSize = std::size_t{1} << 20;
constexpr std::uint64_t kMaxLegacySolidBlockSize = std::uint64_t{4} << 30;
constexpr std::uint64_t kMaxLargeSolidBlockSize = std::uint64_t{64} << 30;
// Large-solid writes default to the conservative 512 MiB working chunk, but
// the AXEC u32 geometry can deliberately be raised to the full LZMA2 limit.
constexpr std::size_t kLargeSolidCodecChunkSize = std::size_t{512} << 20;
constexpr std::size_t kMaxLzmaCodecChunkSize = kMaxLzmaDictionarySize;

std::size_t effective_io_buffer_size(std::size_t requested) {
    if (requested == 0) {
        return kDefaultIoBufferSize;
    }
    return std::clamp(requested, kFileChunk, kMaxIoBufferSize);
}

std::size_t selected_thread_count(std::size_t requested_threads) {
    return requested_threads == 0 ? core::logical_processor_count() : requested_threads;
}

std::size_t selected_geometry_thread_count(std::size_t requested_threads) {
    // Block geometry follows physical cores so SMT helpers do not create
    // smaller match windows. The executor still receives the explicit logical
    // thread request for nested work and independent archive operations.
    const auto physical_threads = std::max<std::size_t>(1, core::physical_core_count());
    return requested_threads == 0
        ? physical_threads
        : std::min(requested_threads, physical_threads);
}

std::size_t effective_solid_block_size(const CompressionOptions& options) {
    const auto block_size = std::max<std::size_t>(1, options.block_size);
    if (!options.auto_block_size_for_threads) {
        return block_size;
    }

    const auto threads = selected_geometry_thread_count(options.thread_count);
    if (threads <= 1) {
        return block_size;
    }

    // The archive container batches files into solid blocks, then the codec splits
    // each solid block internally. Keep the outer block large enough to feed one
    // independent codec block per detected/requested worker by default. With the
    // optimal parser on, the codec keeps its sub-blocks at 4 MiB or larger (see
    // the thorough block floor in core/archive.cpp), so the outer block must
    // scale with that floor or a many-core machine runs half idle.
    const auto per_worker = options.enable_optimal_parser
        ? std::size_t{4} << 20
        : kMinAutoCodecBlockSize;
    if (threads > std::numeric_limits<std::size_t>::max() / per_worker) {
        return block_size;
    }
    return std::max(block_size, threads * per_worker);
}

bool requests_large_solid_blocks(std::size_t block_size) {
    return static_cast<std::uint64_t>(block_size) > kMaxLegacySolidBlockSize;
}

void validate_large_solid_block_options(const CompressionOptions& options,
                                        std::size_t block_size) {
    if (!requests_large_solid_blocks(block_size)) {
        return;
    }
    if (static_cast<std::uint64_t>(block_size) > kMaxLargeSolidBlockSize) {
        throw std::invalid_argument("solid block size exceeds the 64 GiB AXAR limit");
    }
    if (options.method != CompressionMethod::lzma2) {
        throw std::invalid_argument(
            "solid blocks larger than 4 GiB currently require the LZMA2 method");
    }
    if (!options.password.empty()) {
        throw std::invalid_argument(
            "LZMA2 solid blocks larger than 4 GiB cannot be encrypted yet");
    }
    if (options.recovery_percent != 0) {
        throw std::invalid_argument(
            "recovery records are not supported with solid blocks larger than 4 GiB yet");
    }
}

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
constexpr std::uint64_t kExtraSparseMap = 8; // version + allocated extent list
constexpr std::uint64_t kExtraSecurityDescriptor = 9; // self-relative Windows descriptor
constexpr std::uint64_t kExtraXattr = 10;    // vint name length, name, raw value
constexpr std::uint64_t kExtraReparse = 11; // tag + opaque Windows reparse buffer
constexpr std::uint64_t kExtraChunkRefs = 12; // version + content chunk indices

// Archive-level extra record types (the TLV area after the entry list). Separate
// numbering from the per-entry extras above; unknown types are skipped by length.
constexpr std::uint64_t kArchiveComment = 1;  // UTF-8 archive comment (payload = the text)
constexpr std::uint64_t kArchiveLock = 2;     // presence marks the archive read-only (no payload)
constexpr std::uint64_t kArchiveEncryption = 3;  // KDF params + salt + password key-check token
constexpr std::uint64_t kArchiveSignature = 4;   // public key (32) + EdDSA signature (64)
constexpr std::uint64_t kArchiveCaptureReport = 5; // omitted/lossy source capture warnings
constexpr std::uint64_t kArchiveEncryptionV2 = 6; // key id + password slots
constexpr std::uint64_t kArchiveChunkTable = 7; // content-defined chunk index
constexpr std::uint64_t kArchiveSnapshotManifest = 8; // named snapshot entry manifests
constexpr std::uint64_t kArchiveDedupProfile = 9; // immutable content-defined chunk geometry

// ---- little-endian serialization -------------------------------------------

// put_u16 is shared with the other archive TUs and lives in container_internal.hpp.

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

ByteVector archive_header_bytes(std::uint16_t version, std::uint16_t flags) {
    ByteVector header;
    header.insert(header.end(), kArchiveMagic.begin(), kArchiveMagic.end());
    put_u16(header, version);
    put_u16(header, flags);
    put_u32(header, 0);  // reserved
    return header;
}

void patch_archive_header(std::ofstream& out, std::uint16_t version,
                          std::uint16_t flags) {
    const auto header = archive_header_bytes(version, flags);
    out.flush();
    if (!out) {
        throw std::runtime_error("failed before patching archive header");
    }
    out.seekp(0, std::ios::beg);
    out.write(reinterpret_cast<const char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    out.seekp(0, std::ios::end);
    if (!out) {
        throw std::runtime_error("failed to patch archive header");
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
            // At shift 63 only bit zero fits in a u64. Reject the other six
            // payload bits instead of silently wrapping a hostile length.
            if (shift == 63 && (byte & 0x7Eu) != 0) {
                throw FormatError("archive varint overflows 64 bits");
            }
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
        if (count > data_.size() - cursor_) {
            throw FormatError("archive directory is truncated");
        }
    }

    std::span<const std::uint8_t> data_;
    std::size_t cursor_;
};

// ---- timestamps ------------------------------------------------------------

}  // namespace

namespace {

template <typename Clock, typename Duration>
auto file_time_to_system(std::chrono::time_point<Clock, Duration> stamp) {
    if constexpr (requires { Clock::to_sys(stamp); }) {
        return Clock::to_sys(stamp);
    } else {
        // Older standard libraries expose neither file_clock::to_sys nor
        // clock_cast. Preserve the clock offset at the instant of conversion.
        return std::chrono::system_clock::now() +
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                stamp - Clock::now());
    }
}

template <typename Clock, typename Duration>
auto system_time_to_file(
    std::chrono::time_point<std::chrono::system_clock, Duration> stamp) {
    if constexpr (requires { Clock::from_sys(stamp); }) {
        return Clock::from_sys(stamp);
    } else {
        return Clock::now() +
            std::chrono::duration_cast<typename Clock::duration>(
                stamp - std::chrono::system_clock::now());
    }
}

}  // namespace

// Shared with the other archive TUs via container_internal.hpp.
std::int64_t to_unix_seconds(fs::file_time_type stamp) {
    const auto system = file_time_to_system(stamp);
    return std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
}

fs::file_time_type from_unix_seconds(std::int64_t seconds) {
    const std::chrono::system_clock::time_point system{std::chrono::seconds{seconds}};
    return system_time_to_file<fs::file_time_type::clock>(system);
}

// ---- path safety -----------------------------------------------------------
// Shared with the other archive TUs via container_internal.hpp.

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
                              core::generic_path_to_utf8(
                                  it->lexically_relative(dest_norm)));
        }
    }
}

namespace {

// ---- archive model ---------------------------------------------------------

struct SubframeRec {
    // All offsets are relative to the surrounding AXC block. The compressed
    // range includes the complete independently decodable frame header.
    std::uint64_t uncompressed_offset = 0;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t compressed_offset = 0;
    std::uint64_t compressed_size = 0;
    std::uint8_t kind = 0;
    // Parallel frames carry their inner block codec. External frames carry
    // the outer AXC codec id (zstandard/lzma2/deflate).
    std::uint8_t codec = 0;
    std::uint8_t lzma_property = 0;
};

struct BlockRec {
    std::uint64_t compressed_offset = 0;
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;
    std::vector<SubframeRec> subframes;
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
    core::SparseFileMap sparse;
    std::string link_target;  // symlink target (verbatim) for kEntrySymlink
    std::vector<core::AdsStream> ads;  // NTFS alternate data streams (kEntryFile)
    std::vector<std::uint64_t> chunk_refs;
};

struct SnapshotRec {
    std::string name;
    std::uint64_t generation = 0;
    std::int64_t created = 0;
    std::vector<EntryRec> entries;
};

// Exact content identity used for safe reuse. CRC-32 keeps the lookup cheap to
// inspect while BLAKE3 provides the collision-resistant identity and size binds
// the identity to the complete logical file.
struct ContentIdentity {
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
    core::Blake3Digest blake3{};

    bool operator==(const ContentIdentity&) const = default;

    bool operator<(const ContentIdentity& other) const {
        if (size != other.size) return size < other.size;
        if (crc != other.crc) return crc < other.crc;
        return std::lexicographical_compare(
            blake3.begin(), blake3.end(), other.blake3.begin(), other.blake3.end());
    }
};

// Snapshot chunk identity is separate from the ordinary file identity. When
// keyed identifiers are enabled, `id` is a keyed BLAKE3 digest; the plaintext
// CRC remains a chunk-integrity value but is not used for lookup.
struct ChunkIdentity {
    std::uint64_t size = 0;
    core::Blake3Digest id{};

    bool operator==(const ChunkIdentity&) const = default;

    bool operator<(const ChunkIdentity& other) const {
        if (size != other.size) return size < other.size;
        return std::lexicographical_compare(
            id.begin(), id.end(), other.id.begin(), other.id.end());
    }
};

struct ChunkRec {
    ChunkIdentity identity;
    std::uint32_t crc = 0;
    std::uint64_t block_index = 0;
    std::uint64_t offset = 0;
};

struct ArchiveDataReference {
    std::uint64_t first_block = 0;
    std::uint64_t offset = 0;
};

struct ReuseCandidate {
    ContentIdentity identity;
    ArchiveDataReference data;
    std::int64_t source_stamp = 0;
};

struct ArchiveReuseStats {
    std::uint64_t reused_items = 0;
    std::uint64_t reused_bytes = 0;
};

struct DedupProfile {
    bool present = false;
    std::uint64_t chunker = 1;          // gear64
    std::uint64_t chunker_version = 1;
    std::uint64_t minimum_size = 256u << 10;
    std::uint64_t average_size = 1u << 20;
    std::uint64_t maximum_size = 4u << 20;
    std::uint64_t table_id = 1;
    std::uint64_t packing = 0;          // one independently coded block per chunk
};

using ArchiveReuseByPath = std::unordered_map<std::string, ReuseCandidate>;

ContentIdentity content_identity(const EntryRec& entry) {
    return {entry.size, entry.crc, entry.blake3};
}

std::map<ContentIdentity, ArchiveDataReference> build_reuse_candidates(
    const std::vector<EntryRec>& entries) {
    std::map<ContentIdentity, ArchiveDataReference> candidates;
    for (const auto& entry : entries) {
        if (entry.type != kEntryFile || !entry.has_blake3) continue;
        candidates.emplace(content_identity(entry),
                           ArchiveDataReference{entry.first_block, entry.offset});
    }
    return candidates;
}

// Password-encryption parameters recorded in the directory (present only when the
// archive is encrypted). v1 stores one password-derived block key directly; v2
// stores a random data key wrapped independently by each password slot.
struct EncryptionSlot {
    std::uint32_t id = 0;
    core::KdfParams kdf;
    std::vector<std::uint8_t> wrapped_key;
};

struct EncryptionInfo {
    bool enabled = false;
    bool encrypt_directory = false;  // directory sealed + params in the header preamble
    bool v2 = false;
    core::KdfParams kdf;
    std::vector<std::uint8_t> key_check;
    std::array<std::uint8_t, kEncryptionKeyIdSize> key_id{};
    std::vector<EncryptionSlot> slots;
};

// Archive-wide metadata stored in the directory's trailing TLV area.
struct ArchiveMeta {
    std::string comment;     // free-form UTF-8 comment (empty = none)
    bool locked = false;     // read-only: edit operations refuse to modify the archive
    EncryptionInfo encryption;
    bool has_signature = false;
    std::array<std::uint8_t, 32> signature_public_key{};
    std::array<std::uint8_t, 64> signature{};
    std::vector<OperationWarning> capture_warnings;
    bool chunk_table = false;
    bool live_dedup = false;
    bool keyed_chunk_ids = false;
    bool large_solid_blocks = false;
    DedupProfile dedup_profile;
    std::vector<SnapshotRec> snapshots;
};

// Fixed plaintext sealed under the archive key to verify a password without touching
// any block; its AEAD also binds the KDF salt as associated data.
constexpr std::array<std::uint8_t, 16> kKeyCheckPlaintext = {
    'a', 'x', 'i', 'o', 'm', '-', 'k', 'e', 'y', 'c', 'h', 'e', 'c', 'k', '0', '1'};

struct ArchiveIndex {
    std::vector<BlockRec> blocks;
    std::vector<EntryRec> entries;
    std::vector<ChunkRec> chunks;
    ArchiveMeta meta;
};

constexpr std::uint64_t kSnapshotManifestVersion = 1;
constexpr std::uint64_t kChunkTableVersion = 1;
constexpr std::uint64_t kDedupProfileVersion = 1;
constexpr std::uint64_t kMaxSnapshotCount = 1u << 16;
constexpr std::uint64_t kMaxSnapshotEntries = 1u << 24;
constexpr std::uint64_t kMaxChunkCount = 1u << 24;
constexpr std::uint64_t kMaxChunkRefsPerEntry = 1u << 20;
constexpr std::size_t kMaxSnapshotNameBytes = 256;

void validate_dedup_profile(const DedupProfile& profile, bool format_error);

// These are the smallest serialized records that can reach the corresponding
// parsers. Count fields are untrusted, so they must not authorize more records
// than the enclosing payload can possibly contain before a vector grows.
constexpr std::size_t kMinChunkTableRecordBytes = 1 + sizeof(std::uint32_t) + 32 + 1 + 1;
constexpr std::size_t kMinSnapshotEntryRecordBytes = 1 + 1 + 1;

void validate_snapshot_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxSnapshotNameBytes) {
        throw std::invalid_argument("snapshot name must be 1..256 bytes");
    }
    if (name == "." || name == ".." || name.find('/') != std::string_view::npos ||
        name.find('\\') != std::string_view::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char value) {
            return value < 0x20 || value == 0x7F;
        })) {
        throw std::invalid_argument("snapshot name contains an unsupported character");
    }
}

void validate_snapshot_entry_paths_for_write(const std::vector<EntryRec>& entries) {
    if (entries.size() > kMaxSnapshotEntries) {
        throw std::invalid_argument("snapshot contains too many entries");
    }

    std::vector<std::pair<std::string, std::uint8_t>> ordered;
    ordered.reserve(entries.size());
    std::unordered_set<std::string> seen;
    seen.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.type > kEntryHardlink) {
            throw std::invalid_argument("snapshot contains an unknown entry type");
        }
        const auto normalized = normalize_archive_path(entry.path, "snapshot archive path");
        if (normalized != entry.path) {
            throw std::invalid_argument("snapshot archive path is not normalized: " +
                                        entry.path);
        }
        if (!seen.insert(entry.path).second) {
            throw std::invalid_argument("snapshot contains a duplicate path: " + entry.path);
        }
        if (entry.type == kEntryHardlink) {
            const auto target = normalize_archive_path(entry.link_target,
                                                        "snapshot hard-link target");
            if (target != entry.link_target) {
                throw std::invalid_argument(
                    "snapshot hard-link target is not normalized: " + entry.link_target);
            }
        }
        ordered.emplace_back(entry.path, entry.type);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& first, const auto& second) {
                  return first.first < second.first;
              });
    for (std::size_t index = 1; index < ordered.size(); ++index) {
        if (is_same_or_child(ordered[index].first, ordered[index - 1].first) &&
            ordered[index - 1].second != kEntryDir) {
            throw std::invalid_argument(
                "snapshot has a non-directory entry with children: " +
                ordered[index - 1].first);
        }
    }
}

void validate_snapshot_entry_paths_for_read(const std::vector<EntryRec>& entries) {
    if (entries.size() > kMaxSnapshotEntries) {
        throw FormatError("snapshot contains too many entries");
    }

    std::vector<std::pair<std::string, std::uint8_t>> ordered;
    ordered.reserve(entries.size());
    std::unordered_set<std::string> seen;
    seen.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.type > kEntryHardlink) {
            throw FormatError("snapshot contains an unknown entry type");
        }
        try {
            if (normalize_archive_path(entry.path, "snapshot manifest path") != entry.path) {
                throw FormatError("snapshot manifest path is not normalized");
            }
        } catch (const std::invalid_argument&) {
            throw FormatError("snapshot manifest path is not safe");
        }
        if (!seen.insert(entry.path).second) {
            throw FormatError("snapshot manifest contains a duplicate path");
        }
        if (entry.type == kEntryHardlink) {
            try {
                if (normalize_archive_path(entry.link_target,
                                           "snapshot hard-link target") != entry.link_target) {
                    throw FormatError("snapshot hard-link target is not normalized");
                }
            } catch (const std::invalid_argument&) {
                throw FormatError("snapshot hard-link target is not safe");
            }
        }
        ordered.emplace_back(entry.path, entry.type);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& first, const auto& second) {
                  return first.first < second.first;
              });
    for (std::size_t index = 1; index < ordered.size(); ++index) {
        if (is_same_or_child(ordered[index].first, ordered[index - 1].first) &&
            ordered[index - 1].second != kEntryDir) {
            throw FormatError("snapshot has a non-directory entry with children");
        }
    }
}

core::Blake3Digest chunk_digest(std::span<const std::uint8_t> bytes,
                                const core::CryptoKey* key, bool keyed) {
    if (!keyed) return core::Blake3::hash(bytes);
    if (key == nullptr) {
        throw std::invalid_argument("keyed chunk identifiers require archive encryption");
    }
    blake3_hasher hasher;
    blake3_hasher_init_keyed(&hasher, key->data());
    blake3_hasher_update(&hasher, bytes.data(), bytes.size());
    core::Blake3Digest digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    return digest;
}

ChunkIdentity make_chunk_identity(std::span<const std::uint8_t> bytes,
                                  const core::CryptoKey* key, bool keyed) {
    return {static_cast<std::uint64_t>(bytes.size()), chunk_digest(bytes, key, keyed)};
}

std::array<std::uint64_t, 256> snapshot_gear_table() {
    std::array<std::uint64_t, 256> table{};
    std::uint64_t state = 0xA710C0DE5EED1234ull;
    for (auto& value : table) {
        // splitmix64 gives a stable, platform-independent gear table. This is
        // not cryptographic; the BLAKE3 identity below remains authoritative.
        state += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        value = z ^ (z >> 31);
    }
    return table;
}

void validate_snapshot_chunk_sizes(const CompressionOptions& options) {
    const auto min_size = options.snapshot_min_chunk_size;
    const auto average_size = options.snapshot_average_chunk_size;
    const auto max_size = options.snapshot_max_chunk_size;
    if (min_size < (4u << 10) || average_size < min_size || max_size < average_size ||
        max_size > (64u << 20)) {
        throw std::invalid_argument(
            "snapshot chunk sizes must satisfy 4 KiB <= min <= average <= max <= 64 MiB");
    }
}

DedupProfile dedup_profile_from_options(const CompressionOptions& options) {
    validate_snapshot_chunk_sizes(options);
    DedupProfile profile;
    profile.present = true;
    profile.minimum_size = options.snapshot_min_chunk_size;
    profile.average_size = options.snapshot_average_chunk_size;
    profile.maximum_size = options.snapshot_max_chunk_size;
    validate_dedup_profile(profile, false);
    return profile;
}

CompressionOptions options_for_dedup_profile(const CompressionOptions& options,
                                             const DedupProfile& profile) {
    validate_dedup_profile(profile, true);
    auto result = options;
    result.snapshot_min_chunk_size = static_cast<std::size_t>(profile.minimum_size);
    result.snapshot_average_chunk_size = static_cast<std::size_t>(profile.average_size);
    result.snapshot_max_chunk_size = static_cast<std::size_t>(profile.maximum_size);
    return result;
}

template <typename Callback>
void read_content_defined_chunks(const fs::path& path,
                                 const CompressionOptions& options,
                                 const std::shared_ptr<OperationControl>& operation,
                                 Callback&& callback) {
    validate_snapshot_chunk_sizes(options);
    std::ifstream input;
    if (!open_input_with_retry(input, path, options.input_open_retries, operation)) {
        throw std::runtime_error("cannot read input file: " + core::path_to_utf8(path));
    }
    const auto gear = snapshot_gear_table();
    std::size_t mask = 1;
    while (mask < options.snapshot_average_chunk_size &&
           mask <= std::numeric_limits<std::size_t>::max() / 2) {
        mask <<= 1;
    }
    --mask;

    ByteVector chunk;
    chunk.reserve(options.snapshot_max_chunk_size);
    std::uint64_t fingerprint = 0;
    std::vector<char> buffer(effective_io_buffer_size(options.io_buffer_size));
    while (input) {
        operation_checkpoint(operation);
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) break;
        for (std::streamsize index = 0; index < count; ++index) {
            const auto byte = static_cast<std::uint8_t>(buffer[static_cast<std::size_t>(index)]);
            chunk.push_back(byte);
            fingerprint = (fingerprint << 1) ^ gear[byte];
            const bool at_min = chunk.size() >= options.snapshot_min_chunk_size;
            const bool at_cut = at_min && ((fingerprint & mask) == 0);
            const bool at_max = chunk.size() >= options.snapshot_max_chunk_size;
            if (at_cut || at_max) {
                callback(std::move(chunk));
                chunk = {};
                chunk.reserve(options.snapshot_max_chunk_size);
                fingerprint = 0;
            }
        }
    }
    if (input.bad()) {
        throw std::runtime_error("failed while reading input file: " +
                                 core::path_to_utf8(path));
    }
    if (!chunk.empty()) callback(std::move(chunk));
}

std::vector<SubframeRec> make_subframe_map(std::span<const std::uint8_t> compressed) {
    const auto header = core::read_archive_header(compressed);
    // Filters are defined over the complete logical stream. Until the inverse
    // transform can be proven local to a selected range, such blocks retain the
    // safe whole-block path.
    if (!header.transform_ranges.empty() || header.original_size == 0) {
        return {};
    }
    const auto payload = core::archive_payload(compressed, header);
    std::vector<SubframeRec> result;
    if (header.codec == core::CodecId::store) {
        if (payload.empty()) return {};
        result.push_back({0,
                          header.original_size,
                          header.payload_offset,
                          payload.size(),
                          static_cast<std::uint8_t>(kSubframeStore),
                          0,
                          0});
        return result;
    }
    if (header.codec == core::CodecId::parallel_blocks) {
        const auto frames = codec::inspect_parallel_block_frames(
            payload, static_cast<std::size_t>(header.original_size),
            header.format_version >= 6,
            header.format_version >= 7,
            header.format_version >= 8,
            header.format_version >= 9);
        result.reserve(frames.size());
        for (const auto& frame : frames) {
            result.push_back({frame.uncompressed_offset,
                              frame.uncompressed_size,
                              static_cast<std::uint64_t>(header.payload_offset) +
                                  frame.frame_offset,
                              frame.frame_size,
                              static_cast<std::uint8_t>(kSubframeParallelBlock),
                              frame.codec,
                              0});
        }
        return result;
    }

    CompressionMethod method = CompressionMethod::store;
    if (header.codec == core::CodecId::zstandard) {
        method = CompressionMethod::zstandard;
    } else if (header.codec == core::CodecId::lzma2) {
        method = CompressionMethod::lzma2;
    } else if (header.codec == core::CodecId::deflate) {
        method = CompressionMethod::deflate;
    } else {
        return {};
    }
    const auto frames = codec::inspect_external_codec_frames(
        payload, method, static_cast<std::size_t>(header.original_size));
    result.reserve(frames.size());
    for (const auto& frame : frames) {
        result.push_back({frame.uncompressed_offset,
                          frame.uncompressed_size,
                          static_cast<std::uint64_t>(header.payload_offset) +
                              frame.frame_offset,
                          frame.frame_size,
                          static_cast<std::uint8_t>(kSubframeExternalChunk),
                          static_cast<std::uint8_t>(header.codec),
                          frame.lzma_property});
    }
    return result;
}

struct StreamedBlockResult {
    fs::path payload_path;
    std::uint64_t payload_size = 0;
    std::vector<SubframeRec> subframes;
};

// Build one large LZMA2 AXC block without materializing the whole solid block.
// The raw solid block is already staged on disk by the container reader. Each
// external-codec chunk is bounded to the AXEC u32 limit (512 MiB by default,
// or the requested LZMA2 dictionary size), and the resulting subframe map lets
// extraction/test decode only the ranges they consume.
StreamedBlockResult compress_large_lzma2_block(
    const fs::path& raw_path,
    std::uint64_t raw_size,
    std::uint32_t raw_crc,
    const CompressionOptions& options,
    const std::shared_ptr<OperationControl>& operation,
    const std::function<void(std::uint64_t)>& encoded_progress) {
    if (raw_size == 0 || raw_size > kMaxLargeSolidBlockSize) {
        throw std::invalid_argument("large LZMA2 block has an invalid size");
    }
    if (raw_size > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("large LZMA2 block exceeds the platform size limit");
    }

    const auto requested_chunk = options.external_codec_chunk_size == 0
        ? std::max(
              kLargeSolidCodecChunkSize,
              std::min(options.lzma_dictionary_size, kMaxLzmaCodecChunkSize))
        : options.external_codec_chunk_size;
    const auto chunk_size = std::clamp(
        requested_chunk, std::size_t{256} << 10, kMaxLzmaCodecChunkSize);
    const auto chunk_count = 1 + (raw_size - 1) / chunk_size;
    if (chunk_count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("large LZMA2 block has too many codec chunks");
    }

    std::ifstream raw(raw_path, std::ios::binary);
    if (!raw) {
        throw std::runtime_error("cannot open staged large solid block");
    }
    const auto payload_path = core::unique_sibling_path(raw_path, L"lzma2");
    TempFileGuard payload_guard(payload_path);
    std::fstream payload(payload_path, std::ios::binary | std::ios::in |
                                           std::ios::out | std::ios::trunc);
    if (!payload) {
        throw std::runtime_error("cannot create staged large LZMA2 block");
    }

    auto write_bytes = [&](std::span<const std::uint8_t> bytes) {
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            throw std::runtime_error("staged LZMA2 block write exceeds the stream limit");
        }
        payload.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        if (!payload) {
            throw std::runtime_error("failed while writing staged large LZMA2 block");
        }
    };
    auto write_u32 = [&](std::uint32_t value) {
        ByteVector bytes;
        put_u32(bytes, value);
        write_bytes(bytes);
    };
    auto write_u64 = [&](std::uint64_t value) {
        ByteVector bytes;
        put_u64(bytes, value);
        write_bytes(bytes);
    };

    // AXC v10 fixed header. The payload size is patched after all bounded
    // external frames have been written; the staging file is seekable even
    // when the final AXAR destination is a non-seekable output stream.
    constexpr std::array<std::uint8_t, 8> kAxcMagic = {
        'A', 'X', 'I', 'O', 'M', 'C', '1', 0};
    write_bytes(kAxcMagic);
    ByteVector fixed;
    put_u16(fixed, 10);
    fixed.push_back(static_cast<std::uint8_t>(core::CodecId::lzma2));
    fixed.push_back(0);
    put_u64(fixed, raw_size);
    put_u64(fixed, 0);  // patched payload size
    put_u32(fixed, raw_crc);
    put_u32(fixed, 0);  // no transform metadata in the streamed profile
    write_bytes(fixed);

    const auto external_payload_offset = std::uint64_t{36};
    const auto external_header_size = std::uint64_t{17};
    write_bytes(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>("AXEC\x01\x01\0\0"), 8));
    write_u32(static_cast<std::uint32_t>(chunk_size));
    write_u32(static_cast<std::uint32_t>(chunk_count));
    // The LZMA2 property is filled in after the first codec chunk is encoded.
    const auto property_offset = external_payload_offset + 16;
    const std::uint8_t property_placeholder = 0;
    write_bytes(std::span<const std::uint8_t>(&property_placeholder, 1));

    StreamedBlockResult result;
    result.payload_path = payload_path;
    result.subframes.reserve(static_cast<std::size_t>(chunk_count));
    std::uint64_t payload_position = external_header_size;
    std::uint64_t uncompressed_offset = 0;
    // Do not reserve the maximum codec chunk for a short final (or only) chunk;
    // a large-block option must remain cheap when the input itself is small.
    const auto input_capacity = static_cast<std::size_t>(
        std::min<std::uint64_t>(raw_size, chunk_size));
    std::vector<std::uint8_t> input(input_capacity);
    std::uint8_t lzma_property = 0;
    bool property_set = false;

    for (std::uint64_t index = 0; index < chunk_count; ++index) {
        if (operation) operation->checkpoint();
        const auto remaining = raw_size - uncompressed_offset;
        const auto current_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, chunk_size));
        raw.read(reinterpret_cast<char*>(input.data()),
                 static_cast<std::streamsize>(current_size));
        if (raw.gcount() != static_cast<std::streamsize>(current_size)) {
            throw FormatError("staged large LZMA2 block is truncated");
        }

        auto chunk_options = options;
        chunk_options.block_size = chunk_size;
        chunk_options.external_codec_chunk_size = chunk_size;
        chunk_options.auto_block_size_for_threads = false;
        chunk_options.enable_file_filters = false;
        chunk_options.transform_ranges.clear();
        chunk_options.operation = operation;
        chunk_options.encoded_bytes_progress = {};
        chunk_options.encode_progress = {};
        const auto encoded = codec::encode_external_codec_with_dictionary_bound(
            std::span<const std::uint8_t>(input.data(), current_size),
            CompressionMethod::lzma2, chunk_options, input_capacity);
        const auto frames = codec::inspect_external_codec_frames(
            encoded, CompressionMethod::lzma2, current_size);
        if (frames.size() != 1) {
            throw FormatError("large LZMA2 chunk did not produce one external frame");
        }
        const auto& frame = frames.front();
        if (frame.frame_offset > encoded.size() ||
            frame.frame_size > encoded.size() - frame.frame_offset) {
            throw FormatError("large LZMA2 chunk frame exceeds its payload");
        }
        if (!property_set) {
            lzma_property = frame.lzma_property;
            property_set = true;
            payload.seekp(static_cast<std::streamoff>(property_offset), std::ios::beg);
            write_bytes(std::span<const std::uint8_t>(&lzma_property, 1));
            payload.seekp(0, std::ios::end);
            if (!payload) throw std::runtime_error("failed to patch LZMA2 properties");
        } else if (frame.lzma_property != lzma_property) {
            throw FormatError("large LZMA2 chunks have inconsistent properties");
        }

        result.subframes.push_back({
            uncompressed_offset,
            current_size,
            external_payload_offset + frame.frame_offset,
            frame.frame_size,
            static_cast<std::uint8_t>(kSubframeExternalChunk),
            static_cast<std::uint8_t>(core::CodecId::lzma2),
            lzma_property,
        });
        write_bytes(std::span<const std::uint8_t>(
            encoded.data() + static_cast<std::ptrdiff_t>(frame.frame_offset),
            static_cast<std::size_t>(frame.frame_size)));
        payload_position += frame.frame_size;
        uncompressed_offset += current_size;
        if (encoded_progress) encoded_progress(uncompressed_offset);
    }
    if (!property_set || uncompressed_offset != raw_size) {
        throw FormatError("large LZMA2 block has incomplete codec output");
    }

    const auto final_size = payload_position + external_payload_offset;
    payload.seekp(20, std::ios::beg);
    write_u64(payload_position);
    payload.seekp(0, std::ios::end);
    if (!payload || static_cast<std::uint64_t>(payload.tellp()) != final_size) {
        throw std::runtime_error("failed to finalize staged large LZMA2 block");
    }
    payload.flush();
    payload.close();
    raw.close();
    if (!payload || !raw) {
        throw std::runtime_error("failed to close staged large LZMA2 block");
    }
    result.payload_size = final_size;
    payload_guard.dismiss();
    return result;
}

void validate_subframe_geometry(const BlockRec& block,
                                const std::vector<SubframeRec>& subframes) {
    if (subframes.empty()) return;
    if (subframes.size() > kMaxSubframesPerBlock) {
        throw FormatError("subframe map has too many frames");
    }

    std::uint64_t uncompressed_end = 0;
    std::uint64_t compressed_end = 0;
    for (const auto& frame : subframes) {
        if (frame.uncompressed_size == 0 || frame.compressed_size == 0 ||
            frame.uncompressed_offset != uncompressed_end ||
            uncompressed_end > block.uncompressed_size ||
            frame.uncompressed_size > block.uncompressed_size - uncompressed_end) {
            throw FormatError("subframe map has invalid uncompressed geometry");
        }
        if (frame.compressed_offset < 32 ||
            frame.compressed_offset > block.compressed_size ||
            frame.compressed_size > block.compressed_size - frame.compressed_offset ||
            frame.compressed_offset < compressed_end) {
            throw FormatError("subframe map has invalid compressed geometry");
        }
        if (frame.kind != kSubframeStore &&
            frame.kind != kSubframeParallelBlock &&
            frame.kind != kSubframeExternalChunk) {
            throw FormatError("subframe map has an unknown frame kind");
        }
        if (frame.kind == kSubframeParallelBlock && frame.codec > 10) {
            throw FormatError("subframe map has an unknown parallel codec");
        }
        if (frame.kind == kSubframeExternalChunk &&
            (frame.codec < 8 || frame.codec > 10 || frame.lzma_property > 40)) {
            throw FormatError("subframe map has an invalid external codec");
        }
        uncompressed_end += frame.uncompressed_size;
        compressed_end = frame.compressed_offset + frame.compressed_size;
    }
    if (uncompressed_end != block.uncompressed_size) {
        throw FormatError("subframe map does not cover the block");
    }
}

void parse_block_extras(std::span<const std::uint8_t> bytes, BlockRec& block) {
    Reader extras(bytes);
    bool saw_subframe_map = false;
    while (extras.has_more()) {
        const auto record_type = extras.vint();
        const auto length = extras.vint();
        if (length > extras.remaining()) {
            throw FormatError("block extra record exceeds its enclosing range");
        }
        Reader payload(extras.take(static_cast<std::size_t>(length)));
        if (record_type != kBlockExtraSubframeMap) {
            continue;
        }
        if (saw_subframe_map) {
            throw FormatError("block has duplicate subframe maps");
        }
        saw_subframe_map = true;
        if (payload.vint() != kSubframeMapVersion) {
            throw FormatError("unsupported subframe map version");
        }
        const auto count = payload.vint();
        if (count == 0 || count > kMaxSubframesPerBlock) {
            throw FormatError("subframe map has an invalid frame count");
        }
        block.subframes.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
            SubframeRec frame;
            frame.uncompressed_offset = payload.vint();
            frame.uncompressed_size = payload.vint();
            frame.compressed_offset = payload.vint();
            frame.compressed_size = payload.vint();
            const auto kind = payload.vint();
            const auto codec = payload.vint();
            const auto lzma_property = payload.vint();
            if (kind > std::numeric_limits<std::uint8_t>::max() ||
                codec > std::numeric_limits<std::uint8_t>::max() ||
                lzma_property > std::numeric_limits<std::uint8_t>::max()) {
                throw FormatError("subframe map byte field overflows");
            }
            frame.kind = static_cast<std::uint8_t>(kind);
            frame.codec = static_cast<std::uint8_t>(codec);
            frame.lzma_property = static_cast<std::uint8_t>(lzma_property);
            block.subframes.push_back(frame);
        }
        if (payload.has_more()) {
            throw FormatError("subframe map has trailing data");
        }
    }
    validate_subframe_geometry(block, block.subframes);
}

void validate_large_solid_block_map(const BlockRec& block) {
    if (block.uncompressed_size <= kMaxLegacySolidBlockSize) {
        return;
    }
    if (block.subframes.empty()) {
        throw FormatError("large solid block is missing its bounded subframe map");
    }
    for (const auto& frame : block.subframes) {
        if (frame.kind != kSubframeExternalChunk ||
            frame.codec != static_cast<std::uint8_t>(core::CodecId::lzma2) ||
            frame.uncompressed_size > kMaxLzmaCodecChunkSize) {
            throw FormatError(
                "large solid block map must contain bounded LZMA2 external chunks");
        }
    }
}

bool has_sparse_entries(const std::vector<EntryRec>& entries) {
    return std::any_of(entries.begin(), entries.end(), [](const EntryRec& entry) {
        return entry.type == kEntryFile && entry.sparse.is_sparse;
    });
}

bool has_extended_metadata(const std::vector<EntryRec>& entries) {
    return std::any_of(entries.begin(), entries.end(), [](const EntryRec& entry) {
        return entry.meta.has_windows_security_descriptor ||
               !entry.meta.xattrs.empty() || entry.meta.has_reparse_data;
    });
}

std::uint16_t archive_header_flags(const std::vector<EntryRec>& entries,
                                   const ArchiveMeta& meta) {
    std::uint16_t flags = meta.encryption.encrypt_directory
        ? kFlagEncryptedDirectory : static_cast<std::uint16_t>(0);
    if (meta.encryption.v2) flags |= kFlagEncryptionV2;
    if (has_sparse_entries(entries)) flags |= kFlagSparseEntries;
    if (!meta.capture_warnings.empty()) flags |= kFlagCaptureReport;
    if (has_extended_metadata(entries)) flags |= kFlagExtendedMetadata;
    if (!meta.snapshots.empty()) flags |= kFlagChunkTable;
    if (meta.live_dedup) {
        flags |= kFlagLiveDedup;
        // A live deduplicated archive is an explicitly selected v5 profile.
        // Reserve the fidelity bits at creation so later chunk-only appends can
        // add sparse/capture/extended records without rewriting the immutable
        // header and existing block region.
        flags |= kFlagSparseEntries | kFlagCaptureReport | kFlagExtendedMetadata;
    }
    if (meta.large_solid_blocks) flags |= kFlagLargeSolidBlocks;
    return flags;
}

std::uint16_t archive_header_version(const std::vector<EntryRec>& entries,
                                     const ArchiveMeta& meta) {
    return meta.encryption.v2 || has_sparse_entries(entries) ||
           !meta.capture_warnings.empty() || has_extended_metadata(entries) ||
            meta.chunk_table || meta.live_dedup || meta.large_solid_blocks
        ? kArchiveVersion5 : kArchiveVersion4;
}

}  // namespace

// ScanItem and the scanning helpers are shared with the other archive TUs via
// container_internal.hpp.
void scan_input(const fs::path& input, std::vector<ScanItem>& items) {
    std::error_code ec;

    const fs::path base = input.has_parent_path() ? input.parent_path() : fs::path(".");
    auto relative_path = [&](const fs::path& path) {
        const fs::path lexical = path.lexically_relative(base);
        auto rel = core::generic_path_to_utf8(lexical);
        if (rel.empty()) {
            rel = core::generic_path_to_utf8(path.filename());
        }
        return rel;
    };
    auto symlink_item = [&](const fs::path& path) {
        return ScanItem{path, relative_path(path), false, true,
                        core::generic_path_to_utf8(fs::read_symlink(path, ec))};
    };
    auto reparse_item = [&](const fs::path& path) {
        const bool directory = fs::is_directory(path, ec);
        return ScanItem{path, relative_path(path), directory, false, {}, true};
    };

    // A symlink given directly is archived as a symlink, not its target — check
    // before fs::exists (which follows links and rejects dangling ones).
    if (fs::is_symlink(fs::symlink_status(input, ec))) {
        items.push_back(symlink_item(input));
        return;
    }

    // Junctions and other Windows reparse points are not reported as symlinks
    // by every std::filesystem implementation. Capture them as opaque entries
    // and never recurse through them during source scanning.
    if (core::is_reparse_point(input)) {
        items.push_back(reparse_item(input));
        return;
    }

    if (!fs::exists(input, ec)) {
        throw std::runtime_error("input does not exist: " + core::path_to_utf8(input));
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
            const fs::file_status entry_status = fs::symlink_status(entry.path(), ec);
            if (ec) {
                continue;
            }
            if (fs::is_symlink(entry_status)) {
                items.push_back(symlink_item(entry.path()));
            } else if (core::is_reparse_point(entry.path())) {
                items.push_back(reparse_item(entry.path()));
                it.disable_recursion_pending();
            } else if (fs::is_directory(entry_status)) {
                items.push_back({entry.path(), relative_path(entry.path()), true});
            } else if (fs::is_regular_file(entry_status)) {
                items.push_back({entry.path(), relative_path(entry.path()), false});
            }
        }
        return;
    }

    throw std::runtime_error("unsupported input type: " + core::path_to_utf8(input));
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
                                     core::path_to_utf8(path));
        }
        return join_archive_path(destination, core::generic_path_to_utf8(relative));
    };
    auto add_symlink = [&](const fs::path& path) {
        const fs::path target = fs::read_symlink(path, ec);
        if (ec) {
            throw std::runtime_error("cannot read symbolic link: " +
                                     core::path_to_utf8(path));
        }
        items.push_back({path, archive_path_for(path), false, true,
                         core::generic_path_to_utf8(target)});
    };
    auto add_reparse = [&](const fs::path& path) {
        const bool directory = fs::is_directory(path, ec);
        items.push_back({path, archive_path_for(path), directory, false, {}, true});
    };

    const fs::file_status status = fs::symlink_status(input.source, ec);
    if (ec) {
        throw std::runtime_error("cannot inspect input: " +
                                 core::path_to_utf8(input.source));
    }
    if (fs::is_symlink(status)) {
        add_symlink(input.source);
        return;
    }
    if (core::is_reparse_point(input.source)) {
        add_reparse(input.source);
        return;
    }
    if (!fs::exists(status)) {
        throw std::runtime_error("input does not exist: " +
                                 core::path_to_utf8(input.source));
    }
    if (fs::is_regular_file(status)) {
        items.push_back({input.source, destination, false});
        return;
    }
    if (!fs::is_directory(status)) {
        throw std::runtime_error("unsupported input type: " +
                                 core::path_to_utf8(input.source));
    }

    items.push_back({input.source, destination, true});
    report_operation(operation, OperationStage::scanning, 0, 0,
                     items.size(), 0, destination);
    for (fs::recursive_directory_iterator it(
             input.source, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end && !ec; it.increment(ec)) {
        operation_checkpoint(operation);
        const auto& entry = *it;
        const fs::file_status entry_status = fs::symlink_status(entry.path(), ec);
        if (ec) {
            continue;
        }
        if (fs::is_symlink(entry_status)) {
            add_symlink(entry.path());
        } else if (core::is_reparse_point(entry.path())) {
            add_reparse(entry.path());
            it.disable_recursion_pending();
        } else if (fs::is_directory(entry_status)) {
            items.push_back({entry.path(), archive_path_for(entry.path()), true});
        } else if (fs::is_regular_file(entry_status)) {
            items.push_back({entry.path(), archive_path_for(entry.path()), false});
        }
        if ((items.size() & 255u) == 0) {
            report_operation(
                operation, OperationStage::scanning, 0, 0, items.size(), 0,
                core::path_to_utf8(entry.path()));
        }
    }
    if (ec) {
        throw std::runtime_error("failed while scanning input: " +
                                 core::path_to_utf8(input.source));
    }
    report_operation(operation, OperationStage::scanning, 0, 0,
                     items.size(), items.size(),
                     "Source scan complete");
}

namespace {

std::uint8_t scan_item_type(const ScanItem& item) {
    if (item.is_directory) {
        return kEntryDir;
    }
    return item.is_symlink ? kEntrySymlink : kEntryFile;
}

bool same_mapped_entry_class(std::uint8_t stored_type, std::uint8_t incoming_type) {
    if (stored_type == incoming_type) {
        return true;
    }
    // Hard-link entries are an internal storage optimization for regular files.
    // A mapped filesystem scan cannot emit that representation, so replacing an
    // archived hard-link name with a regular-file item is not a type conflict.
    const auto is_file_like = [](std::uint8_t type) {
        return type == kEntryFile || type == kEntryHardlink;
    };
    return is_file_like(stored_type) && is_file_like(incoming_type);
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
            found != current.end() && !same_mapped_entry_class(found->second, type)) {
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

}  // namespace

std::uint64_t scanned_file_bytes(const std::vector<ScanItem>& items) {
    std::uint64_t total = 0;
    for (const auto& item : items) {
        if (item.is_directory || item.is_reparse_point) {
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

bool open_input_with_retry(std::ifstream& input, const fs::path& path,
                           unsigned retries,
                           const std::shared_ptr<OperationControl>& operation) {
    static constexpr std::array<unsigned, 4> kRetryDelayMs{100, 250, 500, 1000};
    for (unsigned attempt = 0;; ++attempt) {
        input.clear();
        input.open(path, std::ios::binary);
        if (input) return true;
        input.close();
        if (attempt >= retries) return false;
        operation_checkpoint(operation);
        const unsigned delay = kRetryDelayMs[
            std::min<std::size_t>(attempt, kRetryDelayMs.size() - 1)];
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
}

void report_skipped_input(const ScanItem& item,
                          const std::shared_ptr<OperationControl>& operation) {
    if (!operation) return;
    operation->add_warning({
        item.archive_path,
        "Skipped because the file disappeared or access remained denied after retries.",
    });
}

namespace {

std::uint64_t archive_file_bytes(const ArchiveIndex& index) {
    std::uint64_t total = 0;
    for (const auto& entry : index.entries) {
        if (entry.type == kEntryFile) {
            total += entry.size;
        }
    }
    return total;
}

}  // namespace

// Shared with the other archive TUs via container_internal.hpp (which also
// carries the default for current_path).
void report_operation(const std::shared_ptr<OperationControl>& operation,
                      OperationStage stage,
                      std::uint64_t completed_bytes,
                      std::uint64_t total_bytes,
                      std::uint64_t completed_items,
                      std::uint64_t total_items,
                      std::string current_path,
                      std::uint64_t current_file_completed_bytes,
                      std::uint64_t current_file_total_bytes,
                      std::uint64_t throughput_bytes,
                      std::uint64_t compressed_bytes,
                      std::uint64_t compressed_source_bytes,
                      std::uint64_t reused_items,
                      std::uint64_t reused_bytes,
                      std::uint64_t archive_bytes_read) {
    if (operation) {
        operation->report(OperationProgress{
            stage,
            completed_bytes,
            total_bytes,
            completed_items,
            total_items,
            std::move(current_path),
            current_file_completed_bytes,
            current_file_total_bytes,
            throughput_bytes,
            compressed_bytes,
            compressed_source_bytes,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            reused_items,
            reused_bytes,
            archive_bytes_read,
        });
    }
}

void operation_checkpoint(const std::shared_ptr<OperationControl>& operation) {
    if (operation) {
        operation->checkpoint();
    }
}

struct HashedInput {
    ContentIdentity identity;
    std::int64_t source_stamp = 0;
};

std::optional<ContentIdentity> try_hash_input_stream(
    std::ifstream& input, std::uint64_t declared_size,
    const CompressionOptions& options,
    const std::shared_ptr<OperationControl>& operation) {
    core::Blake3 hasher;
    auto crc = core::crc32_init();
    std::vector<std::uint8_t> buffer(effective_io_buffer_size(options.io_buffer_size));
    std::uint64_t actual_size = 0;
    while (input) {
        operation_checkpoint(operation);
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) continue;
        const auto bytes = std::span<const std::uint8_t>(
            buffer.data(), static_cast<std::size_t>(count));
        hasher.update(bytes);
        crc = core::crc32_update(crc, bytes);
        actual_size += static_cast<std::uint64_t>(count);
    }
    if (input.bad() || actual_size != declared_size) return std::nullopt;
    return ContentIdentity{
        actual_size, core::crc32_final(crc), hasher.finalize()};
}

std::optional<HashedInput> try_hash_input_for_reuse(
    const ScanItem& item, const CompressionOptions& options,
    const std::shared_ptr<OperationControl>& operation) {
    std::error_code size_error;
    const auto declared_size = fs::file_size(item.absolute, size_error);
    if (size_error) return std::nullopt;

    std::error_code mtime_error;
    const auto stamp = fs::last_write_time(item.absolute, mtime_error);
    if (mtime_error) return std::nullopt;
    const auto source_stamp =
        static_cast<std::int64_t>(stamp.time_since_epoch().count());

    std::ifstream input;
    if (!open_input_with_retry(input, item.absolute, options.input_open_retries,
                               operation)) {
        return std::nullopt;
    }
    const auto identity = try_hash_input_stream(
        input, declared_size, options, operation);
    if (!identity) return std::nullopt;
    return HashedInput{*identity, source_stamp};
}

ArchiveReuseByPath prepare_reuse_by_path(
    const std::vector<ScanItem>& items,
    const std::map<ContentIdentity, ArchiveDataReference>& candidates,
    const CompressionOptions& options,
    const std::shared_ptr<OperationControl>& operation) {
    ArchiveReuseByPath reuse;
    if (items.empty() || candidates.empty()) return reuse;

    std::unordered_set<std::uint64_t> candidate_sizes;
    candidate_sizes.reserve(candidates.size());
    for (const auto& [identity, data] : candidates) {
        (void)data;
        candidate_sizes.insert(identity.size);
    }

    std::uint64_t candidate_total = 0;
    std::uint64_t candidate_items = 0;
    for (const auto& item : items) {
        if (item.is_directory || item.is_symlink || item.is_reparse_point) continue;
        std::error_code ec;
        const auto size = fs::file_size(item.absolute, ec);
        if (!ec && candidate_sizes.contains(static_cast<std::uint64_t>(size))) {
            ++candidate_items;
            candidate_total += static_cast<std::uint64_t>(size);
        }
    }
    if (candidate_items == 0) return reuse;

    std::uint64_t compared_bytes = 0;
    std::uint64_t compared_items = 0;
    report_operation(operation, OperationStage::comparing, 0, candidate_total,
                     0, items.size(), "Checking unchanged file content");
    for (const auto& item : items) {
        operation_checkpoint(operation);
        if (item.is_directory || item.is_symlink || item.is_reparse_point) continue;
        std::error_code ec;
        const auto size = fs::file_size(item.absolute, ec);
        if (ec || !candidate_sizes.contains(static_cast<std::uint64_t>(size))) continue;

        const auto hashed = try_hash_input_for_reuse(item, options, operation);
        if (!hashed) continue;
        compared_bytes += hashed->identity.size;
        ++compared_items;
        report_operation(operation, OperationStage::comparing, compared_bytes,
                         candidate_total, compared_items, items.size(), item.archive_path);
        const auto found = candidates.find(hashed->identity);
        if (found == candidates.end()) continue;
        reuse.emplace(item.archive_path,
                      ReuseCandidate{hashed->identity, found->second,
                                     hashed->source_stamp});
    }
    return reuse;
}

namespace {

// ---- reading ---------------------------------------------------------------

class RandomAccessArchiveSource {
public:
    virtual ~RandomAccessArchiveSource() = default;
    virtual std::uint64_t size() const = 0;
    virtual ByteVector read(std::uint64_t offset, std::uint64_t length) const = 0;
    virtual void close() = 0;
};

std::shared_ptr<RandomAccessArchiveSource> try_open_volume_archive_source(
    const fs::path& archive_path);

// Owns either an ordinary archive file or a logical archive assembled from
// direct, random-access reads over its numbered data volumes.
class ArchiveStream {
public:
    ArchiveStream() = default;
    ArchiveStream(std::ifstream stream, std::uint64_t base_offset,
                  std::uint64_t size)
        : stream_(std::move(stream)), base_offset_(base_offset), size_(size) {}
    explicit ArchiveStream(std::shared_ptr<RandomAccessArchiveSource> source)
        : source_(std::move(source)), size_(source_->size()) {}

    ArchiveStream(ArchiveStream&&) noexcept = default;
    ArchiveStream& operator=(ArchiveStream&&) noexcept = default;
    ArchiveStream(const ArchiveStream&) = delete;
    ArchiveStream& operator=(const ArchiveStream&) = delete;

    std::uint64_t size() const { return size_; }

    ByteVector read(std::uint64_t offset, std::uint64_t length) {
        if (source_) return source_->read(offset, length);
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(base_offset_ + offset), std::ios::beg);
        ByteVector buffer(static_cast<std::size_t>(length));
        if (length > 0) {
            stream_.read(reinterpret_cast<char*>(buffer.data()),
                         static_cast<std::streamsize>(length));
            if (static_cast<std::uint64_t>(stream_.gcount()) != length) {
                throw FormatError("archive is truncated");
            }
        }
        return buffer;
    }

    void close() {
        if (source_) source_->close();
        if (stream_.is_open()) stream_.close();
    }

private:
    std::ifstream stream_;
    std::shared_ptr<RandomAccessArchiveSource> source_;
    std::uint64_t base_offset_ = 0;
    std::uint64_t size_ = 0;
};

// Random-access byte source for an archive, backed by either a file (so large
// archives are read on demand, not loaded whole) or an in-memory span (used by
// tests and fuzzers). Every read is bounds-checked.
struct ArchiveReadStats {
    std::atomic<std::uint64_t> archive_bytes_read{0};
};

class ByteSource {
public:
    ByteSource(ArchiveStream& stream, std::uint64_t size,
               std::shared_ptr<ArchiveReadStats> stats = {})
        : stream_(&stream), size_(size), stats_(std::move(stats)) {}
    explicit ByteSource(std::span<const std::uint8_t> data,
                        std::shared_ptr<ArchiveReadStats> stats = {})
        : data_(data), size_(data.size()), stats_(std::move(stats)) {}

    std::uint64_t size() const { return size_; }

    ByteVector read(std::uint64_t offset, std::uint64_t length) const {
        if (length > size_ || offset > size_ - length) {
            throw FormatError("archive is truncated");
        }
        if (stream_ != nullptr) {
            // Archive-test workers may decode different solid blocks in parallel.
            // Serialize only the short random-access read; decompression happens
            // after the lock is released.
            std::lock_guard lock(stream_mutex_);
            auto result = stream_->read(offset, length);
            if (stats_) {
                stats_->archive_bytes_read.fetch_add(length, std::memory_order_relaxed);
            }
            return result;
        }
        auto result = ByteVector(
            data_.begin() + static_cast<std::ptrdiff_t>(offset),
            data_.begin() + static_cast<std::ptrdiff_t>(offset + length));
        if (stats_) {
            stats_->archive_bytes_read.fetch_add(length, std::memory_order_relaxed);
        }
        return result;
    }

    ByteVector read_compressed(std::uint64_t offset, std::uint64_t length) const {
        return read(offset, length);
    }

private:
    ArchiveStream* stream_ = nullptr;
    std::span<const std::uint8_t> data_{};
    std::uint64_t size_ = 0;
    std::shared_ptr<ArchiveReadStats> stats_;
    mutable std::mutex stream_mutex_;
};

// Sequential archive writers cannot seek back to patch a header or a footer.
// This adapter keeps the byte position explicit while preserving the ordinary
// ostream interface used by the block and directory writers.
class CountedOutput {
public:
    explicit CountedOutput(std::ostream& stream, std::uint64_t position = 0)
        : stream_(stream), position_(position) {}

    void write(const char* data, std::streamsize size) {
        if (size < 0 || static_cast<std::uint64_t>(size) >
                           std::numeric_limits<std::uint64_t>::max() - position_) {
            throw std::runtime_error("archive output position overflows");
        }
        stream_.write(data, size);
        if (stream_) position_ += static_cast<std::uint64_t>(size);
    }

    void flush() { stream_.flush(); }

    // File-backed callers use close() to release the file before recovery is
    // appended. A non-owning stream must only be flushed, never closed.
    void close() { flush(); }

    explicit operator bool() const { return static_cast<bool>(stream_); }
    std::uint64_t position() const { return position_; }

private:
    std::ostream& stream_;
    std::uint64_t position_ = 0;
};

ArchiveStream open_archive(const fs::path& archive_path, std::uint64_t& file_size);

// Where an archive's fixed structure points: directory location, plus the header
// flags and (if the directory is encrypted) the plaintext preamble's extent.
struct ArchiveLayout {
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint64_t directory_offset = 0;
    std::uint64_t directory_size = 0;
    std::uint64_t preamble_offset = 0;  // start of the encryption preamble (if any)
    std::uint64_t preamble_size = 0;    // length of the preamble's parameter bytes
    std::uint64_t footer_offset = 0;    // selected legacy footer, not necessarily EOF
    std::uint64_t generation_offset = 0;
    std::uint64_t generation_size = 0;
    std::uint64_t generation = 0;
    std::uint64_t previous_footer_offset = 0;
    std::uint64_t previous_directory_offset = 0;
    std::uint64_t previous_directory_size = 0;
    std::uint64_t previous_generation_offset = 0;
};

struct FooterCandidate {
    std::uint64_t footer_offset = 0;
    std::uint64_t directory_offset = 0;
    std::uint64_t directory_size = 0;
};

bool generation_extension_is_valid(const ByteSource& source,
                                   const FooterCandidate& candidate);

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

std::optional<FooterCandidate> read_footer_candidate(const ByteSource& source,
                                                      std::uint64_t footer_offset) {
    if (footer_offset > source.size() ||
        kFooterSize > source.size() - footer_offset) {
        return std::nullopt;
    }
    const auto footer = source.read(footer_offset, kFooterSize);
    if (!std::equal(kArchiveMagic.begin(), kArchiveMagic.end(), footer.begin() + 16)) {
        return std::nullopt;
    }
    Reader reader(footer);
    FooterCandidate candidate;
    candidate.footer_offset = footer_offset;
    candidate.directory_offset = reader.u64();
    candidate.directory_size = reader.u64();
    if (candidate.directory_offset < kHeaderSize ||
        candidate.directory_offset > footer_offset ||
        candidate.directory_size > footer_offset - candidate.directory_offset) {
        return std::nullopt;
    }
    return candidate;
}

// A torn append can leave arbitrary bytes after the last complete footer. Scan
// backward in bounded chunks and select the newest structurally valid legacy
// footer; the directory parser performs the deeper format validation later.
std::optional<FooterCandidate> find_latest_footer(const ByteSource& source) {
    if (source.size() < kFooterSize) return std::nullopt;
    const auto last_offset = source.size() - kFooterSize;
    if (const auto direct = read_footer_candidate(source, last_offset)) {
        if (generation_extension_is_valid(source, *direct)) return direct;
    }

    constexpr std::uint64_t scan_chunk = 1u << 20;
    std::uint64_t search_end = last_offset;
    while (true) {
        const auto start = search_end >= scan_chunk - 1
            ? search_end - (scan_chunk - 1) : 0;
        const auto end = search_end + kFooterSize;
        const auto bytes = source.read(start, end - start);
        const auto first = bytes.size() - kFooterSize + 1;
        for (std::size_t position = first; position > 0; --position) {
            const auto local = position - 1;
            if (!std::equal(kArchiveMagic.begin(), kArchiveMagic.end(),
                            bytes.begin() + static_cast<std::ptrdiff_t>(local + 16))) {
                continue;
            }
            if (const auto candidate = read_footer_candidate(
                    source, start + static_cast<std::uint64_t>(local))) {
                if (generation_extension_is_valid(source, *candidate)) return candidate;
            }
        }
        if (start == 0) break;
        search_end = start - 1;
    }
    return std::nullopt;
}

void read_generation_extension(const ByteSource& source, ArchiveLayout& layout) {
    const auto directory_end = layout.directory_offset + layout.directory_size;
    if (directory_end < layout.directory_offset ||
        directory_end > layout.footer_offset ||
        layout.footer_offset - directory_end < kGenerationExtensionSize) {
        return;
    }
    const auto bytes = source.read(directory_end, kGenerationExtensionSize);
    if (!std::equal(kGenerationMagic.begin(), kGenerationMagic.end(), bytes.begin())) {
        return;
    }

    Reader reader(bytes);
    (void)reader.take(kGenerationMagic.size());
    if (reader.u16() != kGenerationVersion) {
        throw FormatError("unsupported AXAR generation extension version");
    }
    if (reader.u16() != 0 || reader.u32() != kGenerationExtensionSize) {
        throw FormatError("invalid AXAR generation extension flags or size");
    }
    layout.generation = reader.u64();
    layout.previous_footer_offset = reader.u64();
    layout.previous_directory_offset = reader.u64();
    layout.previous_directory_size = reader.u64();
    layout.previous_generation_offset = reader.u64();
    if (reader.u64() != 0) {
        throw FormatError("AXAR generation extension has non-zero reserved fields");
    }
    if (layout.generation == 0) {
        throw FormatError("AXAR generation number must be non-zero");
    }
    if (layout.previous_footer_offset == 0) {
        if (layout.previous_directory_offset != 0 ||
            layout.previous_directory_size != 0 ||
            layout.previous_generation_offset != 0) {
            throw FormatError("AXAR first generation has previous-generation metadata");
        }
    } else {
        if (layout.previous_footer_offset < kHeaderSize ||
            layout.previous_footer_offset >= layout.directory_offset) {
            throw FormatError("AXAR previous footer is outside the archive history");
        }
        const auto previous = read_footer_candidate(source, layout.previous_footer_offset);
        if (!previous || previous->directory_offset != layout.previous_directory_offset ||
            previous->directory_size != layout.previous_directory_size) {
            throw FormatError("AXAR previous footer does not match its directory reference");
        }
        if (layout.previous_generation_offset != 0 &&
            (layout.previous_generation_offset < kHeaderSize ||
             layout.previous_generation_offset >= layout.previous_footer_offset)) {
            throw FormatError("AXAR previous generation reference is out of range");
        }
        if (layout.previous_generation_offset == 0) {
            if (layout.generation != 1) {
                throw FormatError(
                    "AXAR first appended generation must be generation one");
            }
        } else {
            const auto previous_directory_end =
                layout.previous_directory_offset + layout.previous_directory_size;
            if (previous_directory_end < layout.previous_directory_offset ||
                previous_directory_end != layout.previous_generation_offset ||
                layout.previous_footer_offset < kGenerationExtensionSize ||
                layout.previous_generation_offset >
                    layout.previous_footer_offset - kGenerationExtensionSize) {
                throw FormatError("AXAR previous generation offset does not match its directory");
            }
            const auto previous_extension = source.read(
                layout.previous_generation_offset, kGenerationExtensionSize);
            Reader previous_reader(previous_extension);
            if (!std::equal(kGenerationMagic.begin(), kGenerationMagic.end(),
                            previous_reader.take(kGenerationMagic.size()).begin()) ||
                previous_reader.u16() != kGenerationVersion ||
                previous_reader.u16() != 0 ||
                previous_reader.u32() != kGenerationExtensionSize) {
                throw FormatError("AXAR previous generation extension is invalid");
            }
            const auto previous_generation = previous_reader.u64();
            (void)previous_reader.u64();
            (void)previous_reader.u64();
            (void)previous_reader.u64();
            (void)previous_reader.u64();
            if (previous_reader.u64() != 0 || previous_generation == 0 ||
                previous_generation == std::numeric_limits<std::uint64_t>::max() ||
                previous_generation + 1 != layout.generation) {
                throw FormatError("AXAR generation numbers are not sequential");
            }
        }
    }
    layout.generation_offset = directory_end;
    layout.generation_size = kGenerationExtensionSize;
}

bool generation_extension_is_valid(const ByteSource& source,
                                   const FooterCandidate& candidate) {
    ArchiveLayout layout;
    layout.footer_offset = candidate.footer_offset;
    layout.directory_offset = candidate.directory_offset;
    layout.directory_size = candidate.directory_size;
    try {
        read_generation_extension(source, layout);
        return true;
    } catch (const FormatError&) {
        // A torn generation may leave a footer-shaped byte sequence whose
        // extension is incomplete. Keep scanning for the previous complete
        // footer instead of allowing that tail to hide it.
        return false;
    }
}

std::optional<RecoveryService> read_recovery_service(const ByteSource& source,
                                                      const ArchiveLayout& layout) {
    if (layout.footer_offset < kRecoveryTailSize) return std::nullopt;
    const std::uint64_t tail_offset = layout.footer_offset - kRecoveryTailSize;
    const auto tail = source.read(tail_offset, kRecoveryTailSize);
    if (!std::equal(kRecoveryMagic.begin(), kRecoveryMagic.end(), tail.begin() + 16)) {
        return std::nullopt;
    }
    Reader tail_reader(tail);
    RecoveryService service;
    service.service_offset = tail_reader.u64();
    service.service_size = tail_reader.u64();
    const auto protected_end = layout.directory_offset + layout.directory_size +
                               layout.generation_size;
    if (protected_end < layout.directory_offset ||
        service.service_size > tail_offset ||
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
        service.directory_offset > service.protected_size ||
        service.directory_size > service.protected_size - service.directory_offset ||
        service.protected_size != protected_end) {
        throw FormatError("invalid recovery service parameters");
    }
    const std::size_t checksum_count =
        static_cast<std::size_t>(service.data_shards + service.parity_shards);
    service.checksums.reserve(checksum_count);
    for (std::size_t i = 0; i < checksum_count; ++i) service.checksums.push_back(reader.u32());
    if (service.shard_size > std::numeric_limits<std::uint64_t>::max() /
                              service.parity_shards) {
        throw FormatError("recovery service parity size overflows");
    }
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
    ArchiveLayout layout;
    layout.version = header_reader.u16();
    if (layout.version != kArchiveVersion4 && layout.version != kArchiveVersion5) {
        throw FormatError("unsupported archive version");
    }
    layout.flags = header_reader.u16();
    if (header_reader.u32() != 0) {
        throw FormatError("archive header has non-zero reserved fields");
    }
    if ((layout.flags & ~kKnownArchiveFlags) != 0) {
        throw FormatError("archive uses features this build does not support");
    }
    if (layout.version < kArchiveVersion5 &&
        (layout.flags & (kFlagSparseEntries | kFlagCaptureReport |
                         kFlagExtendedMetadata | kFlagEncryptionV2 |
                         kFlagChunkTable | kFlagLargeSolidBlocks |
                         kFlagLiveDedup)) != 0) {
        throw FormatError("archive uses v5 features with an older version header");
    }

    const auto footer = find_latest_footer(source);
    if (!footer) {
        throw FormatError("invalid archive footer");
    }
    layout.footer_offset = footer->footer_offset;
    layout.directory_offset = footer->directory_offset;
    layout.directory_size = footer->directory_size;
    read_generation_extension(source, layout);
    if ((layout.flags & kFlagEncryptedDirectory) != 0) {
        // The preamble is a fixed u32 length right after the header, then that many
        // plaintext parameter bytes, before the (encrypted) block region.
        const auto len_bytes = source.read(kHeaderSize, 4);
        Reader len_reader(len_bytes);
        layout.preamble_size = len_reader.u32();
        layout.preamble_offset = kHeaderSize + 4;
        if (layout.preamble_offset > layout.directory_offset ||
            layout.preamble_size > layout.directory_offset - layout.preamble_offset) {
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

ByteVector generation_extension_bytes(std::uint64_t generation,
                                      std::uint64_t previous_footer_offset,
                                      std::uint64_t previous_directory_offset,
                                      std::uint64_t previous_directory_size,
                                      std::uint64_t previous_generation_offset) {
    if (generation == 0) {
        throw std::invalid_argument("archive generation must be non-zero");
    }
    if (previous_footer_offset == 0 &&
        (previous_directory_offset != 0 || previous_directory_size != 0 ||
         previous_generation_offset != 0)) {
        throw std::invalid_argument("archive generation has inconsistent history metadata");
    }
    ByteVector extension;
    extension.insert(extension.end(), kGenerationMagic.begin(), kGenerationMagic.end());
    put_u16(extension, kGenerationVersion);
    put_u16(extension, 0);
    put_u32(extension, static_cast<std::uint32_t>(kGenerationExtensionSize));
    put_u64(extension, generation);
    put_u64(extension, previous_footer_offset);
    put_u64(extension, previous_directory_offset);
    put_u64(extension, previous_directory_size);
    put_u64(extension, previous_generation_offset);
    put_u64(extension, 0);
    if (extension.size() != kGenerationExtensionSize) {
        throw std::logic_error("archive generation extension has the wrong size");
    }
    return extension;
}

struct EncodedRecoveryService {
    ByteVector body;
    ByteVector tail;
};

std::uint64_t recovery_progress_multiply(std::uint64_t left, std::uint64_t right) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

std::uint64_t recovery_progress_add(std::uint64_t left, std::uint64_t right) {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    return right > max - left ? max : left + right;
}

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
    const std::uint64_t parity_work = recovery_progress_multiply(
        static_cast<std::uint64_t>(parity_count), shard_size);
    const std::uint64_t total_work = recovery_progress_add(protected_size, parity_work);

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
        report_operation(operation, OperationStage::recovering, completed, total_work,
                         static_cast<std::uint64_t>(i + 1),
                         static_cast<std::uint64_t>(data_count + parity_count),
                         "Reading protected archive data");
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
    core::ReedSolomon(data_count, parity_count).encode(
        data_spans, parity_spans,
        [&](int parity_index, std::size_t parity_completed, std::size_t parity_total) {
            operation_checkpoint(operation);
            const std::uint64_t encoded_before = recovery_progress_multiply(
                static_cast<std::uint64_t>(std::max(0, parity_index)), shard_size);
            const std::uint64_t encoded_current = std::min<std::uint64_t>(
                static_cast<std::uint64_t>(parity_completed), shard_size);
            const std::uint64_t completed_work = recovery_progress_add(
                protected_size, recovery_progress_add(encoded_before, encoded_current));
            const bool parity_done = parity_completed >= parity_total;
            report_operation(operation, OperationStage::recovering, completed_work, total_work,
                             static_cast<std::uint64_t>(
                                 data_count + parity_index + (parity_done ? 1 : 0)),
                             static_cast<std::uint64_t>(data_count + parity_count),
                             "Encoding recovery parity shard " +
                                 std::to_string(parity_index + 1) + " of " +
                                 std::to_string(parity_count));
        });

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

}  // namespace

// Shared with the other archive TUs via container_internal.hpp.
void replace_archive_file(const fs::path& temporary, const fs::path& destination) {
    core::replace_file(temporary, destination, "archive");
}

namespace {

void reject_volume_mutation(const fs::path& archive_path) {
    if (is_axiom_archive_volume(archive_path)) {
        throw std::runtime_error(
            "Axiom volume sets are read-only; join the volumes before modifying the archive");
    }
}

void rewrite_recovery_service(const fs::path& archive_path, unsigned percent,
                              const std::shared_ptr<OperationControl>& operation,
                              std::size_t io_buffer_size = 0) {
    reject_volume_mutation(archive_path);
    std::uint64_t file_size = 0;
    auto input = open_archive(archive_path, file_size);
    const ByteSource source(input, file_size);
    const ArchiveLayout layout = read_layout(source);
    const std::uint64_t protected_size = layout.directory_offset + layout.directory_size +
                                          layout.generation_size;
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
    const std::size_t io_chunk = effective_io_buffer_size(io_buffer_size);
    std::uint64_t copied = 0;
    while (copied < protected_size) {
        operation_checkpoint(operation);
        const auto count = std::min<std::uint64_t>(io_chunk, protected_size - copied);
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

// A freshly written transaction file already consists of the protected archive
// bytes followed by its ordinary footer. Replace that footer in place with the
// recovery service and a new footer before the transaction is committed. This
// avoids the second full-file copy performed by rewrite_recovery_service while
// retaining atomic replacement of the destination archive.
void append_recovery_to_staged_archive(
    const fs::path& staged_path, unsigned percent,
    const std::shared_ptr<OperationControl>& operation,
    std::size_t requested_threads = 0) {
    if (percent == 0) return;
    if (percent > 100) {
        throw std::invalid_argument("recovery percentage must be between 1 and 100");
    }

    std::uint64_t file_size = 0;
    auto input = open_archive(staged_path, file_size);
    const ByteSource source(input, file_size);
    const ArchiveLayout layout = read_layout(source);
    const std::uint64_t protected_size = layout.directory_offset + layout.directory_size +
                                          layout.generation_size;
    constexpr std::uint64_t target_shard_size = 1u << 20;
    const std::uint64_t desired_data = std::max<std::uint64_t>(
        1, (protected_size + target_shard_size - 1) / target_shard_size);
    const std::uint64_t max_data = std::max<std::uint64_t>(
        1, (255u * 100u) / (100u + percent));
    const int data_count = static_cast<int>(std::min(desired_data, max_data));
    const int parity_count = static_cast<int>(std::max<std::uint64_t>(
        1, std::min<std::uint64_t>(
               255 - data_count,
               (static_cast<std::uint64_t>(data_count) * percent + 99) / 100)));
    const std::uint64_t shard_size =
        std::max<std::uint64_t>(1, (protected_size + data_count - 1) / data_count);
    const std::uint64_t parity_bytes = recovery_progress_multiply(
        static_cast<std::uint64_t>(parity_count), shard_size);
    const std::uint64_t total_work = recovery_progress_add(
        protected_size, recovery_progress_add(parity_bytes, parity_bytes));

    // Encode one stripe across every shard at a time. The previous implementation
    // retained the complete protected archive plus all parity shards in memory.
    // A 64-KiB stripe bounds working memory to roughly
    // (data_shards + parity_shards) * 64 KiB without changing a single recovery
    // byte on disk.
    constexpr std::size_t stripe_capacity = 64u << 10;
    std::vector<std::uint32_t> data_crc(
        static_cast<std::size_t>(data_count), core::crc32_init());
    std::vector<std::uint32_t> parity_crc(
        static_cast<std::size_t>(parity_count), core::crc32_init());

    fs::path parity_path = staged_path;
    parity_path += L".parity.tmp";
    TempFileGuard parity_guard(parity_path);
    std::fstream parity_file(
        parity_path, std::ios::binary | std::ios::in |
                         std::ios::out | std::ios::trunc);
    if (!parity_file) throw std::runtime_error("cannot create recovery parity spool");

    core::ReedSolomon encoder(data_count, parity_count);
    const std::size_t recovery_threads = parity_bytes < (8u << 20)
        ? 1
        : std::max<std::size_t>(
              1, std::min<std::size_t>(
                     selected_thread_count(requested_threads),
                     static_cast<std::size_t>(parity_count)));
    core::TaskExecutor recovery_executor(recovery_threads);
    std::uint64_t read_completed = 0;
    for (std::uint64_t stripe_offset = 0; stripe_offset < shard_size;
         stripe_offset += stripe_capacity) {
        operation_checkpoint(operation);
        const auto stripe_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(stripe_capacity, shard_size - stripe_offset));
        std::vector<std::vector<std::uint8_t>> data(
            static_cast<std::size_t>(data_count),
            std::vector<std::uint8_t>(stripe_size, 0));
        for (int data_index = 0; data_index < data_count; ++data_index) {
            operation_checkpoint(operation);
            const std::uint64_t archive_offset =
                static_cast<std::uint64_t>(data_index) * shard_size + stripe_offset;
            const std::uint64_t count = archive_offset < protected_size
                ? std::min<std::uint64_t>(stripe_size,
                                          protected_size - archive_offset)
                : 0;
            if (count != 0) {
                const auto bytes = source.read(archive_offset, count);
                std::copy(bytes.begin(), bytes.end(),
                          data[static_cast<std::size_t>(data_index)].begin());
                read_completed += count;
            }
            data_crc[static_cast<std::size_t>(data_index)] = core::crc32_update(
                data_crc[static_cast<std::size_t>(data_index)],
                data[static_cast<std::size_t>(data_index)]);
            report_operation(
                operation, OperationStage::recovering, read_completed, total_work,
                static_cast<std::uint64_t>(data_index + 1),
                static_cast<std::uint64_t>(data_count + parity_count),
                "Reading protected archive data");
        }

        std::vector<std::vector<std::uint8_t>> parity(
            static_cast<std::size_t>(parity_count),
            std::vector<std::uint8_t>(stripe_size, 0));
        std::vector<std::span<const std::uint8_t>> data_spans;
        std::vector<std::span<std::uint8_t>> parity_spans;
        data_spans.reserve(data.size());
        parity_spans.reserve(parity.size());
        for (const auto& shard : data) data_spans.emplace_back(shard);
        for (auto& shard : parity) parity_spans.emplace_back(shard);
        std::atomic<std::uint64_t> encoded_in_stripe{0};
        const auto report_parity =
            [&](int parity_index, std::size_t completed, std::size_t) {
                operation_checkpoint(operation);
                const std::uint64_t stripe_base = recovery_progress_multiply(
                    stripe_offset, static_cast<std::uint64_t>(parity_count));
                const std::uint64_t stripe_completed =
                    encoded_in_stripe.fetch_add(
                        completed, std::memory_order_relaxed) + completed;
                report_operation(
                    operation, OperationStage::recovering,
                    recovery_progress_add(
                        protected_size,
                        recovery_progress_add(stripe_base, stripe_completed)),
                    total_work,
                    static_cast<std::uint64_t>(data_count + parity_index),
                    static_cast<std::uint64_t>(data_count + parity_count),
                    "Encoding recovery parity shard " +
                        std::to_string(parity_index + 1) + " of " +
                        std::to_string(parity_count));
            };
        std::vector<std::future<void>> parity_tasks;
        parity_tasks.reserve(static_cast<std::size_t>(parity_count));
        for (int parity_index = 0; parity_index < parity_count; ++parity_index) {
            parity_tasks.push_back(recovery_executor.submit(
                [&, parity_index] {
                    encoder.encode_parity_shard(
                        parity_index, data_spans,
                        parity_spans[static_cast<std::size_t>(parity_index)],
                        report_parity);
                }));
        }
        for (auto& task : parity_tasks) recovery_executor.wait(task);
        for (int parity_index = 0; parity_index < parity_count; ++parity_index) {
            auto& shard = parity[static_cast<std::size_t>(parity_index)];
            parity_crc[static_cast<std::size_t>(parity_index)] = core::crc32_update(
                parity_crc[static_cast<std::size_t>(parity_index)], shard);
            const std::uint64_t spool_offset =
                static_cast<std::uint64_t>(parity_index) * shard_size + stripe_offset;
            parity_file.seekp(static_cast<std::streamoff>(spool_offset));
            parity_file.write(reinterpret_cast<const char*>(shard.data()),
                              static_cast<std::streamsize>(shard.size()));
            if (!parity_file) {
                throw std::runtime_error("failed while spooling recovery parity");
            }
        }
    }
    parity_file.close();
    if (!parity_file) throw std::runtime_error("failed to finalize recovery parity spool");
    input.close();

    ByteVector recovery_header;
    recovery_header.insert(
        recovery_header.end(), kRecoveryMagic.begin(), kRecoveryMagic.end());
    put_u16(recovery_header, kRecoveryVersion);
    put_u16(recovery_header, static_cast<std::uint16_t>(percent));
    put_u16(recovery_header, static_cast<std::uint16_t>(data_count));
    put_u16(recovery_header, static_cast<std::uint16_t>(parity_count));
    put_u64(recovery_header, shard_size);
    put_u64(recovery_header, protected_size);
    put_u64(recovery_header, layout.directory_offset);
    put_u64(recovery_header, layout.directory_size);
    for (auto crc : data_crc) put_u32(recovery_header, core::crc32_final(crc));
    for (auto crc : parity_crc) put_u32(recovery_header, core::crc32_final(crc));

    const std::uint64_t recovery_body_size =
        recovery_progress_add(recovery_header.size(), parity_bytes);
    ByteVector recovery_tail;
    put_u64(recovery_tail, protected_size);
    put_u64(recovery_tail, recovery_body_size);
    recovery_tail.insert(
        recovery_tail.end(), kRecoveryMagic.begin(), kRecoveryMagic.end());
    const auto footer = archive_footer_bytes(layout.directory_offset, layout.directory_size);

    operation_checkpoint(operation);
    std::fstream output(staged_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!output) throw std::runtime_error("cannot reopen staged archive for recovery data");
    output.seekp(static_cast<std::streamoff>(protected_size));
    output.write(reinterpret_cast<const char*>(recovery_header.data()),
                 static_cast<std::streamsize>(recovery_header.size()));
    std::ifstream parity_input(parity_path, std::ios::binary);
    if (!parity_input) throw std::runtime_error("cannot reopen recovery parity spool");
    std::vector<char> copy_buffer(1u << 20);
    std::uint64_t parity_copied = 0;
    while (parity_copied < parity_bytes) {
        operation_checkpoint(operation);
        const auto count = static_cast<std::streamsize>(
            std::min<std::uint64_t>(copy_buffer.size(), parity_bytes - parity_copied));
        parity_input.read(copy_buffer.data(), count);
        if (parity_input.gcount() != count) {
            throw std::runtime_error("recovery parity spool is truncated");
        }
        output.write(copy_buffer.data(), count);
        if (!output) throw std::runtime_error("failed while writing recovery data");
        parity_copied += static_cast<std::uint64_t>(count);
        report_operation(
            operation, OperationStage::recovering,
            recovery_progress_add(
                protected_size,
                recovery_progress_add(parity_bytes, parity_copied)),
            total_work, static_cast<std::uint64_t>(data_count + parity_count),
            static_cast<std::uint64_t>(data_count + parity_count),
            "Writing recovery data");
    }
    parity_input.close();
    output.write(reinterpret_cast<const char*>(recovery_tail.data()),
                 static_cast<std::streamsize>(recovery_tail.size()));
    output.write(reinterpret_cast<const char*>(footer.data()),
                 static_cast<std::streamsize>(footer.size()));
    const std::uint64_t final_size =
        protected_size + recovery_body_size + recovery_tail.size() + footer.size();
    output.close();
    if (!output) throw std::runtime_error("failed to append recovery data");
    std::error_code resize_error;
    fs::resize_file(staged_path, final_size, resize_error);
    if (resize_error) {
        throw fs::filesystem_error(
            "failed to finalize staged recovery data", staged_path, resize_error);
    }
}

// Finish an append generation after its directory has been written without a
// footer. The generation record is placed before the legacy footer so old
// readers still see the current directory. On any write failure, truncate back
// to the previous complete generation; a process crash before this cleanup is
// handled by read_layout's backward footer scan.
void append_generation_trailer(
    const fs::path& staged_path,
    std::uint64_t directory_offset,
    std::uint64_t directory_size,
    std::uint64_t generation,
    std::uint64_t previous_footer_offset,
    std::uint64_t previous_directory_offset,
    std::uint64_t previous_directory_size,
    std::uint64_t previous_generation_offset,
    unsigned recovery_percent,
    const std::shared_ptr<OperationControl>& operation,
    std::size_t requested_threads,
    std::uint64_t rollback_size) {
    try {
        const auto directory_end = directory_offset + directory_size;
        if (directory_end < directory_offset) {
            throw FormatError("archive directory offset overflows generation trailer");
        }
        std::error_code size_error;
        const auto current_size = fs::file_size(staged_path, size_error);
        if (size_error || current_size != directory_end) {
            throw FormatError("generation trailer is not positioned after the directory");
        }
        const auto extension = generation_extension_bytes(
            generation, previous_footer_offset, previous_directory_offset,
            previous_directory_size, previous_generation_offset);
        const auto footer = archive_footer_bytes(directory_offset, directory_size);
        std::ofstream output(staged_path, std::ios::binary | std::ios::app);
        if (!output) {
            throw std::runtime_error("cannot reopen archive for generation trailer");
        }
        output.write(reinterpret_cast<const char*>(extension.data()),
                     static_cast<std::streamsize>(extension.size()));
        output.write(reinterpret_cast<const char*>(footer.data()),
                     static_cast<std::streamsize>(footer.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("failed to write generation trailer");
        }
        if (recovery_percent != 0) {
            append_recovery_to_staged_archive(
                staged_path, recovery_percent, operation, requested_threads);
        }
    } catch (...) {
        std::error_code rollback_error;
        fs::resize_file(staged_path, rollback_size, rollback_error);
        throw;
    }
}

std::uint32_t read_u32_vint(Reader& reader, const char* field) {
    const auto value = reader.vint();
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw FormatError(std::string(field) + " exceeds 32 bits");
    }
    return static_cast<std::uint32_t>(value);
}

void validate_encryption_slot_shape(const EncryptionSlot& slot) {
    if (slot.wrapped_key.size() != kWrappedEncryptionKeySize) {
        throw FormatError("encryption slot has an invalid wrapped key");
    }
}

ByteVector encryption_slot_associated_data(const EncryptionInfo& enc,
                                           std::uint32_t slot_id) {
    ByteVector ad(kEncryptionSlotAd.begin(), kEncryptionSlotAd.end());
    ad.insert(ad.end(), enc.key_id.begin(), enc.key_id.end());
    put_u32(ad, slot_id);
    return ad;
}

EncryptionInfo parse_encryption_v2_payload(std::span<const std::uint8_t> bytes,
                                           bool encrypt_directory) {
    EncryptionInfo enc;
    enc.enabled = true;
    enc.encrypt_directory = encrypt_directory;
    enc.v2 = true;
    Reader reader(bytes);
    const auto magic = reader.take(kEncryptionV2Magic.size());
    if (!std::equal(kEncryptionV2Magic.begin(), kEncryptionV2Magic.end(), magic.begin())) {
        throw FormatError("invalid AXAR encryption-v2 magic");
    }
    if (reader.u16() != kEncryptionV2Version) {
        throw FormatError("unsupported AXAR encryption-v2 version");
    }
    if (reader.u16() != 0) {
        throw FormatError("AXAR encryption-v2 has unknown required options");
    }
    const auto key_id = reader.take(kEncryptionKeyIdSize);
    std::copy(key_id.begin(), key_id.end(), enc.key_id.begin());
    const auto slot_count = reader.vint();
    if (slot_count == 0 || slot_count > kMaxEncryptionSlots) {
        throw FormatError("AXAR encryption-v2 has an invalid password-slot count");
    }
    enc.slots.reserve(static_cast<std::size_t>(slot_count));
    std::unordered_set<std::uint32_t> slot_ids;
    for (std::uint64_t i = 0; i < slot_count; ++i) {
        EncryptionSlot slot;
        slot.id = reader.u32();
        if (!slot_ids.insert(slot.id).second) {
            throw FormatError("AXAR encryption-v2 contains duplicate password slots");
        }
        slot.kdf.algorithm = read_u32_vint(reader, "encryption slot algorithm");
        slot.kdf.mem_blocks = read_u32_vint(reader, "encryption slot memory cost");
        slot.kdf.passes = read_u32_vint(reader, "encryption slot pass count");
        slot.kdf.lanes = read_u32_vint(reader, "encryption slot lane count");
        const auto salt_length = reader.vint();
        if (salt_length != slot.kdf.salt.size()) {
            throw FormatError("encryption slot salt must be 16 bytes");
        }
        const auto salt = reader.take(slot.kdf.salt.size());
        std::copy(salt.begin(), salt.end(), slot.kdf.salt.begin());
        const auto wrapped_length = reader.vint();
        if (wrapped_length != kWrappedEncryptionKeySize) {
            throw FormatError("encryption slot has an invalid wrapped-key length");
        }
        const auto wrapped = reader.take(kWrappedEncryptionKeySize);
        slot.wrapped_key.assign(wrapped.begin(), wrapped.end());
        enc.slots.push_back(std::move(slot));
    }
    if (reader.has_more()) {
        throw FormatError("AXAR encryption-v2 payload has trailing data");
    }
    return enc;
}

// Parse the plaintext encryption parameters carried in the header preamble.
EncryptionInfo parse_encryption_preamble(const ByteVector& bytes) {
    // read_layout consumes the fixed u32 length and exposes only the parameter
    // bytes at preamble_offset. Keep the legacy on-disk outer length out of this
    // parser so v1 and v2 share the same bounded payload path.
    if (bytes.size() >= kEncryptionV2Magic.size() &&
        std::equal(kEncryptionV2Magic.begin(), kEncryptionV2Magic.end(), bytes.begin())) {
        return parse_encryption_v2_payload(bytes, true);
    }

    EncryptionInfo enc;
    enc.enabled = true;
    enc.encrypt_directory = true;
    Reader reader(bytes);
    enc.kdf.algorithm = read_u32_vint(reader, "legacy encryption algorithm");
    enc.kdf.mem_blocks = read_u32_vint(reader, "legacy encryption memory cost");
    enc.kdf.passes = read_u32_vint(reader, "legacy encryption pass count");
    enc.kdf.lanes = read_u32_vint(reader, "legacy encryption lane count");
    const auto salt_len = reader.vint();
    if (salt_len != enc.kdf.salt.size()) {
        throw FormatError("legacy encryption salt must be 16 bytes");
    }
    const auto salt = reader.take(enc.kdf.salt.size());
    std::copy(salt.begin(), salt.end(), enc.kdf.salt.begin());
    const auto check_len = reader.vint();
    if (check_len != core::kAeadOverhead + kKeyCheckPlaintext.size()) {
        throw FormatError("legacy encryption key-check length is invalid");
    }
    const auto check = reader.take(static_cast<std::size_t>(check_len));
    enc.key_check.assign(check.begin(), check.end());
    if (reader.has_more()) {
        throw FormatError("legacy encryption preamble has trailing data");
    }
    return enc;
}

EntryRec parse_snapshot_entry_body(std::span<const std::uint8_t> bytes) {
    Reader body(bytes);
    EntryRec entry;
    const auto type = body.vint();
    if (type > kEntryHardlink) {
        throw FormatError("snapshot manifest has an unknown entry type");
    }
    entry.type = static_cast<std::uint8_t>(type);
    const auto path_length = body.vint();
    if (path_length > body.remaining()) {
        throw FormatError("snapshot manifest path is truncated");
    }
    entry.path = body.str(static_cast<std::size_t>(path_length));
    if (entry.type == kEntryFile) {
        entry.size = body.vint();
        entry.first_block = body.vint();
        entry.offset = body.vint();
    } else if (entry.type == kEntrySymlink || entry.type == kEntryHardlink) {
        const auto target_length = body.vint();
        if (target_length > body.remaining()) {
            throw FormatError("snapshot manifest link target is truncated");
        }
        entry.link_target = body.str(static_cast<std::size_t>(target_length));
    }
    while (body.has_more()) {
        const auto record_type = body.vint();
        const auto payload_length = body.vint();
        if (payload_length > body.remaining()) {
            throw FormatError("snapshot manifest entry extra is truncated");
        }
        Reader payload(body.take(static_cast<std::size_t>(payload_length)));
        if (record_type == kExtraMtime) {
            if (payload.remaining() != 8) throw FormatError("snapshot mtime is invalid");
            entry.mtime = static_cast<std::int64_t>(payload.u64());
        } else if (record_type == kExtraCrc32) {
            if (payload.remaining() != 4) throw FormatError("snapshot CRC is invalid");
            entry.crc = payload.u32();
        } else if (record_type == kExtraBlake3) {
            if (payload.remaining() != entry.blake3.size()) {
                throw FormatError("snapshot BLAKE3 is invalid");
            }
            const auto digest = payload.take(entry.blake3.size());
            std::copy(digest.begin(), digest.end(), entry.blake3.begin());
            entry.has_blake3 = true;
        } else if (record_type == kExtraWinAttrs) {
            if (payload.remaining() != 4) throw FormatError("snapshot Windows attributes are invalid");
            entry.meta.has_windows_attributes = true;
            entry.meta.windows_attributes = payload.u32();
        } else if (record_type == kExtraWinTimes) {
            if (payload.remaining() != 24) throw FormatError("snapshot Windows times are invalid");
            entry.meta.has_windows_times = true;
            entry.meta.windows_creation_time = payload.u64();
            entry.meta.windows_access_time = payload.u64();
            entry.meta.windows_write_time = payload.u64();
        } else if (record_type == kExtraPosix) {
            if (payload.remaining() != 12) throw FormatError("snapshot POSIX metadata is invalid");
            entry.meta.has_posix = true;
            entry.meta.posix_mode = payload.u32();
            entry.meta.posix_uid = payload.u32();
            entry.meta.posix_gid = payload.u32();
        } else if (record_type == kExtraAdsStream) {
            if (entry.ads.size() >= core::kMaxMetadataBlobCount) {
                throw FormatError("too many snapshot alternate data streams");
            }
            const auto name_length = payload.vint();
            if (name_length == 0 || name_length > (4u << 10) ||
                name_length > payload.remaining()) {
                throw FormatError("snapshot alternate data stream name is invalid");
            }
            core::AdsStream stream;
            stream.name = payload.str(static_cast<std::size_t>(name_length));
            if (payload.remaining() > core::kMaxAdsBytes) {
                throw FormatError("snapshot alternate data stream exceeds its metadata limit");
            }
            const auto data = payload.take(payload.remaining());
            stream.data.assign(data.begin(), data.end());
            entry.ads.push_back(std::move(stream));
        } else if (record_type == kExtraSecurityDescriptor) {
            if (payload.remaining() == 0 ||
                payload.remaining() > core::kMaxSecurityDescriptorBytes) {
                throw FormatError("snapshot security descriptor exceeds its metadata limit");
            }
            const auto descriptor = payload.take(payload.remaining());
            entry.meta.has_windows_security_descriptor = true;
            entry.meta.windows_security_descriptor.assign(descriptor.begin(), descriptor.end());
        } else if (record_type == kExtraXattr) {
            if (entry.meta.xattrs.size() >= core::kMaxMetadataBlobCount) {
                throw FormatError("too many snapshot extended attributes");
            }
            const auto name_length = payload.vint();
            if (name_length == 0 || name_length > (4u << 10) ||
                name_length > payload.remaining()) {
                throw FormatError("snapshot extended attribute name is invalid");
            }
            core::MetadataBlob blob;
            blob.name = payload.str(static_cast<std::size_t>(name_length));
            if (payload.remaining() > core::kMaxMetadataBlobBytes) {
                throw FormatError("snapshot extended attribute exceeds its metadata limit");
            }
            const auto value = payload.take(payload.remaining());
            blob.data.assign(value.begin(), value.end());
            entry.meta.xattrs.push_back(std::move(blob));
        } else if (record_type == kExtraReparse) {
            const auto tag = payload.vint();
            const auto data_length = payload.vint();
            if (data_length < 8 || data_length > core::kMaxReparseDataBytes ||
                data_length != payload.remaining()) {
                throw FormatError("snapshot reparse metadata is invalid");
            }
            const auto data = payload.take(static_cast<std::size_t>(data_length));
            const auto stored_tag = static_cast<std::uint32_t>(data[0]) |
                                    (static_cast<std::uint32_t>(data[1]) << 8) |
                                    (static_cast<std::uint32_t>(data[2]) << 16) |
                                    (static_cast<std::uint32_t>(data[3]) << 24);
            const auto stored_length = static_cast<std::uint16_t>(data[4]) |
                                       (static_cast<std::uint16_t>(data[5]) << 8);
            if (stored_tag != static_cast<std::uint32_t>(tag) ||
                static_cast<std::uint64_t>(stored_length) + 8 != data_length) {
                throw FormatError("snapshot reparse metadata header does not match");
            }
            entry.meta.has_reparse_data = true;
            entry.meta.reparse_tag = static_cast<std::uint32_t>(tag);
            entry.meta.reparse_data.assign(data.begin(), data.end());
        } else if (record_type == kExtraChunkRefs) {
            if (entry.type != kEntryFile) {
                throw FormatError("snapshot chunk references are only valid on files");
            }
            if (payload.vint() != kChunkTableVersion) {
                throw FormatError("unsupported snapshot chunk-reference version");
            }
            const auto count = payload.vint();
            if (count > kMaxChunkRefsPerEntry) {
                throw FormatError("snapshot entry has too many chunk references");
            }
            entry.chunk_refs.reserve(static_cast<std::size_t>(count));
            for (std::uint64_t i = 0; i < count; ++i) {
                entry.chunk_refs.push_back(payload.vint());
            }
        } else if (record_type == kExtraSparseMap) {
            if (entry.type != kEntryFile) {
                throw FormatError("snapshot sparse metadata is only valid on files");
            }
            if (payload.vint() != 1) throw FormatError("unsupported snapshot sparse-map version");
            const auto count = payload.vint();
            if (count > (1u << 20)) {
                throw FormatError("snapshot sparse map has too many extents");
            }
            entry.sparse.is_sparse = true;
            std::uint64_t previous_end = 0;
            entry.sparse.allocated.reserve(static_cast<std::size_t>(count));
            for (std::uint64_t i = 0; i < count; ++i) {
                const auto offset = payload.vint();
                const auto length = payload.vint();
                if (length == 0 || offset < previous_end || offset > entry.size ||
                    length > entry.size - offset) {
                    throw FormatError("snapshot sparse map has invalid geometry");
                }
                entry.sparse.allocated.push_back({offset, length});
                previous_end = offset + length;
            }
        }
        if (record_type >= kExtraMtime && record_type <= kExtraChunkRefs &&
            payload.has_more()) {
            throw FormatError("snapshot manifest entry extra has trailing data");
        }
    }
    return entry;
}

// Parse a (decrypted) directory image into the block table, entries, and metadata.
ArchiveIndex parse_directory(const ByteVector& directory, std::uint64_t directory_offset,
                             std::uint16_t archive_flags = 0) {
    Reader reader(directory);

    ArchiveIndex index;
    index.meta.live_dedup = (archive_flags & kFlagLiveDedup) != 0;
    const auto block_count = reader.vint();
    index.blocks.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(block_count, 1u << 16)));
    for (std::uint64_t i = 0; i < block_count; ++i) {
        BlockRec block;
        block.compressed_offset = reader.vint();
        block.compressed_size = reader.vint();
        block.uncompressed_size = reader.vint();
        const auto extra_size = reader.vint();
        if (extra_size > reader.remaining()) {
            throw FormatError("block extra exceeds the directory");
        }
        parse_block_extras(reader.take(static_cast<std::size_t>(extra_size)), block);
        if (block.compressed_offset < kHeaderSize ||
            block.compressed_size > directory_offset ||
            block.compressed_offset > directory_offset - block.compressed_size) {
            throw FormatError("block record points outside the archive");
        }
        // Reject an absurd declared block size before it can drive a huge
        // allocation when the block is later decoded. The large-block profile
        // widens this structural limit only when its required header flag is
        // present; old archives retain the conservative 4 GiB bound.
        const auto max_block_size = (archive_flags & kFlagLargeSolidBlocks) != 0
            ? kMaxLargeSolidBlockSize : kMaxLegacySolidBlockSize;
        if (block.uncompressed_size > max_block_size) {
            throw FormatError("block declares an implausible uncompressed size");
        }
        if (block.uncompressed_size > std::numeric_limits<std::size_t>::max()) {
            throw FormatError("block exceeds this platform's addressable size");
        }
        if ((archive_flags & kFlagLargeSolidBlocks) != 0 &&
            block.uncompressed_size > kMaxLegacySolidBlockSize &&
            block.subframes.empty()) {
            throw FormatError("large solid block is missing its bounded subframe map");
        }
        if ((archive_flags & kFlagLargeSolidBlocks) != 0) {
            validate_large_solid_block_map(block);
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
            const auto require_payload_size = [&payload](std::size_t expected,
                                                          const char* name) {
                if (payload.remaining() != expected) {
                    throw FormatError(std::string(name) + " has an invalid payload length");
                }
            };
            if (record_type == kExtraMtime) {
                require_payload_size(8, "mtime record");
                entry.mtime = static_cast<std::int64_t>(payload.u64());
            } else if (record_type == kExtraCrc32) {
                require_payload_size(4, "CRC record");
                entry.crc = payload.u32();
            } else if (record_type == kExtraBlake3) {
                require_payload_size(entry.blake3.size(), "BLAKE3 record");
                const auto digest = payload.take(entry.blake3.size());
                std::copy(digest.begin(), digest.end(), entry.blake3.begin());
                entry.has_blake3 = true;
            } else if (record_type == kExtraWinAttrs) {
                require_payload_size(4, "Windows attribute record");
                entry.meta.has_windows_attributes = true;
                entry.meta.windows_attributes = payload.u32();
            } else if (record_type == kExtraWinTimes) {
                require_payload_size(24, "Windows time record");
                entry.meta.has_windows_times = true;
                entry.meta.windows_creation_time = payload.u64();
                entry.meta.windows_access_time = payload.u64();
                entry.meta.windows_write_time = payload.u64();
            } else if (record_type == kExtraAdsStream) {
                if (entry.ads.size() >= core::kMaxMetadataBlobCount) {
                    throw FormatError("too many alternate data streams in one entry");
                }
                core::AdsStream stream;
                const auto name_length = payload.vint();
                if (name_length == 0 || name_length > (4u << 10) ||
                    name_length > payload.remaining()) {
                    throw FormatError("alternate data stream name is invalid");
                }
                stream.name = payload.str(static_cast<std::size_t>(name_length));
                if (payload.remaining() > core::kMaxAdsBytes) {
                    throw FormatError("alternate data stream exceeds its metadata limit");
                }
                const auto data = payload.take(payload.remaining());
                stream.data.assign(data.begin(), data.end());
                entry.ads.push_back(std::move(stream));
            } else if (record_type == kExtraPosix) {
                require_payload_size(12, "POSIX metadata record");
                entry.meta.has_posix = true;
                entry.meta.posix_mode = payload.u32();
                entry.meta.posix_uid = payload.u32();
                entry.meta.posix_gid = payload.u32();
            } else if (record_type == kExtraSecurityDescriptor) {
                if (payload.remaining() == 0 ||
                    payload.remaining() > core::kMaxSecurityDescriptorBytes) {
                    throw FormatError("security descriptor exceeds its metadata limit");
                }
                const auto descriptor = payload.take(payload.remaining());
                entry.meta.has_windows_security_descriptor = true;
                entry.meta.windows_security_descriptor.assign(descriptor.begin(), descriptor.end());
            } else if (record_type == kExtraXattr) {
                if (entry.meta.xattrs.size() >= core::kMaxMetadataBlobCount) {
                    throw FormatError("too many extended attributes in one entry");
                }
                const auto name_length = payload.vint();
                if (name_length == 0 || name_length > (4u << 10) ||
                    name_length > payload.remaining()) {
                    throw FormatError("extended attribute name is invalid");
                }
                core::MetadataBlob blob;
                blob.name = payload.str(static_cast<std::size_t>(name_length));
                if (payload.remaining() > core::kMaxMetadataBlobBytes) {
                    throw FormatError("extended attribute exceeds its metadata limit");
                }
                const auto value = payload.take(payload.remaining());
                blob.data.assign(value.begin(), value.end());
                entry.meta.xattrs.push_back(std::move(blob));
            } else if (record_type == kExtraReparse) {
                const auto tag = payload.vint();
                const auto data_length = payload.vint();
                if (data_length < 8 ||
                    data_length > core::kMaxReparseDataBytes ||
                    data_length != payload.remaining()) {
                    throw FormatError("reparse metadata is invalid");
                }
                const auto data = payload.take(static_cast<std::size_t>(data_length));
                const auto stored_tag = static_cast<std::uint32_t>(data[0]) |
                                        (static_cast<std::uint32_t>(data[1]) << 8) |
                                        (static_cast<std::uint32_t>(data[2]) << 16) |
                                        (static_cast<std::uint32_t>(data[3]) << 24);
                const auto stored_length = static_cast<std::uint16_t>(data[4]) |
                                           (static_cast<std::uint16_t>(data[5]) << 8);
                if (stored_tag != static_cast<std::uint32_t>(tag) ||
                    static_cast<std::uint64_t>(stored_length) + 8 != data_length) {
                    throw FormatError("reparse metadata header does not match its payload");
                }
                entry.meta.has_reparse_data = true;
                entry.meta.reparse_tag = static_cast<std::uint32_t>(tag);
                entry.meta.reparse_data.assign(data.begin(), data.end());
            } else if (record_type == kExtraSparseMap) {
                if (entry.type != kEntryFile) {
                    throw FormatError("sparse allocation metadata is only valid on files");
                }
                if (payload.vint() != 1) {
                    throw FormatError("unsupported sparse allocation map version");
                }
                const auto extent_count = payload.vint();
                constexpr std::uint64_t kMaxSparseExtents = 1u << 20;
                if (extent_count > kMaxSparseExtents) {
                    throw FormatError("sparse allocation map has too many extents");
                }
                entry.sparse.is_sparse = true;
                entry.sparse.allocated.reserve(static_cast<std::size_t>(extent_count));
                std::uint64_t previous_end = 0;
                for (std::uint64_t extent_index = 0; extent_index < extent_count;
                     ++extent_index) {
                    const auto offset = payload.vint();
                    const auto length = payload.vint();
                    if (length == 0 || offset < previous_end || offset > entry.size ||
                        length > entry.size - offset) {
                        throw FormatError("sparse allocation map contains an invalid extent");
                    }
                    entry.sparse.allocated.push_back({offset, length});
                    previous_end = offset + length;
                }
                if (payload.has_more()) {
                    throw FormatError("sparse allocation map has trailing data");
                }
            } else if (record_type == kExtraChunkRefs) {
                if (entry.type != kEntryFile) {
                    throw FormatError("chunk references are only valid on files");
                }
                if (payload.vint() != kChunkTableVersion) {
                    throw FormatError("unsupported chunk-reference version");
                }
                const auto count = payload.vint();
                if (count > kMaxChunkRefsPerEntry) {
                    throw FormatError("entry has too many chunk references");
                }
                entry.chunk_refs.reserve(static_cast<std::size_t>(count));
                for (std::uint64_t ref = 0; ref < count; ++ref) {
                    entry.chunk_refs.push_back(payload.vint());
                }
                if (payload.has_more()) {
                    throw FormatError("chunk-reference record has trailing data");
                }
            }
            // Unknown extra records are intentionally skipped (consumed by length).
        }
        index.entries.push_back(std::move(entry));
    }

    // Archive-level extra records (comment, lock, …); unknown ones skipped by length.
    const auto archive_extra_count = reader.vint();
    bool saw_encryption_record = false;
    bool saw_chunk_table = false;
    bool saw_snapshot_manifest = false;
    for (std::uint64_t i = 0; i < archive_extra_count; ++i) {
        const auto record_type = reader.vint();
        Reader payload(reader.take(static_cast<std::size_t>(reader.vint())));
        if (record_type == kArchiveComment) {
            index.meta.comment = payload.str(payload.remaining());
        } else if (record_type == kArchiveLock) {
            index.meta.locked = true;
        } else if (record_type == kArchiveEncryption) {
            if (saw_encryption_record) {
                throw FormatError("archive has duplicate encryption records");
            }
            saw_encryption_record = true;
            auto& enc = index.meta.encryption;
            enc.enabled = true;
            enc.v2 = false;
            enc.kdf.algorithm = read_u32_vint(payload, "legacy encryption algorithm");
            enc.kdf.mem_blocks = read_u32_vint(payload, "legacy encryption memory cost");
            enc.kdf.passes = read_u32_vint(payload, "legacy encryption pass count");
            enc.kdf.lanes = read_u32_vint(payload, "legacy encryption lane count");
            const auto salt_len = payload.vint();
            if (salt_len != enc.kdf.salt.size()) {
                throw FormatError("legacy encryption salt must be 16 bytes");
            }
            const auto salt = payload.take(enc.kdf.salt.size());
            std::copy(salt.begin(), salt.end(), enc.kdf.salt.begin());
            const auto check_len = payload.vint();
            if (check_len != core::kAeadOverhead + kKeyCheckPlaintext.size()) {
                throw FormatError("legacy encryption key-check length is invalid");
            }
            const auto check = payload.take(static_cast<std::size_t>(check_len));
            enc.key_check.assign(check.begin(), check.end());
            if (payload.has_more()) {
                throw FormatError("legacy encryption record has trailing data");
            }
        } else if (record_type == kArchiveEncryptionV2) {
            if (saw_encryption_record) {
                throw FormatError("archive has duplicate encryption records");
            }
            saw_encryption_record = true;
            index.meta.encryption =
                parse_encryption_v2_payload(payload.take(payload.remaining()), false);
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
        } else if (record_type == kArchiveCaptureReport) {
            const auto warning_count = payload.vint();
            constexpr std::uint64_t kMaxCaptureWarnings = 1u << 16;
            if (warning_count > kMaxCaptureWarnings) {
                throw FormatError("archive capture report has too many warnings");
            }
            index.meta.capture_warnings.reserve(
                static_cast<std::size_t>(warning_count));
            for (std::uint64_t warning_index = 0; warning_index < warning_count;
                 ++warning_index) {
                const auto read_report_string = [&payload](const char* field) {
                    const auto length = payload.vint();
                    if (length > payload.remaining() ||
                        length > (std::uint64_t{1} << 20)) {
                        throw FormatError(std::string("capture report ") + field +
                                          " is too large");
                    }
                    return payload.str(static_cast<std::size_t>(length));
                };
                index.meta.capture_warnings.push_back({
                    read_report_string("path"), read_report_string("message")});
            }
            if (payload.has_more()) {
                throw FormatError("archive capture report has trailing data");
            }
        } else if (record_type == kArchiveChunkTable) {
            if (saw_chunk_table) {
                throw FormatError("archive has duplicate chunk tables");
            }
            saw_chunk_table = true;
            if (payload.vint() != kChunkTableVersion) {
                throw FormatError("unsupported chunk-table version");
            }
            const auto table_flags = payload.vint();
            if ((table_flags & ~std::uint64_t{1}) != 0) {
                throw FormatError("chunk table uses unsupported flags");
            }
            index.meta.chunk_table = true;
            index.meta.keyed_chunk_ids = (table_flags & 1u) != 0;
            const auto count = payload.vint();
            if (count > kMaxChunkCount ||
                count > payload.remaining() / kMinChunkTableRecordBytes) {
                throw FormatError("archive chunk table is too large");
            }
            index.chunks.reserve(static_cast<std::size_t>(count));
            for (std::uint64_t chunk_index = 0; chunk_index < count; ++chunk_index) {
                ChunkRec chunk;
                chunk.identity.size = payload.vint();
                chunk.crc = payload.u32();
                const auto digest = payload.take(chunk.identity.id.size());
                std::copy(digest.begin(), digest.end(), chunk.identity.id.begin());
                chunk.block_index = payload.vint();
                chunk.offset = payload.vint();
                index.chunks.push_back(std::move(chunk));
            }
            if (payload.has_more()) {
                throw FormatError("chunk table has trailing data");
            }
        } else if (record_type == kArchiveSnapshotManifest) {
            if (saw_snapshot_manifest) {
                throw FormatError("archive has duplicate snapshot manifests");
            }
            saw_snapshot_manifest = true;
            if (payload.vint() != kSnapshotManifestVersion) {
                throw FormatError("unsupported snapshot manifest version");
            }
            const auto snapshot_count = payload.vint();
            if (snapshot_count == 0 || snapshot_count > kMaxSnapshotCount) {
                throw FormatError("snapshot manifest has an invalid snapshot count");
            }
            index.meta.snapshots.reserve(static_cast<std::size_t>(snapshot_count));
            for (std::uint64_t snapshot_index = 0;
                 snapshot_index < snapshot_count; ++snapshot_index) {
                SnapshotRec snapshot;
                const auto name_length = payload.vint();
                if (name_length == 0 || name_length > kMaxSnapshotNameBytes ||
                    name_length > payload.remaining()) {
                    throw FormatError("snapshot manifest name is invalid");
                }
                snapshot.name = payload.str(static_cast<std::size_t>(name_length));
                validate_snapshot_name(snapshot.name);
                snapshot.generation = payload.vint();
                snapshot.created = static_cast<std::int64_t>(payload.u64());
                const auto snapshot_entry_count = payload.vint();
                if (snapshot_entry_count > kMaxSnapshotEntries ||
                    snapshot_entry_count >
                        payload.remaining() / kMinSnapshotEntryRecordBytes) {
                    throw FormatError("snapshot manifest has too many entries");
                }
                // Do not reserve from the count. A malformed archive can claim
                // millions of entries while providing only a few payload bytes;
                // parse each length-prefixed body before growing this vector.
                for (std::uint64_t entry_index = 0; entry_index < snapshot_entry_count;
                     ++entry_index) {
                    const auto body_size = payload.vint();
                    if (body_size > payload.remaining()) {
                        throw FormatError("snapshot manifest entry is truncated");
                    }
                    snapshot.entries.push_back(parse_snapshot_entry_body(
                        payload.take(static_cast<std::size_t>(body_size))));
                }
                index.meta.snapshots.push_back(std::move(snapshot));
            }
            if (payload.has_more()) {
                throw FormatError("snapshot manifest has trailing data");
            }
        } else if (record_type == kArchiveDedupProfile) {
            if (index.meta.dedup_profile.present) {
                throw FormatError("archive has duplicate deduplication profiles");
            }
            auto& profile = index.meta.dedup_profile;
            if (payload.vint() != kDedupProfileVersion) {
                throw FormatError("unsupported deduplication profile version");
            }
            profile.present = true;
            profile.chunker = payload.vint();
            profile.chunker_version = payload.vint();
            profile.minimum_size = payload.vint();
            profile.average_size = payload.vint();
            profile.maximum_size = payload.vint();
            profile.table_id = payload.vint();
            profile.packing = payload.vint();
            if (payload.has_more()) {
                throw FormatError("deduplication profile has trailing data");
            }
        }
    }

    if (reader.has_more()) {
        throw FormatError("archive directory has trailing data");
    }

    if (saw_snapshot_manifest && !saw_chunk_table) {
        throw FormatError("snapshot manifest is missing its chunk table");
    }
    return index;
}

void validate_dedup_profile(const DedupProfile& profile, bool format_error) {
    const bool valid = profile.present && profile.chunker == 1 &&
        profile.chunker_version == 1 && profile.table_id == 1 &&
        profile.packing == 0 && profile.minimum_size >= (4u << 10) &&
        profile.average_size >= profile.minimum_size &&
        profile.maximum_size >= profile.average_size &&
        profile.maximum_size <= (64u << 20) &&
        profile.maximum_size <= std::numeric_limits<std::size_t>::max();
    if (!valid) {
        if (format_error) throw FormatError("archive has an invalid deduplication profile");
        throw std::invalid_argument("invalid deduplication profile");
    }
}

void validate_chunk_index(const ArchiveIndex& index, const ArchiveLayout& layout) {
    const bool snapshot_flag = (layout.flags & kFlagChunkTable) != 0;
    const bool live_flag = (layout.flags & kFlagLiveDedup) != 0;
    if (snapshot_flag && live_flag) {
        throw FormatError("archive cannot combine live deduplication and snapshot history");
    }
    const bool chunk_flag = snapshot_flag || live_flag;
    if (chunk_flag != index.meta.chunk_table) {
        throw FormatError("chunk-table header flag does not match directory metadata");
    }
    if (!index.meta.chunk_table) {
        if (!index.chunks.empty() || !index.meta.snapshots.empty() ||
            index.meta.dedup_profile.present || index.meta.live_dedup) {
            throw FormatError("ordinary archive contains chunk-addressed metadata");
        }
        return;
    }
    if (layout.version < kArchiveVersion5) {
        throw FormatError("chunk-addressed archive requires AXAR v5");
    }
    if (snapshot_flag != !index.meta.snapshots.empty()) {
        throw FormatError("snapshot-history flag does not match its manifest");
    }
    if (live_flag != index.meta.live_dedup) {
        throw FormatError("live-deduplication flag does not match archive metadata");
    }
    if (live_flag) {
        validate_dedup_profile(index.meta.dedup_profile, true);
    } else if (index.meta.dedup_profile.present) {
        validate_dedup_profile(index.meta.dedup_profile, true);
    }
    if (index.meta.keyed_chunk_ids && !index.meta.encryption.enabled) {
        throw FormatError("keyed chunk identifiers require archive encryption");
    }
    for (const auto& chunk : index.chunks) {
        if (chunk.identity.size == 0 || chunk.block_index >= index.blocks.size()) {
            throw FormatError("chunk table contains an invalid chunk reference");
        }
        const auto& block = index.blocks[static_cast<std::size_t>(chunk.block_index)];
        if (chunk.offset > block.uncompressed_size ||
            chunk.identity.size > block.uncompressed_size - chunk.offset) {
            throw FormatError("chunk table points outside a block");
        }
    }

    const auto validate_entries = [&index](const std::vector<EntryRec>& entries) {
        validate_snapshot_entry_paths_for_read(entries);
        for (const auto& entry : entries) {
            if (entry.type != kEntryFile) {
                if (!entry.chunk_refs.empty()) {
                    throw FormatError("non-file chunk-addressed entry has chunk references");
                }
                continue;
            }
            std::uint64_t total = 0;
            for (const auto ref : entry.chunk_refs) {
                if (ref >= index.chunks.size()) {
                    throw FormatError("entry points outside the chunk table");
                }
                const auto size = index.chunks[static_cast<std::size_t>(ref)].identity.size;
                if (total > entry.size || size > entry.size - total) {
                    throw FormatError("chunk sizes overflow the file");
                }
                total += size;
            }
            if (total != entry.size) {
                throw FormatError("chunks do not cover the file");
            }
        }
    };

    std::unordered_set<std::string> names;
    names.reserve(index.meta.snapshots.size());
    for (const auto& snapshot : index.meta.snapshots) {
        validate_snapshot_name(snapshot.name);
        if (!names.insert(snapshot.name).second) {
            throw FormatError("snapshot manifest contains duplicate names");
        }
        validate_entries(snapshot.entries);
    }
    validate_entries(index.entries);
}

// Read the directory of a non-directory-encrypted archive (plaintext directory).
// A directory-encrypted archive needs the password — callers use load_index instead.
void validate_large_solid_block_payload(const ByteSource& source,
                                        const BlockRec& record);

ArchiveIndex read_index(const ByteSource& source) {
    const auto layout = read_layout(source);
    if ((layout.flags & kFlagEncryptedDirectory) != 0) {
        throw std::runtime_error("archive directory is encrypted; a password is required");
    }
    const auto directory = source.read(layout.directory_offset, layout.directory_size);
    auto index = parse_directory(directory, layout.directory_offset, layout.flags);
    index.meta.large_solid_blocks =
        (layout.flags & kFlagLargeSolidBlocks) != 0;
    if (index.meta.large_solid_blocks && index.meta.encryption.enabled) {
        throw FormatError("encrypted archives cannot use the large solid-block profile");
    }
    if (index.meta.large_solid_blocks) {
        for (const auto& block : index.blocks) {
            validate_large_solid_block_payload(source, block);
        }
    }
    const bool v2_flag = (layout.flags & kFlagEncryptionV2) != 0;
    if (v2_flag != index.meta.encryption.v2) {
        throw FormatError("archive encryption-v2 flag does not match its directory metadata");
    }
    validate_chunk_index(index, layout);
    return index;
}

std::uint64_t archive_block_region_end(const ArchiveLayout& layout,
                                      const ArchiveIndex& index) {
    if (index.blocks.empty()) {
        // An encrypted-directory archive still has its plaintext preamble even
        // when it contains no data blocks; a block-only archive starts at the
        // fixed header end.
        return (layout.flags & kFlagEncryptedDirectory) != 0
            ? layout.directory_offset
            : kHeaderSize;
    }
    const auto& last = index.blocks.back();
    if (last.compressed_size > std::numeric_limits<std::uint64_t>::max() -
                                   last.compressed_offset) {
        throw FormatError("archive block region overflows");
    }
    return last.compressed_offset + last.compressed_size;
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

ByteVector decode_solid_block(const ByteSource& source,
                              const ArchiveIndex& index,
                              std::uint64_t block_index,
                              std::size_t thread_count,
                              const std::shared_ptr<OperationControl>& operation,
                              const std::optional<core::CryptoKey>& key,
                              const std::function<void(std::uint64_t, std::uint64_t)>&
                                  decoded_bytes_progress = {}) {
    operation_checkpoint(operation);
    if (block_index >= index.blocks.size()) {
        throw FormatError("block index out of range");
    }

    const auto& record = index.blocks[block_index];
    auto compressed = source.read_compressed(record.compressed_offset,
                                             record.compressed_size);
    if (key) {
        // Verify + decrypt before decompressing; the index is the AEAD's AD.
        std::vector<std::uint8_t> plaintext;
        if (!core::aead_open(*key, compressed, block_associated_data(block_index), plaintext)) {
            throw FormatError("block authentication failed (wrong password or corrupt archive)");
        }
        compressed = std::move(plaintext);
    }

    // Bound the decode to this block's declared size; the equality check below
    // then confirms the block produced exactly what the directory promised.
    DecompressionOptions decode_options;
    decode_options.max_output_size = static_cast<std::size_t>(record.uncompressed_size);
    decode_options.thread_count = thread_count;
    decode_options.operation = operation;
    decode_options.decoded_bytes_progress = decoded_bytes_progress;
    auto decoded = decompress(compressed, decode_options);
    if (decoded.size() != record.uncompressed_size) {
        throw FormatError("block expands to an unexpected size");
    }
    return decoded;
}

struct AxCFrameContext {
    std::uint16_t version = 0;
    std::uint8_t codec = 0;
    bool transforms = false;
    std::uint64_t payload_offset = 0;
};

AxCFrameContext read_axc_frame_context(const ByteSource& source,
                                       const BlockRec& record) {
    const auto prefix_size = std::min<std::uint64_t>(record.compressed_size, 36);
    const auto prefix = source.read_compressed(record.compressed_offset, prefix_size);
    constexpr std::array<std::uint8_t, 8> magic = {'A', 'X', 'I', 'O', 'M', 'C', '1', 0};
    if (prefix.size() < 32 || !std::equal(magic.begin(), magic.end(), prefix.begin())) {
        throw FormatError("subframe map points to an invalid AXC block");
    }
    const auto version = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(prefix[8]) |
        (static_cast<std::uint16_t>(prefix[9]) << 8));
    const bool legacy = version == 4;
    if (!legacy && version != 5 && version != 6 && version != 7 &&
        version != 8 && version != 9 && version != 10) {
        throw FormatError("subframe map points to an unsupported AXC version");
    }
    AxCFrameContext context;
    context.version = version;
    context.codec = prefix[10];
    if (legacy) {
        context.payload_offset = 32;
    } else {
        if (prefix.size() < 36) {
            throw FormatError("AXC block header is truncated");
        }
        const auto flags = prefix[11];
        if ((flags & ~1u) != 0) {
            throw FormatError("AXC block flags are invalid");
        }
        const auto metadata_size = static_cast<std::uint64_t>(prefix[32]) |
            (static_cast<std::uint64_t>(prefix[33]) << 8) |
            (static_cast<std::uint64_t>(prefix[34]) << 16) |
            (static_cast<std::uint64_t>(prefix[35]) << 24);
        context.transforms = (flags & 1u) != 0;
        context.payload_offset = 36 + metadata_size;
    }
    if (context.payload_offset > record.compressed_size) {
        throw FormatError("AXC block metadata exceeds its compressed size");
    }
    return context;
}

void validate_large_solid_block_payload(const ByteSource& source,
                                        const BlockRec& record) {
    if (record.uncompressed_size <= kMaxLegacySolidBlockSize) {
        return;
    }
    const auto context = read_axc_frame_context(source, record);
    if (context.version != 10 ||
        context.codec != static_cast<std::uint8_t>(core::CodecId::lzma2) ||
        context.transforms) {
        throw FormatError(
            "large solid block must use an AXC v10 LZMA2 payload without transforms");
    }
    constexpr std::uint64_t kExternalHeaderSize = 17;
    if (context.payload_offset > record.compressed_size ||
        kExternalHeaderSize > record.compressed_size - context.payload_offset) {
        throw FormatError("large solid block has a truncated external codec header");
    }
    if (record.compressed_offset >
        std::numeric_limits<std::uint64_t>::max() - context.payload_offset) {
        throw FormatError("large solid block payload offset overflows");
    }
    const auto header = source.read_compressed(
        record.compressed_offset + context.payload_offset, kExternalHeaderSize);
    constexpr std::array<std::uint8_t, 4> kExternalMagic = {'A', 'X', 'E', 'C'};
    if (header.size() != kExternalHeaderSize ||
        !std::equal(kExternalMagic.begin(), kExternalMagic.end(), header.begin()) ||
        header[4] != 1 || header[5] != 1 || header[6] != 0 || header[7] != 0) {
        throw FormatError("large solid block has an invalid external codec header");
    }
    const auto read_header_u32 = [&header](std::size_t offset) {
        return static_cast<std::uint32_t>(header[offset]) |
               (static_cast<std::uint32_t>(header[offset + 1]) << 8) |
               (static_cast<std::uint32_t>(header[offset + 2]) << 16) |
               (static_cast<std::uint32_t>(header[offset + 3]) << 24);
    };
    const auto chunk_size = static_cast<std::uint64_t>(read_header_u32(8));
    const auto chunk_count = static_cast<std::uint64_t>(read_header_u32(12));
    if (chunk_size < (std::uint64_t{256} << 10) ||
        chunk_size > kMaxLzmaCodecChunkSize ||
        chunk_count != record.subframes.size() || chunk_count == 0 ||
        header[16] > 40) {
        throw FormatError("large solid block has invalid external chunk geometry");
    }
    const auto dictionary_size = header[16] == 40
        ? std::numeric_limits<std::uint32_t>::max()
        : (static_cast<std::uint64_t>(2u | (header[16] & 1u))
           << (header[16] / 2u + 11u));
    if (dictionary_size > std::max(chunk_size, std::uint64_t{1} << 12)) {
        throw FormatError("large solid block dictionary exceeds its chunk bound");
    }
    const auto expected_count =
        1 + (record.uncompressed_size - 1) / chunk_size;
    if (chunk_count != expected_count) {
        throw FormatError("large solid block chunk count does not match its size");
    }
    std::uint64_t uncompressed_offset = 0;
    for (const auto& frame : record.subframes) {
        const auto expected_size = std::min<std::uint64_t>(
            chunk_size, record.uncompressed_size - uncompressed_offset);
        if (frame.uncompressed_offset != uncompressed_offset ||
            frame.uncompressed_size != expected_size ||
            frame.lzma_property != header[16]) {
            throw FormatError("large solid block subframe geometry is inconsistent");
        }
        uncompressed_offset += expected_size;
    }
    if (uncompressed_offset != record.uncompressed_size) {
        throw FormatError("large solid block subframes do not cover the block");
    }
}

class BlockSource {
public:
    using DecodeProgressCallback =
        std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>;

    BlockSource(const ByteSource& source,
                const ArchiveIndex& index,
                std::size_t thread_count,
                std::shared_ptr<OperationControl> operation,
                std::optional<core::CryptoKey> key = std::nullopt,
                bool use_subframes = false)
        : source_(source),
          index_(index),
          thread_count_(thread_count),
          operation_(std::move(operation)),
          key_(std::move(key)),
          use_subframes_(use_subframes) {}

    void set_decode_progress(DecodeProgressCallback callback) {
        decode_progress_ = std::move(callback);
    }

    const ByteVector& block(std::uint64_t block_index) {
        if (block_index != cached_index_) {
            const auto progress = decode_progress_;
            cached_ = decode_solid_block(
                source_, index_, block_index, thread_count_, operation_, key_,
                [progress, block_index](std::uint64_t done, std::uint64_t total) {
                    if (progress) {
                        progress(block_index, done, total);
                    }
                });
            cached_index_ = block_index;
        }
        return cached_;
    }

    ByteVector chunk(std::uint64_t chunk_index) {
        if (chunk_index >= index_.chunks.size()) {
            throw FormatError("chunk index out of range");
        }
        const auto& record = index_.chunks[static_cast<std::size_t>(chunk_index)];
        const auto& bytes = block(record.block_index);
        if (record.offset > bytes.size() ||
            record.identity.size > bytes.size() - record.offset) {
            throw FormatError("chunk points outside its decoded block");
        }
        const auto chunk_bytes = std::span<const std::uint8_t>(
            bytes.data() + static_cast<std::ptrdiff_t>(record.offset),
            static_cast<std::size_t>(record.identity.size));
        auto crc = core::crc32_init();
        crc = core::crc32_update(crc, chunk_bytes);
        if (core::crc32_final(crc) != record.crc) {
            throw FormatError("snapshot chunk checksum mismatch");
        }
        const auto digest = chunk_digest(
            chunk_bytes, key_ ? &*key_ : nullptr, index_.meta.keyed_chunk_ids);
        if (digest != record.identity.id) {
            throw FormatError("snapshot chunk identity mismatch");
        }
        return ByteVector(
            bytes.begin() + static_cast<std::ptrdiff_t>(record.offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(record.offset +
                                                         record.identity.size));
    }

    std::uint64_t block_size(std::uint64_t block_index) const {
        if (block_index >= index_.blocks.size()) {
            throw FormatError("block index out of range");
        }
        return index_.blocks[static_cast<std::size_t>(block_index)].uncompressed_size;
    }

    ByteVector read_slice(std::uint64_t block_index,
                          std::uint64_t offset,
                          std::uint64_t length) {
        if (block_index >= index_.blocks.size()) {
            throw FormatError("block index out of range");
        }
        const auto& record = index_.blocks[static_cast<std::size_t>(block_index)];
        if (offset > record.uncompressed_size ||
            length > record.uncompressed_size - offset) {
            throw FormatError("requested block range is outside the block");
        }
        if (length == 0) return {};

        if (!use_subframes_ || key_ || record.subframes.empty()) {
            const auto& bytes = block(block_index);
            return ByteVector(
                bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        }

        const auto context = seek_context(block_index, record);
        if (context.transforms) {
            const auto& bytes = block(block_index);
            return ByteVector(
                bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        }
        const auto request_end = offset + length;
        ByteVector result(static_cast<std::size_t>(length));
        std::uint64_t copied = 0;
        for (std::size_t frame_index = 0; frame_index < record.subframes.size();
             ++frame_index) {
            const auto& frame = record.subframes[frame_index];
            const auto frame_end = frame.uncompressed_offset + frame.uncompressed_size;
            if (frame_end <= offset) continue;
            if (frame.uncompressed_offset >= request_end) break;
            const auto copy_begin = std::max(offset, frame.uncompressed_offset);
            const auto copy_end = std::min(request_end, frame_end);
            const auto& decoded = subframe(block_index, frame_index, context);
            const auto source_offset = copy_begin - frame.uncompressed_offset;
            const auto target_offset = copy_begin - offset;
            const auto copy_size = copy_end - copy_begin;
            std::copy(decoded.begin() + static_cast<std::ptrdiff_t>(source_offset),
                      decoded.begin() + static_cast<std::ptrdiff_t>(source_offset + copy_size),
                      result.begin() + static_cast<std::ptrdiff_t>(target_offset));
            copied += copy_size;
        }
        if (copied != length) {
            throw FormatError("subframe map does not cover the requested range");
        }
        return result;
    }

private:
    const AxCFrameContext& seek_context(std::uint64_t block_index,
                                        const BlockRec& record) {
        if (seek_context_block_ != block_index || !seek_context_) {
            seek_context_ = read_axc_frame_context(source_, record);
            seek_context_block_ = block_index;
            cached_subframe_block_ = std::numeric_limits<std::uint64_t>::max();
            cached_subframe_index_ = std::numeric_limits<std::size_t>::max();
            cached_subframe_.clear();
        }
        const auto& context = *seek_context_;
        const auto expected_parallel = static_cast<std::uint8_t>(
            core::CodecId::parallel_blocks);
        if (context.transforms) return context;
        for (const auto& frame : record.subframes) {
            if (frame.compressed_offset < context.payload_offset) {
                throw FormatError("subframe map points into the AXC header");
            }
            if (frame.kind == kSubframeStore &&
                context.codec != static_cast<std::uint8_t>(core::CodecId::store)) {
                throw FormatError("stored subframe map does not match its AXC codec");
            }
            if (frame.kind == kSubframeParallelBlock && context.codec != expected_parallel) {
                throw FormatError("parallel subframe map does not match its AXC codec");
            }
            if (frame.kind == kSubframeParallelBlock &&
                ((frame.codec == 6 && context.version < 6) ||
                 (frame.codec == 7 && context.version < 7) ||
                 ((frame.codec == 8 || frame.codec == 9) && context.version < 8) ||
                 (frame.codec == 10 && context.version < 9))) {
                throw FormatError("parallel subframe codec requires a newer AXC version");
            }
            if (frame.kind == kSubframeExternalChunk &&
                frame.codec != context.codec) {
                throw FormatError("external subframe map does not match its AXC codec");
            }
            if (frame.kind == kSubframeExternalChunk && context.version < 10) {
                throw FormatError("external subframe codec requires AXC version 10");
            }
        }
        return context;
    }

    const ByteVector& subframe(std::uint64_t block_index,
                               std::size_t frame_index,
                               const AxCFrameContext& context) {
        if (cached_subframe_block_ == block_index &&
            cached_subframe_index_ == frame_index) {
            return cached_subframe_;
        }
        const auto& record = index_.blocks[static_cast<std::size_t>(block_index)];
        const auto& frame = record.subframes[frame_index];
        if (frame.compressed_offset > record.compressed_size ||
            frame.compressed_size > record.compressed_size - frame.compressed_offset ||
            frame.compressed_size >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            frame.uncompressed_size >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            throw FormatError("subframe exceeds the platform size limit");
        }
        if (frame.compressed_offset >
            std::numeric_limits<std::uint64_t>::max() - record.compressed_offset) {
            throw FormatError("subframe archive offset overflows");
        }
        const auto encoded = source_.read_compressed(
            record.compressed_offset + frame.compressed_offset,
            frame.compressed_size);
        cached_subframe_.resize(static_cast<std::size_t>(frame.uncompressed_size));
        if (frame.kind == kSubframeStore) {
            if (encoded.size() != cached_subframe_.size()) {
                throw FormatError("stored subframe size does not match its map");
            }
            std::copy(encoded.begin(), encoded.end(), cached_subframe_.begin());
        } else if (frame.kind == kSubframeParallelBlock) {
            codec::decode_parallel_block_frame(
                encoded, cached_subframe_, frame.codec);
        } else if (frame.kind == kSubframeExternalChunk) {
            CompressionMethod method = CompressionMethod::store;
            if (context.codec == static_cast<std::uint8_t>(core::CodecId::zstandard)) {
                method = CompressionMethod::zstandard;
            } else if (context.codec == static_cast<std::uint8_t>(core::CodecId::lzma2)) {
                method = CompressionMethod::lzma2;
            } else if (context.codec == static_cast<std::uint8_t>(core::CodecId::deflate)) {
                method = CompressionMethod::deflate;
            } else {
                throw FormatError("external subframe map has an invalid AXC codec");
            }
            cached_subframe_ = codec::decode_external_codec_frame(
                encoded, method, cached_subframe_.size(), frame.lzma_property);
        } else {
            throw FormatError("unknown subframe kind");
        }
        if (decode_progress_) {
            decode_progress_(block_index,
                             frame.uncompressed_offset + frame.uncompressed_size,
                             record.uncompressed_size);
        }
        cached_subframe_block_ = block_index;
        cached_subframe_index_ = frame_index;
        return cached_subframe_;
    }

    const ByteSource& source_;
    const ArchiveIndex& index_;
    std::size_t thread_count_ = 0;
    std::shared_ptr<OperationControl> operation_;
    std::optional<core::CryptoKey> key_;
    bool use_subframes_ = false;
    DecodeProgressCallback decode_progress_;
    std::uint64_t cached_index_ = std::numeric_limits<std::uint64_t>::max();
    ByteVector cached_;
    std::uint64_t seek_context_block_ = std::numeric_limits<std::uint64_t>::max();
    std::optional<AxCFrameContext> seek_context_;
    std::uint64_t cached_subframe_block_ = std::numeric_limits<std::uint64_t>::max();
    std::size_t cached_subframe_index_ = std::numeric_limits<std::size_t>::max();
    ByteVector cached_subframe_;
};

void read_file_bytes(BlockSource& source,
                     std::size_t block_count,
                     const EntryRec& entry,
                     const std::shared_ptr<OperationControl>& operation,
                     const std::function<void(std::span<const std::uint8_t>)>& sink,
                     std::size_t io_buffer_size = 0) {
    std::uint64_t remaining = entry.size;
    std::uint64_t block_index = entry.first_block;
    std::uint64_t within = entry.offset;
    const std::size_t io_chunk = effective_io_buffer_size(io_buffer_size);

    if (!entry.chunk_refs.empty()) {
        std::uint64_t emitted = 0;
        for (const auto ref : entry.chunk_refs) {
            operation_checkpoint(operation);
            auto chunk = source.chunk(ref);
            std::size_t offset = 0;
            while (offset < chunk.size()) {
                const auto take = std::min<std::size_t>(io_chunk, chunk.size() - offset);
                sink(std::span<const std::uint8_t>(chunk.data() + offset, take));
                offset += take;
                emitted += take;
            }
        }
        if (emitted != entry.size) {
            throw FormatError("snapshot chunks do not cover the file");
        }
        return;
    }

    while (remaining > 0) {
        operation_checkpoint(operation);
        if (block_index >= block_count) {
            throw FormatError("file extends past the last block");
        }
        const auto block_size = source.block_size(block_index);
        if (within > block_size) {
            throw FormatError("file offset lies past its block");
        }
        const auto available = block_size - within;
        const auto take = std::min<std::uint64_t>(
            std::min<std::uint64_t>(available, remaining), io_chunk);
        const auto bytes = source.read_slice(block_index, within, take);
        sink(bytes);
        remaining -= take;
        within += take;
        // `take` is capped by the extraction I/O buffer, so the returned
        // slice can end well before the solid block does. Advance the archive
        // block only after consuming the block's declared uncompressed range.
        if (within >= block_size) {
            within = 0;
            ++block_index;
        }
    }
}

}  // namespace

// Shared with container_formats.cpp via container_internal.hpp (SFX detection).
// The layout and both the v2 and v1 read paths live in sfx_image.cpp, which the
// SFX runtime links as well so the two cannot disagree.
std::optional<std::pair<std::uint64_t, std::uint64_t>> sfx_embedded_payload_range(
    const fs::path& path) {
    const auto payload = sfx_locate_payload(path);
    if (!payload) return std::nullopt;
    return std::make_pair(payload->payload_offset, payload->payload_size);
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> sfx_embedded_archive_range(
    const fs::path& path) {
    const auto embedded = sfx_embedded_payload_range(path);
    if (!embedded || embedded->second < kHeaderSize + kFooterSize) {
        return std::nullopt;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    const std::uint64_t archive_offset = embedded->first;
    stream.seekg(static_cast<std::streamoff>(archive_offset), std::ios::beg);
    std::array<std::uint8_t, 8> header{};
    stream.read(reinterpret_cast<char*>(header.data()),
                static_cast<std::streamsize>(header.size()));
    if (!stream || !std::equal(kArchiveMagic.begin(), kArchiveMagic.end(), header.begin())) {
        return std::nullopt;
    }
    return embedded;
}

namespace {

ArchiveStream open_archive(const fs::path& archive_path, std::uint64_t& file_size) {
    if (auto volumes = try_open_volume_archive_source(archive_path)) {
        file_size = volumes->size();
        return ArchiveStream(std::move(volumes));
    }
    std::ifstream stream(archive_path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open archive: " +
                                 core::path_to_utf8(archive_path));
    }
    stream.seekg(0, std::ios::end);
    file_size = static_cast<std::uint64_t>(stream.tellg());
    std::uint64_t archive_offset = 0;
    if (const auto embedded = sfx_embedded_archive_range(archive_path)) {
        archive_offset = embedded->first;
        file_size = embedded->second;
    }
    return ArchiveStream(std::move(stream), archive_offset, file_size);
}

// Compress a list of scanned items into solid blocks, appending to `out` and to the
// `blocks`/`entries` vectors and advancing `written`. New blocks are numbered from
// blocks.size(), so seeding `blocks`/`entries`/`written` with an existing archive's
// contents (and having pre-copied its block bytes into `out`) appends to it.
template <typename Output>
void compress_items_into(Output& out, std::uint64_t& written,
                         std::vector<BlockRec>& blocks, std::vector<EntryRec>& entries,
                         const std::vector<ScanItem>& items, const CompressionOptions& options,
                         std::size_t block_size,
                         const std::shared_ptr<OperationControl>& operation,
                         std::uint64_t total_bytes, std::uint64_t total_items,
                         std::uint64_t& completed_bytes_out, std::uint64_t& completed_items_out,
                         bool allow_unreadable_skips,
                         const core::CryptoKey* key = nullptr,
                         ArchiveMeta* archive_meta = nullptr,
                         const ArchiveReuseByPath* reuse_by_path = nullptr,
                         ArchiveReuseStats* reuse_stats = nullptr) {
    validate_large_solid_block_options(options, block_size);
    const bool spool_large_blocks = requests_large_solid_blocks(block_size);
    if (spool_large_blocks && archive_meta != nullptr) {
        archive_meta->large_solid_blocks = true;
    }
    ByteVector buffer;
    std::uint64_t buffer_size = 0;
    std::uint32_t buffer_crc = core::crc32_init();
    fs::path buffer_spool_path;
    std::ofstream buffer_spool;
    std::vector<std::unique_ptr<TempFileGuard>> raw_spool_guards;
    std::string buffer_path;
    std::vector<CompressionTransformRange> buffer_transform_ranges;
    std::uint64_t current_block = blocks.size();
    std::vector<char> io_buffer(effective_io_buffer_size(options.io_buffer_size));
    const auto track_raw_spool = [&](const fs::path& path) {
        raw_spool_guards.push_back(std::make_unique<TempFileGuard>(path));
    };
    const auto record_capture_warning = [&](const OperationWarning& warning) {
        if (operation) operation->add_warning(warning);
        if (archive_meta != nullptr) archive_meta->capture_warnings.push_back(warning);
        if (options.strict_metadata) {
            throw std::runtime_error(warning.message + ": " + warning.path);
        }
    };

    // Shared progress counters: the reader thread advances items, the pipeline
    // worker advances bytes (a block's bytes complete when it finishes
    // compressing). Both threads read them for progress reports.
    std::atomic<std::uint64_t> completed_bytes{completed_bytes_out};
    std::atomic<std::uint64_t> completed_items{completed_items_out};
    std::atomic<std::uint64_t> read_bytes{completed_bytes_out};
    std::atomic<std::uint64_t> reused_items{0};
    std::atomic<std::uint64_t> reused_bytes{0};
    const std::uint64_t compression_base = completed_bytes_out;

    // A sole regular file has an exact mapping between the operation-wide byte
    // counter and current-file progress, even when it spans several solid
    // blocks. Publish that mapping during the expensive compression phase; the
    // reader's earlier per-file telemetry must not collapse back to zero once
    // codec workers take over.
    const ScanItem* single_file = nullptr;
    bool multiple_files = false;
    for (const auto& item : items) {
        if (item.is_directory || item.is_symlink || item.is_reparse_point) continue;
        if (single_file != nullptr) {
            multiple_files = true;
            break;
        }
        single_file = &item;
    }
    std::uint64_t single_file_total = 0;
    if (multiple_files) {
        single_file = nullptr;
    } else if (single_file != nullptr) {
        std::error_code size_error;
        single_file_total = fs::file_size(single_file->absolute, size_error);
        if (size_error) single_file = nullptr;
    }
    const auto single_file_progress = [&](std::uint64_t overall) {
        const std::uint64_t completed = single_file != nullptr && overall > compression_base
            ? std::min(single_file_total, overall - compression_base)
            : 0;
        return std::pair{completed, single_file != nullptr ? single_file_total : 0};
    };

    // Maps a file's on-disk identity to the archive path under which its bytes were
    // first stored; later paths sharing that identity become hardlink entries.
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, std::string> hardlinks;
    // Content identities completed earlier in this same writer pass. A later
    // same-sized input is hashed before it enters the solid buffer and can reuse
    // the earlier range. This makes initial creation and one add batch coalesce
    // duplicate files without changing ordinary AXAR directory semantics.
    std::map<ContentIdentity, ArchiveDataReference> batch_reuse_candidates;
    std::unordered_set<std::uint64_t> batch_candidate_sizes;

    // The usual pipeline overlaps one reader with a codec that consumes all
    // cores. Thorough profiles are different: their ratio is limited by the
    // per-codec-block window, so feeding every core from one solid block turns
    // a 64 MiB solid block into many 4 MiB match windows. Split the core budget
    // across a few independent solid-block jobs instead. Each job still emits a
    // normal self-contained .axc block; only scheduling and writer policy vary.
    const auto execution_budget = selected_thread_count(options.thread_count);
    const auto geometry_budget =
        selected_geometry_thread_count(options.thread_count);
    const bool use_outer_parallelism =
        options.enable_optimal_parser && block_size >= (std::size_t{4} << 20) &&
        geometry_budget >= 8;
    const auto outer_worker_count = use_outer_parallelism
        // Two outer jobs keep 8 MiB-or-larger inner windows on a 16-core
        // machine without multiplying the optimal parser's large working set
        // into a cache and memory-bandwidth bottleneck.
        ? std::min<std::size_t>(2, geometry_budget / 4)
        : std::size_t{1};
    const auto inner_thread_count = std::max<std::size_t>(
        1, geometry_budget / outer_worker_count);
    const auto max_inflight_blocks = outer_worker_count * 2;
    // Outer solid-block workers count toward the operation's CPU budget. Give
    // the shared executor only the remaining helper slots; every nested block,
    // candidate, and entropy task then uses this one pool instead of creating a
    // fresh pool for each solid block.
    const auto helper_budget = execution_budget > outer_worker_count
        ? execution_budget - outer_worker_count
        : std::size_t{0};
    const auto task_executor = helper_budget != 0
        ? std::make_shared<core::TaskExecutor>(helper_budget + 1)
        : std::shared_ptr<core::TaskExecutor>{};

    struct PendingBlock {
        std::uint64_t index = 0;
        ByteVector data;
        fs::path spool_path;
        std::uint64_t original_size = 0;
        std::uint32_t crc = 0;
        std::vector<CompressionTransformRange> transform_ranges;
        std::string path;
    };
    struct CompletedBlock {
        std::uint64_t index = 0;
        std::uint64_t original_size = 0;
        ByteVector payload;
        fs::path payload_path;
        std::uint64_t payload_size = 0;
        fs::path raw_spool_path;
        std::vector<SubframeRec> subframes;
        std::string path;
    };

    std::mutex pipeline_mutex;
    std::condition_variable pipeline_cv;
    std::deque<PendingBlock> pending;
    std::map<std::uint64_t, CompletedBlock> completed;
    std::size_t inflight_blocks = 0;
    bool pipeline_done = false;
    std::exception_ptr pipeline_error;
    std::uint64_t next_write_block = current_block;
    std::atomic<std::uint64_t> reported_progress{completed_bytes_out};
    std::atomic_bool codec_progress_started{false};
    // Bytes the in-flight solid blocks' encoders have scanned so far, summed
    // across the concurrent jobs. Each block contributes its own monotonic
    // share; a shared base+done high-water mark would let the furthest block's
    // first tick swallow every earlier block's progress and reduce the bar to
    // solid-block-sized jumps.
    std::atomic<std::uint64_t> inflight_done{0};
    // Actual archive bytes committed by the ordered writer, including the
    // existing header/preamble, and the source bytes represented by those
    // committed blocks. Codec workers must not publish speculative output.
    std::atomic<std::uint64_t> compressed_bytes{written};
    std::atomic<std::uint64_t> compressed_source_bytes{0};

    const auto throughput_progress = [&] {
        return codec_progress_started.load(std::memory_order_relaxed)
            ? reported_progress.load(std::memory_order_relaxed)
            : read_bytes.load(std::memory_order_relaxed);
    };
    const auto displayed_progress = [&] {
        return reported_progress.load(std::memory_order_relaxed);
    };

    auto publish_progress = [&](const std::string& path) {
        const auto total = completed_bytes.load(std::memory_order_relaxed) +
                           inflight_done.load(std::memory_order_relaxed);
        // Max-clamp the displayed value: the completed/in-flight handoff below
        // is two atomics, so a racing reader could otherwise glimpse a dip.
        auto previous = reported_progress.load(std::memory_order_relaxed);
        while (total > previous &&
               !reported_progress.compare_exchange_weak(previous, total,
                                                         std::memory_order_relaxed)) {
        }
        const auto displayed = std::max(total, previous);
        codec_progress_started.store(true, std::memory_order_relaxed);
        const auto [file_completed, file_total] = single_file_progress(displayed);
        report_operation(operation, OperationStage::compressing, displayed,
                         total_bytes, completed_items, total_items,
                         path.empty() && single_file != nullptr
                             ? single_file->archive_path : path,
                         file_completed, file_total, displayed,
                         compressed_bytes.load(std::memory_order_relaxed),
                         compressed_source_bytes.load(std::memory_order_relaxed),
                         reused_items.load(std::memory_order_relaxed),
                         reused_bytes.load(std::memory_order_relaxed));
    };

    auto compress_block = [&](PendingBlock block) {
        const auto initial_progress = reported_progress.load(std::memory_order_relaxed);
        const auto [file_completed, file_total] = single_file_progress(initial_progress);
        report_operation(operation, OperationStage::compressing,
                         initial_progress, total_bytes, completed_items, total_items,
                         block.path.empty() && single_file != nullptr
                             ? single_file->archive_path : block.path,
                          file_completed, file_total, throughput_progress(),
                          compressed_bytes.load(std::memory_order_relaxed),
                          compressed_source_bytes.load(std::memory_order_relaxed),
                          reused_items.load(std::memory_order_relaxed),
                          reused_bytes.load(std::memory_order_relaxed));
        auto block_options = options;
        block_options.thread_count = inner_thread_count;
        block_options.task_executor = task_executor;
        block_options.transform_ranges = std::move(block.transform_ranges);
        // AXAR classifies filters per source file while reading. An empty list
        // means no file in this solid block qualified; do not let the core
        // reinterpret it as permission to detect a transform across file edges.
        block_options.enable_file_filters = !block_options.transform_ranges.empty();
        auto block_done = std::make_shared<std::atomic<std::uint64_t>>(0);
        block_options.encoded_bytes_progress =
            [&, block_done, path = block.path](std::uint64_t done) {
                // The codec reports cumulative scanned bytes for this block,
                // possibly out of order across its workers: keep the per-block
                // maximum and add only the increase to the shared sum.
                auto previous = block_done->load(std::memory_order_relaxed);
                do {
                    if (done <= previous) {
                        return;
                    }
                } while (!block_done->compare_exchange_weak(previous, done,
                                                            std::memory_order_relaxed));
                inflight_done.fetch_add(done - previous, std::memory_order_relaxed);
                publish_progress(path);
            };
        ByteVector compressed;
        fs::path payload_path;
        std::uint64_t payload_size = 0;
        std::vector<SubframeRec> subframes;
        const auto original_size = block.spool_path.empty()
            ? static_cast<std::uint64_t>(block.data.size()) : block.original_size;
        if (!block.spool_path.empty()) {
            const auto streamed = compress_large_lzma2_block(
                block.spool_path, original_size, block.crc, block_options,
                operation, block_options.encoded_bytes_progress);
            payload_path = streamed.payload_path;
            payload_size = streamed.payload_size;
            subframes = std::move(streamed.subframes);
        } else {
            compressed = compress(block.data, block_options);
            subframes = !key ? make_subframe_map(compressed)
                             : std::vector<SubframeRec>{};
            if (key != nullptr) {
                // The block index is allocated at dispatch, so parallel completion
                // cannot change the AEAD associated data or archive byte order.
                const auto ad = block_associated_data(block.index);
                compressed = core::aead_seal(*key, compressed, ad);
            }
            payload_size = compressed.size();
        }
        // Move this block from in-flight to completed; subtracting first keeps
        // the max-clamped display from overshooting the true total.
        inflight_done.fetch_sub(block_done->load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
        completed_bytes.fetch_add(original_size, std::memory_order_relaxed);
        publish_progress(block.path);
        return CompletedBlock{block.index, original_size, std::move(compressed),
                              std::move(payload_path), payload_size,
                              std::move(block.spool_path), std::move(subframes),
                              std::move(block.path)};
    };

    auto pipeline_worker = [&] {
        try {
            while (true) {
                PendingBlock block;
                {
                    std::unique_lock lock(pipeline_mutex);
                    pipeline_cv.wait(lock, [&] {
                        return pipeline_error || !pending.empty() || pipeline_done;
                    });
                    if (pipeline_error || pending.empty()) {
                        return;  // failed, or done and drained
                    }
                    block = std::move(pending.front());
                    pending.pop_front();
                }
                pipeline_cv.notify_all();

                auto result = compress_block(std::move(block));
                {
                    std::lock_guard lock(pipeline_mutex);
                    completed.emplace(result.index, std::move(result));
                }
                pipeline_cv.notify_all();
            }
        } catch (...) {
            {
                std::lock_guard lock(pipeline_mutex);
                pipeline_error = std::current_exception();
                pending.clear();  // unblock a reader waiting on capacity
            }
            pipeline_cv.notify_all();
        }
    };

    std::vector<std::thread> pipeline_workers;
    pipeline_workers.reserve(outer_worker_count);
    for (std::size_t i = 0; i < outer_worker_count; ++i) {
        pipeline_workers.emplace_back(pipeline_worker);
    }

    // Stop and join workers on every exit path (including reader-side
    // exceptions and cancellation), so no task can outlive these locals.
    struct PipelineGuard {
        std::mutex& mutex;
        std::condition_variable& cv;
        bool& done;
        std::deque<PendingBlock>& pending;
        std::vector<std::thread>& workers;
        ~PipelineGuard() {
            {
                std::lock_guard lock(mutex);
                done = true;
                pending.clear();
            }
            cv.notify_all();
            for (auto& worker : workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
        }
    } pipeline_guard{pipeline_mutex, pipeline_cv, pipeline_done, pending, pipeline_workers};

    auto drain_completed = [&] {
        while (true) {
            CompletedBlock block;
            {
                std::lock_guard lock(pipeline_mutex);
                const auto found = completed.find(next_write_block);
                if (found == completed.end()) {
                    return;
                }
                block = std::move(found->second);
                completed.erase(found);
                --inflight_blocks;
                ++next_write_block;
            }
            pipeline_cv.notify_all();

            operation_checkpoint(operation);
            const auto block_payload_size = block.payload_path.empty()
                ? static_cast<std::uint64_t>(block.payload.size()) : block.payload_size;
            blocks.push_back({written, block_payload_size, block.original_size,
                              std::move(block.subframes)});
            if (block.payload_path.empty()) {
                out.write(reinterpret_cast<const char*>(block.payload.data()),
                          static_cast<std::streamsize>(block.payload.size()));
                if (!out) {
                    throw std::runtime_error("failed while writing archive blocks");
                }
            } else {
                TempFileGuard payload_guard(block.payload_path);
                std::ifstream payload(block.payload_path, std::ios::binary);
                if (!payload) {
                    throw std::runtime_error("cannot reopen staged large LZMA2 block");
                }
                std::vector<char> copy_buffer(effective_io_buffer_size(options.io_buffer_size));
                std::uint64_t remaining = block.payload_size;
                while (remaining > 0) {
                    const auto want = static_cast<std::streamsize>(
                        std::min<std::uint64_t>(remaining, copy_buffer.size()));
                    payload.read(copy_buffer.data(), want);
                    if (payload.gcount() != want) {
                        throw FormatError("staged large LZMA2 block is truncated");
                    }
                    out.write(copy_buffer.data(), want);
                    if (!out) {
                        throw std::runtime_error("failed while writing archive blocks");
                    }
                    remaining -= static_cast<std::uint64_t>(want);
                }
                payload.close();
                if (!payload) {
                    throw std::runtime_error("failed to close staged large LZMA2 block");
                }
                std::error_code cleanup_error;
                fs::remove(block.payload_path, cleanup_error);
                if (cleanup_error) {
                    throw fs::filesystem_error(
                        "failed to remove staged large LZMA2 block",
                        block.payload_path, cleanup_error);
                }
                payload_guard.dismiss();
            }
            if (!block.raw_spool_path.empty()) {
                std::error_code cleanup_error;
                fs::remove(block.raw_spool_path, cleanup_error);
                if (cleanup_error) {
                    throw fs::filesystem_error(
                        "failed to remove staged raw solid block",
                        block.raw_spool_path, cleanup_error);
                }
            }
            written += block_payload_size;
            compressed_bytes.store(written, std::memory_order_relaxed);
            compressed_source_bytes.fetch_add(block.original_size,
                                              std::memory_order_relaxed);
            const auto overall = completed_bytes.load(std::memory_order_relaxed);
            const auto [file_completed, file_total] = single_file_progress(overall);
            report_operation(operation, OperationStage::writing,
                             overall, total_bytes, completed_items, total_items,
                             block.path.empty() && single_file != nullptr
                                 ? single_file->archive_path : std::move(block.path),
                             file_completed, file_total, throughput_progress(),
                             compressed_bytes.load(std::memory_order_relaxed),
                             compressed_source_bytes.load(std::memory_order_relaxed),
                             reused_items.load(std::memory_order_relaxed),
                             reused_bytes.load(std::memory_order_relaxed));
        }
    };

    auto flush_block = [&] {
        if (buffer_size == 0) {
            return;
        }

        if (spool_large_blocks) {
            buffer_spool.flush();
            buffer_spool.close();
            if (!buffer_spool) {
                throw std::runtime_error("failed to close staged raw solid block");
            }
        }

        while (true) {
            drain_completed();
            std::unique_lock lock(pipeline_mutex);
            if (pipeline_error) {
                const auto error = pipeline_error;
                lock.unlock();
                std::rethrow_exception(error);
            }
            if (inflight_blocks < max_inflight_blocks) {
                const auto index = current_block++;
                if (spool_large_blocks) {
                    pending.push_back(PendingBlock{
                        index, {}, std::move(buffer_spool_path), buffer_size,
                        core::crc32_final(buffer_crc), {}, std::move(buffer_path)});
                } else {
                    pending.push_back(PendingBlock{
                        index, std::move(buffer), {}, buffer_size,
                        core::crc32_final(buffer_crc),
                        std::move(buffer_transform_ranges), std::move(buffer_path)});
                }
                ++inflight_blocks;
                lock.unlock();
                pipeline_cv.notify_one();
                buffer = ByteVector{};
                buffer_size = 0;
                buffer_crc = core::crc32_init();
                buffer_spool_path.clear();
                buffer_spool = std::ofstream{};
                buffer_path.clear();
                buffer_transform_ranges = {};
                return;
            }

            // Only the reader writes blocks. Wait until the next ordered
            // completion is available, then drain it before accepting more
            // input so memory remains bounded by max_inflight_blocks.
            pipeline_cv.wait(lock, [&] {
                return pipeline_error || completed.contains(next_write_block);
            });
        }
    };

    const auto ordered_items = compression_order(items);
    std::optional<int> previous_file_class;
    for (const auto* item_pointer : ordered_items) {
        const auto& item = *item_pointer;
        operation_checkpoint(operation);
        if (item.archive_path.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("path too long to archive: " + item.archive_path);
        }

        if (!item.is_directory && !item.is_symlink && !item.is_reparse_point) {
            const int current_class = compression_type_class(item);
            const auto minimum_group = std::max<std::size_t>(
                1, std::min<std::size_t>(std::size_t{1} << 20, block_size / 4));
            if (previous_file_class && *previous_file_class != current_class &&
                buffer_size >= minimum_group) {
                flush_block();
            }
            previous_file_class = current_class;
        }

        EntryRec entry;
        entry.path = item.archive_path;
        entry.meta = core::capture_metadata(item.absolute);
        for (const auto& message : entry.meta.capture_warnings) {
            record_capture_warning({item.archive_path, message});
        }

        if (item.is_reparse_point && !item.is_directory) {
            record_capture_warning({
                item.archive_path,
                "Skipped a non-directory reparse point because following its target is unsafe.",
            });
            ++completed_items;
            report_operation(operation, OperationStage::reading, displayed_progress(), total_bytes,
                             completed_items, total_items, item.archive_path, 0, 0,
                             throughput_progress(), 0, 0,
                             reused_items.load(std::memory_order_relaxed),
                             reused_bytes.load(std::memory_order_relaxed));
            continue;
        }

        if (item.is_symlink) {
            entry.type = kEntrySymlink;
            entry.link_target = item.symlink_target;
            // The link target is already represented by the entry type. Do not
            // duplicate the platform-specific raw reparse buffer for symlinks.
            entry.meta.has_reparse_data = false;
            entry.meta.reparse_tag = 0;
            entry.meta.reparse_data.clear();
            entries.push_back(std::move(entry));
            ++completed_items;
            report_operation(operation, OperationStage::reading, displayed_progress(), total_bytes,
                             completed_items, total_items, item.archive_path, 0, 0,
                             throughput_progress(), 0, 0,
                             reused_items.load(std::memory_order_relaxed),
                             reused_bytes.load(std::memory_order_relaxed));
            continue;
        }

        std::error_code ec;
        std::int64_t source_stamp = 0;
        bool source_stamp_valid = false;
        const auto stamp = fs::last_write_time(item.absolute, ec);
        if (!ec) {
            source_stamp =
                static_cast<std::int64_t>(stamp.time_since_epoch().count());
            source_stamp_valid = true;
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
            report_operation(operation, OperationStage::reading, displayed_progress(), total_bytes,
                             completed_items, total_items, item.archive_path, 0, 0,
                             throughput_progress(), 0, 0,
                             reused_items.load(std::memory_order_relaxed),
                             reused_bytes.load(std::memory_order_relaxed));
            continue;
        }

        std::ifstream in;
        if (!open_input_with_retry(in, item.absolute, options.input_open_retries,
                                   operation)) {
            if (!allow_unreadable_skips) {
                throw std::runtime_error("cannot read input file: " +
                                         core::path_to_utf8(item.absolute));
            }
            report_skipped_input(item, operation);
            const OperationWarning warning{
                item.archive_path,
                "Skipped because the file disappeared or access remained denied after retries.",
            };
            if (archive_meta != nullptr) {
                archive_meta->capture_warnings.push_back(warning);
            }
            if (options.strict_metadata) {
                throw std::runtime_error(warning.message + ": " + item.archive_path);
            }
            ++completed_items;
            report_operation(operation, OperationStage::reading, displayed_progress(),
                             total_bytes, completed_items, total_items,
                             item.archive_path, 0, 0, throughput_progress(), 0, 0,
                             reused_items.load(std::memory_order_relaxed),
                             reused_bytes.load(std::memory_order_relaxed));
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
                report_operation(operation, OperationStage::reading, displayed_progress(), total_bytes,
                                 completed_items, total_items, item.archive_path, 0, 0,
                                 throughput_progress(), 0, 0,
                                 reused_items.load(std::memory_order_relaxed),
                                 reused_bytes.load(std::memory_order_relaxed));
                continue;
            }
            hardlinks.emplace(identity, item.archive_path);  // canonical copy follows below
        }

        std::error_code size_error;
        std::uint64_t file_total = fs::file_size(item.absolute, size_error);
        if (size_error) {
            file_total = 0;
        }
        const ReuseCandidate* reuse_candidate = nullptr;
        ReuseCandidate batch_reuse_candidate;
        bool reuse_candidate_verified = false;
        if (reuse_by_path != nullptr && !size_error && source_stamp_valid) {
            const auto found = reuse_by_path->find(item.archive_path);
            if (found != reuse_by_path->end() &&
                file_total == found->second.identity.size &&
                source_stamp == found->second.source_stamp) {
                reuse_candidate = &found->second;
            }
        }
        if (reuse_candidate == nullptr && !size_error && source_stamp_valid &&
            batch_candidate_sizes.contains(file_total)) {
            std::ifstream comparison;
            if (open_input_with_retry(comparison, item.absolute,
                                      options.input_open_retries, operation)) {
                const auto identity = try_hash_input_stream(
                    comparison, file_total, options, operation);
                std::error_code comparison_stamp_error;
                const auto comparison_stamp =
                    fs::last_write_time(item.absolute, comparison_stamp_error);
                const bool stamp_unchanged = !comparison_stamp_error &&
                    static_cast<std::int64_t>(
                        comparison_stamp.time_since_epoch().count()) == source_stamp;
                if (identity && stamp_unchanged) {
                    const auto found = batch_reuse_candidates.find(*identity);
                    if (found != batch_reuse_candidates.end()) {
                        batch_reuse_candidate = {
                            *identity, found->second, source_stamp};
                        reuse_candidate = &batch_reuse_candidate;
                        reuse_candidate_verified = true;
                    }
                }
            }
        }
        if (reuse_candidate != nullptr && !reuse_candidate_verified) {
            // The comparison pass runs before the archive is staged. Rehash a
            // candidate through a fresh handle so a same-size source mutation
            // between those phases cannot make the directory point at the wrong
            // existing bytes. The original handle remains untouched for the
            // normal compression fallback.
            std::ifstream verification;
            if (!open_input_with_retry(verification, item.absolute,
                                       options.input_open_retries, operation)) {
                reuse_candidate = nullptr;
            } else {
                const auto verified = try_hash_input_stream(
                    verification, file_total, options, operation);
                std::error_code verification_stamp_error;
                const auto verification_stamp =
                    fs::last_write_time(item.absolute, verification_stamp_error);
                const bool stamp_unchanged = !verification_stamp_error &&
                    static_cast<std::int64_t>(
                        verification_stamp.time_since_epoch().count()) == source_stamp;
                if (!verified || *verified != reuse_candidate->identity ||
                    !stamp_unchanged) {
                    reuse_candidate = nullptr;
                }
            }
        }
        if (reuse_candidate != nullptr) {
            // The input was opened above so normal disappearing/permission errors
            // retain the same policy as a fresh compression. Its bytes are already
            // present in an existing block range; only the directory entry changes.
            in.close();
            entry.type = kEntryFile;
            entry.size = reuse_candidate->identity.size;
            entry.crc = reuse_candidate->identity.crc;
            entry.blake3 = reuse_candidate->identity.blake3;
            entry.has_blake3 = true;
            entry.first_block = reuse_candidate->data.first_block;
            entry.offset = reuse_candidate->data.offset;
            if (options.preserve_sparse_files && file_total != 0) {
                const auto sparse_capture =
                    core::capture_sparse_file(item.absolute, file_total);
                if (sparse_capture.map) {
                    entry.sparse = std::move(*sparse_capture.map);
                } else if (!sparse_capture.warning.empty()) {
                    OperationWarning warning{
                        item.archive_path,
                        "Sparse allocation was not captured: " + sparse_capture.warning,
                    };
                    if (archive_meta != nullptr) {
                        archive_meta->capture_warnings.push_back(warning);
                    }
                    if (operation) operation->add_warning(warning);
                    if (options.strict_metadata) {
                        throw std::runtime_error(warning.message + ": " + item.archive_path);
                    }
                }
            }
            entry.ads = core::capture_ads(item.absolute);
            entries.push_back(std::move(entry));
            reused_items.fetch_add(1, std::memory_order_relaxed);
            reused_bytes.fetch_add(file_total, std::memory_order_relaxed);
            read_bytes.fetch_add(file_total, std::memory_order_relaxed);
            compressed_source_bytes.fetch_add(file_total, std::memory_order_relaxed);
            ++completed_items;
            report_operation(operation, OperationStage::reading, displayed_progress(),
                             total_bytes, completed_items, total_items,
                             item.archive_path, file_total, file_total,
                             throughput_progress(), compressed_bytes.load(std::memory_order_relaxed),
                             compressed_source_bytes.load(std::memory_order_relaxed),
                             reused_items.load(std::memory_order_relaxed),
                             reused_bytes.load(std::memory_order_relaxed));
            continue;
        }

        entry.type = kEntryFile;
        entry.first_block = current_block;
        entry.offset = buffer_size;

        auto crc = core::crc32_init();
        core::Blake3 hasher;
        std::uint64_t total = 0;
        codec::TransformHint transform_hint;
        bool transform_classified = false;
        if (options.preserve_sparse_files && file_total != 0) {
            const auto sparse_capture =
                core::capture_sparse_file(item.absolute, file_total);
            if (sparse_capture.map) {
                entry.sparse = std::move(*sparse_capture.map);
            } else if (!sparse_capture.warning.empty()) {
                OperationWarning warning{
                    item.archive_path,
                    "Sparse allocation was not captured: " + sparse_capture.warning,
                };
                if (archive_meta != nullptr) {
                    archive_meta->capture_warnings.push_back(warning);
                }
                if (operation) operation->add_warning(warning);
                if (options.strict_metadata) {
                    throw std::runtime_error(warning.message + ": " + item.archive_path);
                }
            }
        }
        report_operation(operation, OperationStage::reading, displayed_progress(), total_bytes,
                         completed_items, total_items, item.archive_path, 0, file_total,
                         throughput_progress(), 0, 0,
                         reused_items.load(std::memory_order_relaxed),
                         reused_bytes.load(std::memory_order_relaxed));
        while (in) {
            operation_checkpoint(operation);
            in.read(io_buffer.data(), static_cast<std::streamsize>(io_buffer.size()));
            const auto got = in.gcount();
            if (got <= 0) {
                break;
            }
            const std::span<const std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(io_buffer.data()),
                static_cast<std::size_t>(got));
            if (!transform_classified) {
                transform_classified = true;
                if (!spool_large_blocks && options.enable_file_filters) {
                    transform_hint = codec::detect_transform_hint(bytes);
                }
            }
            crc = core::crc32_update(crc, bytes);
            hasher.update(bytes);
            if (!spool_large_blocks &&
                transform_hint.transform != CompressionTransform::none) {
                CompressionTransformRange range{
                    transform_hint.transform,
                    buffer_size,
                    static_cast<std::uint64_t>(bytes.size()),
                    total,
                    transform_hint.parameter,
                };
                if (!buffer_transform_ranges.empty()) {
                    auto& previous = buffer_transform_ranges.back();
                    if (previous.transform == range.transform &&
                        previous.parameter == range.parameter &&
                        previous.offset + previous.size == range.offset &&
                        previous.source_offset + previous.size == range.source_offset) {
                        previous.size += range.size;
                    } else {
                        buffer_transform_ranges.push_back(range);
                    }
                } else {
                    buffer_transform_ranges.push_back(range);
                }
            }
            total += static_cast<std::uint64_t>(got);
            read_bytes.fetch_add(static_cast<std::uint64_t>(got),
                                 std::memory_order_relaxed);
            // completed_bytes intentionally does not advance here: bytes are
            // counted when their block finishes compressing (see flush_block),
            // which is where the wall time actually goes.
            if (buffer_size == 0) {
                buffer_path = item.archive_path;
                if (spool_large_blocks) {
                    buffer_spool_path = core::unique_sibling_path(
                        fs::temp_directory_path() / L"AxiomCompress-large-solid", L"raw");
                    track_raw_spool(buffer_spool_path);
                    buffer_spool.open(buffer_spool_path,
                                      std::ios::binary | std::ios::trunc);
                    if (!buffer_spool) {
                        throw std::runtime_error("cannot create staged raw solid block");
                    }
                }
            }
            buffer_crc = core::crc32_update(buffer_crc, bytes);
            if (spool_large_blocks) {
                buffer_spool.write(reinterpret_cast<const char*>(bytes.data()),
                                   static_cast<std::streamsize>(bytes.size()));
                if (!buffer_spool) {
                    throw std::runtime_error("failed while staging raw solid block");
                }
            } else {
                buffer.insert(buffer.end(), bytes.begin(), bytes.end());
            }
            buffer_size += static_cast<std::uint64_t>(bytes.size());
            report_operation(operation, OperationStage::reading, displayed_progress(), total_bytes,
                             completed_items, total_items, item.archive_path, total,
                             std::max(total, file_total), throughput_progress(), 0, 0,
                             reused_items.load(std::memory_order_relaxed),
                             reused_bytes.load(std::memory_order_relaxed));
            if (buffer_size >= block_size) {
                flush_block();
            }
        }
        if (in.bad()) {
            throw std::runtime_error("failed while reading input file: " +
                                     core::path_to_utf8(item.absolute));
        }

        entry.size = total;
        if (entry.sparse.is_sparse && file_total != total) {
            const OperationWarning warning{
                item.archive_path,
                "Sparse allocation was not captured consistently because the source file changed while it was being read.",
            };
            entry.sparse = {};
            if (archive_meta != nullptr) {
                archive_meta->capture_warnings.push_back(warning);
            }
            if (operation) operation->add_warning(warning);
            if (options.strict_metadata) {
                throw std::runtime_error(warning.message + ": " + item.archive_path);
            }
        }
        entry.crc = core::crc32_final(crc);
        entry.blake3 = hasher.finalize();
        entry.has_blake3 = true;
        entry.ads = core::capture_ads(item.absolute);  // NTFS named streams (Win32 only)
        batch_reuse_candidates.emplace(
            content_identity(entry),
            ArchiveDataReference{entry.first_block, entry.offset});
        batch_candidate_sizes.insert(entry.size);
        entries.push_back(std::move(entry));
        ++completed_items;
        report_operation(operation, OperationStage::reading, displayed_progress(), total_bytes,
                         completed_items, total_items, item.archive_path, total, total,
                         throughput_progress(), 0, 0,
                         reused_items.load(std::memory_order_relaxed),
                         reused_bytes.load(std::memory_order_relaxed));
    }
    flush_block();

    // Stop accepting work, then drain completed blocks in their original order.
    // The central directory records those block indices, so writing in completion
    // order would be a valid but non-deterministic archive layout.
    {
        std::lock_guard lock(pipeline_mutex);
        pipeline_done = true;
    }
    pipeline_cv.notify_all();

    while (true) {
        drain_completed();
        std::unique_lock lock(pipeline_mutex);
        if (pipeline_error) {
            const auto error = pipeline_error;
            lock.unlock();
            std::rethrow_exception(error);
        }
        if (inflight_blocks == 0) {
            break;
        }
        pipeline_cv.wait(lock, [&] {
            return pipeline_error || completed.contains(next_write_block);
        });
    }

    for (auto& worker : pipeline_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    const auto reused_item_count = reused_items.load(std::memory_order_relaxed);
    const auto reused_byte_count = reused_bytes.load(std::memory_order_relaxed);
    completed_bytes.fetch_add(reused_byte_count, std::memory_order_relaxed);
    completed_bytes_out = completed_bytes.load(std::memory_order_relaxed);
    completed_items_out = completed_items.load(std::memory_order_relaxed);
    if (reuse_stats != nullptr) {
        reuse_stats->reused_items = reused_item_count;
        reuse_stats->reused_bytes = reused_byte_count;
    }
    if (reused_item_count != 0) {
        report_operation(operation, OperationStage::finalizing, completed_bytes_out,
                         total_bytes, completed_items_out, total_items, {}, 0, 0,
                         throughput_progress(),
                         compressed_bytes.load(std::memory_order_relaxed),
                         compressed_source_bytes.load(std::memory_order_relaxed),
                         reused_item_count, reused_byte_count);
    }
}

constexpr std::uint32_t kMaxKdfMemBlocks = 1u << 21;  // 2 GiB of 1 KiB blocks
constexpr std::uint32_t kMaxKdfPasses = 64;
constexpr std::size_t kMaxEncryptionPasswordBytes = 1u << 20;

void validate_password_input(const std::string& password, const char* field) {
    if (password.empty()) {
        throw std::invalid_argument(std::string(field) + " must not be empty");
    }
    if (password.size() > kMaxEncryptionPasswordBytes) {
        throw std::invalid_argument(std::string(field) + " is too long");
    }
}

void validate_kdf_parameters(const core::KdfParams& kdf, const char* context) {
    if (kdf.algorithm > 2 || kdf.lanes < 1 ||
        kdf.passes < 1 || kdf.passes > kMaxKdfPasses ||
        kdf.mem_blocks < 8 * kdf.lanes || kdf.mem_blocks > kMaxKdfMemBlocks) {
        throw FormatError(std::string(context) + " has implausible KDF parameters");
    }
}

EncryptionSlot make_encryption_slot(const EncryptionInfo& enc, std::uint32_t id,
                                    const std::string& password,
                                    const core::CryptoKey& data_key) {
    validate_password_input(password, "password");
    EncryptionSlot slot;
    slot.id = id;
    core::random_bytes(slot.kdf.salt);
    auto password_key = core::derive_key(password, slot.kdf);
    const auto ad = encryption_slot_associated_data(enc, slot.id);
    slot.wrapped_key = core::aead_seal(password_key, data_key, ad);
    core::secure_wipe(password_key);
    validate_encryption_slot_shape(slot);
    return slot;
}

// Build the legacy v1 encryption parameters used by old v4 archives. New
// archives use make_encryption_v2 below, but retaining this path keeps a local
// compatibility writer and the old wire format readable/testable forever.
std::pair<EncryptionInfo, core::CryptoKey> make_encryption(const std::string& password) {
    validate_password_input(password, "password");
    EncryptionInfo enc;
    enc.enabled = true;
    enc.kdf = core::KdfParams{};
    core::random_bytes(enc.kdf.salt);
    core::CryptoKey key = core::derive_key(password, enc.kdf);
    const std::span<const std::uint8_t> ad(enc.kdf.salt.data(), enc.kdf.salt.size());
    enc.key_check = core::aead_seal(key, kKeyCheckPlaintext, ad);
    return {std::move(enc), key};
}

std::pair<EncryptionInfo, core::CryptoKey> make_encryption_v2(
    const std::string& password) {
    validate_password_input(password, "password");
    EncryptionInfo enc;
    enc.enabled = true;
    enc.v2 = true;
    core::random_bytes(enc.key_id);
    core::CryptoKey data_key{};
    core::random_bytes(data_key);
    enc.slots.push_back(make_encryption_slot(enc, 1, password, data_key));
    return {std::move(enc), data_key};
}

ByteVector serialize_encryption_v2_payload(const EncryptionInfo& enc) {
    if (!enc.enabled || !enc.v2 || enc.slots.empty() ||
        enc.slots.size() > kMaxEncryptionSlots) {
        throw std::runtime_error("invalid encryption-v2 metadata");
    }
    ByteVector payload;
    payload.insert(payload.end(), kEncryptionV2Magic.begin(), kEncryptionV2Magic.end());
    put_u16(payload, kEncryptionV2Version);
    put_u16(payload, 0);  // no optional/unknown required options
    payload.insert(payload.end(), enc.key_id.begin(), enc.key_id.end());
    put_vint(payload, enc.slots.size());
    std::unordered_set<std::uint32_t> slot_ids;
    for (const auto& slot : enc.slots) {
        if (!slot_ids.insert(slot.id).second) {
            throw std::runtime_error("duplicate encryption-v2 password slot");
        }
        validate_kdf_parameters(slot.kdf, "encryption slot");
        validate_encryption_slot_shape(slot);
        put_u32(payload, slot.id);
        put_vint(payload, slot.kdf.algorithm);
        put_vint(payload, slot.kdf.mem_blocks);
        put_vint(payload, slot.kdf.passes);
        put_vint(payload, slot.kdf.lanes);
        put_vint(payload, slot.kdf.salt.size());
        payload.insert(payload.end(), slot.kdf.salt.begin(), slot.kdf.salt.end());
        put_vint(payload, slot.wrapped_key.size());
        payload.insert(payload.end(), slot.wrapped_key.begin(), slot.wrapped_key.end());
    }
    return payload;
}

// Serialize the plaintext encryption preamble (a fixed u32 length then the Argon2
// parameters, salt, and key-check) that precedes the blocks of a sealed-directory
// archive.
ByteVector serialize_encryption_preamble(const EncryptionInfo& enc) {
    ByteVector params = enc.v2 ? serialize_encryption_v2_payload(enc) : ByteVector{};
    if (!enc.v2) {
        put_vint(params, enc.kdf.algorithm);
        put_vint(params, enc.kdf.mem_blocks);
        put_vint(params, enc.kdf.passes);
        put_vint(params, enc.kdf.lanes);
        put_vint(params, enc.kdf.salt.size());
        params.insert(params.end(), enc.kdf.salt.begin(), enc.kdf.salt.end());
        put_vint(params, enc.key_check.size());
        params.insert(params.end(), enc.key_check.begin(), enc.key_check.end());
    }
    if (params.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("encryption preamble is too large");
    }
    ByteVector out;
    put_u32(out, static_cast<std::uint32_t>(params.size()));
    out.insert(out.end(), params.begin(), params.end());
    return out;
}

bool try_unwrap_encryption_slot(const EncryptionInfo& enc,
                                const EncryptionSlot& slot,
                                const std::string& password,
                                core::CryptoKey& data_key) {
    validate_kdf_parameters(slot.kdf, "encryption slot");
    validate_encryption_slot_shape(slot);
    auto password_key = core::derive_key(password, slot.kdf);
    const auto ad = encryption_slot_associated_data(enc, slot.id);
    std::vector<std::uint8_t> plain;
    const bool ok = core::aead_open(password_key, slot.wrapped_key, ad, plain) &&
                    plain.size() == sizeof(core::CryptoKey);
    core::secure_wipe(password_key);
    if (!ok) {
        core::secure_wipe(plain);
        return false;
    }
    std::copy(plain.begin(), plain.end(), data_key.begin());
    core::secure_wipe(plain);
    return true;
}

// Re-derive an encrypted archive's key from the password and verify it against the
// stored key-check token. Throws on a missing or wrong password before any block is
// touched, so failures are fast and unambiguous.
core::CryptoKey derive_archive_key(const EncryptionInfo& enc, const std::string& password) {
    if (password.empty()) {
        throw std::runtime_error("archive is encrypted; a password is required");
    }
    validate_password_input(password, "password");
    if (enc.v2) {
        if (enc.slots.empty() || enc.slots.size() > kMaxEncryptionSlots) {
            throw FormatError("encrypted archive has no valid password slots");
        }
        for (const auto& slot : enc.slots) {
            core::CryptoKey data_key{};
            if (try_unwrap_encryption_slot(enc, slot, password, data_key)) {
                return data_key;
            }
        }
        throw std::runtime_error("wrong password for encrypted archive");
    }

    // The KDF parameters come from the untrusted header; validate them before
    // Argon2 allocates its memory-hard work area.
    validate_kdf_parameters(enc.kdf, "encrypted archive");
    core::CryptoKey key = core::derive_key(password, enc.kdf);
    const std::span<const std::uint8_t> ad(enc.kdf.salt.data(), enc.kdf.salt.size());
    std::vector<std::uint8_t> check;
    const bool ok = core::aead_open(key, enc.key_check, ad, check) &&
                    check.size() == kKeyCheckPlaintext.size() &&
                    std::equal(check.begin(), check.end(), kKeyCheckPlaintext.begin());
    core::secure_wipe(check);
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

struct OptionalKeyWipeGuard {
    std::optional<core::CryptoKey>& key;
    ~OptionalKeyWipeGuard() {
        if (key) core::secure_wipe(*key);
    }
};

struct CryptoKeyWipeGuard {
    core::CryptoKey& key;
    ~CryptoKeyWipeGuard() { core::secure_wipe(key); }
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
        if (((layout.flags & kFlagEncryptionV2) != 0) != enc.v2) {
            throw FormatError("archive encryption-v2 flag does not match its preamble");
        }
        core::CryptoKey key = derive_archive_key(enc, password);
        const auto sealed = source.read(layout.directory_offset, layout.directory_size);
        ByteVector plain;
        const std::span<const std::uint8_t> ad(kDirectoryAd.data(), kDirectoryAd.size());
        if (!core::aead_open(key, sealed, ad, plain)) {
            core::secure_wipe(key);
            throw std::runtime_error(
                "archive directory failed to decrypt (wrong password or corrupt)");
        }
        loaded.index = parse_directory(plain, layout.directory_offset, layout.flags);
        loaded.index.meta.encryption = enc;
        loaded.key = key;
        validate_chunk_index(loaded.index, layout);
    } else {
        const auto directory = source.read(layout.directory_offset, layout.directory_size);
        loaded.index = parse_directory(directory, layout.directory_offset, layout.flags);
        if (((layout.flags & kFlagEncryptionV2) != 0) !=
            loaded.index.meta.encryption.v2) {
            throw FormatError(
                "archive encryption-v2 flag does not match its directory metadata");
        }
        // Block-only encryption keeps the directory plaintext, so list/comment work
        // without a password; the key is derived only when one is supplied (to read
        // blocks). Callers that need block data check for the key and demand it.
        if (loaded.index.meta.encryption.enabled && !password.empty()) {
            loaded.key = derive_archive_key(loaded.index.meta.encryption, password);
        }
        validate_chunk_index(loaded.index, layout);
    }
    loaded.index.meta.large_solid_blocks =
        (layout.flags & kFlagLargeSolidBlocks) != 0;
    if (loaded.index.meta.large_solid_blocks &&
        loaded.index.meta.encryption.enabled) {
        throw FormatError("encrypted archives cannot use the large solid-block profile");
    }
    if (loaded.index.meta.large_solid_blocks) {
        for (const auto& block : loaded.index.blocks) {
            validate_large_solid_block_payload(source, block);
        }
    }
    return loaded;
}

ByteVector serialize_snapshot_entry_body(const EntryRec& entry);

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
    const std::size_t io_chunk = effective_io_buffer_size(0);
    while (offset < layout.directory_offset) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(io_chunk, layout.directory_offset - offset));
        const auto chunk = source.read(offset, count);
        hasher.update(chunk);
        offset += count;
    }

    // A generation's history is part of what was signed.  Hash the canonical
    // extension bytes rather than reading them from the source so the digest is
    // stable when the signed directory is rewritten and changes its length.
    if (layout.generation_size != 0) {
        if (layout.generation_size != kGenerationExtensionSize) {
            throw FormatError("archive generation extension has an invalid size");
        }
        const auto extension = generation_extension_bytes(
            layout.generation, layout.previous_footer_offset,
            layout.previous_directory_offset, layout.previous_directory_size,
            layout.previous_generation_offset);
        hasher.update(extension);
    }

    ByteVector manifest;
    put_vint(manifest, index.blocks.size());
    const bool has_subframe_map = std::any_of(
        index.blocks.begin(), index.blocks.end(),
        [](const BlockRec& block) { return !block.subframes.empty(); });
    for (const auto& block : index.blocks) {
        put_u64(manifest, block.compressed_offset);
        put_u64(manifest, block.compressed_size);
        put_u64(manifest, block.uncompressed_size);
        if (has_subframe_map) {
            put_vint(manifest, block.subframes.size());
            for (const auto& frame : block.subframes) {
                put_u64(manifest, frame.uncompressed_offset);
                put_u64(manifest, frame.uncompressed_size);
                put_u64(manifest, frame.compressed_offset);
                put_u64(manifest, frame.compressed_size);
                manifest.push_back(frame.kind);
                manifest.push_back(frame.codec);
                manifest.push_back(frame.lzma_property);
            }
        }
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
        if ((layout.flags & kFlagExtendedMetadata) != 0) {
            manifest.push_back(entry.meta.has_windows_security_descriptor ? 1 : 0);
            put_vint(manifest, entry.meta.windows_security_descriptor.size());
            manifest.insert(manifest.end(), entry.meta.windows_security_descriptor.begin(),
                            entry.meta.windows_security_descriptor.end());
            put_vint(manifest, entry.meta.xattrs.size());
            for (const auto& xattr : entry.meta.xattrs) {
                put_vint(manifest, xattr.name.size());
                manifest.insert(manifest.end(), xattr.name.begin(), xattr.name.end());
                put_vint(manifest, xattr.data.size());
                manifest.insert(manifest.end(), xattr.data.begin(), xattr.data.end());
            }
            manifest.push_back(entry.meta.has_reparse_data ? 1 : 0);
            put_u32(manifest, entry.meta.reparse_tag);
            put_vint(manifest, entry.meta.reparse_data.size());
            manifest.insert(manifest.end(), entry.meta.reparse_data.begin(),
                            entry.meta.reparse_data.end());
        }
        if (layout.version >= kArchiveVersion5) {
            manifest.push_back(entry.sparse.is_sparse ? 1 : 0);
            put_vint(manifest, entry.sparse.allocated.size());
            for (const auto& extent : entry.sparse.allocated) {
                put_u64(manifest, extent.offset);
                put_u64(manifest, extent.length);
            }
        }
        if (index.meta.chunk_table) {
            put_vint(manifest, entry.chunk_refs.size());
            for (const auto ref : entry.chunk_refs) put_vint(manifest, ref);
        }
    }
    if (index.meta.chunk_table) {
        // Keep the legacy snapshot manifest prefix byte-for-byte stable so
        // signatures made before the live-dedup profile was introduced remain
        // verifiable. New profile fields are an optional suffix below.
        put_vint(manifest, index.meta.keyed_chunk_ids ? 1 : 0);
        put_vint(manifest, index.chunks.size());
        for (const auto& chunk : index.chunks) {
            put_u64(manifest, chunk.identity.size);
            put_u32(manifest, chunk.crc);
            manifest.insert(manifest.end(), chunk.identity.id.begin(), chunk.identity.id.end());
            put_u64(manifest, chunk.block_index);
            put_u64(manifest, chunk.offset);
        }
        put_vint(manifest, index.meta.snapshots.size());
        for (const auto& snapshot : index.meta.snapshots) {
            put_vint(manifest, snapshot.name.size());
            manifest.insert(manifest.end(), snapshot.name.begin(), snapshot.name.end());
            put_u64(manifest, snapshot.generation);
            put_u64(manifest, static_cast<std::uint64_t>(snapshot.created));
            put_vint(manifest, snapshot.entries.size());
            for (const auto& entry : snapshot.entries) {
                const auto body = serialize_snapshot_entry_body(entry);
                put_vint(manifest, body.size());
                manifest.insert(manifest.end(), body.begin(), body.end());
            }
        }
        if (index.meta.live_dedup || index.meta.dedup_profile.present) {
            put_vint(manifest, index.meta.live_dedup ? 1 : 0);
            put_vint(manifest, index.meta.dedup_profile.present ? 1 : 0);
        }
        if (index.meta.dedup_profile.present) {
            const auto& profile = index.meta.dedup_profile;
            put_vint(manifest, profile.chunker);
            put_vint(manifest, profile.chunker_version);
            put_vint(manifest, profile.minimum_size);
            put_vint(manifest, profile.average_size);
            put_vint(manifest, profile.maximum_size);
            put_vint(manifest, profile.table_id);
            put_vint(manifest, profile.packing);
        }
    }
    put_vint(manifest, index.meta.comment.size());
    manifest.insert(manifest.end(), index.meta.comment.begin(), index.meta.comment.end());
    manifest.push_back(index.meta.locked ? 1 : 0);
    manifest.push_back(index.meta.encryption.enabled ? 1 : 0);
    manifest.push_back(index.meta.encryption.encrypt_directory ? 1 : 0);
    if (index.meta.encryption.enabled) {
        if (index.meta.encryption.v2) {
            manifest.push_back(2);
            manifest.insert(manifest.end(), index.meta.encryption.key_id.begin(),
                            index.meta.encryption.key_id.end());
            put_vint(manifest, index.meta.encryption.slots.size());
            for (const auto& slot : index.meta.encryption.slots) {
                put_u32(manifest, slot.id);
                put_u32(manifest, slot.kdf.algorithm);
                put_u32(manifest, slot.kdf.mem_blocks);
                put_u32(manifest, slot.kdf.passes);
                put_u32(manifest, slot.kdf.lanes);
                manifest.insert(manifest.end(), slot.kdf.salt.begin(), slot.kdf.salt.end());
                put_vint(manifest, slot.wrapped_key.size());
                manifest.insert(manifest.end(), slot.wrapped_key.begin(),
                                slot.wrapped_key.end());
            }
        } else {
            // Keep the legacy manifest layout byte-for-byte stable for signatures
            // made before encryption-v2 existed.
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
    }
    if (layout.version >= kArchiveVersion5) {
        put_vint(manifest, index.meta.capture_warnings.size());
        for (const auto& warning : index.meta.capture_warnings) {
            put_vint(manifest, warning.path.size());
            manifest.insert(manifest.end(), warning.path.begin(), warning.path.end());
            put_vint(manifest, warning.message.size());
            manifest.insert(manifest.end(), warning.message.begin(), warning.message.end());
        }
    }
    hasher.update(manifest);
    return hasher.finalize();
}

struct DirectoryWriteResult {
    std::uint64_t directory_offset = 0;
    std::uint64_t directory_size = 0;
};

ByteVector serialize_block_extras(const BlockRec& block) {
    if (block.subframes.empty()) return {};
    validate_subframe_geometry(block, block.subframes);
    ByteVector map;
    put_vint(map, kSubframeMapVersion);
    put_vint(map, block.subframes.size());
    for (const auto& frame : block.subframes) {
        put_vint(map, frame.uncompressed_offset);
        put_vint(map, frame.uncompressed_size);
        put_vint(map, frame.compressed_offset);
        put_vint(map, frame.compressed_size);
        put_vint(map, frame.kind);
        put_vint(map, frame.codec);
        put_vint(map, frame.lzma_property);
    }

    ByteVector extras;
    put_vint(extras, kBlockExtraSubframeMap);
    put_vint(extras, map.size());
    extras.insert(extras.end(), map.begin(), map.end());
    return extras;
}

ByteVector serialize_snapshot_entry_body(const EntryRec& entry) {
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
        ByteVector chunks;
        put_vint(chunks, kChunkTableVersion);
        put_vint(chunks, entry.chunk_refs.size());
        for (const auto ref : entry.chunk_refs) put_vint(chunks, ref);
        put_vint(body, kExtraChunkRefs);
        put_vint(body, chunks.size());
        body.insert(body.end(), chunks.begin(), chunks.end());
    } else if (entry.type == kEntrySymlink || entry.type == kEntryHardlink) {
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
        if (stream.name.empty() || stream.name.size() > (4u << 10) ||
            stream.data.size() > core::kMaxAdsBytes) {
            throw std::runtime_error("snapshot alternate data stream exceeds its metadata limit");
        }
        ByteVector payload;
        put_vint(payload, stream.name.size());
        payload.insert(payload.end(), stream.name.begin(), stream.name.end());
        payload.insert(payload.end(), stream.data.begin(), stream.data.end());
        put_vint(body, kExtraAdsStream);
        put_vint(body, payload.size());
        body.insert(body.end(), payload.begin(), payload.end());
    }
    if (entry.meta.has_windows_security_descriptor) {
        if (entry.meta.windows_security_descriptor.empty() ||
            entry.meta.windows_security_descriptor.size() >
                core::kMaxSecurityDescriptorBytes) {
            throw std::runtime_error("snapshot security descriptor exceeds its metadata limit");
        }
        put_vint(body, kExtraSecurityDescriptor);
        put_vint(body, entry.meta.windows_security_descriptor.size());
        body.insert(body.end(), entry.meta.windows_security_descriptor.begin(),
                    entry.meta.windows_security_descriptor.end());
    }
    for (const auto& xattr : entry.meta.xattrs) {
        if (xattr.name.empty() || xattr.name.size() > (4u << 10) ||
            xattr.data.size() > core::kMaxMetadataBlobBytes) {
            throw std::runtime_error("snapshot extended attribute exceeds its metadata limit");
        }
        ByteVector payload;
        put_vint(payload, xattr.name.size());
        payload.insert(payload.end(), xattr.name.begin(), xattr.name.end());
        payload.insert(payload.end(), xattr.data.begin(), xattr.data.end());
        put_vint(body, kExtraXattr);
        put_vint(body, payload.size());
        body.insert(body.end(), payload.begin(), payload.end());
    }
    if (entry.meta.has_reparse_data) {
        if (entry.meta.reparse_data.size() < 8 ||
            entry.meta.reparse_data.size() > core::kMaxReparseDataBytes) {
            throw std::runtime_error("snapshot reparse metadata exceeds its safety bounds");
        }
        const auto& data = entry.meta.reparse_data;
        const auto stored_tag = static_cast<std::uint32_t>(data[0]) |
                                (static_cast<std::uint32_t>(data[1]) << 8) |
                                (static_cast<std::uint32_t>(data[2]) << 16) |
                                (static_cast<std::uint32_t>(data[3]) << 24);
        const auto stored_length = static_cast<std::uint16_t>(data[4]) |
                                   (static_cast<std::uint16_t>(data[5]) << 8);
        if (stored_tag != entry.meta.reparse_tag ||
            static_cast<std::size_t>(stored_length) + 8 != data.size()) {
            throw std::runtime_error("snapshot reparse metadata header does not match");
        }
        ByteVector payload;
        put_vint(payload, entry.meta.reparse_tag);
        put_vint(payload, entry.meta.reparse_data.size());
        payload.insert(payload.end(), entry.meta.reparse_data.begin(),
                       entry.meta.reparse_data.end());
        put_vint(body, kExtraReparse);
        put_vint(body, payload.size());
        body.insert(body.end(), payload.begin(), payload.end());
    }
    if (entry.type == kEntryFile && entry.sparse.is_sparse) {
        ByteVector sparse;
        put_vint(sparse, 1);
        put_vint(sparse, entry.sparse.allocated.size());
        for (const auto& extent : entry.sparse.allocated) {
            put_vint(sparse, extent.offset);
            put_vint(sparse, extent.length);
        }
        put_vint(body, kExtraSparseMap);
        put_vint(body, sparse.size());
        body.insert(body.end(), sparse.begin(), sparse.end());
    }
    return body;
}

// Serialize the central directory to `out` (already positioned at `written`).
// The ordinary footer is optional so an append-generation writer can place its
// generation metadata between the directory and the legacy footer.
template <typename Output>
DirectoryWriteResult write_directory_and_footer(
    Output& out, std::uint64_t written,
    const std::vector<BlockRec>& blocks,
    const std::vector<EntryRec>& entries,
    const ArchiveMeta& meta,
    const core::CryptoKey* dir_key = nullptr,
    bool write_footer = true,
    const std::vector<ChunkRec>* chunk_table = nullptr) {
    if (meta.chunk_table) {
        if (chunk_table == nullptr || (meta.live_dedup == !meta.snapshots.empty())) {
            throw std::runtime_error(
                "chunk-addressed archive has an invalid live/history mode");
        }
        if (meta.snapshots.size() > kMaxSnapshotCount ||
            chunk_table->size() > kMaxChunkCount) {
            throw std::runtime_error("snapshot archive exceeds its format limits");
        }
        if (meta.keyed_chunk_ids && !meta.encryption.enabled) {
            throw std::runtime_error("keyed chunk identifiers require archive encryption");
        }
        if (meta.live_dedup || meta.dedup_profile.present) {
            validate_dedup_profile(meta.dedup_profile, false);
        }
        const auto validate_chunks = [&]() {
            for (const auto& chunk : *chunk_table) {
                if (chunk.identity.size == 0 || chunk.block_index >= blocks.size()) {
                    throw std::runtime_error("snapshot chunk table contains an invalid reference");
                }
                const auto& block = blocks[static_cast<std::size_t>(chunk.block_index)];
                if (chunk.offset > block.uncompressed_size ||
                    chunk.identity.size > block.uncompressed_size - chunk.offset) {
                    throw std::runtime_error("snapshot chunk lies outside its block");
                }
            }
        };
        const auto validate_entries = [&](const std::vector<EntryRec>& snapshot_entries) {
            validate_snapshot_entry_paths_for_write(snapshot_entries);
            for (const auto& entry : snapshot_entries) {
                if (entry.type != kEntryFile) {
                    if (!entry.chunk_refs.empty()) {
                        throw std::runtime_error(
                            "non-file chunk-addressed entry has chunk references");
                    }
                    continue;
                }
                if (entry.chunk_refs.size() > kMaxChunkRefsPerEntry) {
                        throw std::runtime_error("entry has too many chunk references");
                }
                std::uint64_t total = 0;
                for (const auto ref : entry.chunk_refs) {
                    if (ref >= chunk_table->size()) {
                        throw std::runtime_error(
                            "entry points outside the chunk table");
                    }
                    const auto size = (*chunk_table)[static_cast<std::size_t>(ref)]
                                          .identity.size;
                    if (total > entry.size || size > entry.size - total) {
                        throw std::runtime_error("chunk sizes overflow the file");
                    }
                    total += size;
                }
                if (total != entry.size) {
                    throw std::runtime_error("chunks do not cover the file");
                }
            }
        };
        validate_chunks();
        validate_entries(entries);
        for (const auto& snapshot : meta.snapshots) {
            if (snapshot.entries.size() > kMaxSnapshotEntries) {
                throw std::runtime_error("snapshot manifest has too many entries");
            }
            validate_entries(snapshot.entries);
        }
    } else if (meta.keyed_chunk_ids || !meta.snapshots.empty() || meta.live_dedup ||
               meta.dedup_profile.present) {
        throw std::runtime_error("chunk-addressed metadata requires a chunk table");
    }
    ByteVector directory;
    put_vint(directory, blocks.size());
    for (const auto& block : blocks) {
        put_vint(directory, block.compressed_offset);
        put_vint(directory, block.compressed_size);
        put_vint(directory, block.uncompressed_size);
        const auto extras = serialize_block_extras(block);
        put_vint(directory, extras.size());
        directory.insert(directory.end(), extras.begin(), extras.end());
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
        if (entry.meta.has_windows_security_descriptor) {
            if (entry.meta.windows_security_descriptor.empty() ||
                entry.meta.windows_security_descriptor.size() >
                    core::kMaxSecurityDescriptorBytes) {
                throw std::runtime_error("security descriptor exceeds its metadata limit");
            }
            put_vint(body, kExtraSecurityDescriptor);
            put_vint(body, entry.meta.windows_security_descriptor.size());
            body.insert(body.end(), entry.meta.windows_security_descriptor.begin(),
                        entry.meta.windows_security_descriptor.end());
        }
        for (const auto& xattr : entry.meta.xattrs) {
            if (xattr.name.empty() || xattr.name.size() > (4u << 10) ||
                xattr.data.size() > core::kMaxMetadataBlobBytes) {
                throw std::runtime_error("extended attribute exceeds its metadata limit");
            }
            ByteVector payload;
            put_vint(payload, xattr.name.size());
            payload.insert(payload.end(), xattr.name.begin(), xattr.name.end());
            payload.insert(payload.end(), xattr.data.begin(), xattr.data.end());
            put_vint(body, kExtraXattr);
            put_vint(body, payload.size());
            body.insert(body.end(), payload.begin(), payload.end());
        }
        if (entry.meta.has_reparse_data) {
            if (entry.meta.reparse_data.size() < 8 ||
                entry.meta.reparse_data.size() > core::kMaxReparseDataBytes) {
                throw std::runtime_error("reparse metadata exceeds its safety bounds");
            }
            const auto& data = entry.meta.reparse_data;
            const auto stored_tag = static_cast<std::uint32_t>(data[0]) |
                                    (static_cast<std::uint32_t>(data[1]) << 8) |
                                    (static_cast<std::uint32_t>(data[2]) << 16) |
                                    (static_cast<std::uint32_t>(data[3]) << 24);
            const auto stored_length = static_cast<std::uint16_t>(data[4]) |
                                       (static_cast<std::uint16_t>(data[5]) << 8);
            if (stored_tag != entry.meta.reparse_tag ||
                static_cast<std::size_t>(stored_length) + 8 != data.size()) {
                throw std::runtime_error("reparse metadata header does not match its payload");
            }
            ByteVector payload;
            put_vint(payload, entry.meta.reparse_tag);
            put_vint(payload, entry.meta.reparse_data.size());
            payload.insert(payload.end(), entry.meta.reparse_data.begin(),
                           entry.meta.reparse_data.end());
            put_vint(body, kExtraReparse);
            put_vint(body, payload.size());
            body.insert(body.end(), payload.begin(), payload.end());
        }
        if (entry.type == kEntryFile && entry.sparse.is_sparse) {
            ByteVector payload;
            put_vint(payload, 1);  // sparse-map payload version
            put_vint(payload, entry.sparse.allocated.size());
            for (const auto& extent : entry.sparse.allocated) {
                put_vint(payload, extent.offset);
                put_vint(payload, extent.length);
            }
            put_vint(body, kExtraSparseMap);
            put_vint(body, payload.size());
            body.insert(body.end(), payload.begin(), payload.end());
        }
        if (meta.chunk_table && entry.type == kEntryFile) {
            ByteVector payload;
            put_vint(payload, kChunkTableVersion);
            put_vint(payload, entry.chunk_refs.size());
            for (const auto ref : entry.chunk_refs) put_vint(payload, ref);
            put_vint(body, kExtraChunkRefs);
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
        ByteVector payload = enc.v2 ? serialize_encryption_v2_payload(enc) : ByteVector{};
        if (!enc.v2) {
            put_vint(payload, enc.kdf.algorithm);
            put_vint(payload, enc.kdf.mem_blocks);
            put_vint(payload, enc.kdf.passes);
            put_vint(payload, enc.kdf.lanes);
            put_vint(payload, enc.kdf.salt.size());
            payload.insert(payload.end(), enc.kdf.salt.begin(), enc.kdf.salt.end());
            put_vint(payload, enc.key_check.size());
            payload.insert(payload.end(), enc.key_check.begin(), enc.key_check.end());
        }
        put_vint(archive_extras, enc.v2 ? kArchiveEncryptionV2 : kArchiveEncryption);
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
    if (!meta.capture_warnings.empty()) {
        ByteVector payload;
        put_vint(payload, meta.capture_warnings.size());
        for (const auto& warning : meta.capture_warnings) {
            put_vint(payload, warning.path.size());
            payload.insert(payload.end(), warning.path.begin(), warning.path.end());
            put_vint(payload, warning.message.size());
            payload.insert(payload.end(), warning.message.begin(), warning.message.end());
        }
        put_vint(archive_extras, kArchiveCaptureReport);
        put_vint(archive_extras, payload.size());
        archive_extras.insert(archive_extras.end(), payload.begin(), payload.end());
        ++archive_extra_count;
    }
    if (meta.chunk_table) {
        if (chunk_table == nullptr) {
            throw std::runtime_error("chunk-addressed archive is missing its chunk table");
        }
        ByteVector payload;
        put_vint(payload, kChunkTableVersion);
        put_vint(payload, meta.keyed_chunk_ids ? 1 : 0);
        put_vint(payload, chunk_table->size());
        for (const auto& chunk : *chunk_table) {
            put_vint(payload, chunk.identity.size);
            put_u32(payload, chunk.crc);
            payload.insert(payload.end(), chunk.identity.id.begin(), chunk.identity.id.end());
            put_vint(payload, chunk.block_index);
            put_vint(payload, chunk.offset);
        }
        put_vint(archive_extras, kArchiveChunkTable);
        put_vint(archive_extras, payload.size());
        archive_extras.insert(archive_extras.end(), payload.begin(), payload.end());
        ++archive_extra_count;
    }
    if (meta.dedup_profile.present) {
        ByteVector payload;
        put_vint(payload, kDedupProfileVersion);
        put_vint(payload, meta.dedup_profile.chunker);
        put_vint(payload, meta.dedup_profile.chunker_version);
        put_vint(payload, meta.dedup_profile.minimum_size);
        put_vint(payload, meta.dedup_profile.average_size);
        put_vint(payload, meta.dedup_profile.maximum_size);
        put_vint(payload, meta.dedup_profile.table_id);
        put_vint(payload, meta.dedup_profile.packing);
        put_vint(archive_extras, kArchiveDedupProfile);
        put_vint(archive_extras, payload.size());
        archive_extras.insert(archive_extras.end(), payload.begin(), payload.end());
        ++archive_extra_count;
    }
    if (!meta.snapshots.empty()) {
        if (!meta.chunk_table || meta.snapshots.size() > kMaxSnapshotCount) {
            throw std::runtime_error("invalid snapshot manifest state");
        }
        ByteVector payload;
        put_vint(payload, kSnapshotManifestVersion);
        put_vint(payload, meta.snapshots.size());
        for (const auto& snapshot : meta.snapshots) {
            validate_snapshot_name(snapshot.name);
            put_vint(payload, snapshot.name.size());
            payload.insert(payload.end(), snapshot.name.begin(), snapshot.name.end());
            put_vint(payload, snapshot.generation);
            put_u64(payload, static_cast<std::uint64_t>(snapshot.created));
            put_vint(payload, snapshot.entries.size());
            for (const auto& entry : snapshot.entries) {
                const auto body = serialize_snapshot_entry_body(entry);
                put_vint(payload, body.size());
                payload.insert(payload.end(), body.begin(), body.end());
            }
        }
        put_vint(archive_extras, kArchiveSnapshotManifest);
        put_vint(archive_extras, payload.size());
        archive_extras.insert(archive_extras.end(), payload.begin(), payload.end());
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

    if (write_footer) {
        const auto footer = archive_footer_bytes(directory_offset, directory.size());
        out.write(reinterpret_cast<const char*>(footer.data()),
                  static_cast<std::streamsize>(footer.size()));
    }

    out.flush();
    if (!out) {
        throw std::runtime_error("failed to finalize archive");
    }
    if (write_footer) {
        out.close();
    }
    return {directory_offset, static_cast<std::uint64_t>(directory.size())};
}

// Rewrite an archive keeping only the entries for which `keep` returns true,
// decoding each kept file's bytes from the old blocks into fresh solid blocks. This
// reclaims dead space (e.g. data left behind by replaced/removed entries) and is the
// engine behind both `delete` (keep = not-deleted) and `repack` (keep = all).
void rebuild_archive_keeping(const fs::path& archive_path,
                             const std::function<bool(const EntryRec&)>& keep,
                             const CompressionOptions& options) {
    reject_volume_mutation(archive_path);
    const auto operation = options.operation;

    std::uint64_t file_size = 0;
    auto in = open_archive(archive_path, file_size);
    const ByteSource bytes(in, file_size);
    const auto layout = read_layout(bytes);
    const auto existing_recovery = read_recovery_service(bytes, layout);
    auto index = load_index(bytes, options.password).index;
    const bool source_large_solid_blocks = index.meta.large_solid_blocks;
    if (index.meta.locked) {
        throw std::runtime_error("archive is locked (read-only)");
    }
    if (index.meta.chunk_table) {
        throw std::runtime_error(
            "snapshot repositories require snapshot operations; use snapshot prune or repack");
    }
    index.meta.has_signature = false;
    // For an encrypted archive, derive the key once: it both decrypts the surviving
    // blocks (via BlockSource) and re-seals the freshly written ones.
    std::optional<core::CryptoKey> key;
    if (index.meta.encryption.enabled) {
        key = derive_archive_key(index.meta.encryption, options.password);
    }
    BlockSource source(bytes, index, 0, operation, key,
                       source_large_solid_blocks);

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

    const auto block_size = effective_solid_block_size(options);
    validate_large_solid_block_options(options, block_size);
    const bool spool_large_blocks = requests_large_solid_blocks(block_size);
    index.meta.large_solid_blocks = spool_large_blocks;

    fs::path temp_path = archive_path;
    temp_path += ".tmp";
    TempFileGuard temp_guard(temp_path);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot create archive: " +
                                 core::path_to_utf8(temp_path));
    }

    const auto header = archive_header_bytes(
        archive_header_version({}, index.meta), archive_header_flags({}, index.meta));
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    std::uint64_t written = header.size();
    if (index.meta.encryption.encrypt_directory) {
        const auto preamble = serialize_encryption_preamble(index.meta.encryption);
        out.write(reinterpret_cast<const char*>(preamble.data()),
                  static_cast<std::streamsize>(preamble.size()));
        written += preamble.size();
    }

    std::vector<BlockRec> new_blocks;
    std::vector<EntryRec> new_entries;
    std::map<ContentIdentity, ArchiveDataReference> repack_candidates;
    ArchiveReuseStats reuse_stats;
    ByteVector buffer;
    std::uint64_t buffer_size = 0;
    std::uint32_t buffer_crc = core::crc32_init();
    fs::path buffer_spool_path;
    std::ofstream buffer_spool;
    std::unique_ptr<TempFileGuard> buffer_spool_guard;
    std::uint64_t current_block = 0;
    std::uint64_t completed_bytes = 0;
    std::uint64_t completed_items = 0;

    auto flush_block = [&]() {
        if (buffer_size == 0) {
            return;
        }
        if (spool_large_blocks) {
            buffer_spool.flush();
            buffer_spool.close();
            if (!buffer_spool) {
                throw std::runtime_error("failed to close staged raw solid block");
            }
        }
        report_operation(operation, OperationStage::compressing, completed_bytes, total_bytes,
                         completed_items, total_items, {}, 0, 0, completed_bytes, written,
                         completed_bytes, reuse_stats.reused_items,
                         reuse_stats.reused_bytes);
        // As in compress_items_into: bytes complete as their sub-blocks finish
        // compressing, so long recompressions tick instead of stalling.
        const auto block_base = completed_bytes;
        std::atomic<std::uint64_t> reported{0};
        auto block_options = options;
        block_options.encoded_bytes_progress = [&](std::uint64_t done) {
            auto previous = reported.load(std::memory_order_relaxed);
            do {
                if (done <= previous) {
                    return;
                }
            } while (!reported.compare_exchange_weak(previous, done,
                                                     std::memory_order_relaxed));
            report_operation(operation, OperationStage::compressing, block_base + done,
                             total_bytes, completed_items, total_items, {}, 0, 0,
                             block_base + done, written, completed_bytes,
                             reuse_stats.reused_items, reuse_stats.reused_bytes);
        };
        if (spool_large_blocks) {
            const auto streamed = compress_large_lzma2_block(
                buffer_spool_path, buffer_size, core::crc32_final(buffer_crc),
                block_options, operation, block_options.encoded_bytes_progress);
            completed_bytes = block_base + buffer_size;
            operation_checkpoint(operation);
            new_blocks.push_back({written, streamed.payload_size, buffer_size,
                                  streamed.subframes});

            TempFileGuard payload_guard(streamed.payload_path);
            std::ifstream payload(streamed.payload_path, std::ios::binary);
            if (!payload) {
                throw std::runtime_error("cannot reopen staged large LZMA2 block");
            }
            std::vector<char> copy_buffer(effective_io_buffer_size(options.io_buffer_size));
            std::uint64_t remaining = streamed.payload_size;
            while (remaining > 0) {
                const auto want = static_cast<std::streamsize>(
                    std::min<std::uint64_t>(remaining, copy_buffer.size()));
                payload.read(copy_buffer.data(), want);
                if (payload.gcount() != want) {
                    throw FormatError("staged large LZMA2 block is truncated");
                }
                out.write(copy_buffer.data(), want);
                if (!out) {
                    throw std::runtime_error("failed while writing archive blocks");
                }
                remaining -= static_cast<std::uint64_t>(want);
            }
            payload.close();
            if (!payload) {
                throw std::runtime_error("failed to close staged large LZMA2 block");
            }
            std::error_code payload_error;
            fs::remove(streamed.payload_path, payload_error);
            if (payload_error) {
                throw fs::filesystem_error(
                    "failed to remove staged large LZMA2 block",
                    streamed.payload_path, payload_error);
            }
            payload_guard.dismiss();
            written += streamed.payload_size;
        } else {
            auto compressed = compress(buffer, block_options);
            auto subframes = !key ? make_subframe_map(compressed)
                                  : std::vector<SubframeRec>{};
            completed_bytes = block_base + buffer_size;
            if (key) {
                compressed = core::aead_seal(
                    *key, compressed, block_associated_data(new_blocks.size()));
            }
            operation_checkpoint(operation);
            new_blocks.push_back({written, static_cast<std::uint64_t>(compressed.size()),
                                  buffer_size, std::move(subframes)});
            out.write(reinterpret_cast<const char*>(compressed.data()),
                      static_cast<std::streamsize>(compressed.size()));
            if (!out) {
                throw std::runtime_error("failed while writing archive blocks");
            }
            written += compressed.size();
        }
        ++current_block;
        buffer.clear();
        buffer_size = 0;
        buffer_crc = core::crc32_init();
        buffer_spool_path.clear();
        buffer_spool = std::ofstream{};
        buffer_spool_guard.reset();
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
        if (entry.type == kEntryFile && entry.has_blake3) {
            const auto found = repack_candidates.find(content_identity(entry));
            if (found != repack_candidates.end()) {
                out_entry.first_block = found->second.first_block;
                out_entry.offset = found->second.offset;
                new_entries.push_back(std::move(out_entry));
                ++completed_items;
                completed_bytes += entry.size;
                ++reuse_stats.reused_items;
                reuse_stats.reused_bytes += entry.size;
                report_operation(operation, OperationStage::writing, completed_bytes,
                                 total_bytes, completed_items, total_items, entry.path,
                                 0, entry.size, completed_bytes, written, completed_bytes,
                                 reuse_stats.reused_items, reuse_stats.reused_bytes);
                continue;
            }
        }
        if (entry.type == kEntryFile) {
            out_entry.first_block = current_block;
            out_entry.offset = buffer_size;
            read_file_bytes(source, index.blocks.size(), entry, operation,
                            [&](std::span<const std::uint8_t> chunk) {
                                if (buffer_size == 0 && spool_large_blocks) {
                                    buffer_spool_path = core::unique_sibling_path(
                                        fs::temp_directory_path() /
                                            L"AxiomCompress-large-repack",
                                        L"raw");
                                    buffer_spool_guard =
                                        std::make_unique<TempFileGuard>(buffer_spool_path);
                                    buffer_spool.open(buffer_spool_path,
                                                      std::ios::binary | std::ios::trunc);
                                    if (!buffer_spool) {
                                        throw std::runtime_error(
                                            "cannot create staged raw solid block");
                                    }
                                }
                                buffer_crc = core::crc32_update(buffer_crc, chunk);
                                if (spool_large_blocks) {
                                    buffer_spool.write(
                                        reinterpret_cast<const char*>(chunk.data()),
                                        static_cast<std::streamsize>(chunk.size()));
                                    if (!buffer_spool) {
                                        throw std::runtime_error(
                                            "failed while staging raw solid block");
                                    }
                                } else {
                                    buffer.insert(buffer.end(), chunk.begin(), chunk.end());
                                }
                                buffer_size += chunk.size();
                                // Counted when the block compresses, not here.
                                if (buffer_size >= block_size) {
                                    flush_block();
                                }
                            },
                             options.io_buffer_size);
            if (entry.has_blake3) {
                repack_candidates.emplace(
                    content_identity(entry),
                    ArchiveDataReference{out_entry.first_block, out_entry.offset});
            }
        }
        new_entries.push_back(std::move(out_entry));
        ++completed_items;
        report_operation(operation, OperationStage::writing, completed_bytes, total_bytes,
                         completed_items, total_items, entry.path, 0, entry.size,
                         completed_bytes, written, completed_bytes,
                         reuse_stats.reused_items, reuse_stats.reused_bytes);
    }
    flush_block();
    in.close();  // release the source handle so the rename can replace it (Windows)

    patch_archive_header(out, archive_header_version(new_entries, index.meta),
                         archive_header_flags(new_entries, index.meta));

    report_operation(operation, OperationStage::finalizing, completed_bytes, total_bytes,
                     completed_items, total_items, {}, 0, 0, completed_bytes, written,
                     completed_bytes, reuse_stats.reused_items,
                     reuse_stats.reused_bytes);
    const core::CryptoKey* directory_key =
        index.meta.encryption.encrypt_directory && key ? &*key : nullptr;
    write_directory_and_footer(out, written, new_blocks, new_entries, index.meta,
                               directory_key);

    if (key) {
        core::secure_wipe(*key);
    }
    const unsigned recovery_percent = options.recovery_percent != 0
        ? options.recovery_percent
        : (existing_recovery ? existing_recovery->percent : 0);
    if (recovery_percent != 0) {
        append_recovery_to_staged_archive(
            temp_path, recovery_percent, operation, options.thread_count);
    }
    report_operation(operation, OperationStage::committing, 0, 1, 0, 1,
                     "Replacing original archive", 0, 0, 0, written, completed_bytes,
                     reuse_stats.reused_items, reuse_stats.reused_bytes);
    replace_archive_file(temp_path, archive_path);
    temp_guard.dismiss();
    report_operation(operation, OperationStage::committing, 1, 1, 1, 1,
                     "Archive committed", 0, 0, 0, written, completed_bytes,
                     reuse_stats.reused_items, reuse_stats.reused_bytes);
    report_operation(operation, OperationStage::finalizing, total_bytes, total_bytes,
                     total_items, total_items, {}, 0, 0, total_bytes, written,
                     total_bytes, reuse_stats.reused_items, reuse_stats.reused_bytes);
}

void append_live_dedup_items_to_archive_indexed(
    const fs::path& archive_path, const std::vector<ScanItem>& items,
    const CompressionOptions& options, ArchiveIndex existing,
    unsigned existing_recovery_percent,
    const std::unordered_set<std::string>* desired_paths,
    bool phased_update,
    const ArchiveSyncFinalization* finalization);

// Append already-scanned items to an existing archive: existing block bytes are
// copied verbatim and the items become new blocks, with same-path items replacing
// the existing entry. Shared by add/update/sync. `meta_override`, when non-null,
// replaces the archive metadata (used to set the comment / lock flag); otherwise the
// existing metadata is preserved.
void append_items_to_archive_indexed(
    const fs::path& archive_path, const std::vector<ScanItem>& items,
    const CompressionOptions& options, ArchiveIndex existing,
    unsigned existing_recovery_percent,
    const ArchiveMeta* meta_override = nullptr,
    const std::unordered_set<std::string>* desired_paths = nullptr,
    bool phased_update = false,
    const ArchiveSyncFinalization* finalization = nullptr) {
    reject_volume_mutation(archive_path);
    const auto operation = options.operation;
    if (existing.meta.locked) {
        throw std::runtime_error("archive is locked (read-only)");
    }
    if (existing.meta.chunk_table && !items.empty()) {
        if (existing.meta.live_dedup && existing.meta.snapshots.empty()) {
            append_live_dedup_items_to_archive_indexed(
                archive_path, items, options, std::move(existing),
                existing_recovery_percent, desired_paths, phased_update,
                finalization);
            return;
        }
        throw std::runtime_error(
            "snapshot repositories require snapshot add for content changes");
    }
    ArchiveLayout existing_layout;
    std::uint64_t existing_physical_size = 0;
    {
        auto existing_input = open_archive(archive_path, existing_physical_size);
        const ByteSource existing_source(existing_input, existing_physical_size);
        existing_layout = read_layout(existing_source);
        existing_input.close();
    }
    // Encrypted archives: existing blocks are copied verbatim (still sealed), so a
    // key is needed for new blocks and for sealing an encrypted directory.
    std::optional<core::CryptoKey> key;
    if (existing.meta.encryption.enabled &&
        (!items.empty() || existing.meta.encryption.encrypt_directory)) {
        key = derive_archive_key(existing.meta.encryption, options.password);
    }
    const core::CryptoKey* key_ptr = key ? &*key : nullptr;
    ArchiveMeta result_meta = meta_override ? *meta_override : existing.meta;
    result_meta.has_signature = false;
    if (finalization != nullptr) {
        if (finalization->comment) result_meta.comment = *finalization->comment;
        if (finalization->lock_archive) result_meta.locked = true;
    }
    const auto reuse_candidates = build_reuse_candidates(existing.entries);
    const auto reuse_by_path = prepare_reuse_by_path(
        items, reuse_candidates, options, operation);
    ArchiveReuseStats reuse_stats;
    const std::uint64_t block_region_end =
        archive_block_region_end(existing_layout, existing);

    // Added paths replace any existing entry with the same path; the replaced data
    // stays in its solid block as dead space until a repack reclaims it.
    std::unordered_set<std::string> new_paths;
    for (const auto& item : items) {
        new_paths.insert(item.archive_path);
    }
    std::vector<EntryRec> entries;
    entries.reserve(existing.entries.size() + items.size());
    for (auto& existing_entry : existing.entries) {
        const bool desired = desired_paths == nullptr ||
            desired_paths->find(existing_entry.path) != desired_paths->end();
        if (desired && new_paths.find(existing_entry.path) == new_paths.end()) {
            entries.push_back(std::move(existing_entry));
        }
    }
    if (desired_paths != nullptr) {
        std::unordered_set<std::string> retained_paths;
        retained_paths.reserve(entries.size() + items.size());
        for (const auto& entry : entries) retained_paths.insert(entry.path);
        for (const auto& item : items) retained_paths.insert(item.archive_path);
        std::erase_if(entries, [&retained_paths](const EntryRec& entry) {
            return entry.type == kEntryHardlink &&
                   retained_paths.find(entry.link_target) == retained_paths.end();
        });
    }
    std::vector<BlockRec> blocks = std::move(existing.blocks);
    std::vector<ChunkRec> chunks = std::move(existing.chunks);

    const auto total_bytes = scanned_file_bytes(items);
    const auto total_items = static_cast<std::uint64_t>(items.size());
    std::uint64_t completed_bytes = 0;
    std::uint64_t completed_items = 0;
    report_operation(operation, OperationStage::reading, completed_bytes, total_bytes,
                     completed_items, total_items);

    const auto block_size = effective_solid_block_size(options);
    validate_large_solid_block_options(options, block_size);
    result_meta.large_solid_blocks =
        result_meta.large_solid_blocks || requests_large_solid_blocks(block_size);

    // A direct generation append can retain the existing header only when the
    // new directory needs the same feature flags. Metadata and sparse capture
    // happen inside compress_items_into, so probe only flags that are not already
    // present. If a probe finds a new v5 feature, the established transactional
    // rewrite path below remains the safe compatibility fallback.
    bool predicted_sparse = has_sparse_entries(existing.entries);
    bool predicted_extended = has_extended_metadata(existing.entries);
    bool predicted_capture_report = !result_meta.capture_warnings.empty();
    const auto existing_flags = existing_layout.flags;
    const bool probe_sparse = (existing_flags & kFlagSparseEntries) == 0;
    const bool probe_extended = (existing_flags & kFlagExtendedMetadata) == 0;
    const bool probe_capture = (existing_flags & kFlagCaptureReport) == 0;
    if (probe_sparse || probe_extended || probe_capture) {
        for (const auto& item : items) {
            if (item.is_reparse_point && !item.is_directory) {
                // compress_items_into records this deliberate skip as a capture
                // warning, even though no entry is emitted for the object.
                predicted_capture_report = true;
                continue;
            }

            const auto metadata = core::capture_metadata(item.absolute);
            if (probe_capture && !metadata.capture_warnings.empty()) {
                predicted_capture_report = true;
            }
            if (probe_extended) {
                predicted_extended = predicted_extended ||
                    (metadata.has_windows_security_descriptor ||
                     !metadata.xattrs.empty() || metadata.has_reparse_data);
            }

            if (item.is_symlink || item.is_directory || !probe_sparse ||
                !options.preserve_sparse_files) {
                continue;
            }
            std::error_code size_error;
            const auto file_size = fs::file_size(item.absolute, size_error);
            if (size_error || file_size == 0) continue;
            const auto sparse = core::capture_sparse_file(item.absolute, file_size);
            if (sparse.map) {
                predicted_sparse = true;
            } else if (!sparse.warning.empty()) {
                predicted_capture_report = true;
            }
        }
    }
    std::uint16_t predicted_flags = archive_header_flags(existing.entries, result_meta);
    if (predicted_sparse) predicted_flags |= kFlagSparseEntries;
    if (predicted_capture_report) predicted_flags |= kFlagCaptureReport;
    if (predicted_extended) predicted_flags |= kFlagExtendedMetadata;
    const std::uint16_t predicted_version =
        result_meta.encryption.v2 ||
                (predicted_flags & (kFlagSparseEntries | kFlagCaptureReport |
                                    kFlagExtendedMetadata)) != 0
            ? kArchiveVersion5 : kArchiveVersion4;
    const bool append_generation =
        !sfx_embedded_archive_range(archive_path).has_value() &&
        predicted_version == existing_layout.version &&
        predicted_flags == existing_layout.flags;

    const unsigned recovery_percent = options.recovery_percent != 0
        ? options.recovery_percent : existing_recovery_percent;

    if (append_generation) {
        if (existing_layout.footer_offset >
            std::numeric_limits<std::uint64_t>::max() - kFooterSize) {
            throw FormatError("archive footer offset overflows append position");
        }
        const auto previous_end = existing_layout.footer_offset + kFooterSize;
        if (existing_physical_size < previous_end) {
            throw FormatError("archive is truncated before its selected footer");
        }
        if (existing_layout.generation == std::numeric_limits<std::uint64_t>::max()) {
            throw FormatError("archive generation number is exhausted");
        }
        const auto generation = existing_layout.generation + 1;
        const auto previous_footer_offset = existing_layout.footer_offset;
        const auto previous_directory_offset = existing_layout.directory_offset;
        const auto previous_directory_size = existing_layout.directory_size;
        const auto previous_generation_offset = existing_layout.generation_offset;

        try {
            if (existing_physical_size != previous_end) {
                std::error_code trim_error;
                fs::resize_file(archive_path, previous_end, trim_error);
                if (trim_error) {
                    throw fs::filesystem_error(
                        "failed to discard an incomplete archive generation",
                        archive_path, trim_error);
                }
            }

            std::ofstream out(archive_path, std::ios::binary | std::ios::app);
            if (!out) {
                throw std::runtime_error("cannot open archive for append: " +
                                         core::path_to_utf8(archive_path));
            }
            if (phased_update && operation) operation->set_progress_phase(2, 6);
            report_operation(operation, OperationStage::copying, 1, 1,
                             static_cast<std::uint64_t>(existing.blocks.size()),
                             static_cast<std::uint64_t>(existing.blocks.size()),
                             "Retaining previous archive generations");
            std::uint64_t written = previous_end;

            if (phased_update && operation) operation->set_progress_phase(3, 6);
            compress_items_into(out, written, blocks, entries, items, options, block_size,
                                operation, total_bytes, total_items, completed_bytes,
                                completed_items, false, key_ptr, &result_meta,
                                &reuse_by_path, &reuse_stats);

            // A source can change between the preflight probe and capture. Do not
            // publish a directory whose feature flags disagree with the immutable
            // header; the catch block leaves the previous generation intact.
            const auto actual_version = archive_header_version(entries, result_meta);
            const auto actual_flags = archive_header_flags(entries, result_meta);
            if (actual_version != existing_layout.version ||
                actual_flags != existing_layout.flags) {
                throw std::runtime_error(
                    "append generation requires a header feature change; retry with a repack");
            }

            report_operation(operation, OperationStage::finalizing, completed_bytes,
                             total_bytes, completed_items, total_items, {}, 0, 0,
                             completed_bytes, written, completed_bytes,
                             reuse_stats.reused_items, reuse_stats.reused_bytes);
            const core::CryptoKey* directory_key =
                result_meta.encryption.encrypt_directory && key ? &*key : nullptr;
            const auto unsigned_directory = write_directory_and_footer(
                out, written, blocks, entries, result_meta, directory_key, false, &chunks);
            out.close();
            if (!out) throw std::runtime_error("failed to close appended archive directory");

            if (finalization != nullptr && finalization->signing_key != nullptr) {
                // Build a provisional generation so the signature digest includes
                // the exact generation history that the final directory will carry.
                append_generation_trailer(
                    archive_path, unsigned_directory.directory_offset,
                    unsigned_directory.directory_size, generation,
                    previous_footer_offset, previous_directory_offset,
                    previous_directory_size, previous_generation_offset, 0,
                    operation, options.thread_count, previous_end);

                std::uint64_t staged_size = 0;
                auto staged_input = open_archive(archive_path, staged_size);
                const ByteSource staged_source(staged_input, staged_size);
                const ArchiveLayout staged_layout = read_layout(staged_source);
                ArchiveIndex signing_index{blocks, entries, chunks, result_meta};
                const auto digest = archive_signature_digest(
                    staged_source, staged_layout, signing_index);
                const auto signature = core::sign_message(
                    finalization->signing_key->secret_key, digest);
                if (!core::verify_message(
                        finalization->signing_key->public_key, signature, digest)) {
                    throw std::invalid_argument(
                        "signing key public and secret components do not match");
                }
                staged_input.close();

                result_meta.has_signature = true;
                result_meta.signature_public_key =
                    finalization->signing_key->public_key;
                result_meta.signature = signature;

                std::error_code truncate_error;
                fs::resize_file(archive_path, unsigned_directory.directory_offset,
                                truncate_error);
                if (truncate_error) {
                    throw fs::filesystem_error(
                        "failed to stage archive signature", archive_path, truncate_error);
                }
                std::ofstream signed_output(
                    archive_path, std::ios::binary | std::ios::app);
                if (!signed_output) {
                    throw std::runtime_error(
                        "cannot reopen archive for generation signature");
                }
                const auto signed_directory = write_directory_and_footer(
                    signed_output, unsigned_directory.directory_offset, blocks, entries,
                    result_meta, result_meta.encryption.encrypt_directory && key ? &*key
                                                                                   : nullptr,
                    false, &chunks);
                signed_output.close();
                if (!signed_output) {
                    throw std::runtime_error(
                        "failed to close signed archive directory");
                }
                append_generation_trailer(
                    archive_path, signed_directory.directory_offset,
                    signed_directory.directory_size, generation,
                    previous_footer_offset, previous_directory_offset,
                    previous_directory_size, previous_generation_offset,
                    recovery_percent, operation, options.thread_count, previous_end);
            } else {
                append_generation_trailer(
                    archive_path, unsigned_directory.directory_offset,
                    unsigned_directory.directory_size, generation,
                    previous_footer_offset, previous_directory_offset,
                    previous_directory_size, previous_generation_offset,
                    recovery_percent, operation, options.thread_count, previous_end);
            }

            if (key) core::secure_wipe(*key);
            if (phased_update && operation) operation->set_progress_phase(5, 6);
            report_operation(operation, OperationStage::committing, 1, 1, 1, 1,
                             "Archive generation committed");
            std::uint64_t logical_archive_bytes = 0;
            for (const auto& entry : entries) {
                if (entry.type == kEntryFile) logical_archive_bytes += entry.size;
            }
            report_operation(operation, OperationStage::finalizing, total_bytes, total_bytes,
                             total_items, total_items, {}, 0, 0, total_bytes,
                             static_cast<std::uint64_t>(fs::file_size(archive_path)),
                             logical_archive_bytes, reuse_stats.reused_items,
                             reuse_stats.reused_bytes);
            return;
        } catch (...) {
            if (key) core::secure_wipe(*key);
            std::error_code rollback_error;
            fs::resize_file(archive_path, previous_end, rollback_error);
            throw;
        }
    }

    fs::path temp_path = archive_path;
    temp_path += ".tmp";
    TempFileGuard temp_guard(temp_path);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot create archive: " +
                                 core::path_to_utf8(temp_path));
    }

    // Copy the existing header + block region verbatim — the existing files are not
    // recompressed, only the directory is rebuilt with the merged entry list.
    {
        std::ifstream src(archive_path, std::ios::binary);
        if (!src) {
            throw std::runtime_error("cannot open archive: " +
                                     core::path_to_utf8(archive_path));
        }
        std::uint64_t copied = 0;
        std::uint64_t remaining = block_region_end;
        std::vector<char> chunk(effective_io_buffer_size(options.io_buffer_size));
        if (phased_update && operation) operation->set_progress_phase(2, 6);
        report_operation(operation, OperationStage::copying, 0, block_region_end,
                         0, blocks.size(), "Copying unchanged compressed blocks");
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
            copied += static_cast<std::uint64_t>(got);
            report_operation(operation, OperationStage::copying, copied, block_region_end,
                             0, blocks.size(), "Copying unchanged compressed blocks");
        }
    }
    std::uint64_t written = block_region_end;

    if (phased_update && operation) operation->set_progress_phase(3, 6);
    compress_items_into(out, written, blocks, entries, items, options, block_size, operation,
                        total_bytes, total_items, completed_bytes, completed_items, false,
                        key_ptr, &result_meta, &reuse_by_path, &reuse_stats);
    patch_archive_header(out, archive_header_version(entries, result_meta),
                         archive_header_flags(entries, result_meta));

    report_operation(operation, OperationStage::finalizing, completed_bytes, total_bytes,
                     completed_items, total_items, {}, 0, 0, completed_bytes,
                     written, completed_bytes, reuse_stats.reused_items,
                     reuse_stats.reused_bytes);
    const core::CryptoKey* directory_key =
        result_meta.encryption.encrypt_directory && key ? &*key : nullptr;
    write_directory_and_footer(out, written, blocks, entries, result_meta, directory_key,
                               true, &chunks);

    if (finalization != nullptr && finalization->signing_key != nullptr) {
        std::uint64_t staged_size = 0;
        auto staged_input = open_archive(temp_path, staged_size);
        const ByteSource staged_source(staged_input, staged_size);
        const ArchiveLayout staged_layout = read_layout(staged_source);
        ArchiveIndex signing_index{blocks, entries, chunks, result_meta};
        const auto digest =
            archive_signature_digest(staged_source, staged_layout, signing_index);
        const auto signature = core::sign_message(
            finalization->signing_key->secret_key, digest);
        if (!core::verify_message(
                finalization->signing_key->public_key, signature, digest)) {
            throw std::invalid_argument(
                "signing key public and secret components do not match");
        }
        staged_input.close();
        result_meta.has_signature = true;
        result_meta.signature_public_key =
            finalization->signing_key->public_key;
        result_meta.signature = signature;

        std::error_code truncate_error;
        fs::resize_file(temp_path, written, truncate_error);
        if (truncate_error) {
            throw fs::filesystem_error(
                "failed to stage archive signature", temp_path, truncate_error);
        }
        std::ofstream signed_output(
            temp_path, std::ios::binary | std::ios::app);
        if (!signed_output) {
            throw std::runtime_error(
                "cannot reopen staged archive for signing");
        }
        write_directory_and_footer(
            signed_output, written, blocks, entries, result_meta,
            result_meta.encryption.encrypt_directory && key ? &*key : nullptr,
            true, &chunks);
    }

    if (key) {
        core::secure_wipe(*key);
    }

    if (phased_update && operation) operation->set_progress_phase(4, 6);
    if (recovery_percent != 0) {
        append_recovery_to_staged_archive(
            temp_path, recovery_percent, operation, options.thread_count);
    } else {
        report_operation(operation, OperationStage::finalizing, 1, 1, 1, 1,
                         "No recovery record requested");
    }
    if (phased_update && operation) operation->set_progress_phase(5, 6);
    report_operation(operation, OperationStage::committing, 0, 1, 0, 1,
                     "Replacing original archive");
    replace_archive_file(temp_path, archive_path);
    temp_guard.dismiss();
    report_operation(operation, OperationStage::committing, 1, 1, 1, 1,
                     "Archive committed");
    std::uint64_t logical_archive_bytes = 0;
    for (const auto& entry : entries) {
        if (entry.type == kEntryFile) logical_archive_bytes += entry.size;
    }
    report_operation(operation, OperationStage::finalizing, total_bytes, total_bytes,
                     total_items, total_items, {}, 0, 0, total_bytes,
                     static_cast<std::uint64_t>(fs::file_size(archive_path)),
                     logical_archive_bytes, reuse_stats.reused_items,
                     reuse_stats.reused_bytes);
}

void append_items_to_archive(const fs::path& archive_path,
                             const std::vector<ScanItem>& items,
                             const CompressionOptions& options,
                             const ArchiveMeta* meta_override = nullptr) {
    // Read the existing archive's directory once. New solid blocks are appended
    // after its block region, so existing block indices stay valid.
    ArchiveIndex existing;
    unsigned existing_recovery_percent = 0;
    {
        std::uint64_t file_size = 0;
        auto in = open_archive(archive_path, file_size);
        const ByteSource source(in, file_size);
        const auto layout = read_layout(source);
        if (const auto recovery = read_recovery_service(source, layout)) {
            existing_recovery_percent = recovery->percent;
        }
        existing = load_index(source, options.password).index;
    }
    append_items_to_archive_indexed(
        archive_path, items, options, std::move(existing),
        existing_recovery_percent, meta_override, nullptr, false, nullptr);
}

void rewrite_archive_directory(const fs::path& archive_path, ArchiveIndex index,
                               const CompressionOptions& options) {
    reject_volume_mutation(archive_path);
    const auto operation = options.operation;
    std::uint64_t file_size = 0;
    auto in = open_archive(archive_path, file_size);
    const ByteSource source(in, file_size);
    const ArchiveLayout layout = read_layout(source);
    const auto existing_recovery = read_recovery_service(source, layout);
    if (index.meta.locked) {
        throw std::runtime_error("archive is locked (read-only)");
    }
    if (index.meta.chunk_table && !index.meta.snapshots.empty()) {
        throw std::runtime_error(
            "snapshot repositories do not support archive entry moves");
    }
    std::optional<core::CryptoKey> key;
    if (index.meta.encryption.enabled) {
        key = derive_archive_key(index.meta.encryption, options.password);
    }

    const std::uint64_t block_region_end = archive_block_region_end(layout, index);

    fs::path temp_path = archive_path;
    temp_path += ".tmp";
    TempFileGuard temp_guard(temp_path);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot create archive: " +
                                 core::path_to_utf8(temp_path));
    }

    std::uint64_t copied = 0;
    const std::uint64_t total_items = index.entries.size();
    report_operation(operation, OperationStage::writing, 0, block_region_end, 0, total_items);
    const auto chunk_size = effective_io_buffer_size(options.io_buffer_size);
    while (copied < block_region_end) {
        operation_checkpoint(operation);
        const auto want = std::min<std::uint64_t>(block_region_end - copied, chunk_size);
        const auto chunk = source.read(copied, want);
        out.write(reinterpret_cast<const char*>(chunk.data()),
                  static_cast<std::streamsize>(chunk.size()));
        if (!out) {
            throw std::runtime_error("failed while copying archive blocks");
        }
        copied += want;
        report_operation(operation, OperationStage::writing, copied, block_region_end,
                         0, total_items);
    }
    in.close();

    patch_archive_header(out, archive_header_version(index.entries, index.meta),
                         archive_header_flags(index.entries, index.meta));

    report_operation(operation, OperationStage::finalizing, copied, block_region_end,
                     total_items, total_items);
    const core::CryptoKey* directory_key =
        index.meta.encryption.encrypt_directory && key ? &*key : nullptr;
    write_directory_and_footer(out, block_region_end, index.blocks, index.entries,
                               index.meta, directory_key, true,
                               index.meta.chunk_table ? &index.chunks : nullptr);
    if (key) {
        core::secure_wipe(*key);
    }

    const unsigned recovery_percent = options.recovery_percent != 0
        ? options.recovery_percent
        : (existing_recovery ? existing_recovery->percent : 0);
    if (recovery_percent != 0) {
        append_recovery_to_staged_archive(
            temp_path, recovery_percent, operation, options.thread_count);
    }
    report_operation(operation, OperationStage::committing, 0, 1, 0, 1,
                     "Replacing original archive");
    replace_archive_file(temp_path, archive_path);
    temp_guard.dismiss();
    report_operation(operation, OperationStage::committing, 1, 1, 1, 1,
                     "Archive committed");
    report_operation(operation, OperationStage::finalizing, block_region_end, block_region_end,
                     total_items, total_items);
}

struct ChunkIdentityHash {
    std::size_t operator()(const ChunkIdentity& identity) const noexcept {
        std::size_t hash = static_cast<std::size_t>(identity.size);
        for (const auto byte : identity.id) {
            hash ^= static_cast<std::size_t>(byte) + 0x9e3779b9u +
                    (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

using SnapshotChunkLookup =
    std::unordered_map<ChunkIdentity, std::uint64_t, ChunkIdentityHash>;

SnapshotChunkLookup build_snapshot_chunk_lookup(const std::vector<ChunkRec>& chunks) {
    SnapshotChunkLookup lookup;
    for (std::uint64_t index = 0; index < chunks.size(); ++index) {
        const auto [it, inserted] = lookup.emplace(chunks[static_cast<std::size_t>(index)].identity,
                                                    index);
        if (!inserted) {
            // A valid table may contain one physical chunk more than once only
            // when a future profile adds aliases. Reuse the first stable index.
            (void)it;
        }
    }
    return lookup;
}

template <typename Output>
std::uint64_t append_snapshot_chunk(
    Output& out, std::uint64_t& written, std::vector<BlockRec>& blocks,
    std::vector<ChunkRec>& chunks, SnapshotChunkLookup& lookup,
    const ByteVector& data, const CompressionOptions& options,
    const std::shared_ptr<OperationControl>& operation,
    const core::CryptoKey* key, bool keyed, ArchiveReuseStats& reuse_stats) {
    const auto identity = make_chunk_identity(data, key, keyed);
    const auto found = lookup.find(identity);
    if (found != lookup.end()) {
        reuse_stats.reused_items++;
        reuse_stats.reused_bytes += data.size();
        return found->second;
    }
    if (chunks.size() >= kMaxChunkCount) {
        throw std::runtime_error("snapshot chunk table is full");
    }

    operation_checkpoint(operation);
    auto chunk_options = options;
    chunk_options.enable_content_dedup = false;
    chunk_options.enable_snapshot_dedup = false;
    chunk_options.transform_ranges.clear();
    chunk_options.task_executor.reset();
    auto compressed = compress(data, chunk_options);
    auto subframes = !key ? make_subframe_map(compressed)
                           : std::vector<SubframeRec>{};
    const auto block_index = static_cast<std::uint64_t>(blocks.size());
    if (key != nullptr) {
        compressed = core::aead_seal(*key, compressed, block_associated_data(block_index));
    }
    const auto archive_offset = written;
    out.write(reinterpret_cast<const char*>(compressed.data()),
              static_cast<std::streamsize>(compressed.size()));
    if (!out) throw std::runtime_error("failed while writing snapshot chunk");
    blocks.push_back({archive_offset, static_cast<std::uint64_t>(compressed.size()),
                      static_cast<std::uint64_t>(data.size()), std::move(subframes)});
    chunks.push_back({identity, core::crc32_final(core::crc32_update(
                          core::crc32_init(), data)), block_index, 0});
    written += compressed.size();
    const auto chunk_index = static_cast<std::uint64_t>(chunks.size() - 1);
    lookup.emplace(identity, chunk_index);
    return chunk_index;
}

template <typename Output>
std::vector<EntryRec> build_chunked_entries(
    Output& out, std::uint64_t& written, std::vector<BlockRec>& blocks,
    std::vector<ChunkRec>& chunks, SnapshotChunkLookup& lookup,
    const std::vector<ScanItem>& items, const CompressionOptions& options,
    const std::shared_ptr<OperationControl>& operation,
    const core::CryptoKey* key, bool keyed, ArchiveMeta& meta,
    ArchiveReuseStats& reuse_stats) {
    validate_snapshot_chunk_sizes(options);
    const auto total_bytes = scanned_file_bytes(items);
    const auto total_items = static_cast<std::uint64_t>(items.size());
    std::uint64_t completed_bytes = 0;
    std::uint64_t completed_items = 0;
    report_operation(operation, OperationStage::reading, 0, total_bytes, 0, total_items);

    const auto record_capture_warning = [&](const OperationWarning& warning) {
        if (operation) operation->add_warning(warning);
        meta.capture_warnings.push_back(warning);
        if (options.strict_metadata) {
            throw std::runtime_error(warning.message + ": " + warning.path);
        }
    };

    std::vector<EntryRec> entries;
    entries.reserve(items.size());
    if (items.size() > kMaxSnapshotEntries) {
        throw std::invalid_argument("snapshot contains too many input entries");
    }
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, std::string> hardlinks;
    for (const auto& item : items) {
        operation_checkpoint(operation);
        EntryRec entry;
        entry.path = normalize_archive_path(item.archive_path, "snapshot archive path");
        entry.meta = core::capture_metadata(item.absolute);
        for (const auto& message : entry.meta.capture_warnings) {
            record_capture_warning({item.archive_path, message});
        }
        if (item.is_reparse_point && !item.is_directory) {
            record_capture_warning({
                item.archive_path,
                "Skipped a non-directory reparse point because following its target is unsafe.",
            });
            ++completed_items;
            continue;
        }
        if (item.is_symlink) {
            entry.type = kEntrySymlink;
            entry.link_target = item.symlink_target;
            entry.meta.has_reparse_data = false;
            entry.meta.reparse_tag = 0;
            entry.meta.reparse_data.clear();
            entries.push_back(std::move(entry));
            ++completed_items;
            continue;
        }
        std::error_code stamp_error;
        const auto stamp = fs::last_write_time(item.absolute, stamp_error);
        if (!stamp_error) {
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
            continue;
        }

        std::ifstream probe;
        if (!open_input_with_retry(probe, item.absolute, options.input_open_retries, operation)) {
            if (!options.skip_unreadable_files) {
                throw std::runtime_error("cannot read input file: " +
                                         core::path_to_utf8(item.absolute));
            }
            record_capture_warning({
                item.archive_path,
                "Skipped because the file disappeared or access remained denied after retries.",
            });
            ++completed_items;
            continue;
        }
        probe.close();

        if (auto id = core::hardlink_identity(item.absolute)) {
            const auto identity = std::make_tuple(id->volume, id->index_high, id->index_low);
            const auto found = hardlinks.find(identity);
            if (found != hardlinks.end()) {
                entry.type = kEntryHardlink;
                entry.link_target = found->second;
                entry.mtime = 0;
                entry.meta = {};
                entries.push_back(std::move(entry));
                ++completed_items;
                continue;
            }
            hardlinks.emplace(identity, item.archive_path);
        }

        std::error_code size_error;
        const auto declared_size = fs::file_size(item.absolute, size_error);
        if (size_error) {
            throw std::runtime_error("cannot stat input file: " +
                                     core::path_to_utf8(item.absolute));
        }
        entry.type = kEntryFile;
        auto crc = core::crc32_init();
        core::Blake3 hasher;
        std::uint64_t total = 0;
        if (options.preserve_sparse_files && declared_size != 0) {
            const auto sparse_capture = core::capture_sparse_file(item.absolute, declared_size);
            if (sparse_capture.map) {
                entry.sparse = std::move(*sparse_capture.map);
            } else if (!sparse_capture.warning.empty()) {
                record_capture_warning({
                    item.archive_path,
                    "Sparse allocation was not captured: " + sparse_capture.warning,
                });
            }
        }
        read_content_defined_chunks(
            item.absolute, options, operation,
            [&](ByteVector chunk) {
                const auto bytes = std::span<const std::uint8_t>(chunk);
                crc = core::crc32_update(crc, bytes);
                hasher.update(bytes);
                if (entry.chunk_refs.size() >= kMaxChunkRefsPerEntry) {
                    throw std::runtime_error("snapshot file has too many chunk references");
                }
                const auto chunk_ref = append_snapshot_chunk(
                    out, written, blocks, chunks, lookup, chunk, options,
                    operation, key, keyed, reuse_stats);
                if (entry.chunk_refs.empty()) {
                    const auto& first_chunk = chunks[static_cast<std::size_t>(chunk_ref)];
                    entry.first_block = first_chunk.block_index;
                    entry.offset = first_chunk.offset;
                }
                entry.chunk_refs.push_back(chunk_ref);
                total += bytes.size();
                completed_bytes += bytes.size();
                report_operation(operation, OperationStage::writing, completed_bytes,
                                 total_bytes, completed_items, total_items,
                                 item.archive_path, total, declared_size,
                                 completed_bytes, written, completed_bytes,
                                 reuse_stats.reused_items, reuse_stats.reused_bytes);
            });
        if (total != declared_size) {
            throw std::runtime_error("source file changed while creating snapshot: " +
                                     item.archive_path);
        }
        entry.size = total;
        entry.crc = core::crc32_final(crc);
        entry.blake3 = hasher.finalize();
        entry.has_blake3 = true;
        entry.ads = core::capture_ads(item.absolute);
        entries.push_back(std::move(entry));
        ++completed_items;
        report_operation(operation, OperationStage::writing, completed_bytes, total_bytes,
                         completed_items, total_items, item.archive_path, total, total,
                         completed_bytes, written, completed_bytes,
                         reuse_stats.reused_items, reuse_stats.reused_bytes);
    }
    validate_snapshot_entry_paths_for_write(entries);
    return entries;
}

void append_live_dedup_items_to_archive_indexed(
    const fs::path& archive_path, const std::vector<ScanItem>& items,
    const CompressionOptions& options, ArchiveIndex existing,
    unsigned existing_recovery_percent,
    const std::unordered_set<std::string>* desired_paths,
    bool phased_update,
    const ArchiveSyncFinalization* finalization) {
    reject_volume_mutation(archive_path);
    if (!existing.meta.live_dedup || !existing.meta.chunk_table ||
        !existing.meta.snapshots.empty()) {
        throw std::runtime_error("archive is not a live deduplicated archive");
    }
    if (existing.meta.locked) {
        throw std::runtime_error("archive is locked (read-only)");
    }

    std::uint64_t physical_size = 0;
    auto input = open_archive(archive_path, physical_size);
    const ByteSource source(input, physical_size);
    const auto layout = read_layout(source);
    if (layout.footer_offset > std::numeric_limits<std::uint64_t>::max() - kFooterSize) {
        throw FormatError("archive footer offset overflows append position");
    }
    const auto previous_end = layout.footer_offset + kFooterSize;
    if (physical_size < previous_end) {
        throw FormatError("archive is truncated before its selected footer");
    }
    if (layout.generation == std::numeric_limits<std::uint64_t>::max()) {
        throw FormatError("archive generation number is exhausted");
    }

    std::optional<core::CryptoKey> key;
    if (existing.meta.encryption.enabled) {
        key = derive_archive_key(existing.meta.encryption, options.password);
    }
    OptionalKeyWipeGuard key_wipe{key};

    auto meta = std::move(existing.meta);
    meta.has_signature = false;
    if (finalization != nullptr) {
        if (finalization->comment) meta.comment = *finalization->comment;
        if (finalization->lock_archive) meta.locked = true;
    }
    validate_dedup_profile(meta.dedup_profile, false);

    std::unordered_set<std::string> new_paths;
    new_paths.reserve(items.size());
    for (const auto& item : items) new_paths.insert(item.archive_path);

    std::vector<EntryRec> entries;
    entries.reserve(existing.entries.size() + items.size());
    for (auto& entry : existing.entries) {
        const bool desired = desired_paths == nullptr ||
            desired_paths->contains(entry.path);
        if (desired && !new_paths.contains(entry.path)) {
            entries.push_back(std::move(entry));
        }
    }
    if (desired_paths != nullptr) {
        std::unordered_set<std::string> retained;
        retained.reserve(entries.size() + items.size());
        for (const auto& entry : entries) retained.insert(entry.path);
        for (const auto& item : items) retained.insert(item.archive_path);
        std::erase_if(entries, [&retained](const EntryRec& entry) {
            return entry.type == kEntryHardlink &&
                   !retained.contains(entry.link_target);
        });
    }

    auto blocks = std::move(existing.blocks);
    auto chunks = std::move(existing.chunks);
    auto lookup = build_snapshot_chunk_lookup(chunks);
    const auto generation = layout.generation + 1;
    const auto previous_footer_offset = layout.footer_offset;
    const auto previous_directory_offset = layout.directory_offset;
    const auto previous_directory_size = layout.directory_size;
    const auto previous_generation_offset = layout.generation_offset;
    const unsigned recovery_percent = options.recovery_percent != 0
        ? options.recovery_percent : existing_recovery_percent;
    input.close();

    try {
        if (physical_size != previous_end) {
            std::error_code trim_error;
            fs::resize_file(archive_path, previous_end, trim_error);
            if (trim_error) {
                throw fs::filesystem_error(
                    "failed to discard an incomplete archive generation",
                    archive_path, trim_error);
            }
        }

        std::ofstream out(archive_path, std::ios::binary | std::ios::app);
        if (!out) {
            throw std::runtime_error("cannot open deduplicated archive for append");
        }
        if (phased_update && options.operation) options.operation->set_progress_phase(3, 6);
        std::uint64_t written = previous_end;
        ArchiveReuseStats reuse_stats;
        auto chunk_options = options_for_dedup_profile(options, meta.dedup_profile);
        chunk_options.enable_content_dedup = true;
        chunk_options.enable_snapshot_dedup = false;
        chunk_options.keyed_chunk_ids = meta.keyed_chunk_ids;
        auto added_entries = build_chunked_entries(
            out, written, blocks, chunks, lookup, items, chunk_options,
            options.operation, key ? &*key : nullptr, meta.keyed_chunk_ids,
            meta, reuse_stats);
        entries.insert(entries.end(),
                       std::make_move_iterator(added_entries.begin()),
                       std::make_move_iterator(added_entries.end()));
        validate_snapshot_entry_paths_for_write(entries);

        const auto actual_version = archive_header_version(entries, meta);
        const auto actual_flags = archive_header_flags(entries, meta);
        if (actual_version != layout.version || actual_flags != layout.flags) {
            throw std::runtime_error(
                "deduplicated append requires an unexpected header feature change");
        }

        const core::CryptoKey* directory_key =
            meta.encryption.encrypt_directory && key ? &*key : nullptr;
        const auto unsigned_directory = write_directory_and_footer(
            out, written, blocks, entries, meta, directory_key, false, &chunks);
        out.close();
        if (!out) throw std::runtime_error("failed to close deduplicated archive directory");

        if (finalization != nullptr && finalization->signing_key != nullptr) {
            append_generation_trailer(
                archive_path, unsigned_directory.directory_offset,
                unsigned_directory.directory_size, generation,
                previous_footer_offset, previous_directory_offset,
                previous_directory_size, previous_generation_offset, 0,
                options.operation, options.thread_count, previous_end);

            std::uint64_t staged_size = 0;
            auto staged_input = open_archive(archive_path, staged_size);
            const ByteSource staged_source(staged_input, staged_size);
            const ArchiveLayout staged_layout = read_layout(staged_source);
            ArchiveIndex signing_index{blocks, entries, chunks, meta};
            const auto digest = archive_signature_digest(
                staged_source, staged_layout, signing_index);
            const auto signature = core::sign_message(
                finalization->signing_key->secret_key, digest);
            if (!core::verify_message(
                    finalization->signing_key->public_key, signature, digest)) {
                throw std::invalid_argument(
                    "signing key public and secret components do not match");
            }
            staged_input.close();
            meta.has_signature = true;
            meta.signature_public_key = finalization->signing_key->public_key;
            meta.signature = signature;

            std::error_code truncate_error;
            fs::resize_file(archive_path, unsigned_directory.directory_offset,
                            truncate_error);
            if (truncate_error) {
                throw fs::filesystem_error(
                    "failed to stage deduplicated archive signature",
                    archive_path, truncate_error);
            }
            std::ofstream signed_output(
                archive_path, std::ios::binary | std::ios::app);
            if (!signed_output) {
                throw std::runtime_error(
                    "cannot reopen deduplicated archive for signing");
            }
            const auto signed_directory = write_directory_and_footer(
                signed_output, unsigned_directory.directory_offset, blocks,
                entries, meta, directory_key, false, &chunks);
            signed_output.close();
            append_generation_trailer(
                archive_path, signed_directory.directory_offset,
                signed_directory.directory_size, generation,
                previous_footer_offset, previous_directory_offset,
                previous_directory_size, previous_generation_offset,
                recovery_percent, options.operation, options.thread_count,
                previous_end);
        } else {
            append_generation_trailer(
                archive_path, unsigned_directory.directory_offset,
                unsigned_directory.directory_size, generation,
                previous_footer_offset, previous_directory_offset,
                previous_directory_size, previous_generation_offset,
                recovery_percent, options.operation, options.thread_count,
                previous_end);
        }

        if (phased_update && options.operation) options.operation->set_progress_phase(5, 6);
        const auto logical_bytes = std::accumulate(
            entries.begin(), entries.end(), std::uint64_t{0},
            [](std::uint64_t total, const EntryRec& entry) {
                return entry.type == kEntryFile ? total + entry.size : total;
            });
        const auto source_bytes = scanned_file_bytes(items);
        report_operation(
            options.operation, OperationStage::finalizing, source_bytes,
            source_bytes, items.size(), items.size(), {}, 0, 0, source_bytes,
            static_cast<std::uint64_t>(fs::file_size(archive_path)),
            logical_bytes, reuse_stats.reused_items, reuse_stats.reused_bytes);
    } catch (...) {
        std::error_code rollback_error;
        fs::resize_file(archive_path, previous_end, rollback_error);
        throw;
    }
}

std::int64_t snapshot_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

const SnapshotRec& find_snapshot(const ArchiveMeta& meta, std::string_view name) {
    validate_snapshot_name(name);
    const auto found = std::find_if(
        meta.snapshots.begin(), meta.snapshots.end(),
        [name](const SnapshotRec& snapshot) { return snapshot.name == name; });
    if (found == meta.snapshots.end()) {
        throw std::invalid_argument("snapshot does not exist: " + std::string(name));
    }
    return *found;
}

void require_snapshot_profile(const ArchiveIndex& index) {
    if (!index.meta.chunk_table || index.meta.snapshots.empty()) {
        throw std::runtime_error(
            "archive is not a snapshot repository; create one with the snapshot command");
    }
}

void require_chunk_profile(const ArchiveIndex& index) {
    if (!index.meta.chunk_table) {
        throw std::runtime_error("archive does not use chunk-addressed content");
    }
}

bool same_snapshot_entry(const EntryRec& left, const EntryRec& right) {
    const auto same_blobs = [](const auto& first, const auto& second) {
        if (first.size() != second.size()) return false;
        return std::equal(first.begin(), first.end(), second.begin(),
                          [](const auto& a, const auto& b) {
                              return a.name == b.name && a.data == b.data;
                          });
    };
    const auto same_metadata = [&](const core::FileMetadata& first,
                                   const core::FileMetadata& second) {
        return first.has_windows_attributes == second.has_windows_attributes &&
               (!first.has_windows_attributes ||
                first.windows_attributes == second.windows_attributes) &&
               first.has_windows_times == second.has_windows_times &&
               (!first.has_windows_times ||
                (first.windows_creation_time == second.windows_creation_time &&
                 first.windows_access_time == second.windows_access_time &&
                 first.windows_write_time == second.windows_write_time)) &&
               first.has_posix == second.has_posix &&
               (!first.has_posix ||
                (first.posix_mode == second.posix_mode &&
                 first.posix_uid == second.posix_uid &&
                 first.posix_gid == second.posix_gid)) &&
               first.has_windows_security_descriptor ==
                   second.has_windows_security_descriptor &&
               (!first.has_windows_security_descriptor ||
                first.windows_security_descriptor == second.windows_security_descriptor) &&
               same_blobs(first.xattrs, second.xattrs) &&
               first.has_reparse_data == second.has_reparse_data &&
               (!first.has_reparse_data ||
                (first.reparse_tag == second.reparse_tag &&
                 first.reparse_data == second.reparse_data));
    };
    const auto same_sparse_map = [](const core::SparseFileMap& first,
                                    const core::SparseFileMap& second) {
        if (first.is_sparse != second.is_sparse ||
            first.allocated.size() != second.allocated.size()) {
            return false;
        }
        return std::equal(first.allocated.begin(), first.allocated.end(),
                          second.allocated.begin(),
                          [](const core::SparseExtent& a,
                             const core::SparseExtent& b) {
                              return a.offset == b.offset && a.length == b.length;
                          });
    };
    return left.type == right.type && left.path == right.path &&
           left.size == right.size && left.mtime == right.mtime &&
           left.crc == right.crc && left.has_blake3 == right.has_blake3 &&
           (!left.has_blake3 || left.blake3 == right.blake3) &&
           left.chunk_refs == right.chunk_refs &&
           left.link_target == right.link_target &&
           same_blobs(left.ads, right.ads) &&
           same_metadata(left.meta, right.meta) &&
           same_sparse_map(left.sparse, right.sparse);
}

std::vector<ArchiveSnapshotChange> snapshot_changes(
    const std::vector<EntryRec>& before_entries,
    const std::vector<EntryRec>& after_entries) {
    std::map<std::string, const EntryRec*> before;
    std::map<std::string, const EntryRec*> after;
    for (const auto& entry : before_entries) before.emplace(entry.path, &entry);
    for (const auto& entry : after_entries) after.emplace(entry.path, &entry);

    std::vector<ArchiveSnapshotChange> result;
    auto before_it = before.begin();
    auto after_it = after.begin();
    while (before_it != before.end() || after_it != after.end()) {
        if (after_it == after.end() ||
            (before_it != before.end() && before_it->first < after_it->first)) {
            result.push_back({before_it->first, ArchiveSnapshotChangeKind::removed,
                              before_it->second->size, 0});
            ++before_it;
            continue;
        }
        if (before_it == before.end() || after_it->first < before_it->first) {
            result.push_back({after_it->first, ArchiveSnapshotChangeKind::added,
                              0, after_it->second->size});
            ++after_it;
            continue;
        }
        if (!same_snapshot_entry(*before_it->second, *after_it->second)) {
            result.push_back({after_it->first, ArchiveSnapshotChangeKind::modified,
                              before_it->second->size, after_it->second->size});
        }
        ++before_it;
        ++after_it;
    }
    return result;
}

std::uint64_t checked_storage_sum(std::uint64_t total, std::uint64_t value,
                                  const char* field) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        throw FormatError(std::string("archive storage ") + field + " overflows");
    }
    return total + value;
}

struct ArchiveStorageUsage {
    std::uint64_t logical_bytes = 0;
    std::set<std::uint64_t> chunks;
    std::set<std::uint64_t> blocks;
};

ArchiveStorageUsage storage_usage_for_entries(const std::vector<EntryRec>& entries,
                                               const ArchiveIndex& index) {
    ArchiveStorageUsage usage;
    for (const auto& entry : entries) {
        if (entry.type != kEntryFile) continue;
        usage.logical_bytes = checked_storage_sum(
            usage.logical_bytes, entry.size, "logical byte count");
        if (!entry.chunk_refs.empty()) {
            for (const auto ref : entry.chunk_refs) {
                if (ref >= index.chunks.size()) {
                    throw FormatError("snapshot entry points outside the chunk table");
                }
                usage.chunks.insert(ref);
                const auto block = index.chunks[static_cast<std::size_t>(ref)].block_index;
                if (block >= index.blocks.size()) {
                    throw FormatError("archive chunk points outside the block table");
                }
                usage.blocks.insert(block);
            }
            continue;
        }
        std::uint64_t remaining = entry.size;
        std::uint64_t block_index = entry.first_block;
        std::uint64_t offset = entry.offset;
        while (remaining != 0) {
            if (block_index >= index.blocks.size()) {
                throw FormatError("file extends past the last block");
            }
            const auto& block = index.blocks[static_cast<std::size_t>(block_index)];
            if (offset >= block.uncompressed_size || block.uncompressed_size == 0) {
                throw FormatError("file offset lies outside its block");
            }
            usage.blocks.insert(block_index);
            const auto take = std::min(remaining, block.uncompressed_size - offset);
            remaining -= take;
            offset = 0;
            ++block_index;
        }
    }
    return usage;
}

std::uint64_t storage_chunk_bytes(const std::set<std::uint64_t>& chunks,
                                  const ArchiveIndex& index) {
    std::uint64_t total = 0;
    for (const auto chunk : chunks) {
        if (chunk >= index.chunks.size()) {
            throw FormatError("archive storage chunk index is invalid");
        }
        total = checked_storage_sum(
            total, index.chunks[static_cast<std::size_t>(chunk)].identity.size,
            "content byte count");
    }
    return total;
}

std::uint64_t storage_block_bytes(const std::set<std::uint64_t>& blocks,
                                  const ArchiveIndex& index, bool compressed) {
    std::uint64_t total = 0;
    for (const auto block : blocks) {
        if (block >= index.blocks.size()) {
            throw FormatError("archive storage block index is invalid");
        }
        const auto& record = index.blocks[static_cast<std::size_t>(block)];
        total = checked_storage_sum(total,
                                    compressed ? record.compressed_size
                                               : record.uncompressed_size,
                                    compressed ? "stored payload" : "content byte count");
    }
    return total;
}

std::uint64_t storage_content_bytes(const ArchiveStorageUsage& usage,
                                    const ArchiveIndex& index) {
    return index.meta.chunk_table
        ? storage_chunk_bytes(usage.chunks, index)
        : storage_block_bytes(usage.blocks, index, false);
}

std::optional<std::uint64_t> storage_entry_packed_bytes(
    const EntryRec& entry, const ArchiveIndex& index, bool& estimated) {
    estimated = false;
    if (entry.type != kEntryFile) return std::nullopt;
    if (entry.size == 0) return std::uint64_t{0};
    if (!entry.chunk_refs.empty()) {
        std::uint64_t packed = 0;
        for (const auto ref : entry.chunk_refs) {
            if (ref >= index.chunks.size()) {
                throw FormatError("snapshot entry points outside the chunk table");
            }
            const auto block = index.chunks[static_cast<std::size_t>(ref)].block_index;
            if (block >= index.blocks.size()) {
                throw FormatError("archive chunk points outside the block table");
            }
            packed = checked_storage_sum(
                packed, index.blocks[static_cast<std::size_t>(block)].compressed_size,
                "file packed size");
        }
        return packed;
    }

    std::uint64_t remaining = entry.size;
    std::uint64_t block_index = entry.first_block;
    std::uint64_t offset = entry.offset;
    long double packed = 0.0L;
    while (remaining != 0) {
        if (block_index >= index.blocks.size()) {
            throw FormatError("file extends past the last block");
        }
        const auto& block = index.blocks[static_cast<std::size_t>(block_index)];
        if (offset >= block.uncompressed_size || block.uncompressed_size == 0) {
            throw FormatError("file offset lies outside its block");
        }
        const auto take = std::min(remaining, block.uncompressed_size - offset);
        packed += static_cast<long double>(take) *
                  static_cast<long double>(block.compressed_size) /
                  static_cast<long double>(block.uncompressed_size);
        remaining -= take;
        offset = 0;
        ++block_index;
    }
    estimated = true;
    if (packed > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(packed + 0.5L));
}

enum class PasswordMutation {
    add,
    remove,
    change,
};

std::size_t find_password_slot(const EncryptionInfo& enc,
                               const std::string& password,
                               const core::CryptoKey& expected_key) {
    validate_password_input(password, "password");
    for (std::size_t i = 0; i < enc.slots.size(); ++i) {
        core::CryptoKey candidate{};
        if (!try_unwrap_encryption_slot(enc, enc.slots[i], password, candidate)) {
            continue;
        }
        const bool matches = candidate == expected_key;
        core::secure_wipe(candidate);
        if (matches) {
            return i;
        }
    }
    throw std::runtime_error("password does not unlock an archive password slot");
}

std::uint32_t next_encryption_slot_id(const EncryptionInfo& enc) {
    std::uint32_t next = 1;
    for (const auto& slot : enc.slots) {
        if (slot.id == std::numeric_limits<std::uint32_t>::max()) {
            continue;
        }
        next = std::max(next, static_cast<std::uint32_t>(slot.id + 1));
    }
    while (std::any_of(enc.slots.begin(), enc.slots.end(),
                       [next](const EncryptionSlot& slot) { return slot.id == next; })) {
        if (next == std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("archive password slot identifiers are exhausted");
        }
        ++next;
    }
    return next;
}

void rewrite_archive_password(const fs::path& archive_path,
                              const std::string& current_password,
                              const std::string& secondary_password,
                              PasswordMutation mutation,
                              const CompressionOptions& options) {
    reject_volume_mutation(archive_path);
    validate_password_input(current_password, "current password");
    if (mutation != PasswordMutation::remove) {
        validate_password_input(secondary_password, "new password");
    } else {
        validate_password_input(secondary_password, "password to remove");
    }

    std::uint64_t file_size = 0;
    auto input = open_archive(archive_path, file_size);
    const ByteSource source(input, file_size);
    const auto layout = read_layout(source);
    const auto existing_recovery = read_recovery_service(source, layout);
    auto loaded = load_index(source, current_password);
    OptionalKeyWipeGuard loaded_key_wipe{loaded.key};
    if (loaded.index.meta.locked) {
        throw std::runtime_error("archive is locked (read-only)");
    }
    if (!loaded.index.meta.encryption.enabled || !loaded.key) {
        throw std::runtime_error("archive is not password-encrypted");
    }

    core::CryptoKey data_key = *loaded.key;
    CryptoKeyWipeGuard data_key_wipe{data_key};
    EncryptionInfo updated;
    const auto& old = loaded.index.meta.encryption;
    if (old.v2) {
        updated = old;
        if (mutation == PasswordMutation::add) {
            if (updated.slots.size() >= kMaxEncryptionSlots) {
                throw std::runtime_error("archive already has the maximum password slots");
            }
            updated.slots.push_back(make_encryption_slot(
                updated, next_encryption_slot_id(updated), secondary_password, data_key));
        } else if (mutation == PasswordMutation::remove) {
            const auto index = find_password_slot(updated, secondary_password, data_key);
            if (updated.slots.size() <= 1) {
                throw std::runtime_error("cannot remove the archive's last password");
            }
            updated.slots.erase(updated.slots.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            const auto index = find_password_slot(updated, current_password, data_key);
            updated.slots[index] = make_encryption_slot(
                updated, updated.slots[index].id, secondary_password, data_key);
        }
    } else {
        // A legacy archive has exactly one password-derived data key. Migration
        // wraps that same key in v2 slots, so every compressed/encrypted byte is
        // retained even when the directory preamble grows.
        updated.enabled = true;
        updated.encrypt_directory = old.encrypt_directory;
        updated.v2 = true;
        core::random_bytes(updated.key_id);
        if (mutation == PasswordMutation::remove) {
            throw std::runtime_error("cannot remove the last legacy archive password");
        }
        updated.slots.push_back(make_encryption_slot(
            updated, 1, mutation == PasswordMutation::change
                           ? secondary_password : current_password,
            data_key));
        if (mutation == PasswordMutation::add) {
            updated.slots.push_back(make_encryption_slot(
                updated, 2, secondary_password, data_key));
        }
    }
    loaded.index.meta.encryption = std::move(updated);
    loaded.index.meta.has_signature = false;

    const std::uint64_t old_block_start =
        (layout.flags & kFlagEncryptedDirectory) != 0
            ? layout.preamble_offset + layout.preamble_size
            : kHeaderSize;
    const std::uint64_t old_block_end = archive_block_region_end(layout, loaded.index);
    if (old_block_end < old_block_start) {
        throw FormatError("archive block region is before its encryption preamble");
    }
    for (const auto& block : loaded.index.blocks) {
        if (block.compressed_offset < old_block_start ||
            block.compressed_offset > old_block_end ||
            block.compressed_size > old_block_end - block.compressed_offset) {
            throw FormatError("archive block lies outside its protected block region");
        }
    }

    fs::path temp_path = archive_path;
    temp_path += ".tmp";
    TempFileGuard temp_guard(temp_path);
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create archive: " + core::path_to_utf8(temp_path));
    }
    const auto header = archive_header_bytes(
        archive_header_version(loaded.index.entries, loaded.index.meta),
        archive_header_flags(loaded.index.entries, loaded.index.meta));
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    std::uint64_t new_block_start = header.size();
    if (loaded.index.meta.encryption.encrypt_directory) {
        const auto preamble = serialize_encryption_preamble(loaded.index.meta.encryption);
        output.write(reinterpret_cast<const char*>(preamble.data()),
                     static_cast<std::streamsize>(preamble.size()));
        new_block_start += preamble.size();
    }
    if (!output) {
        throw std::runtime_error("failed to write archive encryption header");
    }

    const auto shift_offset = [&](std::uint64_t offset) {
        if (new_block_start >= old_block_start) {
            const auto delta = new_block_start - old_block_start;
            if (offset > std::numeric_limits<std::uint64_t>::max() - delta) {
                throw FormatError("archive block offset overflows during rekeying");
            }
            return offset + delta;
        }
        const auto delta = old_block_start - new_block_start;
        if (offset < delta) {
            throw FormatError("archive block offset underflows during rekeying");
        }
        return offset - delta;
    };
    for (auto& block : loaded.index.blocks) {
        block.compressed_offset = shift_offset(block.compressed_offset);
    }

    std::uint64_t copied = 0;
    const std::uint64_t copy_size = old_block_end - old_block_start;
    const auto chunk_size = effective_io_buffer_size(options.io_buffer_size);
    report_operation(options.operation, OperationStage::copying, 0, copy_size,
                     0, loaded.index.blocks.size(), "Preserving encrypted blocks");
    while (copied < copy_size) {
        operation_checkpoint(options.operation);
        const auto want = std::min<std::uint64_t>(chunk_size, copy_size - copied);
        const auto chunk = source.read(old_block_start + copied, want);
        output.write(reinterpret_cast<const char*>(chunk.data()),
                     static_cast<std::streamsize>(chunk.size()));
        if (!output) {
            throw std::runtime_error("failed while preserving encrypted blocks");
        }
        copied += want;
        report_operation(options.operation, OperationStage::copying, copied, copy_size,
                         loaded.index.blocks.size(), loaded.index.blocks.size(),
                         "Preserving encrypted blocks");
    }
    const std::uint64_t written = new_block_start + copy_size;
    patch_archive_header(output,
                         archive_header_version(loaded.index.entries, loaded.index.meta),
                         archive_header_flags(loaded.index.entries, loaded.index.meta));
    const core::CryptoKey* directory_key =
        loaded.index.meta.encryption.encrypt_directory ? &data_key : nullptr;
    write_directory_and_footer(output, written, loaded.index.blocks, loaded.index.entries,
                               loaded.index.meta, directory_key, true,
                               loaded.index.meta.chunk_table ? &loaded.index.chunks : nullptr);
    input.close();

    const unsigned recovery_percent = options.recovery_percent != 0
        ? options.recovery_percent
        : (existing_recovery ? existing_recovery->percent : 0);
    if (recovery_percent != 0) {
        append_recovery_to_staged_archive(
            temp_path, recovery_percent, options.operation, options.thread_count);
    }
    replace_archive_file(temp_path, archive_path);
    temp_guard.dismiss();
}

void create_chunked_archive_impl(
    const std::vector<std::filesystem::path>& inputs,
    const fs::path& archive_path,
    const std::optional<std::string>& snapshot_name,
    const CompressionOptions& options) {
    reject_volume_mutation(archive_path);
    if (snapshot_name) validate_snapshot_name(*snapshot_name);

    const auto operation = options.operation;
    report_operation(operation, OperationStage::scanning, 0, 0, 0, inputs.size());
    std::vector<ScanItem> items;
    for (const auto& input : inputs) {
        operation_checkpoint(operation);
        scan_input(input, items);
    }

    CompressionOptions chunk_options = options;
    chunk_options.enable_content_dedup = !snapshot_name.has_value();
    chunk_options.enable_snapshot_dedup = snapshot_name.has_value();
    const bool keyed = !options.password.empty() && options.keyed_chunk_ids;

    fs::path temp_path = archive_path;
    temp_path += ".tmp";
    TempFileGuard temp_guard(temp_path);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot create deduplicated archive: " +
                                 core::path_to_utf8(temp_path));
    }

    ArchiveMeta meta;
    meta.chunk_table = true;
    meta.live_dedup = !snapshot_name.has_value();
    meta.keyed_chunk_ids = keyed;
    meta.dedup_profile = dedup_profile_from_options(chunk_options);
    core::CryptoKey key{};
    struct KeyWipeGuard {
        core::CryptoKey& key;
        ~KeyWipeGuard() { core::secure_wipe(key); }
    } key_wipe{key};
    const core::CryptoKey* key_ptr = nullptr;
    const bool encrypt_dir = !options.password.empty() && options.encrypt_header;
    if (!options.password.empty()) {
        auto [enc, derived] = options.use_encryption_v2
            ? make_encryption_v2(options.password)
            : make_encryption(options.password);
        enc.encrypt_directory = encrypt_dir;
        meta.encryption = std::move(enc);
        key = derived;
        key_ptr = &key;
    }

    auto header = archive_header_bytes(
        archive_header_version({}, meta), archive_header_flags({}, meta));
    out.write(reinterpret_cast<const char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    std::uint64_t written = header.size();
    if (encrypt_dir) {
        const auto preamble = serialize_encryption_preamble(meta.encryption);
        out.write(reinterpret_cast<const char*>(preamble.data()),
                  static_cast<std::streamsize>(preamble.size()));
        written += preamble.size();
    }
    if (!out) throw std::runtime_error("failed to write deduplicated archive header");

    std::vector<BlockRec> blocks;
    std::vector<ChunkRec> chunks;
    SnapshotChunkLookup lookup;
    ArchiveReuseStats reuse_stats;
    const auto entries = build_chunked_entries(
        out, written, blocks, chunks, lookup, items, chunk_options, operation,
        key_ptr, keyed, meta, reuse_stats);
    if (snapshot_name) {
        meta.snapshots.push_back({*snapshot_name, 0, snapshot_now(), entries});
    }

    out.seekp(0, std::ios::beg);
    header = archive_header_bytes(archive_header_version(entries, meta),
                                  archive_header_flags(entries, meta));
    out.write(reinterpret_cast<const char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    out.seekp(0, std::ios::end);
    if (!out) throw std::runtime_error("failed to finalize deduplicated archive header");
    const auto source_bytes = scanned_file_bytes(items);
    report_operation(operation, OperationStage::finalizing, source_bytes, source_bytes,
                     items.size(), items.size(), {}, 0, 0, source_bytes, written,
                     source_bytes, reuse_stats.reused_items, reuse_stats.reused_bytes);
    write_directory_and_footer(out, written, blocks, entries, meta,
                               encrypt_dir ? key_ptr : nullptr, true, &chunks);
    core::secure_wipe(key);
    if (options.recovery_percent != 0) {
        append_recovery_to_staged_archive(
            temp_path, options.recovery_percent, operation, options.thread_count);
    }
    replace_archive_file(temp_path, archive_path);
    temp_guard.dismiss();
    report_operation(operation, OperationStage::finalizing, source_bytes, source_bytes,
                     items.size(), items.size(), {}, 0, 0, source_bytes,
                     static_cast<std::uint64_t>(fs::file_size(archive_path)),
                     source_bytes, reuse_stats.reused_items, reuse_stats.reused_bytes);
}

}  // namespace

void create_archive(const std::vector<std::filesystem::path>& inputs,
                     const std::filesystem::path& archive_path,
                     const CompressionOptions& options) {
    if (options.enable_content_dedup) {
        create_chunked_archive_impl(inputs, archive_path, std::nullopt, options);
        return;
    }
    reject_volume_mutation(archive_path);
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

    const auto block_size = effective_solid_block_size(options);
    validate_large_solid_block_options(options, block_size);

    fs::path temp_path = archive_path;
    temp_path += ".tmp";
    TempFileGuard temp_guard(temp_path);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot create archive: " +
                                 core::path_to_utf8(temp_path));
    }

    // Encryption (optional). Derive the key + key-check first so we can flag the
    // header and, for a sealed directory, emit the plaintext parameter preamble.
    ArchiveMeta meta;
    meta.large_solid_blocks = requests_large_solid_blocks(block_size);
    core::CryptoKey key{};
    const core::CryptoKey* key_ptr = nullptr;
    const bool encrypt_dir = !options.password.empty() && options.encrypt_header;
    if (!options.password.empty()) {
        auto [enc, derived] = options.use_encryption_v2
            ? make_encryption_v2(options.password)
            : make_encryption(options.password);
        enc.encrypt_directory = encrypt_dir;
        meta.encryption = std::move(enc);
        key = derived;
        key_ptr = &key;
    }

    const auto header = archive_header_bytes(
        archive_header_version({}, meta), archive_header_flags({}, meta));
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
    ArchiveReuseStats reuse_stats;
    compress_items_into(out, written, blocks, entries, items, options, block_size, operation,
                        total_bytes, total_items, completed_bytes, completed_items,
                        options.skip_unreadable_files, key_ptr, &meta, nullptr, &reuse_stats);

    patch_archive_header(out, archive_header_version(entries, meta),
                         archive_header_flags(entries, meta));

    report_operation(operation, OperationStage::finalizing, completed_bytes, total_bytes,
                     completed_items, total_items, {}, 0, 0, completed_bytes,
                     written, completed_bytes, reuse_stats.reused_items,
                     reuse_stats.reused_bytes);
    write_directory_and_footer(out, written, blocks, entries, meta,
                               encrypt_dir ? key_ptr : nullptr);
    core::secure_wipe(key);

    if (options.recovery_percent != 0) {
        append_recovery_to_staged_archive(
            temp_path, options.recovery_percent, operation, options.thread_count);
    }
    report_operation(operation, OperationStage::committing, 0, 1, 0, 1,
                     "Committing archive");
    replace_archive_file(temp_path, archive_path);
    temp_guard.dismiss();
    report_operation(operation, OperationStage::committing, 1, 1, 1, 1,
                     "Archive committed");
    report_operation(operation, OperationStage::finalizing, total_bytes, total_bytes,
                     total_items, total_items, {}, 0, 0, total_bytes,
                     static_cast<std::uint64_t>(fs::file_size(archive_path)),
                     total_bytes, reuse_stats.reused_items, reuse_stats.reused_bytes);
}

void create_snapshot_archive(
    const std::vector<std::filesystem::path>& inputs,
    const std::filesystem::path& archive_path,
    const std::string& snapshot_name,
    const CompressionOptions& options) {
    if (inputs.empty()) throw std::invalid_argument("snapshot needs at least one input");
    create_chunked_archive_impl(inputs, archive_path, snapshot_name, options);
}

void add_archive_snapshot(
    const std::vector<std::filesystem::path>& inputs,
    const std::filesystem::path& archive_path,
    const std::string& snapshot_name,
    const CompressionOptions& options) {
    reject_volume_mutation(archive_path);
    validate_snapshot_name(snapshot_name);
    if (inputs.empty()) throw std::invalid_argument("snapshot needs at least one input");
    if (!fs::exists(archive_path)) {
        throw std::runtime_error("snapshot repository does not exist; use snapshot create");
    }

    const auto operation = options.operation;
    report_operation(operation, OperationStage::scanning, 0, 0, 0, inputs.size());
    std::vector<ScanItem> items;
    for (const auto& input : inputs) {
        operation_checkpoint(operation);
        scan_input(input, items);
    }

    std::uint64_t physical_size = 0;
    auto input = open_archive(archive_path, physical_size);
    const ByteSource source(input, physical_size);
    const auto layout = read_layout(source);
    const auto recovery = read_recovery_service(source, layout);
    auto loaded = load_index(source, options.password);
    OptionalKeyWipeGuard key_wipe{loaded.key};
    require_snapshot_profile(loaded.index);
    if (loaded.index.meta.locked) throw std::runtime_error("archive is locked (read-only)");
    if (std::any_of(loaded.index.meta.snapshots.begin(), loaded.index.meta.snapshots.end(),
                    [&snapshot_name](const SnapshotRec& snapshot) {
                        return snapshot.name == snapshot_name;
                    })) {
        throw std::invalid_argument("snapshot name already exists: " + snapshot_name);
    }
    if (loaded.index.meta.encryption.enabled && !loaded.key) {
        throw std::runtime_error("encrypted snapshot repository requires a password");
    }
    const bool keyed = loaded.index.meta.encryption.enabled && options.keyed_chunk_ids;
    if (keyed != loaded.index.meta.keyed_chunk_ids) {
        throw std::invalid_argument(
            "snapshot chunk identifier mode does not match the repository");
    }
    if (layout.footer_offset > std::numeric_limits<std::uint64_t>::max() - kFooterSize) {
        throw FormatError("archive footer offset overflows append position");
    }
    const auto previous_end = layout.footer_offset + kFooterSize;
    if (physical_size < previous_end) throw FormatError("archive is truncated before its footer");
    if (layout.generation == std::numeric_limits<std::uint64_t>::max()) {
        throw FormatError("archive generation number is exhausted");
    }
    const auto generation = layout.generation + 1;
    const auto previous_footer_offset = layout.footer_offset;
    const auto previous_directory_offset = layout.directory_offset;
    const auto previous_directory_size = layout.directory_size;
    const auto previous_generation_offset = layout.generation_offset;
    auto blocks = std::move(loaded.index.blocks);
    auto chunks = std::move(loaded.index.chunks);
    auto meta = std::move(loaded.index.meta);
    meta.has_signature = false;
    auto lookup = build_snapshot_chunk_lookup(chunks);
    ArchiveReuseStats reuse_stats;

    input.close();
    try {
        if (physical_size != previous_end) {
            std::error_code error;
            fs::resize_file(archive_path, previous_end, error);
            if (error) throw fs::filesystem_error("failed to trim archive tail",
                                                   archive_path, error);
        }
        std::ofstream out(archive_path, std::ios::binary | std::ios::app);
        if (!out) throw std::runtime_error("cannot open snapshot repository for append");
        std::uint64_t written = previous_end;
        CompressionOptions snapshot_options = meta.dedup_profile.present
            ? options_for_dedup_profile(options, meta.dedup_profile)
            : options;
        snapshot_options.enable_snapshot_dedup = true;
        const auto entries = build_chunked_entries(
            out, written, blocks, chunks, lookup, items, snapshot_options, operation,
            loaded.key ? &*loaded.key : nullptr, keyed, meta, reuse_stats);
        meta.snapshots.push_back({snapshot_name, generation, snapshot_now(), entries});
        const auto actual_version = archive_header_version(entries, meta);
        const auto actual_flags = archive_header_flags(entries, meta);
        if (actual_version != layout.version || actual_flags != layout.flags) {
            throw std::runtime_error(
                "snapshot capture changed required header features; retry with a fresh repository");
        }
        const auto directory_key =
            meta.encryption.encrypt_directory && loaded.key ? &*loaded.key : nullptr;
        const auto directory = write_directory_and_footer(
            out, written, blocks, entries, meta, directory_key, false, &chunks);
        out.close();
        if (!out) throw std::runtime_error("failed to close snapshot directory");
        append_generation_trailer(
            archive_path, directory.directory_offset, directory.directory_size,
            generation, previous_footer_offset, previous_directory_offset,
            previous_directory_size, previous_generation_offset,
            options.recovery_percent != 0
                ? options.recovery_percent : (recovery ? recovery->percent : 0),
            operation, options.thread_count, previous_end);
    } catch (...) {
        std::error_code error;
        fs::resize_file(archive_path, previous_end, error);
        throw;
    }
    report_operation(operation, OperationStage::finalizing, scanned_file_bytes(items),
                     scanned_file_bytes(items), items.size(), items.size(), {}, 0, 0,
                     scanned_file_bytes(items),
                     static_cast<std::uint64_t>(fs::file_size(archive_path)),
                     scanned_file_bytes(items), reuse_stats.reused_items,
                     reuse_stats.reused_bytes);
}

std::vector<ArchiveSnapshotInfo> list_archive_snapshots(
    const std::filesystem::path& archive_path, const std::string& password) {
    std::uint64_t file_size = 0;
    auto input = open_archive(archive_path, file_size);
    const ByteSource source(input, file_size);
    auto loaded = load_index(source, password);
    OptionalKeyWipeGuard key_wipe{loaded.key};
    require_snapshot_profile(loaded.index);

    std::vector<ArchiveSnapshotInfo> result;
    result.reserve(loaded.index.meta.snapshots.size());
    for (std::size_t index = 0; index < loaded.index.meta.snapshots.size(); ++index) {
        const auto& snapshot = loaded.index.meta.snapshots[index];
        ArchiveSnapshotInfo info;
        info.name = snapshot.name;
        info.generation = snapshot.generation;
        info.created = snapshot.created;
        info.entry_count = snapshot.entries.size();
        info.current = index + 1 == loaded.index.meta.snapshots.size();
        for (const auto& entry : snapshot.entries) {
            if (entry.type == kEntryFile) info.file_bytes += entry.size;
        }
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<ArchiveSnapshotChange> diff_archive_snapshots(
    const std::filesystem::path& archive_path,
    const std::string& from_snapshot,
    const std::string& to_snapshot,
    const std::string& password) {
    std::uint64_t file_size = 0;
    auto input = open_archive(archive_path, file_size);
    const ByteSource source(input, file_size);
    auto loaded = load_index(source, password);
    OptionalKeyWipeGuard key_wipe{loaded.key};
    require_snapshot_profile(loaded.index);
    const auto& from = find_snapshot(loaded.index.meta, from_snapshot);
    const auto& to = find_snapshot(loaded.index.meta, to_snapshot);

    return snapshot_changes(from.entries, to.entries);
}

ArchiveStorageAnalysis analyze_archive_storage(
    const std::filesystem::path& archive_path, const std::string& password) {
    std::uint64_t physical_size = 0;
    auto input = open_archive(archive_path, physical_size);
    const ByteSource source(input, physical_size);
    auto loaded = load_index(source, password);
    OptionalKeyWipeGuard key_wipe{loaded.key};
    const auto& index = loaded.index;

    ArchiveStorageAnalysis result;
    result.physical_bytes = physical_size;
    result.physical_layout_exact = true;
    result.packed_sizes_complete = true;
    result.snapshot_repository =
        index.meta.chunk_table && !index.meta.snapshots.empty();
    result.deduplicated = index.meta.chunk_table || index.meta.live_dedup;

    const auto current_usage = storage_usage_for_entries(index.entries, index);
    result.logical_bytes = current_usage.logical_bytes;

    ArchiveStorageUsage retained_usage;
    if (result.snapshot_repository) {
        std::set<std::uint64_t> cumulative_chunks;
        std::set<std::uint64_t> cumulative_blocks;
        const std::vector<EntryRec> empty_entries;
        const std::vector<EntryRec>* previous_entries = &empty_entries;
        result.snapshots.reserve(index.meta.snapshots.size());
        for (std::size_t snapshot_index = 0;
             snapshot_index < index.meta.snapshots.size(); ++snapshot_index) {
            const auto& snapshot = index.meta.snapshots[snapshot_index];
            const auto usage = storage_usage_for_entries(snapshot.entries, index);
            result.referenced_logical_bytes = checked_storage_sum(
                result.referenced_logical_bytes, usage.logical_bytes,
                "referenced logical byte count");

            ArchiveSnapshotStorageInfo info;
            info.snapshot.name = snapshot.name;
            info.snapshot.generation = snapshot.generation;
            info.snapshot.created = snapshot.created;
            info.snapshot.entry_count = snapshot.entries.size();
            info.snapshot.file_bytes = usage.logical_bytes;
            info.snapshot.current = snapshot_index + 1 == index.meta.snapshots.size();
            info.unique_content_bytes = storage_content_bytes(usage, index);
            info.stored_payload_bytes = storage_block_bytes(usage.blocks, index, true);

            std::set<std::uint64_t> new_chunks;
            std::set<std::uint64_t> new_blocks;
            std::set_difference(usage.chunks.begin(), usage.chunks.end(),
                                cumulative_chunks.begin(), cumulative_chunks.end(),
                                std::inserter(new_chunks, new_chunks.end()));
            std::set_difference(usage.blocks.begin(), usage.blocks.end(),
                                cumulative_blocks.begin(), cumulative_blocks.end(),
                                std::inserter(new_blocks, new_blocks.end()));
            info.new_content_bytes = index.meta.chunk_table
                ? storage_chunk_bytes(new_chunks, index)
                : storage_block_bytes(new_blocks, index, false);
            info.new_stored_bytes = storage_block_bytes(new_blocks, index, true);
            info.changes = snapshot_changes(*previous_entries, snapshot.entries);
            for (const auto& change : info.changes) {
                switch (change.kind) {
                    case ArchiveSnapshotChangeKind::added: ++info.added_entries; break;
                    case ArchiveSnapshotChangeKind::modified: ++info.modified_entries; break;
                    case ArchiveSnapshotChangeKind::removed: ++info.removed_entries; break;
                }
            }
            result.snapshots.push_back(std::move(info));

            cumulative_chunks.insert(usage.chunks.begin(), usage.chunks.end());
            cumulative_blocks.insert(usage.blocks.begin(), usage.blocks.end());
            retained_usage.chunks.insert(usage.chunks.begin(), usage.chunks.end());
            retained_usage.blocks.insert(usage.blocks.begin(), usage.blocks.end());
            previous_entries = &snapshot.entries;
        }
    } else {
        result.referenced_logical_bytes = current_usage.logical_bytes;
        retained_usage = current_usage;
    }

    result.unique_content_bytes = storage_content_bytes(retained_usage, index);
    result.stored_payload_bytes = storage_block_bytes(retained_usage.blocks, index, true);
    if (result.referenced_logical_bytes > result.unique_content_bytes) {
        result.deduplication_saved_bytes =
            result.referenced_logical_bytes - result.unique_content_bytes;
    }
    if (result.unique_content_bytes > result.stored_payload_bytes) {
        result.compression_saved_bytes =
            result.unique_content_bytes - result.stored_payload_bytes;
    }

    std::uint64_t all_payload_bytes = 0;
    for (const auto& block : index.blocks) {
        all_payload_bytes = checked_storage_sum(
            all_payload_bytes, block.compressed_size, "total stored payload");
    }
    if (all_payload_bytes > result.physical_bytes ||
        result.stored_payload_bytes > all_payload_bytes) {
        throw FormatError("archive storage accounting exceeds the physical file size");
    }
    result.unreferenced_payload_bytes = all_payload_bytes - result.stored_payload_bytes;
    result.metadata_and_service_bytes = result.physical_bytes - all_payload_bytes;

    if (result.snapshot_repository) {
        std::set<std::uint64_t> history_chunks;
        std::set<std::uint64_t> history_blocks;
        std::set_difference(retained_usage.chunks.begin(), retained_usage.chunks.end(),
                            current_usage.chunks.begin(), current_usage.chunks.end(),
                            std::inserter(history_chunks, history_chunks.end()));
        std::set_difference(retained_usage.blocks.begin(), retained_usage.blocks.end(),
                            current_usage.blocks.begin(), current_usage.blocks.end(),
                            std::inserter(history_blocks, history_blocks.end()));
        result.history_only_content_bytes = index.meta.chunk_table
            ? storage_chunk_bytes(history_chunks, index)
            : storage_block_bytes(history_blocks, index, false);
        result.history_only_stored_bytes =
            storage_block_bytes(history_blocks, index, true);
    }

    result.files.reserve(index.entries.size());
    for (const auto& entry : index.entries) {
        if (entry.type != kEntryFile) continue;
        ArchiveStorageFileInfo file;
        file.path = entry.path;
        file.logical_bytes = entry.size;
        file.chunk_count = entry.chunk_refs.size();
        file.packed_bytes = storage_entry_packed_bytes(
            entry, index, file.packed_bytes_estimated);
        result.files.push_back(std::move(file));
    }
    std::sort(result.files.begin(), result.files.end(),
              [](const ArchiveStorageFileInfo& left,
                 const ArchiveStorageFileInfo& right) {
                  if (left.logical_bytes != right.logical_bytes) {
                      return left.logical_bytes > right.logical_bytes;
                  }
                  return left.path < right.path;
              });
    return result;
}

void prune_archive_snapshots(
    const std::filesystem::path& archive_path,
    const std::vector<std::string>& snapshot_names,
    const CompressionOptions& options) {
    reject_volume_mutation(archive_path);
    if (snapshot_names.empty()) {
        throw std::invalid_argument("prune requires at least one snapshot name");
    }

    std::uint64_t physical_size = 0;
    auto input = open_archive(archive_path, physical_size);
    const ByteSource source(input, physical_size);
    const auto layout = read_layout(source);
    const auto recovery = read_recovery_service(source, layout);
    auto loaded = load_index(source, options.password);
    OptionalKeyWipeGuard key_wipe{loaded.key};
    require_snapshot_profile(loaded.index);
    if (loaded.index.meta.locked) throw std::runtime_error("archive is locked (read-only)");

    std::unordered_set<std::string> remove;
    remove.reserve(snapshot_names.size());
    for (const auto& name : snapshot_names) {
        validate_snapshot_name(name);
        if (!remove.insert(name).second) {
            throw std::invalid_argument("duplicate snapshot name: " + name);
        }
        (void)find_snapshot(loaded.index.meta, name);
    }
    const auto& current = loaded.index.meta.snapshots.back();
    if (remove.find(current.name) != remove.end()) {
        throw std::invalid_argument("cannot prune the current snapshot: " + current.name);
    }

    std::vector<SnapshotRec> retained;
    retained.reserve(loaded.index.meta.snapshots.size() - remove.size());
    for (auto& snapshot : loaded.index.meta.snapshots) {
        if (remove.find(snapshot.name) == remove.end()) {
            retained.push_back(std::move(snapshot));
        }
    }
    loaded.index.meta.snapshots = std::move(retained);
    loaded.index.meta.has_signature = false;

    if (layout.footer_offset > std::numeric_limits<std::uint64_t>::max() - kFooterSize) {
        throw FormatError("archive footer offset overflows append position");
    }
    const auto previous_end = layout.footer_offset + kFooterSize;
    if (physical_size < previous_end) throw FormatError("archive is truncated before its footer");
    if (layout.generation == std::numeric_limits<std::uint64_t>::max()) {
        throw FormatError("archive generation number is exhausted");
    }
    const auto generation = layout.generation + 1;
    const auto previous_footer_offset = layout.footer_offset;
    const auto previous_directory_offset = layout.directory_offset;
    const auto previous_directory_size = layout.directory_size;
    const auto previous_generation_offset = layout.generation_offset;
    input.close();

    try {
        if (physical_size != previous_end) {
            std::error_code error;
            fs::resize_file(archive_path, previous_end, error);
            if (error) throw fs::filesystem_error("failed to trim archive tail",
                                                   archive_path, error);
        }
        std::ofstream out(archive_path, std::ios::binary | std::ios::app);
        if (!out) throw std::runtime_error("cannot open snapshot repository for append");
        std::uint64_t written = previous_end;
        const auto directory_key = loaded.index.meta.encryption.encrypt_directory && loaded.key
            ? &*loaded.key : nullptr;
        const auto directory = write_directory_and_footer(
            out, written, loaded.index.blocks, loaded.index.entries, loaded.index.meta,
            directory_key, false, &loaded.index.chunks);
        out.close();
        if (!out) throw std::runtime_error("failed to close pruned snapshot directory");
        append_generation_trailer(
            archive_path, directory.directory_offset, directory.directory_size,
            generation, previous_footer_offset, previous_directory_offset,
            previous_directory_size, previous_generation_offset,
            options.recovery_percent != 0
                ? options.recovery_percent : (recovery ? recovery->percent : 0),
            options.operation, options.thread_count, previous_end);
    } catch (...) {
        std::error_code error;
        fs::resize_file(archive_path, previous_end, error);
        throw;
    }
}

void restore_archive_snapshot(
    const std::filesystem::path& archive_path,
    const std::string& snapshot_name,
    const std::filesystem::path& dest_dir,
    const ExtractOptions& options);

void create_archive_to_stream(
    std::ostream& output,
    const std::vector<std::filesystem::path>& inputs,
    const CompressionOptions& options) {
    if (!output) throw std::invalid_argument("archive output stream is not writable");
    if (options.recovery_percent != 0) {
        throw std::invalid_argument(
            "stream archive output does not support recovery records; write a file archive first");
    }
    if (options.enable_content_dedup) {
        throw std::invalid_argument(
            "deduplicated AXAR output requires a seekable file archive");
    }

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

    const auto block_size = effective_solid_block_size(options);
    validate_large_solid_block_options(options, block_size);

    ArchiveMeta meta;
    meta.large_solid_blocks = requests_large_solid_blocks(block_size);
    core::CryptoKey key{};
    struct KeyWipeGuard {
        core::CryptoKey& key;
        ~KeyWipeGuard() { core::secure_wipe(key); }
    } key_wipe{key};
    const core::CryptoKey* key_ptr = nullptr;
    const bool encrypt_dir = !options.password.empty() && options.encrypt_header;
    if (!options.password.empty()) {
        auto [enc, derived] = options.use_encryption_v2
            ? make_encryption_v2(options.password)
            : make_encryption(options.password);
        enc.encrypt_directory = encrypt_dir;
        meta.encryption = std::move(enc);
        key = derived;
        key_ptr = &key;
    }

    // The feature bits are reserved up front because a non-seekable stream cannot
    // patch them after metadata capture. Unused capability bits are harmless: the
    // directory still carries the authoritative per-entry records.
    constexpr std::uint16_t stream_feature_flags =
        kFlagSparseEntries | kFlagCaptureReport | kFlagExtendedMetadata;
    const auto header = archive_header_bytes(
        kArchiveVersion5,
        static_cast<std::uint16_t>(archive_header_flags({}, meta) |
                                    stream_feature_flags |
                                    (meta.large_solid_blocks
                                         ? kFlagLargeSolidBlocks : 0)));
    CountedOutput sink(output);
    std::uint64_t written = 0;
    sink.write(reinterpret_cast<const char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    written += header.size();
    if (!sink) throw std::runtime_error("failed to write archive stream header");

    if (encrypt_dir) {
        const auto preamble = serialize_encryption_preamble(meta.encryption);
        sink.write(reinterpret_cast<const char*>(preamble.data()),
                   static_cast<std::streamsize>(preamble.size()));
        written += preamble.size();
        if (!sink) throw std::runtime_error("failed to write archive stream preamble");
    }

    std::vector<BlockRec> blocks;
    std::vector<EntryRec> entries;
    compress_items_into(sink, written, blocks, entries, items,
                        options, block_size, operation,
                        total_bytes, total_items, completed_bytes, completed_items,
                        options.skip_unreadable_files, key_ptr, &meta);

    report_operation(operation, OperationStage::finalizing, completed_bytes, total_bytes,
                     completed_items, total_items, {}, 0, 0, completed_bytes,
                     written, completed_bytes);
    const core::CryptoKey* directory_key = encrypt_dir ? key_ptr : nullptr;
    write_directory_and_footer(sink, written, blocks, entries, meta, directory_key);
    if (!sink) throw std::runtime_error("failed to finalize archive stream");
    const auto final_size = sink.position();
    report_operation(operation, OperationStage::finalizing, total_bytes, total_bytes,
                     total_items, total_items, {}, 0, 0, total_bytes,
                     final_size, total_bytes);
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
        existing = load_index(source, options.password).index;
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
    ArchiveIndex index;
    {
        std::uint64_t file_size = 0;
        auto input = open_archive(archive_path, file_size);
        const ByteSource source(input, file_size);
        index = load_index(source, options.password).index;
    }
    if (index.meta.live_dedup) {
        std::erase_if(index.entries, [&](const EntryRec& entry) {
            return is_deleted(entry.path);
        });
        std::unordered_set<std::string> kept_paths;
        kept_paths.reserve(index.entries.size());
        for (const auto& entry : index.entries) kept_paths.insert(entry.path);
        std::erase_if(index.entries, [&kept_paths](const EntryRec& entry) {
            return entry.type == kEntryHardlink &&
                   !kept_paths.contains(entry.link_target);
        });
        rewrite_archive_directory(archive_path, std::move(index), options);
        return;
    }
    rebuild_archive_keeping(
        archive_path, [&](const EntryRec& entry) { return !is_deleted(entry.path); }, options);
}

namespace {

void repack_snapshot_archive(const std::filesystem::path& archive_path,
                             const CompressionOptions& options) {
    reject_volume_mutation(archive_path);
    std::uint64_t physical_size = 0;
    auto input = open_archive(archive_path, physical_size);
    const ByteSource source(input, physical_size);
    const auto layout = read_layout(source);
    const auto recovery = read_recovery_service(source, layout);
    auto loaded = load_index(source, options.password);
    OptionalKeyWipeGuard key_wipe{loaded.key};
    require_chunk_profile(loaded.index);
    if (loaded.index.meta.locked) throw std::runtime_error("archive is locked (read-only)");
    if (loaded.index.meta.encryption.enabled && !loaded.key) {
        throw std::runtime_error("encrypted chunk-addressed archive requires a password to repack");
    }

    try {
        std::vector<bool> live_blocks(loaded.index.blocks.size(), false);
        const auto mark_entries = [&](const std::vector<EntryRec>& entries) {
            for (const auto& entry : entries) {
                for (const auto chunk_index : entry.chunk_refs) {
                    if (chunk_index >= loaded.index.chunks.size()) {
                        throw FormatError("snapshot entry references an unknown chunk");
                    }
                    const auto block_index = loaded.index.chunks[
                        static_cast<std::size_t>(chunk_index)].block_index;
                    if (block_index >= live_blocks.size()) {
                        throw FormatError("snapshot chunk references an unknown block");
                    }
                    live_blocks[static_cast<std::size_t>(block_index)] = true;
                }
            }
        };
        mark_entries(loaded.index.entries);
        for (const auto& snapshot_record : loaded.index.meta.snapshots) {
            mark_entries(snapshot_record.entries);
        }

        std::vector<std::uint64_t> block_remap(
            loaded.index.blocks.size(), std::numeric_limits<std::uint64_t>::max());
        std::vector<std::uint64_t> chunk_remap(
            loaded.index.chunks.size(), std::numeric_limits<std::uint64_t>::max());
        std::vector<BlockRec> new_blocks;
        std::vector<ChunkRec> new_chunks;
        new_blocks.reserve(loaded.index.blocks.size());
        new_chunks.reserve(loaded.index.chunks.size());

        fs::path temp_path = archive_path;
        temp_path += ".tmp";
        TempFileGuard temp_guard(temp_path);
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot create archive: " +
                                     core::path_to_utf8(temp_path));
        }

        auto meta = loaded.index.meta;
        meta.has_signature = false;
        const auto header = archive_header_bytes(
            archive_header_version(loaded.index.entries, meta),
            archive_header_flags(loaded.index.entries, meta));
        out.write(reinterpret_cast<const char*>(header.data()),
                  static_cast<std::streamsize>(header.size()));
        std::uint64_t written = header.size();
        if (meta.encryption.encrypt_directory) {
            const auto preamble = serialize_encryption_preamble(meta.encryption);
            out.write(reinterpret_cast<const char*>(preamble.data()),
                      static_cast<std::streamsize>(preamble.size()));
            written += preamble.size();
        }
        if (!out) throw std::runtime_error("failed to write snapshot repack header");

        report_operation(options.operation, OperationStage::copying, 0,
                         archive_block_region_end(layout, loaded.index), 0,
                         loaded.index.blocks.size(), "Reclaiming snapshot chunks");
        for (std::uint64_t old_index = 0; old_index < loaded.index.blocks.size(); ++old_index) {
            operation_checkpoint(options.operation);
            if (!live_blocks[static_cast<std::size_t>(old_index)]) continue;
            const auto& old_block = loaded.index.blocks[static_cast<std::size_t>(old_index)];
            auto encoded = source.read_compressed(old_block.compressed_offset,
                                                   old_block.compressed_size);
            if (loaded.key) {
                ByteVector plaintext;
                if (!core::aead_open(*loaded.key, encoded,
                                     block_associated_data(old_index), plaintext)) {
                    throw FormatError("snapshot block authentication failed during repack");
                }
                encoded = core::aead_seal(
                    *loaded.key, plaintext,
                    block_associated_data(static_cast<std::uint64_t>(new_blocks.size())));
            }
            const auto new_index = static_cast<std::uint64_t>(new_blocks.size());
            block_remap[static_cast<std::size_t>(old_index)] = new_index;
            out.write(reinterpret_cast<const char*>(encoded.data()),
                      static_cast<std::streamsize>(encoded.size()));
            if (!out) throw std::runtime_error("failed while writing snapshot blocks");
            new_blocks.push_back({written, static_cast<std::uint64_t>(encoded.size()),
                                  old_block.uncompressed_size, old_block.subframes});
            written += encoded.size();
            report_operation(options.operation, OperationStage::copying,
                             old_block.compressed_offset + old_block.compressed_size,
                             archive_block_region_end(layout, loaded.index),
                             old_index + 1, loaded.index.blocks.size(),
                             "Reclaiming snapshot chunks");
        }

        for (std::uint64_t old_index = 0; old_index < loaded.index.chunks.size(); ++old_index) {
            const auto& old_chunk = loaded.index.chunks[static_cast<std::size_t>(old_index)];
            if (old_chunk.block_index >= block_remap.size() ||
                block_remap[static_cast<std::size_t>(old_chunk.block_index)] ==
                    std::numeric_limits<std::uint64_t>::max()) {
                continue;
            }
            chunk_remap[static_cast<std::size_t>(old_index)] = new_chunks.size();
            auto chunk = old_chunk;
            chunk.block_index = block_remap[static_cast<std::size_t>(old_chunk.block_index)];
            new_chunks.push_back(std::move(chunk));
        }

        const auto remap_entries = [&](std::vector<EntryRec>& entries) {
            for (auto& entry : entries) {
                for (auto& chunk_index : entry.chunk_refs) {
                    if (chunk_index >= chunk_remap.size() ||
                        chunk_remap[static_cast<std::size_t>(chunk_index)] ==
                            std::numeric_limits<std::uint64_t>::max()) {
                        throw FormatError("snapshot entry references a reclaimed chunk");
                    }
                    chunk_index = chunk_remap[static_cast<std::size_t>(chunk_index)];
                }
                if (!entry.chunk_refs.empty()) {
                    const auto& first = new_chunks[
                        static_cast<std::size_t>(entry.chunk_refs.front())];
                    entry.first_block = first.block_index;
                    entry.offset = first.offset;
                } else {
                    entry.first_block = 0;
                    entry.offset = 0;
                }
            }
        };
        remap_entries(loaded.index.entries);
        for (auto& snapshot_record : meta.snapshots) {
            remap_entries(snapshot_record.entries);
        }

        patch_archive_header(out,
                             archive_header_version(loaded.index.entries, meta),
                             archive_header_flags(loaded.index.entries, meta));
        const core::CryptoKey* directory_key =
            meta.encryption.encrypt_directory && loaded.key ? &*loaded.key : nullptr;
        write_directory_and_footer(out, written, new_blocks, loaded.index.entries, meta,
                                   directory_key, true, &new_chunks);
        out.close();
        input.close();

        const unsigned recovery_percent = options.recovery_percent != 0
            ? options.recovery_percent : (recovery ? recovery->percent : 0);
        if (recovery_percent != 0) {
            append_recovery_to_staged_archive(
                temp_path, recovery_percent, options.operation, options.thread_count);
        }
        replace_archive_file(temp_path, archive_path);
        temp_guard.dismiss();
    } catch (...) {
        throw;
    }
}

}  // namespace

void repack_archive(const std::filesystem::path& archive_path,
                    const CompressionOptions& options) {
    std::uint64_t file_size = 0;
    auto input = open_archive(archive_path, file_size);
    const ByteSource source(input, file_size);
    const auto index = load_index(source, options.password).index;
    input.close();
    if (index.meta.chunk_table) {
        repack_snapshot_archive(archive_path, options);
        return;
    }
    rebuild_archive_keeping(archive_path, [](const EntryRec&) { return true; }, options);
}

namespace {

struct LoadedMutationState {
    ArchiveIndex index;
    unsigned recovery_percent = 0;
};

LoadedMutationState load_mutation_state(const fs::path& archive_path,
                                        const std::string& password) {
    std::uint64_t file_size = 0;
    auto input = open_archive(archive_path, file_size);
    const ByteSource source(input, file_size);
    const auto layout = read_layout(source);
    LoadedMutationState state;
    if (const auto recovery = read_recovery_service(source, layout)) {
        state.recovery_percent = recovery->percent;
    }
    state.index = load_index(source, password).index;
    return state;
}

std::vector<ScanItem> select_update_items(
    const std::vector<ScanItem>& items, const ArchiveIndex& existing,
    bool fresh_only, const std::shared_ptr<OperationControl>& operation) {
    if (operation) operation->set_progress_phase(1, 6);
    std::unordered_map<std::string, std::int64_t> existing_mtime;
    existing_mtime.reserve(existing.entries.size());
    for (const auto& entry : existing.entries) {
        existing_mtime.emplace(entry.path, entry.mtime);
    }

    std::vector<ScanItem> selected;
    selected.reserve(items.size());
    std::error_code ec;
    std::uint64_t compared = 0;
    report_operation(operation, OperationStage::comparing, 0, items.size(),
                     0, items.size(), "Comparing source with archive");
    for (const auto& item : items) {
        operation_checkpoint(operation);
        const auto found = existing_mtime.find(item.archive_path);
        const bool in_archive = found != existing_mtime.end();
        if (item.is_directory || item.is_symlink) {
            if (!in_archive && !fresh_only) selected.push_back(item);
        } else {
            ec.clear();
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
                if (disk_mtime > found->second) selected.push_back(item);
            } else if (!fresh_only) {
                selected.push_back(item);
            }
        }
        ++compared;
        report_operation(operation, OperationStage::comparing, compared, items.size(),
                         compared, items.size(), item.archive_path);
    }
    return selected;
}

void update_archive_items(const std::vector<ScanItem>& items,
                          const std::filesystem::path& archive_path,
                          const CompressionOptions& options,
                          bool fresh_only, bool validate_mapping) {
    auto state = load_mutation_state(archive_path, options.password);
    if (validate_mapping) {
        validate_mapped_items(items, state.index, options.operation);
    }
    auto selected = select_update_items(
        items, state.index, fresh_only, options.operation);
    if (selected.empty()) {
        if (options.recovery_percent != 0 &&
            options.recovery_percent != state.recovery_percent) {
            if (options.operation) options.operation->set_progress_phase(4, 6);
            rewrite_recovery_service(
                archive_path, options.recovery_percent, options.operation,
                options.io_buffer_size);
        }
        if (options.operation) options.operation->set_progress_phase(5, 6);
        report_operation(options.operation, OperationStage::committing,
                         1, 1, 1, 1, "Archive already up to date");
        return;
    }
    append_items_to_archive_indexed(
        archive_path, selected, options, std::move(state.index),
        state.recovery_percent, nullptr, nullptr, true);
}

void sync_archive_items(const std::vector<ScanItem>& items,
                        const std::filesystem::path& archive_path,
                        const CompressionOptions& options,
                        bool validate_mapping,
                        const ArchiveSyncFinalization& finalization) {
    auto state = load_mutation_state(archive_path, options.password);
    if (validate_mapping) {
        validate_mapped_items(items, state.index, options.operation);
    }
    auto selected = select_update_items(
        items, state.index, /*fresh_only=*/false, options.operation);

    std::unordered_set<std::string> wanted;
    wanted.reserve(items.size());
    for (const auto& item : items) {
        wanted.insert(item.archive_path);
    }

    std::unordered_set<std::string> existing_paths;
    existing_paths.reserve(state.index.entries.size());
    for (const auto& entry : state.index.entries) {
        existing_paths.insert(entry.path);
    }
    const auto added_count = static_cast<std::size_t>(std::count_if(
        selected.begin(), selected.end(), [&existing_paths](const ScanItem& item) {
            return existing_paths.find(item.archive_path) == existing_paths.end();
        }));
    const auto updated_count = selected.size() - added_count;
    const auto stale_count = static_cast<std::size_t>(std::count_if(
        state.index.entries.begin(), state.index.entries.end(),
        [&wanted](const EntryRec& entry) {
            return wanted.find(entry.path) == wanted.end();
        }));
    const std::size_t unchanged_count =
        items.size() > selected.size() ? items.size() - selected.size() : 0;
    if (options.operation) {
        options.operation->set_sync_plan(
            added_count, updated_count, stale_count, unchanged_count);
    }
    report_operation(
        options.operation, OperationStage::comparing, items.size(), items.size(),
        items.size(), items.size(),
        "Plan: add " + std::to_string(added_count) +
            ", update " + std::to_string(updated_count) +
            ", remove " + std::to_string(stale_count) +
            ", unchanged " + std::to_string(unchanged_count));
    const bool metadata_change =
        (finalization.comment &&
         *finalization.comment != state.index.meta.comment) ||
        (finalization.lock_archive && !state.index.meta.locked) ||
        finalization.signing_key != nullptr;
    if (selected.empty() && stale_count == 0 && !metadata_change) {
        if (options.recovery_percent != 0 &&
            options.recovery_percent != state.recovery_percent) {
            if (options.operation) options.operation->set_progress_phase(4, 6);
            rewrite_recovery_service(
                archive_path, options.recovery_percent, options.operation,
                options.io_buffer_size);
        }
        if (options.operation) options.operation->set_progress_phase(5, 6);
        report_operation(options.operation, OperationStage::committing,
                         1, 1, 1, 1, "Archive already synchronized");
        return;
    }

    append_items_to_archive_indexed(
        archive_path, selected, options, std::move(state.index),
        state.recovery_percent, nullptr, &wanted, true, &finalization);
}

}  // namespace

void update_archive(const std::vector<std::filesystem::path>& inputs,
                    const std::filesystem::path& archive_path, const CompressionOptions& options,
                    bool fresh_only) {
    const auto operation = options.operation;
    if (operation) operation->set_progress_phase(0, 6);
    report_operation(operation, OperationStage::scanning, 0, 0, 0, inputs.size());

    std::vector<ScanItem> items;
    for (const auto& input : inputs) {
        operation_checkpoint(operation);
        scan_input(input, items);
    }

    if (!fs::exists(archive_path)) {
        // Nothing to refresh against. `update` seeds a new archive; `fresh` is a no-op.
        if (!fresh_only) {
            if (operation) operation->set_progress_phase(0, 0);
            create_archive(inputs, archive_path, options);
        }
        return;
    }
    update_archive_items(items, archive_path, options, fresh_only, false);
}

void update_archive(const std::vector<ArchiveInput>& inputs,
                    const std::filesystem::path& archive_path,
                    const CompressionOptions& options, bool fresh_only) {
    const auto operation = options.operation;
    if (operation) operation->set_progress_phase(0, 6);
    report_operation(operation, OperationStage::scanning, 0, 0, 0, inputs.size());

    std::vector<ScanItem> items;
    for (const auto& input : inputs) {
        operation_checkpoint(operation);
        scan_input_at(input, items, operation);
    }
    if (!fs::exists(archive_path)) {
        if (fresh_only) return;
        throw std::runtime_error("path-aware update requires an existing archive");
    }
    update_archive_items(items, archive_path, options, fresh_only, true);
}

void sync_archive(const std::vector<std::filesystem::path>& inputs,
                  const std::filesystem::path& archive_path,
                  const CompressionOptions& options,
                  const ArchiveSyncFinalization& finalization) {
    const auto operation = options.operation;
    if (operation) operation->set_progress_phase(0, 6);
    report_operation(operation, OperationStage::scanning, 0, 0, 0, inputs.size());
    std::vector<ScanItem> items;
    for (const auto& input : inputs) {
        operation_checkpoint(operation);
        scan_input(input, items);
    }
    if (!fs::exists(archive_path)) {
        if (operation) operation->set_progress_phase(0, 0);
        create_archive(inputs, archive_path, options);
        return;
    }
    sync_archive_items(items, archive_path, options, false, finalization);
}

void sync_archive(const std::vector<ArchiveInput>& inputs,
                  const std::filesystem::path& archive_path,
                  const CompressionOptions& options,
                  const ArchiveSyncFinalization& finalization) {
    const auto operation = options.operation;
    if (operation) operation->set_progress_phase(0, 6);
    report_operation(operation, OperationStage::scanning, 0, 0, 0, inputs.size());
    std::vector<ScanItem> items;
    for (const auto& input : inputs) {
        operation_checkpoint(operation);
        scan_input_at(input, items, operation);
    }
    if (!fs::exists(archive_path)) {
        throw std::runtime_error("path-aware synchronization requires an existing archive");
    }
    sync_archive_items(items, archive_path, options, true, finalization);
}

void finalize_archive_metadata(
    const std::filesystem::path& archive_path,
    const ArchiveSyncFinalization& finalization,
    const CompressionOptions& options) {
    auto state = load_mutation_state(archive_path, options.password);
    const bool changed =
        (finalization.comment &&
         *finalization.comment != state.index.meta.comment) ||
        (finalization.lock_archive && !state.index.meta.locked) ||
        finalization.signing_key != nullptr;
    if (!changed) return;
    append_items_to_archive_indexed(
        archive_path, {}, options, std::move(state.index),
        state.recovery_percent, nullptr, nullptr, false, &finalization);
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
        index = load_index(source, options.password).index;
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
        meta = load_index(source, options.password).index.meta;
    }
    if (meta.locked) {
        throw std::runtime_error("archive is locked (read-only)");
    }
    if (meta.comment == comment) return;
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
        meta = load_index(source, options.password).index.meta;  // preserve any existing comment
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

ArchiveCaptureReport archive_capture_report(const std::filesystem::path& archive_path,
                                            const std::string& password) {
    std::uint64_t file_size = 0;
    auto in = open_archive(archive_path, file_size);
    const ByteSource source(in, file_size);
    const auto warnings = load_index(source, password).index.meta.capture_warnings;
    return ArchiveCaptureReport{warnings.empty(), warnings};
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

bool archive_is_deduplicated(const std::filesystem::path& archive_path) {
    std::uint64_t file_size = 0;
    auto in = open_archive(archive_path, file_size);
    const ByteSource source(in, file_size);
    return (read_layout(source).flags & kFlagLiveDedup) != 0;
}

bool archive_is_snapshot_repository(const std::filesystem::path& archive_path) {
    std::uint64_t file_size = 0;
    auto in = open_archive(archive_path, file_size);
    const ByteSource source(in, file_size);
    return (read_layout(source).flags & kFlagChunkTable) != 0;
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
    if (!input) {
        throw std::runtime_error("cannot open archive volume: " +
                                 core::path_to_utf8(path));
    }
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

bool compatible_volume_header(const VolumeHeader& header, const VolumeHeader& seed,
                              bool recovery, std::uint32_t index) {
    return header.recovery == recovery && header.index == index &&
           header.data_count == seed.data_count &&
           header.parity_count == seed.parity_count &&
           header.shard_size == seed.shard_size &&
           header.archive_size == seed.archive_size && header.digest == seed.digest;
}

class DirectVolumeArchiveSource final : public RandomAccessArchiveSource {
public:
    DirectVolumeArchiveSource(fs::path root, VolumeHeader seed)
        : root_(std::move(root)), seed_(std::move(seed)), streams_(seed_.data_count) {
        const std::uint64_t expected_data_count = seed_.archive_size == 0 ? 0 :
            1 + (seed_.archive_size - 1) / seed_.shard_size;
        if (expected_data_count != seed_.data_count) {
            throw FormatError("archive volume set has inconsistent size parameters");
        }
        paths_.reserve(seed_.data_count);
        for (std::uint32_t index = 0; index < seed_.data_count; ++index) {
            const fs::path path = data_volume_path(root_, index);
            std::error_code error;
            if (!fs::is_regular_file(path, error)) {
                throw FormatError("archive data volume " + std::to_string(index + 1) +
                                  " is missing; reconstruct the set with its recovery volumes");
            }
            const auto header = read_volume_header(path);
            if (!compatible_volume_header(header, seed_, false, index)) {
                throw FormatError("archive data volume " + std::to_string(index + 1) +
                                  " belongs to a different volume set");
            }
            const std::uint64_t offset = static_cast<std::uint64_t>(index) * seed_.shard_size;
            const std::uint64_t payload_size = std::min(seed_.shard_size,
                                                        seed_.archive_size - offset);
            const auto physical_size = fs::file_size(path, error);
            if (error || physical_size != kVolumeHeaderSize + payload_size) {
                throw FormatError("archive data volume " + std::to_string(index + 1) +
                                  " is truncated; reconstruct the set with its recovery volumes");
            }
            paths_.push_back(path);
        }
    }

    std::uint64_t size() const override { return seed_.archive_size; }

    ByteVector read(std::uint64_t offset, std::uint64_t length) const override {
        if (length > seed_.archive_size || offset > seed_.archive_size - length) {
            throw FormatError("archive is truncated");
        }
        ByteVector bytes(static_cast<std::size_t>(length));
        std::uint64_t remaining = length;
        std::uint64_t logical = offset;
        std::size_t output_offset = 0;
        while (remaining != 0) {
            const auto index = static_cast<std::size_t>(logical / seed_.shard_size);
            const std::uint64_t within = logical % seed_.shard_size;
            const std::uint64_t count = std::min(remaining, seed_.shard_size - within);
            auto& stream = streams_[index];
            if (!stream) {
                stream = std::make_unique<std::ifstream>(paths_[index], std::ios::binary);
                if (!*stream) {
                    throw std::runtime_error("cannot open archive data volume: " +
                                             core::path_to_utf8(paths_[index]));
                }
            }
            stream->clear();
            stream->seekg(static_cast<std::streamoff>(kVolumeHeaderSize + within),
                          std::ios::beg);
            stream->read(reinterpret_cast<char*>(bytes.data() + output_offset),
                         static_cast<std::streamsize>(count));
            if (static_cast<std::uint64_t>(stream->gcount()) != count) {
                throw FormatError("archive data volume " + std::to_string(index + 1) +
                                  " became truncated while it was being read");
            }
            logical += count;
            remaining -= count;
            output_offset += static_cast<std::size_t>(count);
        }
        return bytes;
    }

    void close() override {
        for (auto& stream : streams_) stream.reset();
    }

private:
    fs::path root_;
    VolumeHeader seed_;
    std::vector<fs::path> paths_;
    mutable std::vector<std::unique_ptr<std::ifstream>> streams_;
};

std::shared_ptr<RandomAccessArchiveSource> try_open_volume_archive_source(
    const fs::path& archive_path) {
    std::ifstream probe(archive_path, std::ios::binary);
    if (!probe) return nullptr;
    std::array<std::uint8_t, kVolumeMagic.size()> magic{};
    probe.read(reinterpret_cast<char*>(magic.data()),
               static_cast<std::streamsize>(magic.size()));
    if (probe.gcount() != static_cast<std::streamsize>(magic.size()) ||
        magic != kVolumeMagic) {
        return nullptr;
    }
    const auto seed = read_volume_header(archive_path);
    return std::make_shared<DirectVolumeArchiveSource>(volume_root(archive_path), seed);
}

core::Blake3Digest hash_file(const fs::path& path,
                             const std::shared_ptr<OperationControl>& operation = nullptr,
                             std::size_t io_buffer_size = 0) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open archive: " + core::path_to_utf8(path));
    }
    core::Blake3 hash;
    std::vector<std::uint8_t> buffer(effective_io_buffer_size(io_buffer_size));
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
    const auto layout = read_layout(source);
    const auto service = read_recovery_service(source, layout);
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
    reject_volume_mutation(archive_path);
    std::uint64_t file_size = 0;
    auto input = open_archive(archive_path, file_size);
    const ByteSource source(input, file_size);
    const auto layout = read_layout(source);
    const auto service_optional = read_recovery_service(source, layout);
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
    reject_volume_mutation(archive_path);
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
        const std::uint64_t parity_work = recovery_progress_multiply(
            static_cast<std::uint64_t>(parity_count), volume_size);
        std::vector<std::span<const std::uint8_t>> data_spans;
        std::vector<std::span<std::uint8_t>> parity_spans;
        for (const auto& shard : data) data_spans.emplace_back(shard);
        for (auto& shard : parity) parity_spans.emplace_back(shard);
        core::ReedSolomon(data_count, parity_count).encode(
            data_spans, parity_spans,
            [&](int parity_index, std::size_t parity_completed, std::size_t parity_total) {
                operation_checkpoint(operation);
                const std::uint64_t encoded_before = recovery_progress_multiply(
                    static_cast<std::uint64_t>(std::max(0, parity_index)), volume_size);
                const std::uint64_t encoded_current = std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(parity_completed), volume_size);
                const std::uint64_t completed = recovery_progress_add(
                    encoded_before, encoded_current);
                const bool parity_done = parity_completed >= parity_total;
                report_operation(operation, OperationStage::finalizing,
                                 completed, parity_work,
                                 static_cast<std::uint64_t>(
                                     data_count + parity_index + (parity_done ? 1 : 0)),
                                 static_cast<std::uint64_t>(data_count + parity_count),
                                 "Encoding recovery volumes");
            });
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
                             index + 1, data_count + parity_count,
                             core::path_to_utf8(path.filename()));
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

bool is_axiom_archive_volume(const std::filesystem::path& path) noexcept {
    try {
        (void)read_volume_header(path);
        return true;
    } catch (...) {
        return false;
    }
}

bool archive_volume_data_set_complete(const std::filesystem::path& any_volume) noexcept {
    try {
        return try_open_volume_archive_source(any_volume) != nullptr;
    } catch (...) {
        return false;
    }
}

std::filesystem::path archive_volume_primary_path(
    const std::filesystem::path& any_volume) {
    (void)read_volume_header(any_volume);
    return data_volume_path(volume_root(any_volume), 0);
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
        } catch (const OperationCancelled&) {
            throw;
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception&) {
            // I/O failures reading a shard mean this volume is unusable, which
            // recovery treats the same as a missing volume.
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
                         index + 1, data_count,
                         core::path_to_utf8(output_archive.filename()));
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
    reject_volume_mutation(archive_path);
    std::uint64_t file_size = 0;
    auto stream = open_archive(archive_path, file_size);
    const ByteSource source(stream, file_size);
    const ArchiveLayout layout = read_layout(source);
    auto loaded = load_index(source, options.password);
    if (loaded.index.meta.locked) {
        throw std::runtime_error("archive is locked (read-only)");
    }
    const auto existing_recovery = read_recovery_service(source, layout);
    stream.close();

    // Signing is a metadata mutation. Route it through the same generation
    // transaction as synchronize so a signed archive keeps its history and the
    // generation extension is included in the signed digest. Archives that need
    // a header rewrite (or are embedded in an SFX) fall back to the established
    // directory-rewrite path inside append_items_to_archive_indexed.
    ArchiveSyncFinalization finalization;
    finalization.signing_key = &key;
    append_items_to_archive_indexed(
        archive_path, {}, options, std::move(loaded.index),
        existing_recovery ? existing_recovery->percent : 0, nullptr, nullptr,
        true, &finalization);
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

void add_archive_password(const std::filesystem::path& archive_path,
                          const std::string& current_password,
                          const std::string& new_password,
                          const CompressionOptions& options) {
    rewrite_archive_password(archive_path, current_password, new_password,
                             PasswordMutation::add, options);
}

void remove_archive_password(const std::filesystem::path& archive_path,
                             const std::string& current_password,
                             const std::string& password_to_remove,
                             const CompressionOptions& options) {
    rewrite_archive_password(archive_path, current_password, password_to_remove,
                             PasswordMutation::remove, options);
}

void change_archive_password(const std::filesystem::path& archive_path,
                             const std::string& current_password,
                             const std::string& new_password,
                             const CompressionOptions& options) {
    rewrite_archive_password(archive_path, current_password, new_password,
                             PasswordMutation::change, options);
}

void create_sfx_archive(const std::filesystem::path& archive_path,
                        std::span<const std::uint8_t> stub_image,
                        const std::filesystem::path& output_executable,
                        const std::shared_ptr<OperationControl>& operation,
                        std::size_t io_buffer_size,
                        std::span<const std::uint8_t> config) {
    const auto normalized_output = fs::absolute(output_executable).lexically_normal();
    const auto normalized_archive = fs::absolute(archive_path).lexically_normal();
    if (normalized_archive == normalized_output) {
        throw std::invalid_argument("SFX output must differ from its archive");
    }
    if (stub_image.empty()) throw std::invalid_argument("SFX stub image is empty");
    if (config.size() > kSfxMaxConfigSize) {
        throw std::invalid_argument("SFX configuration is too large");
    }
    std::error_code error;
    if (!fs::is_regular_file(archive_path, error)) {
        throw std::invalid_argument("SFX archive input is not a regular file");
    }
    const std::uint64_t archive_size = fs::file_size(archive_path, error);
    if (error) throw std::runtime_error("cannot read SFX archive: " + error.message());
    if (archive_size == 0) {
        throw std::invalid_argument("SFX archive input is empty");
    }
    if (fs::exists(output_executable, error) &&
        fs::equivalent(archive_path, output_executable, error)) {
        throw std::invalid_argument("SFX output must not alias its archive");
    }
    if (error) throw std::runtime_error("cannot inspect SFX output: " + error.message());

    // The v2 payload is anchored to the end of the PE image, not to the end of
    // the file, so that an Authenticode certificate can be appended afterwards
    // without displacing it. That requires knowing where the image ends.
    std::uint64_t image_end = 0;
    {
        std::string stub_bytes(reinterpret_cast<const char*>(stub_image.data()),
                               stub_image.size());
        std::istringstream stub_stream(std::move(stub_bytes),
                                       std::ios::in | std::ios::binary);
        const auto computed = pe_image_end(stub_stream);
        if (!computed) {
            throw std::invalid_argument("SFX stub is not a valid PE image");
        }
        if (pe_certificate_table(stub_stream)) {
            // Appending would invalidate the stub's signature, and dropping it
            // silently would be worse. Signing belongs on the finished output.
            throw std::invalid_argument(
                "SFX stub is already signed; sign the generated executable "
                "instead of the stub");
        }
        image_end = *computed;
    }
    if (image_end > stub_image.size()) {
        throw std::invalid_argument("SFX stub image is truncated");
    }
    if (image_end != stub_image.size()) {
        throw std::invalid_argument(
            "SFX stub contains trailing data; provide the PE image without an overlay");
    }

    fs::path temporary = core::unique_sibling_path(output_executable, L"sfx");
    TempFileGuard guard(temporary);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create SFX output");

    std::uint64_t completed = 0;
    if (archive_size > std::numeric_limits<std::uint64_t>::max() - image_end) {
        throw std::invalid_argument("SFX output size overflows the file range");
    }
    const std::uint64_t total = image_end + archive_size;
    const std::size_t io_chunk = effective_io_buffer_size(io_buffer_size);
    // Any bytes past image_end are not part of the PE and are not carried over.
    for (std::uint64_t offset = 0; offset < image_end;) {
        operation_checkpoint(operation);
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(io_chunk, image_end - offset));
        output.write(
            reinterpret_cast<const char*>(stub_image.data() + offset),
            static_cast<std::streamsize>(count));
        if (!output) throw std::runtime_error("failed while writing SFX output");
        offset += count;
        completed += count;
        report_operation(operation, OperationStage::writing, completed, total,
                         0, 2, "embedded SFX module");
    }

    SfxPayload descriptor_fields;
    if (!config.empty()) {
        descriptor_fields.config_offset = image_end + kSfxDescriptorSize;
        descriptor_fields.config_size = config.size();
    }
    descriptor_fields.payload_offset =
        image_end + kSfxDescriptorSize + config.size();
    descriptor_fields.payload_size = archive_size;

    // The descriptor precedes the payload but records its hash, so a
    // placeholder goes down first and is rewritten once the payload is known.
    // That keeps this to a single pass over the archive.
    const auto placeholder = sfx_encode_descriptor(image_end, descriptor_fields);
    output.write(reinterpret_cast<const char*>(placeholder.data()),
                 static_cast<std::streamsize>(placeholder.size()));
    if (!config.empty()) {
        output.write(reinterpret_cast<const char*>(config.data()),
                     static_cast<std::streamsize>(config.size()));
    }
    if (!output) throw std::runtime_error("failed while writing SFX output");

    core::Blake3 payload_hasher;
    std::uint64_t copied_payload = 0;
    auto copy = [&](const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot read SFX input: " +
                                     core::path_to_utf8(path));
        }
        std::vector<char> chunk(io_chunk);
        while (input) {
            operation_checkpoint(operation);
            input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto count = input.gcount();
            if (count <= 0) break;
            const auto count_u64 = static_cast<std::uint64_t>(count);
            if (copied_payload > archive_size ||
                count_u64 > archive_size - copied_payload) {
                throw std::runtime_error(
                    "SFX archive changed while it was being packaged");
            }
            output.write(chunk.data(), count);
            if (!output) throw std::runtime_error("failed while writing SFX output");
            payload_hasher.update(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(chunk.data()),
                static_cast<std::size_t>(count)));
            copied_payload += count_u64;
            completed += count_u64;
            report_operation(operation, OperationStage::writing, completed, total, 0, 2,
                             core::path_to_utf8(path.filename()));
        }
        if (input.bad() || copied_payload != archive_size) {
            throw std::runtime_error("SFX archive changed while it was being packaged");
        }
    };
    copy(archive_path);

    const auto digest = payload_hasher.finalize();
    std::copy(digest.begin(), digest.begin() + descriptor_fields.payload_hash.size(),
              descriptor_fields.payload_hash.begin());
    const auto final_descriptor =
        sfx_encode_descriptor(image_end, descriptor_fields);
    output.seekp(static_cast<std::streamoff>(image_end), std::ios::beg);
    if (!output) throw std::runtime_error("failed while writing SFX output");
    output.write(reinterpret_cast<const char*>(final_descriptor.data()),
                 static_cast<std::streamsize>(final_descriptor.size()));
    if (!output) throw std::runtime_error("failed while writing SFX output");
    output.close();
    if (!output) throw std::runtime_error("failed to finalize SFX output");
    core::replace_file(temporary, output_executable, "SFX output");
    guard.dismiss();
}

void create_sfx_archive(const std::filesystem::path& archive_path,
                        const std::filesystem::path& stub_executable,
                        const std::filesystem::path& output_executable,
                        const std::shared_ptr<OperationControl>& operation,
                        std::size_t io_buffer_size,
                        std::span<const std::uint8_t> config) {
    const auto normalized_output =
        fs::absolute(output_executable).lexically_normal();
    const auto normalized_stub = fs::absolute(stub_executable).lexically_normal();
    if (normalized_stub == normalized_output) {
        throw std::invalid_argument("SFX output must differ from its stub");
    }
    std::error_code error;
    if (fs::exists(output_executable, error) &&
        fs::equivalent(stub_executable, output_executable, error)) {
        throw std::invalid_argument("SFX output must not alias its stub");
    }
    if (error) throw std::runtime_error("cannot inspect SFX output: " + error.message());
    const std::uint64_t stub_size = fs::file_size(stub_executable, error);
    if (error) {
        throw std::runtime_error("cannot read SFX stub: " + error.message());
    }
    if (stub_size > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("SFX stub is too large");
    }
    std::ifstream input(stub_executable, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read SFX stub");
    ByteVector stub(static_cast<std::size_t>(stub_size));
    if (!stub.empty() &&
        !input.read(reinterpret_cast<char*>(stub.data()),
                    static_cast<std::streamsize>(stub.size()))) {
        throw std::runtime_error("cannot read SFX stub");
    }
    create_sfx_archive(archive_path, std::span<const std::uint8_t>(stub),
                       output_executable, operation, io_buffer_size, config);
}

std::optional<std::vector<std::uint8_t>> sfx_archive_config(
    const std::filesystem::path& sfx_executable) {
    const auto payload = sfx_locate_payload(sfx_executable);
    if (!payload || payload->config_size == 0 ||
        payload->config_size > kSfxMaxConfigSize ||
        payload->config_offset >
            static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return std::nullopt;
    }
    std::ifstream stream(sfx_executable, std::ios::binary);
    if (!stream) return std::nullopt;
    stream.seekg(static_cast<std::streamoff>(payload->config_offset), std::ios::beg);
    std::vector<std::uint8_t> blob(static_cast<std::size_t>(payload->config_size));
    stream.read(reinterpret_cast<char*>(blob.data()),
                static_cast<std::streamsize>(blob.size()));
    if (stream.gcount() != static_cast<std::streamsize>(blob.size())) {
        return std::nullopt;
    }
    return blob;
}

std::optional<std::uint64_t> estimate_solid_entry_packed_size(
    const EntryRec& entry,
    const std::vector<BlockRec>& blocks);

void append_entry_subframe_ranges(const EntryRec& entry,
                                  const std::vector<BlockRec>& blocks,
                                  ArchiveEntry& output) {
    // Snapshot files are assembled from independently compressed chunks. Their
    // chunk table already supplies the seek unit; the solid-block subframe map
    // describes neither their logical file order nor their reusable storage.
    if (entry.type != kEntryFile || entry.size == 0 || !entry.chunk_refs.empty()) return;
    std::uint64_t remaining = entry.size;
    std::uint64_t block_index = entry.first_block;
    std::uint64_t within = entry.offset;
    std::uint64_t entry_offset = 0;
    while (remaining > 0) {
        if (block_index >= blocks.size()) {
            throw FormatError("file extends past the last block");
        }
        const auto& block = blocks[static_cast<std::size_t>(block_index)];
        if (within > block.uncompressed_size) {
            throw FormatError("file offset lies outside its block");
        }
        const auto block_take = std::min(remaining, block.uncompressed_size - within);
        if (!block.subframes.empty()) {
            const auto block_end = within + block_take;
            for (const auto& frame : block.subframes) {
                const auto frame_end = frame.uncompressed_offset + frame.uncompressed_size;
                const auto begin = std::max(within, frame.uncompressed_offset);
                const auto end = std::min(block_end, frame_end);
                if (begin >= end) continue;
                if (block.compressed_offset >
                    std::numeric_limits<std::uint64_t>::max() - frame.compressed_offset) {
                    throw FormatError("subframe archive offset overflows");
                }
                output.subframes.push_back({
                    entry_offset + (begin - within), end - begin,
                    block.compressed_offset + frame.compressed_offset,
                    frame.compressed_size});
            }
        }
        remaining -= block_take;
        entry_offset += block_take;
        within = 0;
        ++block_index;
    }
}

namespace {

// Turn an already-read directory into the public entry shape. Shared by
// list_archive() and read_axar_directory_state() so opening an archive for
// browsing does not read the directory twice.
std::vector<ArchiveEntry> entries_from_index(const ArchiveIndex& index) {
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
        if (entry.type == kEntryFile) {
            if (!entry.chunk_refs.empty()) {
                std::uint64_t packed = 0;
                for (const auto ref : entry.chunk_refs) {
                    if (ref >= index.chunks.size()) {
                        throw FormatError("snapshot entry points outside the chunk table");
                    }
                    const auto block_index = index.chunks[static_cast<std::size_t>(ref)].block_index;
                    if (block_index >= index.blocks.size() ||
                        packed > std::numeric_limits<std::uint64_t>::max() -
                                     index.blocks[static_cast<std::size_t>(block_index)].compressed_size) {
                        throw FormatError("snapshot packed size overflows");
                    }
                    packed += index.blocks[static_cast<std::size_t>(block_index)].compressed_size;
                }
                out.packed_size = packed;
                out.packed_size_estimated = false;
            } else {
                out.packed_size = estimate_solid_entry_packed_size(entry, index.blocks);
                out.packed_size_estimated = entry.size != 0;
            }
        }
        out.mtime = entry.mtime;
        out.crc32 = entry.crc;
        out.has_crc32 = entry.type == kEntryFile;
        out.has_blake3 = entry.has_blake3;
        out.blake3 = entry.blake3;
        out.chunk_count = entry.chunk_refs.size();
        out.is_sparse = entry.sparse.is_sparse;
        out.sparse_extents.reserve(entry.sparse.allocated.size());
        for (const auto& extent : entry.sparse.allocated) {
            out.sparse_extents.push_back({extent.offset, extent.length});
        }
        out.has_security_descriptor = entry.meta.has_windows_security_descriptor;
        out.security_descriptor = entry.meta.windows_security_descriptor;
        out.xattrs.reserve(entry.meta.xattrs.size());
        for (const auto& xattr : entry.meta.xattrs) {
            out.xattrs.push_back({xattr.name, xattr.data});
        }
        out.has_reparse_data = entry.meta.has_reparse_data;
        out.reparse_tag = entry.meta.reparse_tag;
        out.reparse_data = entry.meta.reparse_data;
        append_entry_subframe_ranges(entry, index.blocks, out);
        result.push_back(std::move(out));
    }
    return result;
}

}  // namespace

std::vector<ArchiveEntry> list_archive(const std::filesystem::path& archive_path,
                                       const std::string& password) {
    std::uint64_t file_size = 0;
    auto stream = open_archive(archive_path, file_size);
    const ByteSource source(stream, file_size);
    return entries_from_index(load_index(source, password).index);
}

AxarDirectoryState read_axar_directory_state(const std::filesystem::path& archive_path,
                                             const std::string& password) {
    std::uint64_t file_size = 0;
    auto stream = open_archive(archive_path, file_size);
    const ByteSource source(stream, file_size);
    // The header answers the profile questions; everything else comes from the
    // one directory read, which is the expensive part.
    const auto layout = read_layout(source);
    const auto loaded = load_index(source, password);
    const auto& index = loaded.index;

    AxarDirectoryState state;
    state.snapshot_repository = (layout.flags & kFlagChunkTable) != 0;
    state.deduplicated = (layout.flags & kFlagLiveDedup) != 0;
    state.directory_encrypted = (layout.flags & kFlagEncryptedDirectory) != 0;
    state.encrypted = state.directory_encrypted || index.meta.encryption.enabled;
    state.locked = index.meta.locked;
    state.entries = entries_from_index(index);
    return state;
}

std::optional<std::uint64_t> estimate_solid_entry_packed_size(
    const EntryRec& entry,
    const std::vector<BlockRec>& blocks) {
    if (entry.type != kEntryFile) {
        return std::nullopt;
    }
    if (entry.size == 0) {
        return std::uint64_t{0};
    }

    std::uint64_t remaining = entry.size;
    std::uint64_t block_index = entry.first_block;
    std::uint64_t offset = entry.offset;
    long double estimated = 0.0L;
    while (remaining > 0) {
        if (block_index >= blocks.size()) {
            throw FormatError("file extends past the last block");
        }
        const auto& block = blocks[static_cast<std::size_t>(block_index)];
        if (offset >= block.uncompressed_size || block.uncompressed_size == 0) {
            throw FormatError("file offset lies outside its block");
        }
        const std::uint64_t available = block.uncompressed_size - offset;
        const std::uint64_t take = std::min(remaining, available);
        estimated +=
            (static_cast<long double>(take) *
             static_cast<long double>(block.compressed_size)) /
            static_cast<long double>(block.uncompressed_size);
        if (estimated > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        remaining -= take;
        offset = 0;
        ++block_index;
    }

    auto rounded = static_cast<std::uint64_t>(estimated + 0.5L);
    if (rounded == 0) {
        rounded = 1;
    }
    return rounded;
}

void test_archive(const std::filesystem::path& archive_path,
                  const DecompressionOptions& options) {
    const auto operation = options.operation;
    std::uint64_t file_size = 0;
    auto stream = open_archive(archive_path, file_size);
    const auto read_stats = std::make_shared<ArchiveReadStats>();
    const ByteSource bytes(stream, file_size, read_stats);
    auto loaded = load_index(bytes, options.password);
    OptionalKeyWipeGuard key_wipe{loaded.key};
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
    const auto total_bytes = archive_file_bytes(index);
    const auto total_items = static_cast<std::uint64_t>(index.entries.size());
    std::uint64_t completed_bytes = 0;
    std::uint64_t completed_items = 0;
    report_operation(operation, OperationStage::testing, completed_bytes, total_bytes,
                     completed_items, total_items);

    // A test checks every file hash, so the old on-demand cache serialised all
    // solid-block decodes before it could visit their file slices. For moderate
    // archives, decode each independent solid block concurrently, then retain
    // the validated bytes long enough to run the existing per-file CRC/BLAKE3
    // checks. The cap preserves the archive reader's bounded-memory behavior
    // for large backups, which continue to use the one-block cache below.
    constexpr std::uint64_t kParallelTestDecodeLimit = std::uint64_t{512} << 20;
    auto decode_budget = options.thread_count;
    if (decode_budget == 0) {
        decode_budget = core::logical_processor_count();
    }
    if (decode_budget == 0) {
        decode_budget = 1;
    }
    const auto outer_decode_workers = std::min<std::size_t>(
        index.blocks.size(), std::min<std::size_t>(4, decode_budget));
    const bool use_parallel_test_decode =
        outer_decode_workers > 1 && total_bytes <= kParallelTestDecodeLimit;

    auto verify_entries = [&](auto&& read_entry) {
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
            std::uint64_t current_file_bytes = 0;
            read_entry(entry, [&](std::span<const std::uint8_t> file_bytes) {
                crc = core::crc32_update(crc, file_bytes);
                hasher.update(file_bytes);
                completed_bytes += file_bytes.size();
                current_file_bytes += file_bytes.size();
                report_operation(operation, OperationStage::testing,
                                 completed_bytes, total_bytes,
                                 completed_items, total_items, entry.path,
                                 current_file_bytes, entry.size);
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
    };

    const auto validate_snapshot_chunk = [&](const ChunkRec& chunk,
                                             std::span<const std::uint8_t> bytes) {
        auto crc = core::crc32_init();
        crc = core::crc32_update(crc, bytes);
        if (core::crc32_final(crc) != chunk.crc) {
            throw FormatError("snapshot chunk checksum mismatch");
        }
        const auto digest = chunk_digest(
            bytes, loaded.key ? &*loaded.key : nullptr, index.meta.keyed_chunk_ids);
        if (digest != chunk.identity.id) {
            throw FormatError("snapshot chunk identity mismatch");
        }
    };

    if (!use_parallel_test_decode) {
        BlockSource source(bytes, index, options.thread_count, operation, loaded.key,
                           index.meta.large_solid_blocks);
        if (index.meta.chunk_table) {
            for (std::uint64_t chunk_index = 0; chunk_index < index.chunks.size();
                 ++chunk_index) {
                operation_checkpoint(operation);
                (void)source.chunk(chunk_index);
            }
        }
        verify_entries([&](const EntryRec& entry, const auto& sink) {
            read_file_bytes(source, index.blocks.size(), entry, operation, sink,
                            options.io_buffer_size);
        });
        report_operation(operation, OperationStage::testing, completed_bytes, total_bytes,
                         completed_items, total_items, {}, 0, 0, 0, 0, 0, 0, 0,
                         read_stats->archive_bytes_read.load(std::memory_order_relaxed));
        return;
    }

    std::vector<ByteVector> decoded(index.blocks.size());
    std::atomic_size_t next_block = 0;
    std::atomic_bool failed = false;
    std::mutex exception_mutex;
    std::exception_ptr first_exception;
    const auto inner_decode_threads = std::max<std::size_t>(
        1, decode_budget / outer_decode_workers);
    auto decode_worker = [&] {
        try {
            while (!failed.load(std::memory_order_relaxed)) {
                const auto block_index = next_block.fetch_add(1, std::memory_order_relaxed);
                if (block_index >= index.blocks.size()) {
                    return;
                }
                decoded[block_index] = decode_solid_block(bytes, index, block_index,
                                                          inner_decode_threads, operation,
                                                          loaded.key);
            }
        } catch (...) {
            failed.store(true, std::memory_order_relaxed);
            std::lock_guard lock(exception_mutex);
            if (!first_exception) {
                first_exception = std::current_exception();
            }
        }
    };

    std::vector<std::thread> decode_workers;
    decode_workers.reserve(outer_decode_workers);
    for (std::size_t i = 0; i < outer_decode_workers; ++i) {
        decode_workers.emplace_back(decode_worker);
    }
    for (auto& worker : decode_workers) {
        worker.join();
    }
    if (first_exception) {
        std::rethrow_exception(first_exception);
    }

    if (index.meta.chunk_table) {
        for (const auto& chunk : index.chunks) {
            operation_checkpoint(operation);
            if (chunk.block_index >= decoded.size()) {
                throw FormatError("snapshot chunk points outside the block table");
            }
            const auto& block = decoded[static_cast<std::size_t>(chunk.block_index)];
            if (chunk.offset > block.size() ||
                chunk.identity.size > block.size() - chunk.offset) {
                throw FormatError("snapshot chunk points outside its block");
            }
            const auto chunk_bytes = std::span<const std::uint8_t>(
                block.data() + static_cast<std::size_t>(chunk.offset),
                static_cast<std::size_t>(chunk.identity.size));
            validate_snapshot_chunk(chunk, chunk_bytes);
        }
    }

    verify_entries([&](const EntryRec& entry, const auto& sink) {
        if (!entry.chunk_refs.empty()) {
            const auto io_chunk = effective_io_buffer_size(options.io_buffer_size);
            for (const auto ref : entry.chunk_refs) {
                if (ref >= index.chunks.size()) {
                    throw FormatError("snapshot entry points outside the chunk table");
                }
                const auto& chunk = index.chunks[static_cast<std::size_t>(ref)];
                if (chunk.block_index >= decoded.size()) {
                    throw FormatError("snapshot chunk points outside the block table");
                }
                const auto& block = decoded[static_cast<std::size_t>(chunk.block_index)];
                if (chunk.offset > block.size() ||
                    chunk.identity.size > block.size() - chunk.offset) {
                    throw FormatError("snapshot chunk points outside its block");
                }
                const auto chunk_bytes = std::span<const std::uint8_t>(
                    block.data() + static_cast<std::size_t>(chunk.offset),
                    static_cast<std::size_t>(chunk.identity.size));
                validate_snapshot_chunk(chunk, chunk_bytes);
                std::uint64_t offset = 0;
                while (offset < chunk.identity.size) {
                    const auto take = std::min<std::uint64_t>(
                        io_chunk, chunk.identity.size - offset);
                    sink(std::span<const std::uint8_t>(
                        block.data() + chunk.offset + offset,
                        static_cast<std::size_t>(take)));
                    offset += take;
                }
            }
            return;
        }
        std::uint64_t remaining = entry.size;
        std::uint64_t block_index = entry.first_block;
        std::uint64_t within = entry.offset;
        const auto io_chunk = effective_io_buffer_size(options.io_buffer_size);
        while (remaining > 0) {
            operation_checkpoint(operation);
            if (block_index >= decoded.size()) {
                throw FormatError("file extends past the last block");
            }
            const auto& block = decoded[block_index];
            if (within > block.size()) {
                throw FormatError("file offset lies past its block");
            }
            const auto available = static_cast<std::uint64_t>(block.size()) - within;
            const auto take = std::min<std::uint64_t>(
                std::min<std::uint64_t>(available, remaining), io_chunk);
            sink(std::span<const std::uint8_t>(block.data() + within,
                                               static_cast<std::size_t>(take)));
            remaining -= take;
            within += take;
            if (within >= block.size()) {
                within = 0;
                ++block_index;
            }
        }
    });
    report_operation(operation, OperationStage::testing, completed_bytes, total_bytes,
                     completed_items, total_items, {}, 0, 0, 0, 0, 0, 0, 0,
                     read_stats->archive_bytes_read.load(std::memory_order_relaxed));
}

namespace {

// Directory records are still parsed and validated eagerly because encrypted
// directories arrive as one authenticated image. Path lookup itself is lazy:
// ordinary full extraction does not pay for a duplicate hash table unless a
// hard-link or an explicit selection actually needs one.
class LazyPathIndex {
public:
    explicit LazyPathIndex(const std::vector<EntryRec>& entries) : entries_(entries) {}

    std::optional<std::size_t> find(const std::string& path) const {
        ensure();
        const auto found = index_->find(path);
        if (found == index_->end()) return std::nullopt;
        return found->second;
    }

private:
    void ensure() const {
        if (index_) return;
        index_.emplace();
        index_->reserve(entries_.size());
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            index_->emplace(entries_[i].path, i);
        }
    }

    const std::vector<EntryRec>& entries_;
    mutable std::optional<std::unordered_map<std::string, std::size_t>> index_;
};

void extract_entries_impl(const std::filesystem::path& archive_path,
                          const std::vector<std::string>* requested_entries,
                          const std::filesystem::path& dest_dir,
                          const ExtractOptions& options,
                          const std::string* snapshot_name = nullptr) {
    const auto operation = options.operation;
    std::uint64_t file_size = 0;
    auto stream = open_archive(archive_path, file_size);
    const auto read_stats = std::make_shared<ArchiveReadStats>();
    const ByteSource bytes(stream, file_size, read_stats);
    auto loaded = load_index(bytes, options.password);
    OptionalKeyWipeGuard key_wipe{loaded.key};
    if (snapshot_name != nullptr) {
        require_snapshot_profile(loaded.index);
        const auto& snapshot = find_snapshot(loaded.index.meta, *snapshot_name);
        loaded.index.entries = snapshot.entries;
    }
    const auto& index = loaded.index;
    if (index.meta.encryption.enabled && !loaded.key) {
        throw std::runtime_error("archive is encrypted; a password is required");
    }
    if (options.strict_metadata && !index.meta.capture_warnings.empty()) {
        throw FormatError("archive contains source-capture warnings");
    }
    BlockSource source(bytes, index, options.thread_count, operation, loaded.key,
                       requested_entries != nullptr || index.meta.large_solid_blocks);

    LazyPathIndex entry_by_path(index.entries);
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
            if (found) {
                selected[*found] = true;
                matched = true;
            }
            if (!found || index.entries[*found].type == kEntryDir) {
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
            if (!target || index.entries[*target].type != kEntryFile) {
                throw FormatError("archive contains a dangling hard link: " + entry.path);
            }
            if (!selected[*target]) {
                total_bytes += index.entries[*target].size;
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
        std::string archive_path;
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
        const fs::path target =
            (dest_dir / core::path_from_utf8(entry.path)).lexically_normal();
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
                    throw std::runtime_error("target already exists: " +
                                             core::path_to_utf8(target));
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
            for (const auto& warning : core::apply_metadata(target, entry.meta,
                                                            options.restore_mtime)) {
                if (operation) operation->add_warning({entry.path, warning});
                if (options.strict_metadata) {
                    throw std::runtime_error(warning + ": " + entry.path);
                }
            }
            ++completed_items;
            report_operation(operation, OperationStage::extracting, completed_bytes, total_bytes,
                             completed_items, total_items, entry.path);
            continue;
        }

        const EntryRec* file_entry = &entry;
        if (entry.type == kEntryHardlink) {
            const auto canonical = entry_by_path.find(entry.link_target);
            if (!canonical || index.entries[*canonical].type != kEntryFile) {
                throw FormatError("archive contains a dangling hard link: " + entry.path);
            }
            if (!selected[*canonical]) {
                // A selected hardlink whose canonical file is outside the selection
                // is materialized directly from the canonical entry. This keeps the
                // requested output self-contained without exposing unrelated paths.
                file_entry = &index.entries[*canonical];
            } else {
            // The canonical file precedes its hard links in the directory, so its
            // target already exists on disk by the time we reach this entry.
            const fs::path link_to =
                (dest_dir / core::path_from_utf8(entry.link_target)).lexically_normal();
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
                    throw std::runtime_error("target already exists: " +
                                             core::path_to_utf8(target));
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
            // All directory metadata is deferred until descendants are written;
            // restoring a restrictive ACL/read-only attribute first could make a
            // later child write fail.
            deferred_dirs.push_back({target, entry.path, entry.meta, entry.mtime});
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
                throw std::runtime_error("target already exists: " +
                                         core::path_to_utf8(target));
            }
        }

        fs::path temp_target = target;
        temp_target += ".axtmp";
        TempFileGuard temp_guard(temp_target);
        {
            std::ofstream file_out(temp_target, std::ios::binary | std::ios::trunc);
            if (!file_out) {
                throw std::runtime_error("cannot write file: " +
                                         core::path_to_utf8(temp_target));
            }
            std::uint64_t current_file_bytes = 0;
            auto file_crc = core::crc32_init();
            core::Blake3 file_hasher;
            // A solid block can span several files. While its inner blocks are
            // decoding, map their cumulative bytes onto the current file's
            // overlap so the file bar continues moving before the first output
            // write is available. The mapping is deliberately conservative and
            // is superseded by exact byte counts in the write callback below.
            auto visible_file_bytes = std::make_shared<std::atomic<std::uint64_t>>(0);
            const auto advance_file_progress = [visible_file_bytes](std::uint64_t value) {
                auto previous = visible_file_bytes->load(std::memory_order_relaxed);
                while (value > previous &&
                       !visible_file_bytes->compare_exchange_weak(
                           previous, value, std::memory_order_relaxed)) {
                }
                return std::max(value, previous);
            };
            source.set_decode_progress(
                [&, file_entry, advance_file_progress](std::uint64_t decoded_block,
                                                        std::uint64_t decoded,
                                                        std::uint64_t block_total) {
                    if (decoded_block < file_entry->first_block || block_total == 0) {
                        return;
                    }
                    std::uint64_t remaining = file_entry->size;
                    std::uint64_t within = file_entry->offset;
                    std::uint64_t block = file_entry->first_block;
                    std::uint64_t before = 0;
                    while (block < decoded_block && remaining > 0 &&
                           block < index.blocks.size()) {
                        const auto& record = index.blocks[static_cast<std::size_t>(block++)];
                        if (within > record.uncompressed_size) {
                            return;
                        }
                        const auto take = std::min(remaining, record.uncompressed_size - within);
                        before += take;
                        remaining -= take;
                        within = 0;
                    }
                    if (block != decoded_block || remaining == 0 ||
                        block >= index.blocks.size()) {
                        return;
                    }
                    const auto& record = index.blocks[static_cast<std::size_t>(block)];
                    if (within > record.uncompressed_size) {
                        return;
                    }
                    const auto overlap = std::min(remaining, record.uncompressed_size - within);
                    const auto decoded_in_file = decoded >= block_total
                        ? overlap
                        : static_cast<std::uint64_t>(
                              static_cast<long double>(decoded) * overlap / block_total);
                    const auto file_done = advance_file_progress(
                        std::min(file_entry->size, before + decoded_in_file));
                    report_operation(operation, OperationStage::extracting,
                                     completed_bytes, total_bytes,
                                     completed_items, total_items, entry.path,
                                     file_done, file_entry->size);
                });
            read_file_bytes(source, index.blocks.size(), *file_entry, operation,
                            [&](std::span<const std::uint8_t> bytes) {
                                operation_checkpoint(operation);
                                file_out.write(reinterpret_cast<const char*>(bytes.data()),
                                               static_cast<std::streamsize>(bytes.size()));
                                if (!file_out) {
                                    throw std::runtime_error(
                                        "failed writing file: " +
                                        core::path_to_utf8(temp_target));
                                }
                                file_crc = core::crc32_update(file_crc, bytes);
                                file_hasher.update(bytes);
                                completed_bytes += bytes.size();
                                current_file_bytes += bytes.size();
                                const auto file_done = advance_file_progress(current_file_bytes);
                                report_operation(operation, OperationStage::extracting,
                                                 completed_bytes, total_bytes,
                                                 completed_items, total_items, entry.path,
                                                 file_done, file_entry->size);
                            },
                            options.io_buffer_size);
            if (current_file_bytes != file_entry->size ||
                core::crc32_final(file_crc) != file_entry->crc) {
                throw FormatError("checksum mismatch for extracted file: " + entry.path);
            }
            if (file_entry->has_blake3 && file_hasher.finalize() != file_entry->blake3) {
                throw FormatError("BLAKE3 mismatch for extracted file: " + entry.path);
            }
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

        if (file_entry->sparse.is_sparse) {
            std::string sparse_error;
            if (!core::restore_sparse_file(target, file_entry->sparse,
                                           file_entry->size, &sparse_error)) {
                OperationWarning warning{
                    entry.path,
                    "Sparse allocation could not be restored: " + sparse_error,
                };
                if (operation) operation->add_warning(warning);
                if (options.strict_metadata) {
                    throw std::runtime_error(warning.message + ": " + entry.path);
                }
            }
        }

        // NTFS named streams are written before timestamps so restoring the write
        // time isn't disturbed by the stream writes that follow it.
        core::apply_ads(target, file_entry->ads);
        // High-precision Windows times (when present) supersede the seconds mtime.
        for (const auto& warning : core::apply_metadata(target, file_entry->meta,
                                                        options.restore_mtime)) {
            if (operation) operation->add_warning({entry.path, warning});
            if (options.strict_metadata) {
                throw std::runtime_error(warning + ": " + entry.path);
            }
        }
        if (file_entry->meta.has_reparse_data) {
            std::string reparse_error;
            if (!core::apply_reparse_point(target, file_entry->meta.reparse_tag,
                                           file_entry->meta.reparse_data,
                                           &reparse_error)) {
                if (operation) operation->add_warning({entry.path, reparse_error});
                if (options.strict_metadata) {
                    throw std::runtime_error(reparse_error + ": " + entry.path);
                }
            }
        }
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
        for (const auto& warning : core::apply_metadata(it->target, it->meta,
                                                        options.restore_mtime)) {
            if (operation) operation->add_warning({it->archive_path, warning});
            if (options.strict_metadata) {
                throw std::runtime_error(warning + ": " + core::path_to_utf8(it->target));
            }
        }
        if (it->meta.has_reparse_data) {
            std::string reparse_error;
            if (!core::apply_reparse_point(it->target, it->meta.reparse_tag,
                                           it->meta.reparse_data, &reparse_error)) {
                if (operation) operation->add_warning({
                    it->archive_path, reparse_error});
                if (options.strict_metadata) {
                    throw std::runtime_error(reparse_error + ": " +
                                             core::path_to_utf8(it->target));
                }
            }
        }
        if (options.restore_mtime && !it->meta.has_windows_times && it->mtime != 0) {
            try {
                fs::last_write_time(it->target, from_unix_seconds(it->mtime), ec);
            } catch (...) {
                // best effort
            }
        }
    }
    report_operation(operation, OperationStage::extracting, completed_bytes, total_bytes,
                     completed_items, total_items, {}, 0, 0, 0, 0, 0, 0, 0,
                     read_stats->archive_bytes_read.load(std::memory_order_relaxed));
}

}  // namespace

void extract_archive(const std::filesystem::path& archive_path,
                     const std::filesystem::path& dest_dir,
                     const ExtractOptions& options) {
    extract_entries_impl(archive_path, nullptr, dest_dir, options, nullptr);
}

void extract_entries(const std::filesystem::path& archive_path,
                     const std::vector<std::string>& entries,
                     const std::filesystem::path& dest_dir,
                     const ExtractOptions& options) {
    extract_entries_impl(archive_path, &entries, dest_dir, options, nullptr);
}

void restore_archive_snapshot(
    const std::filesystem::path& archive_path,
    const std::string& snapshot_name,
    const std::filesystem::path& dest_dir,
    const ExtractOptions& options) {
    validate_snapshot_name(snapshot_name);
    extract_entries_impl(archive_path, nullptr, dest_dir, options, &snapshot_name);
}


namespace detail {

void fuzz_read_archive(std::span<const std::uint8_t> bytes) {
    const ByteSource source(bytes);
    const auto index = read_index(source);
    BlockSource blocks(source, index, 0, nullptr, std::nullopt,
                       index.meta.large_solid_blocks);
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
