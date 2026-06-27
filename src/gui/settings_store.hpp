#pragma once

#include "gui/archive_dialogs.hpp"

#include <windows.h>

#include <string>

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
};

PersistedGuiSettings load_gui_settings();
void save_gui_settings(const PersistedGuiSettings& settings);

}  // namespace axiom::gui
