#pragma once

#include <windows.h>

namespace axiom::gui {

void show_about_dialog(HWND owner, HINSTANCE instance, UINT dpi, bool dark,
                       UINT check_updates_command);

}  // namespace axiom::gui

