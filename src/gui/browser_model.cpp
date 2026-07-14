#define NOMINMAX
#include "gui/browser_model.hpp"
#include "core/windows_time.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace axiom::gui {
namespace fs = std::filesystem;
namespace {

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            text.data(), static_cast<int>(text.size()),
                                            nullptr, 0);
    if (length <= 0) return L"?";
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string normalize_archive_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    const auto first = path.find_first_not_of('/');
    if (first == std::string::npos) return {};
    if (first != 0) path.erase(0, first);
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
}

std::string archive_parent(std::string path) {
    path = normalize_archive_path(std::move(path));
    const auto separator = path.find_last_of('/');
    return separator == std::string::npos ? std::string{} : path.substr(0, separator);
}

std::wstring file_time_text(const FILETIME& file_time,
                            core::LocalTimeConverter* converter = nullptr) {
    if (converter != nullptr) {
        ULARGE_INTEGER ticks{};
        ticks.LowPart = file_time.dwLowDateTime;
        ticks.HighPart = file_time.dwHighDateTime;
        constexpr std::uint64_t epoch_ticks = 11644473600ull * 10000000ull;
        if (ticks.QuadPart < epoch_ticks) return {};
        SYSTEMTIME system{};
        const auto seconds = static_cast<std::int64_t>(
            (ticks.QuadPart - epoch_ticks) / 10000000ull);
        if (!converter->unix_to_local(seconds, system)) return {};
        wchar_t buffer[64]{};
        swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u",
                   system.wYear, system.wMonth, system.wDay,
                   system.wHour, system.wMinute);
        return buffer;
    }
    FILETIME local{};
    SYSTEMTIME system{};
    if (!FileTimeToLocalFileTime(&file_time, &local) ||
        !FileTimeToSystemTime(&local, &system)) {
        return {};
    }
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u",
               system.wYear, system.wMonth, system.wDay,
               system.wHour, system.wMinute);
    return buffer;
}

std::wstring unix_time_text(std::int64_t seconds,
                            core::LocalTimeConverter* converter = nullptr) {
    if (seconds <= 0) return {};
    if (converter != nullptr) {
        SYSTEMTIME system{};
        if (!converter->unix_to_local(seconds, system)) return {};
        wchar_t buffer[64]{};
        swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u",
                   system.wYear, system.wMonth, system.wDay,
                   system.wHour, system.wMinute);
        return buffer;
    }
    constexpr std::uint64_t epoch_difference = 11644473600ull;
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(seconds) + epoch_difference) * 10000000ull;
    FILETIME file_time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    return file_time_text(file_time);
}

std::wstring attributes_text(DWORD attributes) {
    std::wstring result;
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) result += L'R';
    if ((attributes & FILE_ATTRIBUTE_HIDDEN) != 0) result += L'H';
    if ((attributes & FILE_ATTRIBUTE_SYSTEM) != 0) result += L'S';
    if ((attributes & FILE_ATTRIBUTE_ARCHIVE) != 0) result += L'A';
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) result += L'L';
    if ((attributes & FILE_ATTRIBUTE_OFFLINE) != 0) result += L'O';
    if ((attributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0) result += L'C';
    return result;
}

std::wstring lower_ascii_wide(std::wstring text) {
    for (wchar_t& ch : text) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return text;
}

bool has_ascii_suffix(std::wstring_view text, std::wstring_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

bool has_rar_part_extension(std::wstring_view extension) {
    return extension.size() == 4 && extension[0] == L'.' && extension[1] == L'r' &&
           extension[2] >= L'0' && extension[2] <= L'9' &&
           extension[3] >= L'0' && extension[3] <= L'9';
}

std::optional<std::wstring> archive_type_from_name(const fs::path& path) {
    const std::wstring name = lower_ascii_wide(path.filename().wstring());
    const std::wstring extension = lower_ascii_wide(path.extension().wstring());
    if (extension == L".axar") return L"Axiom archive";
    if (extension == L".zip") return L"ZIP archive";
    if (extension == L".jar") return L"JAR archive";
    if (extension == L".war") return L"WAR archive";
    if (extension == L".apk") return L"APK archive";
    if (extension == L".7z") return L"7z archive";
    if (extension == L".rar" || has_rar_part_extension(extension) ||
        (name.find(L".part") != std::wstring::npos && has_ascii_suffix(name, L".rar"))) {
        return L"RAR archive";
    }
    constexpr std::array<std::wstring_view, 9> tar_suffixes{
        L".tar", L".tar.gz", L".tgz", L".tar.xz", L".txz",
        L".tar.bz2", L".tbz2", L".tar.zst", L".tzst"};
    if (std::any_of(tar_suffixes.begin(), tar_suffixes.end(),
                    [&](std::wstring_view suffix) { return has_ascii_suffix(name, suffix); })) {
        return L"TAR archive";
    }
    if (extension == L".iso") return L"ISO image";
    if (extension == L".cab") return L"CAB archive";
    return std::nullopt;
}

std::wstring extension_type(const fs::path& path, bool directory) {
    if (directory) return L"File folder";
    if (auto archive_type = archive_type_from_name(path)) return *archive_type;
    std::wstring extension = path.extension().wstring();
    if (extension.empty()) return L"File";
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towupper(value)); });
    if (extension.front() == L'.') extension.erase(extension.begin());
    return extension + L" file";
}

std::uint64_t stable_id(std::wstring_view value) {
    std::uint64_t hash = 1469598103934665603ull;
    for (wchar_t character : value) {
        hash ^= static_cast<std::uint16_t>(std::towlower(character));
        hash *= 1099511628211ull;
    }
    return hash;
}

int item_rank(BrowserItemKind kind) {
    if (kind == BrowserItemKind::parent) return 0;
    if (kind == BrowserItemKind::drive || kind == BrowserItemKind::directory) return 1;
    return 2;
}

bool item_less(const BrowserItem& left, const BrowserItem& right) {
    if (item_rank(left.kind) != item_rank(right.kind)) {
        return item_rank(left.kind) < item_rank(right.kind);
    }
    return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
}

BrowserItem parent_item() {
    BrowserItem item;
    item.id = stable_id(L"..");
    item.kind = BrowserItemKind::parent;
    item.name = L"..";
    item.type = L"Parent folder";
    return item;
}

std::wstring windows_error_message(DWORD error) {
    wchar_t* raw = nullptr;
    const DWORD count = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring message = count != 0 && raw != nullptr
        ? std::wstring(raw, count)
        : L"Windows error";
    if (raw != nullptr) LocalFree(raw);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

BrowserSnapshot load_computer(const BrowserLocation& location, std::stop_token stop) {
    BrowserSnapshot snapshot;
    snapshot.location = location;
    const DWORD required = GetLogicalDriveStringsW(0, nullptr);
    if (required == 0) {
        snapshot.error = windows_error_message(GetLastError());
        return snapshot;
    }
    std::wstring drives(static_cast<std::size_t>(required) + 1, L'\0');
    GetLogicalDriveStringsW(static_cast<DWORD>(drives.size()), drives.data());
    for (const wchar_t* drive = drives.c_str(); *drive != L'\0'; drive += wcslen(drive) + 1) {
        if (stop.stop_requested()) return snapshot;
        wchar_t volume[MAX_PATH]{};
        GetVolumeInformationW(drive, volume, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        BrowserItem item;
        item.kind = BrowserItemKind::drive;
        item.filesystem_path = drive;
        item.name = volume[0] == L'\0'
            ? std::wstring(drive)
            : std::wstring(volume) + L" (" + drive[0] + L":)";
        item.type = GetDriveTypeW(drive) == DRIVE_REMOVABLE ? L"Removable drive" : L"Local drive";
        ULARGE_INTEGER available{}, total{}, free{};
        if (GetDiskFreeSpaceExW(drive, &available, &total, &free)) item.size = total.QuadPart;
        item.id = stable_id(item.filesystem_path.wstring());
        snapshot.items.push_back(std::move(item));
    }
    std::sort(snapshot.items.begin(), snapshot.items.end(), item_less);
    return snapshot;
}

BrowserSnapshot load_filesystem(const BrowserLocation& location, std::stop_token stop) {
    BrowserSnapshot snapshot;
    snapshot.location = location;
    snapshot.items.reserve(1024);
    snapshot.items.push_back(parent_item());
    const fs::path pattern = location.filesystem_path / L"*";
    WIN32_FIND_DATAW data{};
    core::LocalTimeConverter time_converter;
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        snapshot.error = windows_error_message(GetLastError());
        return snapshot;
    }
    do {
        if (stop.stop_requested()) {
            FindClose(find);
            return snapshot;
        }
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        BrowserItem item;
        item.filesystem_path = location.filesystem_path / data.cFileName;
        item.name = data.cFileName;
        const auto archive_type = directory
            ? std::optional<std::wstring>{}
            : archive_type_from_name(item.filesystem_path);
        item.kind = directory ? BrowserItemKind::directory
                              : (archive_type ? BrowserItemKind::archive
                                              : BrowserItemKind::file);
        item.type = directory ? L"File folder"
                              : (archive_type ? *archive_type
                                              : extension_type(item.filesystem_path, false));
        item.size = directory ? 0
            : (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        item.modified = file_time_text(data.ftLastWriteTime, &time_converter);
        item.attributes = attributes_text(data.dwFileAttributes);
        item.id = stable_id(item.filesystem_path.wstring());
        snapshot.items.push_back(std::move(item));
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return snapshot;
}

}  // namespace

BrowserLocation BrowserLocation::computer() { return {}; }

BrowserLocation BrowserLocation::filesystem(fs::path path) {
    BrowserLocation location;
    location.kind = BrowserLocationKind::filesystem;
    std::error_code error;
    location.filesystem_path = fs::absolute(std::move(path), error).lexically_normal();
    return location;
}

BrowserLocation BrowserLocation::archive(fs::path path, std::string directory) {
    BrowserLocation location;
    location.kind = BrowserLocationKind::archive;
    std::error_code error;
    location.archive_path = fs::absolute(std::move(path), error).lexically_normal();
    location.archive_directory = normalize_archive_path(std::move(directory));
    return location;
}

std::wstring BrowserLocation::display_name() const {
    if (kind == BrowserLocationKind::computer) return L"This PC";
    if (kind == BrowserLocationKind::filesystem) return filesystem_path.wstring();
    std::wstring result = archive_path.wstring() + L" :: /";
    if (!archive_directory.empty()) result += widen(archive_directory);
    return result;
}

std::wstring BrowserLocation::identity() const { return display_name(); }

bool BrowserLocation::operator==(const BrowserLocation& other) const {
    return kind == other.kind && filesystem_path == other.filesystem_path &&
           archive_path == other.archive_path && archive_directory == other.archive_directory;
}

bool BrowserItem::is_container() const {
    return kind == BrowserItemKind::parent || kind == BrowserItemKind::drive ||
           kind == BrowserItemKind::directory || kind == BrowserItemKind::archive;
}

bool BrowserItem::is_parent() const { return kind == BrowserItemKind::parent; }

ArchiveCatalog::ArchiveCatalog(fs::path path, const axiom::ArchiveProvider& provider,
                               std::vector<ArchiveEntry> entries,
                               ArchiveCapabilities capabilities)
    : path_(std::move(path)),
      provider_(&provider),
      entries_(std::move(entries)),
      capabilities_(capabilities) {
    build_directory_index();
}

void ArchiveCatalog::build_directory_index() {
    struct NodeLocation {
        std::string parent;
        std::size_t index = 0;
    };

    std::unordered_map<std::string, NodeLocation> locations;
    locations.reserve(entries_.size() * 2u);
    children_by_directory_.reserve(entries_.size() / 4u + 1u);

    const std::wstring archive_identity = path_.wstring() + L"|";
    core::LocalTimeConverter time_converter;
    for (auto& entry : entries_) {
        entry.path = normalize_archive_path(std::move(entry.path));
        if (entry.path.empty()) continue;

        std::string parent;
        std::size_t component_start = 0;
        while (component_start < entry.path.size()) {
            const std::size_t separator = entry.path.find('/', component_start);
            const bool final_component = separator == std::string::npos;
            const std::size_t component_end = final_component ? entry.path.size() : separator;
            if (component_end == component_start) {
                component_start = separator + 1;
                continue;
            }
            const std::string_view component(entry.path.data() + component_start,
                                             component_end - component_start);
            std::string child_path;
            child_path.reserve(parent.size() + (parent.empty() ? 0u : 1u) + component.size());
            if (!parent.empty()) {
                child_path = parent;
                child_path.push_back('/');
            }
            child_path.append(component);

            auto location = locations.find(child_path);
            if (location == locations.end()) {
                BrowserItem item;
                item.archive_path = child_path;
                item.name = widen(component);
                item.id = stable_id(archive_identity + widen(child_path));
                item.kind = BrowserItemKind::directory;
                item.type = L"File folder";
                auto& children = children_by_directory_[parent];
                const std::size_t index = children.size();
                children.push_back(std::move(item));
                location = locations.emplace(child_path, NodeLocation{parent, index}).first;
            }

            auto& item = children_by_directory_[location->second.parent][location->second.index];
            if (!final_component) {
                // A malformed archive can describe a path first as a file and later
                // use it as a parent. Browsing must still expose the reachable tree.
                item.kind = BrowserItemKind::directory;
                item.type = L"File folder";
                item.size = 0;
                item.packed_size.reset();
                item.crc32.reset();
            } else if (entry.is_directory) {
                item.kind = BrowserItemKind::directory;
                item.type = L"File folder";
                item.size = 0;
                item.packed_size.reset();
                item.crc32.reset();
                item.modified = unix_time_text(entry.mtime, &time_converter);
            } else {
                item.kind = entry.is_symlink ? BrowserItemKind::symlink
                    : entry.is_hardlink ? BrowserItemKind::hardlink
                                        : BrowserItemKind::file;
                item.type = entry.is_symlink ? L"Symbolic link"
                    : entry.is_hardlink ? L"Hard link"
                                        : extension_type(fs::path(item.name), false);
                item.size = entry.size;
                item.packed_size = entry.packed_size;
                item.packed_size_estimated = entry.packed_size_estimated;
                item.crc32 = entry.has_crc32
                    ? std::optional<std::uint32_t>{entry.crc32}
                    : std::nullopt;
                item.modified = unix_time_text(entry.mtime, &time_converter);
            }

            parent = std::move(child_path);
            if (final_component) break;
            component_start = separator + 1;
        }
    }

    for (auto& [directory, children] : children_by_directory_) {
        (void)directory;
        std::sort(children.begin(), children.end(), item_less);
    }
}

std::shared_ptr<const ArchiveCatalog> ArchiveCatalog::load(const fs::path& path,
                                                           const std::string& password) {
    const auto* provider = axiom::archive_provider_for_path(path);
    if (provider == nullptr) {
        throw std::runtime_error("unsupported archive format");
    }
    auto capabilities = provider->capabilities(path, password);
    return std::shared_ptr<const ArchiveCatalog>(new ArchiveCatalog(
        path, *provider, provider->list(path, password), capabilities));
}

const fs::path& ArchiveCatalog::path() const { return path_; }

const axiom::ArchiveProvider& ArchiveCatalog::provider() const { return *provider_; }

const axiom::ArchiveFormatInfo& ArchiveCatalog::format_info() const {
    return provider_->info();
}

const std::vector<ArchiveEntry>& ArchiveCatalog::entries() const { return entries_; }

const ArchiveCapabilities& ArchiveCatalog::capabilities() const { return capabilities_; }

BrowserSnapshot ArchiveCatalog::list(const BrowserLocation& location, std::stop_token stop) const {
    BrowserSnapshot snapshot;
    snapshot.location = location;
    if (!location.archive_directory.empty()) snapshot.items.push_back(parent_item());
    const std::string directory = normalize_archive_path(location.archive_directory);
    if (const auto found = children_by_directory_.find(directory);
        found != children_by_directory_.end()) {
        snapshot.items.reserve(snapshot.items.size() + found->second.size());
        for (const auto& item : found->second) {
            if (stop.stop_requested()) return snapshot;
            snapshot.items.push_back(item);
        }
    }
    return snapshot;
}

BrowserLoadResult load_browser_location(
    const BrowserLocation& location,
    std::uint64_t generation,
    std::shared_ptr<const ArchiveCatalog> archive_catalog,
    std::stop_token stop,
    const std::string& archive_password) {
    BrowserLoadResult result;
    try {
        if (location.kind == BrowserLocationKind::computer) {
            result.snapshot = load_computer(location, stop);
        } else if (location.kind == BrowserLocationKind::filesystem) {
            result.snapshot = load_filesystem(location, stop);
        } else {
            result.archive_password_supplied = !archive_password.empty();
            if (const auto* provider = axiom::archive_provider_for_path(
                    location.archive_path)) {
                result.archive_capabilities = provider->capabilities(
                    location.archive_path, archive_password);
                result.archive_capabilities_available = true;
            }
            if (!archive_catalog || archive_catalog->path() != location.archive_path) {
                archive_catalog = ArchiveCatalog::load(location.archive_path, archive_password);
            }
            result.snapshot = archive_catalog->list(location, stop);
            result.archive_catalog = std::move(archive_catalog);
        }
    } catch (const std::exception& error) {
        result.snapshot.location = location;
        result.snapshot.error = widen(error.what());
    }
    result.snapshot.generation = generation;
    return result;
}

void NavigationHistory::reset(BrowserLocation location) {
    entries_.assign(1, std::move(location));
    index_ = 0;
}

void NavigationHistory::navigate(BrowserLocation location) {
    if (!entries_.empty() && entries_[index_] == location) return;
    if (!entries_.empty()) {
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index_ + 1), entries_.end());
    }
    entries_.push_back(std::move(location));
    index_ = entries_.size() - 1;
}

std::optional<BrowserLocation> NavigationHistory::back() {
    if (!can_back()) return std::nullopt;
    return entries_[--index_];
}

std::optional<BrowserLocation> NavigationHistory::forward() {
    if (!can_forward()) return std::nullopt;
    return entries_[++index_];
}

bool NavigationHistory::can_back() const { return !entries_.empty() && index_ > 0; }
bool NavigationHistory::can_forward() const { return !entries_.empty() && index_ + 1 < entries_.size(); }
std::size_t NavigationHistory::index() const { return index_; }
std::size_t NavigationHistory::size() const { return entries_.size(); }

const BrowserLocation& NavigationHistory::current() const {
    static const BrowserLocation computer_location = BrowserLocation::computer();
    return entries_.empty() ? computer_location : entries_[index_];
}

bool is_axiom_archive(const fs::path& path) {
    return axiom::is_native_archive(path);
}

bool is_supported_archive(const fs::path& path) {
    return axiom::is_supported_archive(path);
}

bool path_has_supported_archive_extension(const fs::path& path) {
    return archive_type_from_name(path).has_value();
}

std::optional<BrowserLocation> parent_location(const BrowserLocation& location) {
    if (location.kind == BrowserLocationKind::computer) return std::nullopt;
    if (location.kind == BrowserLocationKind::filesystem) {
        const fs::path parent = location.filesystem_path.parent_path();
        if (parent.empty() || parent == location.filesystem_path) return BrowserLocation::computer();
        return BrowserLocation::filesystem(parent);
    }
    if (location.archive_directory.empty()) {
        return BrowserLocation::filesystem(location.archive_path.parent_path());
    }
    return BrowserLocation::archive(location.archive_path, archive_parent(location.archive_directory));
}

}  // namespace axiom::gui
