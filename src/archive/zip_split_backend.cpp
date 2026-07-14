#include "archive/zip_split_backend.hpp"

#include "archive/container_internal.hpp"
#include "core/file_replace.hpp"
#include "core/path_text.hpp"
#include "third_party/minizip-ng/mz.h"
#include "third_party/minizip-ng/mz_strm.h"
#include "third_party/minizip-ng/mz_zip.h"
#include "third_party/minizip-ng/mz_zip_rw.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace axiom {
namespace {

class ReaderHandle {
public:
    ReaderHandle() : value_(mzng_zip_reader_create()) {
        if (!value_) throw std::bad_alloc();
    }
    ~ReaderHandle() {
        if (value_) mzng_zip_reader_delete(&value_);
    }
    void* get() const { return value_; }
private:
    void* value_ = nullptr;
};

class WriterHandle {
public:
    WriterHandle() : value_(mzng_zip_writer_create()) {
        if (!value_) throw std::bad_alloc();
    }
    ~WriterHandle() {
        if (value_) mzng_zip_writer_delete(&value_);
    }
    void* get() const { return value_; }
private:
    void* value_ = nullptr;
};

void check_mz(int32_t result, const char* action) {
    if (result != MZ_OK) {
        throw FormatError(std::string(action) + " (minizip-ng error " +
                          std::to_string(result) + ")");
    }
}

void raw_copy_zip(const std::filesystem::path& source,
                  const std::filesystem::path& output,
                  std::uint64_t disk_size,
                  const std::shared_ptr<OperationControl>& operation,
                  OperationStage stage) {
    if (disk_size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("ZIP volume size is too large");
    }
    ReaderHandle reader;
    WriterHandle writer;
    const auto source_text = core::path_to_utf8(source);
    const auto output_text = core::path_to_utf8(output);
    check_mz(mzng_zip_reader_open_file(reader.get(), source_text.c_str()),
             "cannot open ZIP source");
    check_mz(mzng_zip_writer_open_file(writer.get(), output_text.c_str(),
                                     static_cast<std::int64_t>(disk_size), 0),
             "cannot create ZIP output");

    int32_t result = mzng_zip_reader_goto_first_entry(reader.get());
    std::uint64_t completed = 0;
    while (result == MZ_OK) {
        operation_checkpoint(operation);
        check_mz(mzng_zip_writer_copy_from_reader(writer.get(), reader.get()),
                 "cannot copy ZIP entry");
        ++completed;
        report_operation(operation, stage, completed, 0,
                         completed, 0, "Copying ZIP entries", 0, 0);
        result = mzng_zip_reader_goto_next_entry(reader.get());
    }
    if (result != MZ_END_OF_LIST) check_mz(result, "cannot enumerate ZIP entries");
    check_mz(mzng_zip_writer_close(writer.get()), "cannot finalize ZIP output");
    check_mz(mzng_zip_reader_close(reader.get()), "cannot close ZIP source");
}

}  // namespace

void create_split_zip(const std::filesystem::path& source_zip,
                      const std::filesystem::path& output_zip,
                      std::uint64_t volume_size,
                      const std::shared_ptr<OperationControl>& operation) {
    if (volume_size == 0) throw std::invalid_argument("ZIP volume size must be non-zero");
    raw_copy_zip(source_zip, output_zip, volume_size, operation, OperationStage::writing);
}

struct SplitZipReader::Impl {
    explicit Impl(const std::filesystem::path& final_zip,
                  std::shared_ptr<OperationControl> control)
        : operation(std::move(control)) {
        const auto path = core::path_to_utf8(final_zip);
        check_mz(mzng_zip_reader_open_file(reader.get(), path.c_str()),
                 "cannot open split ZIP");
        open = true;

        int32_t status = mzng_zip_reader_goto_first_entry(reader.get());
        while (status == MZ_OK) {
            operation_checkpoint(operation);
            mzng_zip_file* file = nullptr;
            check_mz(mzng_zip_reader_entry_get_info(reader.get(), &file),
                     "cannot read split ZIP directory");
            if (file == nullptr || file->compressed_size < 0 ||
                file->uncompressed_size < 0) {
                throw FormatError("split ZIP entry has invalid sizes");
            }
            SplitZipEntryInfo entry;
            if (file->filename != nullptr) {
                entry.path.assign(file->filename, file->filename_size);
            }
            if (file->comment != nullptr) {
                entry.comment.assign(file->comment, file->comment_size);
            }
            if (file->extrafield != nullptr && file->extrafield_size != 0) {
                entry.extra.assign(file->extrafield,
                                   file->extrafield + file->extrafield_size);
            }
            entry.method = file->compression_method;
            entry.flags = file->flag;
            entry.zipcrypto_verifier = file->pk_verify;
            entry.aes_version = file->aes_version;
            entry.aes_strength = file->aes_strength;
            entry.crc32 = file->crc;
            entry.compressed_size = static_cast<std::uint64_t>(file->compressed_size);
            entry.uncompressed_size = static_cast<std::uint64_t>(file->uncompressed_size);
            entry.modified_time = static_cast<std::int64_t>(file->modified_date);
            entry.directory = mzng_zip_reader_entry_is_dir(reader.get()) == MZ_OK;
            entries.push_back(std::move(entry));
            status = mzng_zip_reader_goto_next_entry(reader.get());
        }
        if (status != MZ_END_OF_LIST) {
            check_mz(status, "cannot enumerate split ZIP entries");
        }
    }

    ~Impl() {
        if (open) mzng_zip_reader_close(reader.get());
    }

    void goto_entry(std::size_t wanted) {
        int32_t status = mzng_zip_reader_goto_first_entry(reader.get());
        std::size_t index = 0;
        while (status == MZ_OK && index < wanted) {
            status = mzng_zip_reader_goto_next_entry(reader.get());
            ++index;
        }
        if (status != MZ_OK || index != wanted) {
            throw FormatError("split ZIP entry index is invalid");
        }
    }

    ReaderHandle reader;
    std::shared_ptr<OperationControl> operation;
    std::vector<SplitZipEntryInfo> entries;
    bool open = false;
};

SplitZipReader::SplitZipReader(
    const std::filesystem::path& final_zip,
    const std::shared_ptr<OperationControl>& operation)
    : impl_(std::make_unique<Impl>(final_zip, operation)) {}

SplitZipReader::~SplitZipReader() = default;
SplitZipReader::SplitZipReader(SplitZipReader&&) noexcept = default;
SplitZipReader& SplitZipReader::operator=(SplitZipReader&&) noexcept = default;

const std::vector<SplitZipEntryInfo>& SplitZipReader::entries() const {
    return impl_->entries;
}

ByteVector SplitZipReader::read_raw_entry(std::size_t index) {
    if (index >= impl_->entries.size()) {
        throw std::out_of_range("split ZIP entry index is invalid");
    }
    const std::uint64_t expected = impl_->entries[index].compressed_size;
    if (expected > std::numeric_limits<std::size_t>::max()) {
        throw FormatError("split ZIP entry is too large for this process");
    }
    impl_->goto_entry(index);
    mzng_zip_reader_set_raw(impl_->reader.get(), 1);
    check_mz(mzng_zip_reader_entry_open(impl_->reader.get()),
             "cannot open split ZIP entry");

    ByteVector result(static_cast<std::size_t>(expected));
    std::size_t offset = 0;
    try {
        while (offset < result.size()) {
            operation_checkpoint(impl_->operation);
            const std::size_t amount = (std::min<std::size_t>)(
                result.size() - offset,
                static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()));
            const int32_t read = mzng_zip_reader_entry_read(
                impl_->reader.get(), result.data() + offset,
                static_cast<std::int32_t>(amount));
            if (read < 0) check_mz(read, "cannot read split ZIP entry");
            if (read == 0) break;
            offset += static_cast<std::size_t>(read);
        }
        check_mz(mzng_zip_reader_entry_close(impl_->reader.get()),
                 "cannot close split ZIP entry");
    } catch (...) {
        mzng_zip_reader_entry_close(impl_->reader.get());
        throw;
    }
    if (offset != result.size()) {
        throw FormatError("split ZIP entry is truncated");
    }
    return result;
}

namespace {

class TempDirectoryGuard {
public:
    explicit TempDirectoryGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~TempDirectoryGuard() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
private:
    std::filesystem::path path_;
};

bool is_split_sibling(const std::filesystem::path& candidate,
                      const std::filesystem::path& archive_path) {
    const auto stem = archive_path.stem().wstring();
    if (candidate.stem().wstring() != stem) return false;
    const auto extension = candidate.extension().wstring();
    if (extension.size() < 3 || (extension[1] != L'z' && extension[1] != L'Z')) {
        return false;
    }
    return std::all_of(extension.begin() + 2, extension.end(),
                       [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
}

std::vector<std::filesystem::path> volume_set_files(
    const std::filesystem::path& archive_path) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    if (std::filesystem::is_regular_file(archive_path, error)) {
        result.push_back(archive_path);
    }
    error.clear();
    for (const auto& item : std::filesystem::directory_iterator(
             archive_path.parent_path().empty() ? std::filesystem::path{"."} :
                                                  archive_path.parent_path(), error)) {
        if (error || !item.is_regular_file(error)) continue;
        if (is_split_sibling(item.path(), archive_path)) result.push_back(item.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

void restore_backups(
    const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>& backups) {
    std::error_code ignored;
    for (auto item = backups.rbegin(); item != backups.rend(); ++item) {
        std::filesystem::rename(item->second, item->first, ignored);
        ignored.clear();
    }
}
}  // namespace

void create_zip_volumes(const std::filesystem::path& archive_path,
                        std::uint64_t volume_size,
    const std::shared_ptr<OperationControl>& operation) {
    if (volume_size == 0) throw std::invalid_argument("ZIP volume size must be non-zero");
    if (!std::filesystem::is_regular_file(archive_path)) {
        throw std::runtime_error("ZIP source does not exist: " +
                                 core::path_to_utf8(archive_path));
    }

    const auto staging = core::unique_sibling_path(archive_path, L"split-stage");
    const auto backup = core::unique_sibling_path(archive_path, L"split-backup");
    std::filesystem::create_directory(staging);
    TempDirectoryGuard staging_guard(staging);
    std::filesystem::create_directory(backup);
    TempDirectoryGuard backup_guard(backup);

    const auto staged_archive = staging / archive_path.filename();
    create_split_zip(archive_path, staged_archive, volume_size, operation);
    const auto staged_files = volume_set_files(staged_archive);
    if (staged_files.empty()) {
        throw std::runtime_error("ZIP split backend did not create a volume set");
    }

    // A volume set spans several files, so stage the complete replacement first
    // and keep the old set in a sibling rollback directory until every new file
    // has been installed successfully.
    operation_checkpoint(operation);
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> backups;
    try {
        for (const auto& existing : volume_set_files(archive_path)) {
            const auto saved = backup / existing.filename();
            std::filesystem::rename(existing, saved);
            backups.emplace_back(existing, saved);
        }
    } catch (...) {
        restore_backups(backups);
        throw;
    }

    std::vector<std::filesystem::path> installed;
    try {
        for (const auto& staged : staged_files) {
            operation_checkpoint(operation);
            const auto target = archive_path.parent_path() / staged.filename();
            std::filesystem::rename(staged, target);
            installed.push_back(target);
        }
    } catch (...) {
        std::error_code ignored;
        for (const auto& target : installed) {
            std::filesystem::remove(target, ignored);
            ignored.clear();
        }
        restore_backups(backups);
        throw;
    }
}

}  // namespace axiom
