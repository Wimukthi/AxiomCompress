#pragma once

#include "gui/browser_model.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace axiom::gui {

// The instant filter is deliberately a pure model operation. Keeping parsing
// and matching outside the HWND layer makes malformed queries deterministic and
// lets the same rules cover filesystem and archive rows.
bool browser_item_matches_filter(const BrowserItem& item, std::wstring_view query);

// Returns an empty string when `name` is a portable Windows leaf name. Archive
// renames use the same rules as filesystem renames so a created entry can always
// be extracted on Windows later.
std::wstring file_manager_leaf_name_error(std::wstring_view name);

std::wstring extraction_folder_name(const std::filesystem::path& archive);

std::vector<std::filesystem::path> batch_extraction_destinations(
    const std::filesystem::path& root,
    const std::vector<std::filesystem::path>& archives);

// What text in the address bar points at.
struct AddressTarget {
    enum class Kind { computer, path, archive };
    Kind kind = Kind::path;
    std::filesystem::path path;      // the folder or file, or the archive
    std::wstring archive_directory;  // '/'-separated, inside the archive
};

// Read address bar text as people type and paste it: surrounding quotes,
// "This PC", a drive as Explorer names it ("DATA-A (D:)"), "D:" for a drive's
// root, file: URLs, "archive :: /folder", a trailing separator, "." and "..",
// and a path relative to `base_folder` (when there is one). Paths come back
// normalized; nullopt when the text cannot name a location.
std::optional<AddressTarget> parse_address_text(std::wstring_view text,
                                                const std::filesystem::path& base_folder);

}  // namespace axiom::gui
