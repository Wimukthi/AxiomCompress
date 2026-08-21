#pragma once

#include <windows.h>
#include <commctrl.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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
    bool center_child_windows = true;
};

enum class DialogInputFilter {
    unsigned_integer,
    byte_size,
    hexadecimal_color,
};

enum class DialogPathKind {
    existing_file,
    existing_folder,
    output_file,
    destination_folder,
};

struct DialogPathValidation {
    std::filesystem::path path;
    std::wstring error;

    explicit operator bool() const { return error.empty(); }
};

int scale_for_dialog_dpi(int value, UINT dpi);
SIZE dialog_window_size_for_client(int logical_width, int logical_height,
                                   DWORD style, DWORD extended_style, UINT dpi);
HFONT create_dialog_font(UINT dpi);
void delete_dialog_font(HFONT font);

HICON load_axiom_icon(HINSTANCE instance, int width, int height);
void assign_axiom_window_class_icons(WNDCLASSEXW& window_class, HINSTANCE instance);
void apply_axiom_window_icons(HWND window, HINSTANCE instance);

DialogColors dialog_colors(bool dark);
bool dialog_system_prefers_dark_mode();
bool dialog_high_contrast_enabled();
void set_dialog_appearance(const DialogAppearance& appearance);
DialogAppearance dialog_appearance();
bool dialog_should_use_dark();
bool handle_dialog_theme_setting_change(LPARAM lparam);
COLORREF resolve_dialog_accent_color(int mode, COLORREF custom_color);
COLORREF dialog_accent_color();
int dialog_icon_style();
void apply_dialog_dark_frame(HWND window, bool dark);
void apply_dialog_control_theme(HWND control, bool dark);
void set_dialog_control_font(HWND control, HFONT font);

// One tooltip manager per top-level UI surface. In addition to ordinary
// TTF_SUBCLASS tools, this registers owner-relative overlays so disabled
// controls can still explain why they are unavailable.
class TooltipManager {
public:
    TooltipManager() = default;
    ~TooltipManager();

    TooltipManager(const TooltipManager&) = delete;
    TooltipManager& operator=(const TooltipManager&) = delete;

    bool create(HWND owner, UINT layout_dpi, bool dark);
    bool add(HWND control, const wchar_t* text);
    void remove(HWND control);
    void update_dpi(UINT layout_dpi);
    void update_layout() const;
    void apply_theme(bool dark);
    void destroy();

    HWND hwnd() const { return hwnd_; }

private:
    struct ToolEntry {
        HWND control;
        UINT_PTR id;
    };

    static UINT_PTR overlay_id(HWND control);
    void update_rect(TOOLINFOW& tool, HWND control) const;
    static LRESULT CALLBACK owner_subclass_proc(
        HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
        UINT_PTR subclass_id, DWORD_PTR reference);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    std::vector<ToolEntry> tools_;
};

inline void add_dialog_tooltip(TooltipManager& tooltips, HWND control,
                               const wchar_t* text) {
    tooltips.add(control, text);
}
void apply_dialog_input_filter(HWND control, DialogInputFilter filter,
                               UINT maximum_characters = 0);
std::wstring trim_dialog_input(std::wstring text);
DialogPathValidation validate_dialog_path(
    std::wstring text, DialogPathKind kind);
void draw_dialog_button(const DRAWITEMSTRUCT& draw, bool dark);
void draw_dialog_checkbox(const DRAWITEMSTRUCT& draw, bool dark, bool checked);
void draw_dialog_radio_button(const DRAWITEMSTRUCT& draw, bool dark, bool checked);
void draw_dialog_combo_item(const DRAWITEMSTRUCT& draw, bool dark);
bool disable_dialog_owner(HWND owner, HWND dialog);
// Close a window registered by disable_dialog_owner. Restoring its owner
// immediately before destruction lets USER32 transfer activation directly
// within Axiom instead of briefly raising the application underneath it.
void destroy_modal_dialog(HWND dialog);
void restore_dialog_owner(HWND owner, bool was_enabled);
bool message_targets_window(HWND window, const MSG& message);
bool window_placement_is_visible(const WINDOWPLACEMENT& placement);
POINT centered_window_position(HWND owner, int width, int height);
// Restores geometry without making the window visible. The returned SW_*
// command preserves a saved maximized state and must be passed to ShowWindow
// after the caller has finished theming and constructing the first frame.
int restore_named_window_placement(HWND window, HWND owner, std::wstring_view name);
void save_named_window_placement(std::wstring_view name, HWND window);
std::wstring last_error_text(DWORD error = GetLastError());

}  // namespace axiom::gui
