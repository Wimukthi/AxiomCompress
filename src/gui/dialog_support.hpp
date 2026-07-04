#pragma once

#include <windows.h>

#include <string>
#include <string_view>

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

struct DialogAppearance {
    int theme_mode = 0;  // 0 = system, 1 = dark, 2 = light.
    int accent_color_mode = 0;  // 0 = Windows accent, 1 = Axiom amber, 2..5 presets, 6 = custom.
    COLORREF custom_accent_color = RGB(255, 185, 60);
    int icon_style = 0;  // 0 = theme-tinted, 1 = colorful, 2 = accent-colored.
};

int scale_for_dialog_dpi(int value, UINT dpi);
HFONT create_dialog_font(UINT dpi);
void delete_dialog_font(HFONT font);

HICON load_axiom_icon(HINSTANCE instance, int width, int height);
void assign_axiom_window_class_icons(WNDCLASSEXW& window_class, HINSTANCE instance);
void apply_axiom_window_icons(HWND window, HINSTANCE instance);

DialogColors dialog_colors(bool dark);
bool dialog_system_prefers_dark_mode();
void set_dialog_appearance(const DialogAppearance& appearance);
DialogAppearance dialog_appearance();
bool dialog_should_use_dark();
COLORREF resolve_dialog_accent_color(int mode, COLORREF custom_color);
COLORREF dialog_accent_color();
int dialog_icon_style();
void apply_dialog_dark_frame(HWND window, bool dark);
void apply_dialog_control_theme(HWND control, bool dark);
void set_dialog_control_font(HWND control, HFONT font);
void draw_dialog_button(const DRAWITEMSTRUCT& draw, bool dark);
void draw_dialog_checkbox(const DRAWITEMSTRUCT& draw, bool dark, bool checked);
void draw_dialog_combo_item(const DRAWITEMSTRUCT& draw, bool dark);
bool disable_dialog_owner(HWND owner);
void restore_dialog_owner(HWND owner, bool was_enabled);
bool message_targets_window(HWND window, const MSG& message);
bool window_placement_is_visible(const WINDOWPLACEMENT& placement);
POINT centered_window_position(HWND owner, int width, int height);
void restore_named_window_placement(HWND window, HWND owner, std::wstring_view name);
void save_named_window_placement(std::wstring_view name, HWND window);
std::wstring last_error_text(DWORD error = GetLastError());

}  // namespace axiom::gui
