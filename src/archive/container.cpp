#include "axiom/archive.hpp"

#include "archive/fuzz_support.hpp"
#include "core/checksum.hpp"
#include "core/file_meta.hpp"
#include "core/hash.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace axiom {
namespace {

namespace fs = std::filesystem;

constexpr std::array<std::uint8_t, 8> kArchiveMagic = {'A', 'X', 'I', 'O', 'M', 'A', 'R', '\0'};
constexpr std::uint16_t kArchiveVersion = 4;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kFooterSize = 24;
constexpr std::uint8_t kEntryFile = 0;
constexpr std::uint8_t kEntryDir = 1;
constexpr std::uint8_t kEntrySymlink = 2;
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
};

struct ArchiveIndex {
    std::vector<BlockRec> blocks;
    std::vector<EntryRec> entries;
};

struct ScanItem {
    fs::path absolute;
    std::string archive_path;
    bool is_directory = false;
    bool is_symlink = false;
    std::string symlink_target{};  // verbatim target when is_symlink
};

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

ArchiveIndex read_index(const ByteSource& source) {
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
    if (header_reader.u16() != 0) {
        throw FormatError("archive uses features this build does not support");
    }

    const auto footer = source.read(file_size - kFooterSize, kFooterSize);
    if (!std::equal(kArchiveMagic.begin(), kArchiveMagic.end(), footer.begin() + 16)) {
        throw FormatError("invalid archive footer");
    }
    Reader footer_reader(footer);
    const auto directory_offset = footer_reader.u64();
    const auto directory_size = footer_reader.u64();
    if (directory_offset < kHeaderSize ||
        directory_offset + directory_size > file_size - kFooterSize) {
        throw FormatError("invalid directory location");
    }

    const auto directory = source.read(directory_offset, directory_size);
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
            entry.type != kEntrySymlink) {
            throw FormatError("unknown archive entry type");
        }
        entry.path = body.str(static_cast<std::size_t>(body.vint()));
        if (entry.type == kEntryFile) {
            entry.size = body.vint();
            entry.first_block = body.vint();
            entry.offset = body.vint();
        } else if (entry.type == kEntrySymlink) {
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
            }
            // Unknown extra records are intentionally skipped (consumed by length).
        }
        index.entries.push_back(std::move(entry));
    }

    // Archive-level extra records (reserved); consumed by length, ignored today.
    const auto archive_extra_count = reader.vint();
    for (std::uint64_t i = 0; i < archive_extra_count; ++i) {
        (void)reader.vint();  // record type
        (void)reader.take(static_cast<std::size_t>(reader.vint()));  // payload
    }

    return index;
}

// Decodes solid blocks on demand, caching the most recently used one so the many
// small files sharing a block are not decoded repeatedly.
class BlockSource {
public:
    BlockSource(const ByteSource& source,
                const ArchiveIndex& index,
                std::size_t thread_count,
                std::shared_ptr<OperationControl> operation)
        : source_(source),
          index_(index),
          thread_count_(thread_count),
          operation_(std::move(operation)) {}

    const ByteVector& block(std::uint64_t block_index) {
        if (block_index != cached_index_) {
            operation_checkpoint(operation_);
            if (block_index >= index_.blocks.size()) {
                throw FormatError("block index out of range");
            }
            const auto& record = index_.blocks[block_index];
            const auto compressed = source_.read(record.compressed_offset, record.compressed_size);
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

    ByteVector header;
    header.insert(header.end(), kArchiveMagic.begin(), kArchiveMagic.end());
    put_u16(header, kArchiveVersion);
    put_u16(header, 0);  // flags
    put_u32(header, 0);  // reserved
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    std::uint64_t written = header.size();

    std::vector<BlockRec> blocks;
    std::vector<EntryRec> entries;
    ByteVector buffer;
    std::uint64_t current_block = 0;

    auto flush_block = [&](std::string current_path = {}) {
        if (buffer.empty()) {
            return;
        }
        report_operation(operation, OperationStage::compressing, completed_bytes, total_bytes,
                         completed_items, total_items, current_path);
        const auto compressed = compress(buffer, options);
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
        entry.meta = core::capture_metadata(item.absolute);

        if (item.is_directory) {
            entry.type = kEntryDir;
            entries.push_back(std::move(entry));
            ++completed_items;
            report_operation(operation, OperationStage::reading, completed_bytes, total_bytes,
                             completed_items, total_items, item.archive_path);
            continue;
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
        entries.push_back(std::move(entry));
        ++completed_items;
        report_operation(operation, OperationStage::reading, completed_bytes, total_bytes,
                         completed_items, total_items, item.archive_path);
    }
    flush_block();

    report_operation(operation, OperationStage::finalizing, completed_bytes, total_bytes,
                     completed_items, total_items);
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
        if (entry.type == kEntrySymlink) {
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
        put_vint(directory, body.size());
        directory.insert(directory.end(), body.begin(), body.end());
    }

    // Archive-level extra records (TLV), reserved for service data added later —
    // comment, recovery-record parameters, encryption parameters, volume info.
    put_vint(directory, 0);  // archive extra record count

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
    report_operation(operation, OperationStage::finalizing, total_bytes, total_bytes,
                     total_items, total_items);
}

std::vector<ArchiveEntry> list_archive(const std::filesystem::path& archive_path) {
    std::uint64_t file_size = 0;
    auto stream = open_archive(archive_path, file_size);
    const ByteSource source(stream, file_size);
    const auto index = read_index(source);

    std::vector<ArchiveEntry> result;
    result.reserve(index.entries.size());
    for (const auto& entry : index.entries) {
        ArchiveEntry out;
        out.path = entry.path;
        out.is_directory = entry.type == kEntryDir;
        out.is_symlink = entry.type == kEntrySymlink;
        out.symlink_target = entry.link_target;
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
    const auto index = read_index(bytes);
    BlockSource source(bytes, index, options.thread_count, operation);

    const auto total_bytes = archive_file_bytes(index);
    const auto total_items = static_cast<std::uint64_t>(index.entries.size());
    std::uint64_t completed_bytes = 0;
    std::uint64_t completed_items = 0;
    report_operation(operation, OperationStage::testing, completed_bytes, total_bytes,
                     completed_items, total_items);

    for (const auto& entry : index.entries) {
        operation_checkpoint(operation);
        if (entry.type == kEntryDir || entry.type == kEntrySymlink) {
            // No block content to verify (a symlink's target lives in the directory).
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

void extract_archive(const std::filesystem::path& archive_path,
                     const std::filesystem::path& dest_dir,
                     const ExtractOptions& options) {
    const auto operation = options.operation;
    std::uint64_t file_size = 0;
    auto stream = open_archive(archive_path, file_size);
    const ByteSource bytes(stream, file_size);
    const auto index = read_index(bytes);
    BlockSource source(bytes, index, options.thread_count, operation);

    const auto total_bytes = archive_file_bytes(index);
    const auto total_items = static_cast<std::uint64_t>(index.entries.size());
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

    for (const auto& entry : index.entries) {
        operation_checkpoint(operation);
        if (!is_safe_relative(entry.path)) {
            throw FormatError("archive contains an unsafe path: " + entry.path);
        }
        const fs::path target = (dest_dir / fs::path(entry.path)).lexically_normal();
        if (!is_within(dest_norm, target)) {
            throw FormatError("archive path escapes the destination: " + entry.path);
        }

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

        if (entry.type == kEntryDir) {
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
                completed_bytes += entry.size;
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
            read_file_bytes(source, index.blocks.size(), entry, operation,
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

        // High-precision Windows times (when present) supersede the seconds mtime.
        core::apply_metadata(target, entry.meta, options.restore_mtime);
        if (options.restore_mtime && !entry.meta.has_windows_times && entry.mtime != 0) {
            try {
                fs::last_write_time(target, from_unix_seconds(entry.mtime), ec);
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
