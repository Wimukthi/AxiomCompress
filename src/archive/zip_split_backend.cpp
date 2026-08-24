#include "archive/zip_split_backend.hpp"

#include "archive/container_internal.hpp"
#include "core/file_replace.hpp"
#include "core/path_text.hpp"
#include "third_party/minizip-ng/mz.h"
#include "third_party/minizip-ng/mz_strm.h"
#include "third_party/minizip-ng/mz_zip.h"
#include "third_party/minizip-ng/mz_zip_rw.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
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

}  // namespace

namespace {

struct ZipRangeStream {
    mzng_stream stream{};
    std::ifstream file;
    std::uint64_t base = 0;
    std::uint64_t length = 0;
    std::uint64_t position = 0;
    int32_t last_error = MZ_OK;
};

int32_t zip_range_open(void*, const char*, int32_t) { return MZ_OK; }

int32_t zip_range_is_open(void* stream) {
    const auto* range = static_cast<const ZipRangeStream*>(stream);
    return range->file.is_open() ? MZ_OK : MZ_OPEN_ERROR;
}

int32_t zip_range_read(void* stream, void* buffer, int32_t size) {
    auto* range = static_cast<ZipRangeStream*>(stream);
    if (size < 0 || !range->file.is_open()) return MZ_PARAM_ERROR;
    const std::uint64_t remaining = range->length - range->position;
    const auto amount = static_cast<std::uint64_t>(size);
    const auto wanted = static_cast<std::streamsize>((std::min)(remaining, amount));
    if (wanted == 0) return 0;
    if (range->base > static_cast<std::uint64_t>(
                          std::numeric_limits<std::streamoff>::max()) -
                          range->position) {
        range->last_error = MZ_SEEK_ERROR;
        return MZ_SEEK_ERROR;
    }
    range->file.clear();
    range->file.seekg(static_cast<std::streamoff>(range->base + range->position),
                      std::ios::beg);
    if (!range->file) {
        range->last_error = MZ_SEEK_ERROR;
        return MZ_SEEK_ERROR;
    }
    range->file.read(static_cast<char*>(buffer), wanted);
    const auto read = range->file.gcount();
    if (read < 0) {
        range->last_error = MZ_READ_ERROR;
        return MZ_READ_ERROR;
    }
    range->position += static_cast<std::uint64_t>(read);
    return static_cast<int32_t>(read);
}

int32_t zip_range_write(void*, const void*, int32_t) { return MZ_WRITE_ERROR; }

int64_t zip_range_tell(void* stream) {
    const auto* range = static_cast<const ZipRangeStream*>(stream);
    if (range->position > static_cast<std::uint64_t>(
                              std::numeric_limits<std::int64_t>::max())) {
        return MZ_TELL_ERROR;
    }
    return static_cast<std::int64_t>(range->position);
}

int32_t zip_range_seek(void* stream, int64_t offset, int32_t origin) {
    auto* range = static_cast<ZipRangeStream*>(stream);
    std::uint64_t anchor = 0;
    if (origin == MZ_SEEK_CUR) {
        anchor = range->position;
    } else if (origin == MZ_SEEK_END) {
        anchor = range->length;
    } else if (origin != MZ_SEEK_SET) {
        return MZ_PARAM_ERROR;
    }
    std::uint64_t target = 0;
    if (offset < 0) {
        const std::uint64_t distance =
            static_cast<std::uint64_t>(-(offset + 1)) + 1;
        if (distance > anchor) return MZ_SEEK_ERROR;
        target = anchor - distance;
    } else {
        const auto distance = static_cast<std::uint64_t>(offset);
        if (distance > range->length - (std::min)(anchor, range->length)) {
            return MZ_SEEK_ERROR;
        }
        target = anchor + distance;
    }
    if (target > range->length) return MZ_SEEK_ERROR;
    range->position = target;
    return MZ_OK;
}

int32_t zip_range_close(void*) { return MZ_OK; }
int32_t zip_range_error(void* stream) {
    return static_cast<ZipRangeStream*>(stream)->last_error;
}
void* zip_range_create() { return nullptr; }
void zip_range_delete(void**) {}

int32_t zip_range_get_prop(void*, int32_t prop, int64_t* value) {
    if (!value) return MZ_PARAM_ERROR;
    if (prop == MZ_STREAM_PROP_DISK_NUMBER || prop == MZ_STREAM_PROP_DISK_SIZE) {
        *value = 0;
        return MZ_OK;
    }
    return MZ_EXIST_ERROR;
}

int32_t zip_range_set_prop(void*, int32_t, int64_t) { return MZ_EXIST_ERROR; }

mzng_stream_vtbl kZipRangeVtable{
    zip_range_open, zip_range_is_open, zip_range_read, zip_range_write,
    zip_range_tell, zip_range_seek, zip_range_close, zip_range_error,
    zip_range_create, zip_range_delete, zip_range_get_prop, zip_range_set_prop};

}  // namespace

struct ZipBackendReader::Impl {
    explicit Impl(
        const std::filesystem::path& archive_path,
        std::shared_ptr<OperationControl> control,
        const std::optional<std::pair<std::uint64_t, std::uint64_t>>& payload_range)
        : operation(std::move(control)) {
        if (payload_range) {
            const auto [offset, size] = *payload_range;
            const auto file_size = std::filesystem::file_size(archive_path);
            if (offset > file_size || size > file_size - offset ||
                size > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())) {
                throw FormatError("embedded ZIP payload is outside the executable");
            }
            range.stream.vtbl = &kZipRangeVtable;
            range.base = offset;
            range.length = size;
            range.file.open(archive_path, std::ios::binary);
            if (!range.file) {
                throw std::runtime_error("cannot open ZIP archive: " +
                                         core::path_to_utf8(archive_path));
            }
            check_mz(mzng_zip_reader_open(reader.get(), &range),
                     "cannot open embedded ZIP payload");
        } else {
            const auto path = core::path_to_utf8(archive_path);
            check_mz(mzng_zip_reader_open_file(reader.get(), path.c_str()),
                     "cannot open ZIP archive");
        }
        open = true;

        void* zip_handle = nullptr;
        if (mzng_zip_reader_get_zip_handle(reader.get(), &zip_handle) == MZ_OK &&
            zip_handle != nullptr) {
            std::uint32_t central_disk = 0;
            if (mzng_zip_get_disk_number_with_cd(zip_handle, &central_disk) == MZ_OK) {
                split = central_disk != 0;
            }
        }

        int32_t status = mzng_zip_reader_goto_first_entry(reader.get());
        while (status == MZ_OK) {
            operation_checkpoint(operation);
            mzng_zip_file* file = nullptr;
            check_mz(mzng_zip_reader_entry_get_info(reader.get(), &file),
                     "cannot read ZIP directory");
            if (file == nullptr || file->compressed_size < 0 ||
                file->uncompressed_size < 0) {
                throw FormatError("ZIP entry has invalid sizes");
            }
            ZipBackendEntryInfo entry;
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
            entry.version_made_by = file->version_madeby;
            entry.version_needed = file->version_needed;
            entry.method = file->compression_method;
            entry.flags = file->flag;
            entry.zipcrypto_verifier = file->pk_verify;
            entry.aes_version = file->aes_version;
            entry.aes_strength = file->aes_strength;
            entry.crc32 = file->crc;
            entry.compressed_size = static_cast<std::uint64_t>(file->compressed_size);
            entry.uncompressed_size = static_cast<std::uint64_t>(file->uncompressed_size);
            entry.modified_time = static_cast<std::int64_t>(file->modified_date);
            entry.internal_attributes = file->internal_fa;
            entry.external_attributes = file->external_fa;
            entry.zip64 = file->zip64 != 0;
            entry.directory = mzng_zip_reader_entry_is_dir(reader.get()) == MZ_OK;
            split = split || file->disk_number != 0;
            entries.push_back(std::move(entry));
            status = mzng_zip_reader_goto_next_entry(reader.get());
        }
        if (status != MZ_END_OF_LIST) {
            check_mz(status, "cannot enumerate ZIP entries");
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
            throw FormatError("ZIP entry index is invalid");
        }
    }

    ReaderHandle reader;
    ZipRangeStream range;
    std::shared_ptr<OperationControl> operation;
    std::vector<ZipBackendEntryInfo> entries;
    bool open = false;
    bool split = false;
};

ZipBackendReader::ZipBackendReader(
    const std::filesystem::path& archive_path,
    const std::shared_ptr<OperationControl>& operation,
    const std::optional<std::pair<std::uint64_t, std::uint64_t>>& payload_range)
    : impl_(std::make_unique<Impl>(archive_path, operation, payload_range)) {}

ZipBackendReader::~ZipBackendReader() = default;
ZipBackendReader::ZipBackendReader(ZipBackendReader&&) noexcept = default;
ZipBackendReader& ZipBackendReader::operator=(ZipBackendReader&&) noexcept = default;

const std::vector<ZipBackendEntryInfo>& ZipBackendReader::entries() const {
    return impl_->entries;
}

void ZipBackendReader::read_raw_entry(
    std::size_t index, const ZipRawChunkCallback& callback) {
    if (index >= impl_->entries.size()) {
        throw std::out_of_range("ZIP entry index is invalid");
    }
    impl_->goto_entry(index);
    mzng_zip_reader_set_raw(impl_->reader.get(), 1);
    const int32_t open_result = mzng_zip_reader_entry_open(impl_->reader.get());
    if (open_result != MZ_OK) {
        throw FormatError("cannot open ZIP entry '" + impl_->entries[index].path +
                          "' (minizip-ng error " +
                          std::to_string(open_result) + ")");
    }
    std::array<std::uint8_t, 64u << 10> buffer{};
    std::uint64_t total = 0;
    try {
        for (;;) {
            operation_checkpoint(impl_->operation);
            const int32_t read = mzng_zip_reader_entry_read(
                impl_->reader.get(), buffer.data(),
                static_cast<std::int32_t>(buffer.size()));
            if (read < 0) check_mz(read, "cannot read ZIP entry");
            if (read == 0) break;
            total += static_cast<std::uint64_t>(read);
            callback(std::span<const std::uint8_t>(
                buffer.data(), static_cast<std::size_t>(read)));
        }
        check_mz(mzng_zip_reader_entry_close(impl_->reader.get()),
                 "cannot close ZIP entry");
    } catch (...) {
        mzng_zip_reader_entry_close(impl_->reader.get());
        throw;
    }
    if (total != impl_->entries[index].compressed_size) {
        throw FormatError("ZIP entry is truncated");
    }
}

bool ZipBackendReader::is_split() const noexcept { return impl_->split; }

struct ZipBackendWriter::Impl {
    Impl(const std::filesystem::path& archive_path, std::uint64_t volume_size,
         std::shared_ptr<OperationControl> control)
        : path(archive_path), operation(std::move(control)) {
        if (volume_size > static_cast<std::uint64_t>(
                              std::numeric_limits<std::int64_t>::max())) {
            throw std::invalid_argument("ZIP volume size is too large");
        }
        const auto text = core::path_to_utf8(path);
        check_mz(mzng_zip_writer_open_file(
                     writer.get(), text.c_str(),
                     static_cast<std::int64_t>(volume_size), 0),
                 "cannot create ZIP archive");
        open = true;
    }

    ~Impl() {
        if (open) mzng_zip_writer_close(writer.get());
    }

    WriterHandle writer;
    std::filesystem::path path;
    std::shared_ptr<OperationControl> operation;
    std::string entry_path;
    std::string entry_comment;
    ByteVector entry_extra;
    mzng_zip_file entry_info{};
    std::uint64_t raw_bytes = 0;
    bool open = false;
    bool entry_open = false;
};

ZipBackendWriter::ZipBackendWriter(
    const std::filesystem::path& archive_path, std::uint64_t volume_size,
    const std::shared_ptr<OperationControl>& operation)
    : impl_(std::make_unique<Impl>(archive_path, volume_size, operation)) {}

ZipBackendWriter::~ZipBackendWriter() = default;
ZipBackendWriter::ZipBackendWriter(ZipBackendWriter&&) noexcept = default;
ZipBackendWriter& ZipBackendWriter::operator=(ZipBackendWriter&&) noexcept = default;

void ZipBackendWriter::copy_entry(
    ZipBackendReader& source, std::size_t index,
    const std::optional<std::string>& renamed_path) {
    if (impl_->entry_open) throw std::logic_error("ZIP entry is already open");
    if (index >= source.impl_->entries.size()) {
        throw std::out_of_range("ZIP entry index is invalid");
    }
    source.impl_->goto_entry(index);
    mzng_zip_file* source_info = nullptr;
    check_mz(mzng_zip_reader_entry_get_info(source.impl_->reader.get(), &source_info),
             "cannot read ZIP entry metadata");
    if (!source_info) throw FormatError("ZIP entry metadata is missing");

    mzng_zip_file output_info = *source_info;
    std::string output_name;
    if (renamed_path) {
        output_name = *renamed_path;
        if (source.impl_->entries[index].directory &&
            (output_name.empty() || output_name.back() != '/')) {
            output_name.push_back('/');
        }
        output_info.filename = output_name.c_str();
        if (output_name.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("ZIP entry path is too long");
        }
        output_info.filename_size = static_cast<std::uint16_t>(output_name.size());
    }

    mzng_zip_reader_set_raw(source.impl_->reader.get(), 1);
    mzng_zip_writer_set_raw(impl_->writer.get(), 1);
    check_mz(mzng_zip_reader_entry_open(source.impl_->reader.get()),
             "cannot open ZIP source entry");
    bool writer_open = false;
    try {
        check_mz(mzng_zip_writer_entry_open(impl_->writer.get(), &output_info),
                 "cannot open ZIP output entry");
        writer_open = true;
        std::array<std::uint8_t, 64u << 10> buffer{};
        std::uint64_t copied = 0;
        for (;;) {
            operation_checkpoint(impl_->operation);
            const int32_t read = mzng_zip_reader_entry_read(
                source.impl_->reader.get(), buffer.data(),
                static_cast<std::int32_t>(buffer.size()));
            if (read < 0) check_mz(read, "cannot read ZIP source entry");
            if (read == 0) break;
            const int32_t written = mzng_zip_writer_entry_write(
                impl_->writer.get(), buffer.data(), read);
            if (written != read) check_mz(MZ_WRITE_ERROR, "cannot write ZIP output entry");
            copied += static_cast<std::uint64_t>(read);
        }
        if (copied != source.impl_->entries[index].compressed_size) {
            throw FormatError("ZIP source entry is truncated");
        }
        check_mz(mzng_zip_writer_entry_close(impl_->writer.get()),
                 "cannot close ZIP output entry");
        writer_open = false;
        check_mz(mzng_zip_reader_entry_close(source.impl_->reader.get()),
                 "cannot close ZIP source entry");
        impl_->raw_bytes += copied;
    } catch (...) {
        if (writer_open) mzng_zip_writer_entry_close(impl_->writer.get());
        mzng_zip_reader_entry_close(source.impl_->reader.get());
        throw;
    }
}

void ZipBackendWriter::begin_raw_entry(const ZipBackendWriteInfo& info) {
    if (!impl_->open || impl_->entry_open) {
        throw std::logic_error("ZIP writer is not ready for a new entry");
    }
    if (info.path.size() > std::numeric_limits<std::uint16_t>::max() ||
        info.comment.size() > std::numeric_limits<std::uint16_t>::max() ||
        info.extra.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("ZIP entry metadata is too large");
    }
    impl_->entry_path = info.path;
    if (info.directory &&
        (impl_->entry_path.empty() || impl_->entry_path.back() != '/')) {
        impl_->entry_path.push_back('/');
    }
    impl_->entry_comment = info.comment;
    impl_->entry_extra = info.extra;
    impl_->entry_info = {};
    impl_->entry_info.version_madeby = info.version_made_by;
    impl_->entry_info.version_needed = info.version_needed;
    impl_->entry_info.flag = info.flags;
    impl_->entry_info.compression_method = info.method;
    impl_->entry_info.modified_date = static_cast<time_t>(info.modified_time);
    impl_->entry_info.uncompressed_size =
        static_cast<std::int64_t>(info.uncompressed_size);
    impl_->entry_info.internal_fa = info.internal_attributes;
    impl_->entry_info.external_fa = info.external_attributes;
    impl_->entry_info.filename = impl_->entry_path.c_str();
    impl_->entry_info.filename_size =
        static_cast<std::uint16_t>(impl_->entry_path.size());
    impl_->entry_info.comment = impl_->entry_comment.empty()
        ? nullptr : impl_->entry_comment.c_str();
    impl_->entry_info.comment_size =
        static_cast<std::uint16_t>(impl_->entry_comment.size());
    impl_->entry_info.extrafield = impl_->entry_extra.empty()
        ? nullptr : impl_->entry_extra.data();
    impl_->entry_info.extrafield_size =
        static_cast<std::uint16_t>(impl_->entry_extra.size());
    impl_->entry_info.zip64 = MZ_ZIP64_AUTO;
    mzng_zip_writer_set_raw(impl_->writer.get(), 1);
    check_mz(mzng_zip_writer_entry_open(impl_->writer.get(), &impl_->entry_info),
             "cannot open ZIP output entry");
    impl_->entry_open = true;
}

void ZipBackendWriter::write_raw(std::span<const std::uint8_t> bytes) {
    if (!impl_->entry_open) throw std::logic_error("ZIP entry is not open");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        operation_checkpoint(impl_->operation);
        const auto amount = static_cast<std::int32_t>((std::min)(
            bytes.size() - offset,
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())));
        const int32_t written = mzng_zip_writer_entry_write(
            impl_->writer.get(), bytes.data() + offset, amount);
        if (written != amount) check_mz(MZ_WRITE_ERROR, "cannot write ZIP entry");
        offset += static_cast<std::size_t>(written);
        impl_->raw_bytes += static_cast<std::uint64_t>(written);
    }
}

void ZipBackendWriter::finish_raw_entry(
    std::uint32_t crc32, std::uint64_t uncompressed_size) {
    if (!impl_->entry_open) throw std::logic_error("ZIP entry is not open");
    if (uncompressed_size > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("ZIP entry is too large");
    }
    void* zip_handle = nullptr;
    check_mz(mzng_zip_writer_get_zip_handle(impl_->writer.get(), &zip_handle),
             "cannot access ZIP writer");
    check_mz(mzng_zip_entry_close_raw(
                 zip_handle, static_cast<std::int64_t>(uncompressed_size), crc32),
             "cannot close ZIP entry");
    impl_->entry_open = false;
}

void ZipBackendWriter::finalize() {
    if (!impl_->open || impl_->entry_open) {
        throw std::logic_error("ZIP writer cannot be finalized");
    }
    check_mz(mzng_zip_writer_close(impl_->writer.get()),
             "cannot finalize ZIP archive");
    impl_->open = false;
}

std::uint64_t ZipBackendWriter::size() const noexcept {
    std::error_code error;
    const auto file_size = std::filesystem::file_size(impl_->path, error);
    return error ? impl_->raw_bytes : static_cast<std::uint64_t>(file_size);
}

void create_split_zip(const std::filesystem::path& source_zip,
                      const std::filesystem::path& output_zip,
                      std::uint64_t volume_size,
                      const std::shared_ptr<OperationControl>& operation) {
    if (volume_size == 0) {
        throw std::invalid_argument("ZIP volume size must be non-zero");
    }
    ZipBackendReader reader(source_zip, operation);
    ZipBackendWriter writer(output_zip, volume_size, operation);
    const auto total = static_cast<std::uint64_t>(reader.entries().size());
    std::uint64_t completed = 0;
    report_operation(operation, OperationStage::writing, 0, total, 0, total,
                     "Copying ZIP entries");
    for (std::size_t index = 0; index < reader.entries().size(); ++index) {
        operation_checkpoint(operation);
        writer.copy_entry(reader, index);
        ++completed;
        report_operation(operation, OperationStage::writing,
                         completed, total, completed, total,
                         reader.entries()[index].path);
    }
    writer.finalize();
}

namespace {

class TempDirectoryGuard {
public:
    explicit TempDirectoryGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~TempDirectoryGuard() {
        if (!active_) return;
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    void dismiss() noexcept { active_ = false; }
private:
    std::filesystem::path path_;
    bool active_ = true;
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

bool restore_backups(
    const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>& backups) {
    bool restored = true;
    for (auto item = backups.rbegin(); item != backups.rend(); ++item) {
        std::error_code error;
        std::filesystem::rename(item->second, item->first, error);
        restored = restored && !error;
    }
    return restored;
}
}  // namespace

std::uint64_t zip_volume_set_size(
    const std::filesystem::path& archive_path) {
    std::uint64_t total = 0;
    for (const auto& file : volume_set_files(archive_path)) {
        const auto size = std::filesystem::file_size(file);
        if (size > std::numeric_limits<std::uint64_t>::max() - total) {
            throw std::overflow_error("ZIP volume set is too large");
        }
        total += static_cast<std::uint64_t>(size);
    }
    return total;
}

void install_zip_volume_set(
    const std::filesystem::path& staged_archive,
    const std::filesystem::path& archive_path,
    const std::shared_ptr<OperationControl>& operation) {
    const auto backup = core::unique_sibling_path(archive_path, L"split-backup");
    std::filesystem::create_directory(backup);
    TempDirectoryGuard backup_guard(backup);

    const auto staged_files = volume_set_files(staged_archive);
    if (staged_files.empty()) {
        throw std::runtime_error("ZIP backend did not create a staged volume set");
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
        if (!restore_backups(backups)) {
            backup_guard.dismiss();
            throw std::runtime_error(
                "failed to stage the old ZIP volume set; recovery files remain at " +
                core::path_to_utf8(backup));
        }
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
        if (!restore_backups(backups)) {
            backup_guard.dismiss();
            throw std::runtime_error(
                "failed to install the ZIP volume set; recovery files remain at " +
                core::path_to_utf8(backup));
        }
        throw;
    }
}

void create_zip_volumes(const std::filesystem::path& archive_path,
                        std::uint64_t volume_size,
    const std::shared_ptr<OperationControl>& operation) {
    if (volume_size == 0) throw std::invalid_argument("ZIP volume size must be non-zero");
    if (!std::filesystem::is_regular_file(archive_path)) {
        throw std::runtime_error("ZIP source does not exist: " +
                                 core::path_to_utf8(archive_path));
    }

    const auto staging = core::unique_sibling_path(archive_path, L"split-stage");
    std::filesystem::create_directory(staging);
    TempDirectoryGuard staging_guard(staging);
    const auto staged_archive = staging / archive_path.filename();
    create_split_zip(archive_path, staged_archive, volume_size, operation);
    install_zip_volume_set(staged_archive, archive_path, operation);
}

}  // namespace axiom
