#pragma once

#include "axiom/archive.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace axiom::gui {

enum class BrowserLocationKind { computer, filesystem, archive };

struct BrowserLocation {
    BrowserLocationKind kind = BrowserLocationKind::computer;
    std::filesystem::path filesystem_path;
    std::filesystem::path archive_path;
    std::string archive_directory;

    static BrowserLocation computer();
    static BrowserLocation filesystem(std::filesystem::path path);
    static BrowserLocation archive(std::filesystem::path path, std::string directory = {});

    std::wstring display_name() const;
    std::wstring identity() const;
    bool operator==(const BrowserLocation& other) const;
};

enum class BrowserItemKind { parent, drive, directory, file, archive, symlink, hardlink };

struct BrowserItem {
    std::uint64_t id = 0;
    BrowserItemKind kind = BrowserItemKind::file;
    std::wstring name;
    std::wstring type;
    std::wstring modified;
    std::wstring attributes;
    std::uint64_t size = 0;
    std::optional<std::uint64_t> packed_size;
    bool packed_size_estimated = false;
    std::optional<std::uint32_t> crc32;
    std::filesystem::path filesystem_path;
    std::string archive_path;

    bool is_container() const;
    bool is_parent() const;
};

struct BrowserSnapshot {
    std::uint64_t generation = 0;
    BrowserLocation location;
    std::vector<BrowserItem> items;
    std::wstring error;
};

using ArchiveCapabilities = axiom::ArchiveCapabilities;

class ArchiveCatalog {
public:
    static std::shared_ptr<const ArchiveCatalog> load(const std::filesystem::path& path,
                                                      const std::string& password = {});

    const std::filesystem::path& path() const;
    const axiom::ArchiveProvider& provider() const;
    const axiom::ArchiveFormatInfo& format_info() const;
    const std::vector<ArchiveEntry>& entries() const;
    const ArchiveCapabilities& capabilities() const;
    BrowserSnapshot list(const BrowserLocation& location, std::stop_token stop) const;

private:
    ArchiveCatalog(std::filesystem::path path, const axiom::ArchiveProvider& provider,
                   std::vector<ArchiveEntry> entries, ArchiveCapabilities capabilities);

    std::filesystem::path path_;
    const axiom::ArchiveProvider* provider_ = nullptr;
    std::vector<ArchiveEntry> entries_;
    ArchiveCapabilities capabilities_;
};

struct BrowserLoadResult {
    BrowserSnapshot snapshot;
    std::shared_ptr<const ArchiveCatalog> archive_catalog;
};

BrowserLoadResult load_browser_location(
    const BrowserLocation& location,
    std::uint64_t generation,
    std::shared_ptr<const ArchiveCatalog> archive_catalog,
    std::stop_token stop,
    const std::string& archive_password = {});

class NavigationHistory {
public:
    void reset(BrowserLocation location);
    void navigate(BrowserLocation location);
    std::optional<BrowserLocation> back();
    std::optional<BrowserLocation> forward();

    bool can_back() const;
    bool can_forward() const;
    std::size_t index() const;
    std::size_t size() const;
    const BrowserLocation& current() const;

private:
    std::vector<BrowserLocation> entries_;
    std::size_t index_ = 0;
};

bool is_axiom_archive(const std::filesystem::path& path);
bool is_supported_archive(const std::filesystem::path& path);
bool path_has_supported_archive_extension(const std::filesystem::path& path);
std::optional<BrowserLocation> parent_location(const BrowserLocation& location);

}  // namespace axiom::gui
