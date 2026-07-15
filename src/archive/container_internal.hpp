#pragma once

// Helpers shared between the src/archive translation units (container.cpp,
// container_formats.cpp, container_zip.cpp). This header is internal to the
// archive engine and is not part of the public API in include/axiom.

#include "axiom/archive.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace axiom {

// Magic prefix of an .axar archive (also embedded in SFX executables).
inline constexpr std::array<std::uint8_t, 8> kArchiveMagic = {'A', 'X', 'I', 'O',
                                                              'M', 'A', 'R', '\0'};

// ---- built-in format table ---------------------------------------------------

inline constexpr std::size_t kAxarFormatIndex = 0;
inline constexpr std::size_t kZipFormatIndex = 1;
inline constexpr std::size_t kSevenZFormatIndex = 2;
inline constexpr std::size_t kRarFormatIndex = 3;
inline constexpr std::size_t kTarFormatIndex = 4;
inline constexpr std::size_t kIsoFormatIndex = 5;
inline constexpr std::size_t kCabFormatIndex = 6;

inline constexpr std::array<ArchiveFormatInfo, 7> kArchiveFormats{{
    {
        ArchiveFormat::axar,
        "axar",
        "Axiom archive",
        "Axiom archive",
        ".axar",
        L"Axiom archives (*.axar)",
        L"*.axar",
        true,
    },
    {
        ArchiveFormat::zip,
        "zip",
        "ZIP archive",
        "ZIP archive",
        ".zip",
        L"ZIP archives (*.zip)",
        L"*.zip",
        false,
    },
    {
        ArchiveFormat::seven_z,
        "7z",
        "7z archive",
        "7z archive",
        ".7z",
        L"7z archives (*.7z)",
        L"*.7z",
        false,
    },
    {
        ArchiveFormat::rar,
        "rar",
        "RAR archive",
        "RAR archive",
        ".rar",
        L"RAR archives (*.rar;*.r00;*.part*.rar)",
        L"*.rar;*.r00;*.part*.rar",
        false,
    },
    {
        ArchiveFormat::tar,
        "tar",
        "TAR archive",
        "TAR archive",
        ".tar",
        L"TAR archives (*.tar;*.tar.gz;*.tgz;*.tar.xz;*.txz;*.tar.bz2;*.tbz2;*.tar.zst;*.tzst)",
        L"*.tar;*.tar.gz;*.tgz;*.tar.xz;*.txz;*.tar.bz2;*.tbz2;*.tar.zst;*.tzst",
        false,
    },
    {
        ArchiveFormat::iso,
        "iso",
        "ISO image",
        "ISO image",
        ".iso",
        L"ISO images (*.iso)",
        L"*.iso",
        false,
    },
    {
        ArchiveFormat::cab,
        "cab",
        "CAB archive",
        "CAB archive",
        ".cab",
        L"CAB archives (*.cab)",
        L"*.cab",
        false,
    },
}};

// ---- input scanning ------------------------------------------------------------

// One filesystem object queued for archiving, with the archive path it maps to.
struct ScanItem {
    std::filesystem::path absolute;
    std::string archive_path;
    bool is_directory = false;
    bool is_symlink = false;
    std::string symlink_target{};  // verbatim target when is_symlink
};

void scan_input(const std::filesystem::path& input, std::vector<ScanItem>& items);
void scan_input_at(const ArchiveInput& input, std::vector<ScanItem>& items,
                   const std::shared_ptr<OperationControl>& operation);
std::uint64_t scanned_file_bytes(const std::vector<ScanItem>& items);
bool open_input_with_retry(std::ifstream& input,
                           const std::filesystem::path& path,
                           unsigned retries,
                           const std::shared_ptr<OperationControl>& operation);
void report_skipped_input(const ScanItem& item,
                          const std::shared_ptr<OperationControl>& operation);

// ---- operation reporting -------------------------------------------------------

void report_operation(const std::shared_ptr<OperationControl>& operation,
                      OperationStage stage,
                      std::uint64_t completed_bytes,
                      std::uint64_t total_bytes,
                      std::uint64_t completed_items,
                      std::uint64_t total_items,
                      std::string current_path = {},
                      std::uint64_t current_file_completed_bytes = 0,
                      std::uint64_t current_file_total_bytes = 0,
                      std::uint64_t throughput_bytes = 0);
void operation_checkpoint(const std::shared_ptr<OperationControl>& operation);

// ---- archive path safety -------------------------------------------------------

bool is_safe_relative(const std::string& path);
std::string normalize_archive_path(std::string path, const char* field_name);
bool is_same_or_child(std::string_view candidate, std::string_view parent);
std::string join_archive_path(std::string_view parent, std::string_view child);

// ---- extraction destination safety ----------------------------------------------

bool is_within(const std::filesystem::path& base, const std::filesystem::path& target);
void reject_symlinked_ancestor(const std::filesystem::path& dest_norm,
                               const std::filesystem::path& target);

// ---- misc shared utilities -------------------------------------------------------

std::int64_t to_unix_seconds(std::filesystem::file_time_type stamp);
std::filesystem::file_time_type from_unix_seconds(std::int64_t seconds);

// Atomically replace `destination` with a completed sibling temporary without
// deleting the valid destination first.
void replace_archive_file(const std::filesystem::path& temporary,
                          const std::filesystem::path& destination);

// Offset/size of the archive embedded in an Axiom SFX executable, if any.
std::optional<std::pair<std::uint64_t, std::uint64_t>> sfx_embedded_payload_range(
    const std::filesystem::path& path);
std::optional<std::pair<std::uint64_t, std::uint64_t>> sfx_embedded_archive_range(
    const std::filesystem::path& path);

// Deletes the guarded temporary file on scope exit unless dismissed.
class TempFileGuard {
public:
    explicit TempFileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~TempFileGuard() {
        if (active_) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }

    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;

    void dismiss() {
        active_ = false;
    }

private:
    std::filesystem::path path_;
    bool active_ = true;
};

inline void put_u16(ByteVector& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

inline std::string lower_ascii(std::string text) {
    for (auto& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

inline std::wstring lower_ascii(std::wstring text) {
    for (auto& ch : text) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return text;
}

// ---- provider registry glue ------------------------------------------------------

// The built-in ZIP provider singleton, defined in container_zip.cpp.
const ArchiveProvider& zip_archive_provider();

}  // namespace axiom
