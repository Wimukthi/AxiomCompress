#pragma once

#include "gui/browser_model.hpp"

#include <filesystem>
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

}  // namespace axiom::gui
