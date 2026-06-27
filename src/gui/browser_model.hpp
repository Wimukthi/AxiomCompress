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

struct ArchiveCapabilities {
    bool packed_sizes = false;
    bool selective_extract = false;
    bool update = false;
    bool encryption = false;
    bool recovery_records = false;
    bool multi_volume = false;
    bool comments = false;
    bool lock = false;
    bool metadata = false;
    bool links = false;
    bool authenticity = false;
    bool locked = false;
    bool encrypted = false;
    bool directory_encrypted = false;
};

class ArchiveCatalog {
public:
    static std::shared_ptr<const ArchiveCatalog> load(const std::filesystem::path& path,
                                                      const std::string& password = {});

    const std::filesystem::path& path() const;
    const ArchiveCapabilities& capabilities() const;
    BrowserSnapshot list(const BrowserLocation& location, std::stop_token stop) const;

private:
    ArchiveCatalog(std::filesystem::path path, std::vector<ArchiveEntry> entries,
                   const std::string& password);

    std::filesystem::path path_;
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
    const BrowserLocation& current() const;

private:
    std::vector<BrowserLocation> entries_;
    std::size_t index_ = 0;
};

bool is_axiom_archive(const std::filesystem::path& path);
std::optional<BrowserLocation> parent_location(const BrowserLocation& location);

}  // namespace axiom::gui
