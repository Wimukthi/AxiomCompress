// Theming and dark-mode drawing for the main window: palette
// construction, fonts and DPI, theme brushes, and owner-drawn painting.

#include "gui/main_window_internal.hpp"

namespace axiom::gui {

bool system_prefers_dark_mode() {
if (high_contrast_enabled()) {
    return false;
}

DWORD apps_use_light_theme = 1;
DWORD size = sizeof(apps_use_light_theme);
const auto result = RegGetValueW(
    HKEY_CURRENT_USER,
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
    L"AppsUseLightTheme",
    RRF_RT_REG_DWORD,
    nullptr,
    &apps_use_light_theme,
    &size);
return result == ERROR_SUCCESS && apps_use_light_theme == 0;
}

ThemePalette make_theme(int preference,
                    int accent_mode,
                    COLORREF custom_accent) {
ThemePalette theme;
theme.dark = preference == 1
    ? true
    : preference == 2 ? false : system_prefers_dark_mode();
if (high_contrast_enabled()) {
    theme.accent = GetSysColor(COLOR_HIGHLIGHT);
    theme.selection = GetSysColor(COLOR_HIGHLIGHT);
    theme.selection_text = GetSysColor(COLOR_HIGHLIGHTTEXT);
    theme.focus = GetSysColor(COLOR_HOTLIGHT);
    return theme;
}
theme.accent = axiom::gui::resolve_dialog_accent_color(accent_mode, custom_accent);
if (theme.dark) {
    theme.window = RGB(32, 32, 32);
    theme.panel = RGB(45, 45, 48);
    theme.edit = RGB(37, 37, 38);
    theme.text = RGB(241, 241, 241);
    theme.muted_text = RGB(180, 180, 180);
    theme.border = RGB(64, 64, 64);
    theme.button = RGB(45, 45, 48);
    theme.button_hot = blend_color(theme.button, theme.accent, 16);
    theme.button_pressed = blend_color(theme.button, theme.accent, 28);
    theme.selection = blend_color(theme.window, theme.accent, 44);
    theme.selection_text = readable_selection_text(theme.selection);
    theme.focus = theme.accent;
    theme.scrollbar_track = RGB(45, 45, 48);
    theme.scrollbar_thumb = RGB(92, 92, 96);
    theme.scrollbar_thumb_pressed = RGB(122, 122, 126);
} else {
    theme.button_hot = blend_color(theme.button, theme.accent, 12);
    theme.button_pressed = blend_color(theme.button, theme.accent, 22);
    theme.selection = theme.accent;
    theme.selection_text = readable_selection_text(theme.selection);
    theme.focus = theme.accent;
}
return theme;
}

void configure_dialog_appearance(const axiom::gui::ApplicationDialogOptions& options) {
axiom::gui::set_dialog_appearance({
    options.theme_mode,
    options.accent_color_mode,
    options.custom_accent_color,
    options.toolbar_icon_style,
});
}

axiom::gui::OperationWindowTheme make_operation_window_theme(const ThemePalette& theme) {
return {
    theme.dark,
    theme.window,
    theme.panel,
    theme.text,
    theme.muted_text,
    theme.border,
    theme.button,
    theme.button_hot,
    theme.button_pressed,
    theme.edit,
    theme.selection,
};
}

axiom::gui::ToolbarIcon toolbar_icon_for_button(UINT id) {
using axiom::gui::ToolbarIcon;
switch (id) {
    case kAddFiles: return ToolbarIcon::archive;
    case kOpenArchive: return ToolbarIcon::open;
    case kExtract: return ToolbarIcon::extract;
    case kTest: return ToolbarIcon::test;
    case kNavigateBack: return ToolbarIcon::back;
    case kNavigateForward: return ToolbarIcon::forward;
    case kNavigateUp: return ToolbarIcon::up;
    case kNavigateRefresh: return ToolbarIcon::refresh;
    case kAddressGo: return ToolbarIcon::forward;
    case kView: return ToolbarIcon::view;
    case kDelete: return ToolbarIcon::delete_item;
    case kInfo: return ToolbarIcon::info;
    case kSettings: return ToolbarIcon::settings;
    default: return ToolbarIcon::none;
}
}

int MainWindow::scale(int value) const {
    return MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
}

void MainWindow::reset_font() {
    if (ui_font_ != nullptr) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void MainWindow::rebuild_font() {
    reset_font();

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                    &metrics, 0, dpi_)) {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    }
    ui_font_ = CreateFontIndirectW(&metrics.lfMessageFont);
}

void MainWindow::set_control_font(HWND control) const {
    if (control != nullptr && ui_font_ != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    }
}

void MainWindow::apply_fonts() {
    HWND controls[] = {
        add_files_, open_archive_, extract_, test_,
        list_, status_,
        navigate_back_, navigate_forward_, navigate_up_, navigate_refresh_,
        address_edit_, address_go_, view_, delete_, info_, settings_,
        tooltip_,
    };
    for (HWND control : controls) {
        set_control_font(control);
    }
    menu_bar_.set_font(ui_font_);
    table_.set_font(ui_font_);
    tree_view_.set_font(ui_font_);
    for (HWND label : transient_labels_) {
        set_control_font(label);
    }
}

void MainWindow::update_dpi(UINT dpi) {
    if (dpi == 0) {
        dpi = USER_DEFAULT_SCREEN_DPI;
    }
    dpi_ = dpi;
    rebuild_font();
    menu_bar_.set_dpi(dpi_);
    table_.set_dpi(dpi_);
    tree_view_.set_dpi(dpi_);
    tree_width_ = std::clamp(tree_width_, scale(180), scale(420));
    apply_fonts();
    apply_edit_margins();
}

void MainWindow::reset_theme_brushes() {
    if (window_brush_ != nullptr) {
        DeleteObject(window_brush_);
        window_brush_ = nullptr;
    }
    if (panel_brush_ != nullptr) {
        DeleteObject(panel_brush_);
        panel_brush_ = nullptr;
    }
    if (edit_brush_ != nullptr) {
        DeleteObject(edit_brush_);
        edit_brush_ = nullptr;
    }
}

void MainWindow::rebuild_theme_brushes() {
    reset_theme_brushes();
    window_brush_ = CreateSolidBrush(theme_.window);
    panel_brush_ = CreateSolidBrush(theme_.panel);
    edit_brush_ = CreateSolidBrush(theme_.edit);
}

void MainWindow::apply_theme_to_control(HWND control) const {
    if (control == nullptr) {
        return;
    }
    SetWindowTheme(control, theme_.dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    InvalidateRect(control, nullptr, FALSE);
}

void MainWindow::apply_theme() {
    configure_dialog_appearance(application_options_);
    theme_ = make_theme(application_options_.theme_mode,
                        application_options_.accent_color_mode,
                        application_options_.custom_accent_color);
    rebuild_theme_brushes();
    set_dark_title_bar(hwnd_, theme_.dark);

    HWND controls[] = {
        add_files_, open_archive_, extract_, test_,
        list_, status_,
        navigate_back_, navigate_forward_, navigate_up_, navigate_refresh_,
        address_edit_, address_go_, view_, delete_, info_, settings_,
        tooltip_,
    };
    for (HWND control : controls) {
        apply_theme_to_control(control);
    }
    axiom::gui::apply_dialog_control_theme(address_edit_, theme_.dark);
    menu_bar_.set_theme({
        theme_.dark ? RGB(31, 31, 31) : RGB(250, 250, 250),
        theme_.button_hot,
        theme_.button_pressed,
        theme_.text,
        theme_.muted_text,
        theme_.dark ? RGB(42, 42, 42) : RGB(210, 210, 210),
        theme_.dark ? RGB(54, 54, 54) : RGB(210, 210, 210),
    });
    table_.set_theme(theme_);
    tree_view_.set_theme(theme_);
    operation_window_.set_theme(make_operation_window_theme(theme_));

    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT MainWindow::paint_control_background(HWND control, HDC dc, UINT message) {
    SetTextColor(dc, theme_.text);

    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        SetBkColor(dc, theme_.edit);
        return reinterpret_cast<LRESULT>(edit_brush_);
    }

    if (message == WM_CTLCOLORBTN) {
        SetBkColor(dc, theme_.panel);
        return reinterpret_cast<LRESULT>(panel_brush_);
    }

    if (control == status_) {
        SetBkColor(dc, theme_.window);
        return reinterpret_cast<LRESULT>(window_brush_);
    }

    SetBkMode(dc, TRANSPARENT);
    return reinterpret_cast<LRESULT>(window_brush_);
}

void MainWindow::fill_rect(HDC dc, const RECT& rect, COLORREF color) const {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void MainWindow::frame_rect(HDC dc, const RECT& rect, COLORREF color) const {
    HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}

void MainWindow::draw_owner_button(const DRAWITEMSTRUCT& draw) const {
    if (!is_button_id(draw.CtlID)) {
        return;
    }

    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    RECT rect = draw.rcItem;
    const COLORREF button_color = pressed ? theme_.button_pressed :
        (hot ? theme_.button_hot : theme_.button);
    fill_rect(draw.hDC, rect, button_color);

    HFONT font = ui_font_ != nullptr ? ui_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ old_font = SelectObject(draw.hDC, font);
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, disabled ? theme_.muted_text : theme_.text);

    // Buttons communicate focus through a restrained border; a blue focus
    // ring is visually too loud in the compact dark toolbar.
    const COLORREF border = (focused || hot || pressed) ? theme_.button_hot : theme_.border;
    frame_rect(draw.hDC, rect, border);

    std::wstring text = get_text(draw.hwndItem);
    if (pressed) {
        OffsetRect(&rect, scale(1), scale(1));
    }

    const auto icon = toolbar_icon_for_button(draw.CtlID);
    const int icon_mode = std::clamp(application_options_.toolbar_icon_style, 0, 2);
    const COLORREF content_color = disabled ? theme_.muted_text
        : icon_mode == 2 ? theme_.accent : theme_.text;
    const auto icon_style = !disabled && icon_mode == 1
        ? axiom::gui::ToolbarIconStyle::colorful
        : axiom::gui::ToolbarIconStyle::monochrome;
    if (icon == axiom::gui::ToolbarIcon::none) {
        DrawTextW(draw.hDC, text.c_str(), -1, &rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else if (is_icon_only_button(draw.CtlID)) {
        axiom::gui::draw_toolbar_icon(draw.hDC, icon, rect, content_color,
                                      dpi_, 18, icon_style);
    } else {
        SIZE text_size{};
        GetTextExtentPoint32W(draw.hDC, text.c_str(), static_cast<int>(text.size()), &text_size);
        const int icon_size = scale(18);
        const int gap = scale(5);
        const int content_width = icon_size + gap + static_cast<int>(text_size.cx);
        const int content_left = rect.left + (rect.right - rect.left - content_width) / 2;
        RECT icon_rect{content_left, rect.top, content_left + icon_size, rect.bottom};
        axiom::gui::draw_toolbar_icon(draw.hDC, icon, icon_rect, content_color,
                                      dpi_, 18, icon_style);

        RECT text_rect{icon_rect.right + gap, rect.top,
                       icon_rect.right + gap + static_cast<int>(text_size.cx), rect.bottom};
        DrawTextW(draw.hDC, text.c_str(), -1, &text_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    SelectObject(draw.hDC, old_font);
}

void MainWindow::paint_shell() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    // Several layout paths intentionally invalidate without erasing to avoid
    // classic Win32 white flashes.  The parent still owns every exposed pixel
    // between child windows, so paint the dirty shell area here instead of
    // relying on WM_ERASEBKGND.  WS_CLIPCHILDREN keeps the custom tree/table
    // controls out of this fill.
    fill_rect(dc, paint.rcPaint, theme_.window);
    if (tree_splitter_rect_.right > tree_splitter_rect_.left &&
        tree_splitter_rect_.bottom > tree_splitter_rect_.top) {
        fill_rect(dc, tree_splitter_rect_, theme_.border);
        RECT grip = tree_splitter_rect_;
        const int inset_x = std::max(1, scale(2));
        InflateRect(&grip, -inset_x, 0);
        fill_rect(dc, grip, theme_.panel);
    }
    EndPaint(hwnd_, &paint);
}

void MainWindow::draw_address_entry(const DRAWITEMSTRUCT& draw) const {
    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    fill_rect(draw.hDC, draw.rcItem, selected ? theme_.selection : theme_.edit);
    if (draw.itemID == static_cast<UINT>(-1)) return;
    const LRESULT entry_index = SendMessageW(
        address_edit_, CB_GETITEMDATA, draw.itemID, 0);
    if (entry_index < 0 ||
        static_cast<std::size_t>(entry_index) >= address_entries_.size()) return;
    const AddressEntry& entry = address_entries_[static_cast<std::size_t>(entry_index)];
    RECT text_rect = draw.rcItem;
    text_rect.left += scale(6);
    if (entry.icon.image_list != nullptr && entry.icon.index >= 0) {
        int icon_width = 0;
        int icon_height = 0;
        ImageList_GetIconSize(entry.icon.image_list, &icon_width, &icon_height);
        const int icon_y = static_cast<int>(text_rect.top) + std::max(
            0, (static_cast<int>(text_rect.bottom - text_rect.top) - icon_height) / 2);
        ImageList_Draw(entry.icon.image_list, entry.icon.index, draw.hDC,
                       static_cast<int>(text_rect.left), icon_y,
                       ILD_TRANSPARENT);
        text_rect.left += icon_width + scale(6);
    }
    text_rect.right -= scale(5);
    HFONT font = ui_font_ != nullptr
        ? ui_font_
        : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ old_font = SelectObject(draw.hDC, font);
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, selected ? theme_.selection_text : theme_.text);
    DrawTextW(draw.hDC, entry.label.c_str(), -1, &text_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(draw.hDC, old_font);
    if ((draw.itemState & ODS_FOCUS) != 0) {
        RECT focus = draw.rcItem;
        InflateRect(&focus, -2, -2);
        DrawFocusRect(draw.hDC, &focus);
    }
}

}  // namespace axiom::gui
