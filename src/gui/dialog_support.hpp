#pragma once

#include <windows.h>

#include <string>

namespace axiom::gui {

struct DialogColors {
    COLORREF background;
    COLORREF text;
    COLORREF control_background;
    COLORREF selection_background;
    COLORREF selection_text;
    COLORREF disabled_text;
    COLORREF border;
    COLORREF focus_border;
};

int scale_for_dialog_dpi(int value, UINT dpi);
HFONT create_dialog_font(UINT dpi);
void delete_dialog_font(HFONT font);

HICON load_axiom_icon(HINSTANCE instance, int width, int height);
void assign_axiom_window_class_icons(WNDCLASSEXW& window_class, HINSTANCE instance);
void apply_axiom_window_icons(HWND window, HINSTANCE instance);

DialogColors dialog_colors(bool dark);
bool dialog_system_prefers_dark_mode();
void apply_dialog_dark_frame(HWND window, bool dark);
void apply_dialog_control_theme(HWND control, bool dark);
void set_dialog_control_font(HWND control, HFONT font);
void draw_dialog_button(const DRAWITEMSTRUCT& draw, bool dark);
void draw_dialog_checkbox(const DRAWITEMSTRUCT& draw, bool dark, bool checked);
bool message_targets_window(HWND window, const MSG& message);
std::wstring last_error_text(DWORD error = GetLastError());

}  // namespace axiom::gui
