#pragma once

#include "gui/archive_dialogs.hpp"

#include <windows.h>

#include <string>
#include <vector>

namespace axiom::gui {

struct PersistedGuiSettings {
    ApplicationDialogOptions application;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    bool has_placement = false;
    std::wstring last_location;
    std::wstring last_archive_output_folder;
    std::wstring last_extract_destination_folder;
    int sort_column = 0;
    bool sort_ascending = true;
    int tree_width = 0;
    bool tree_pane_visible = true;
    std::vector<std::wstring> tree_expanded_nodes;
    std::wstring tree_selected_node;
    int tree_scroll_position = 0;
    std::vector<int> column_widths;
    std::vector<std::wstring> recent_locations;
    std::vector<std::wstring> recent_archives;
    std::vector<std::wstring> favorite_locations;
};

PersistedGuiSettings load_gui_settings();
void save_gui_settings(const PersistedGuiSettings& settings);

}  // namespace axiom::gui
