#include "gui/dialog_support.hpp"

#include "gui/resource.hpp"
#include "gui/toolbar_icons.hpp"

#include <algorithm>
#include <commctrl.h>
#include <windowsx.h>
#include <cwchar>
#include <cwctype>
#include <dwmapi.h>
#include <gdiplus.h>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <wimukthi/win32_theme.hpp>

namespace axiom::gui {
namespace {

constexpr UINT_PTR kDarkComboSubclass = 1;
constexpr UINT_PTR kModalOwnerSubclass = 2;
constexpr UINT_PTR kTypedInputSubclass = 3;
constexpr const wchar_t* kSavedComboStyleProperty = L"AxiomSavedComboStyle";
constexpr const wchar_t* kSavedComboExStyleProperty = L"AxiomSavedComboExStyle";
constexpr const wchar_t* kDarkComboHotProperty = L"AxiomDarkComboHot";
constexpr const wchar_t* kDarkComboTrackingProperty = L"AxiomDarkComboTracking";
constexpr const wchar_t* kRestoreModalActivationProperty =
    L"AxiomRestoreModalActivation";
constexpr const wchar_t* kWindowLayoutsRegistryPath =
    L"Software\\AxiomCompress\\GUI\\WindowLayouts";

DialogAppearance g_dialog_appearance{};

class GdiplusSession {
public:
    GdiplusSession() {
        Gdiplus::GdiplusStartupInput input;
        ready_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
    }

    ~GdiplusSession() {
        if (ready_) Gdiplus::GdiplusShutdown(token_);
    }

    bool ready() const { return ready_; }

private:
    ULONG_PTR token_ = 0;
    bool ready_ = false;
};

bool gdiplus_ready() {
    static GdiplusSession session;
    return session.ready();
}

Gdiplus::Color gdiplus_color(COLORREF color) {
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

void configure_antialiased_shape_drawing(Gdiplus::Graphics& graphics) {
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    // Gamma-corrected coverage keeps saturated accents from looking washed out
    // against the light theme while retaining smooth round edges.
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityGammaCorrected);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
}

void draw_antialiased_checkmark(HDC dc, const RECT& box, UINT dpi, COLORREF color) {
    if (!gdiplus_ready()) {
        HPEN pen = CreatePen(PS_SOLID, scale_for_dialog_dpi(2, dpi), color);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        MoveToEx(dc, box.left + scale_for_dialog_dpi(3, dpi),
                 box.top + scale_for_dialog_dpi(8, dpi), nullptr);
        LineTo(dc, box.left + scale_for_dialog_dpi(7, dpi),
               box.bottom - scale_for_dialog_dpi(3, dpi));
        LineTo(dc, box.right - scale_for_dialog_dpi(3, dpi),
               box.top + scale_for_dialog_dpi(3, dpi));
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        return;
    }
    Gdiplus::Graphics graphics(dc);
    configure_antialiased_shape_drawing(graphics);
    const Gdiplus::REAL scaled_width = static_cast<Gdiplus::REAL>(
        scale_for_dialog_dpi(2, dpi));
    Gdiplus::Pen pen(gdiplus_color(color),
                     scaled_width < 1.75f ? 1.75f : scaled_width);
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound,
                   Gdiplus::DashCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::PointF points[] = {
        {static_cast<Gdiplus::REAL>(box.left + scale_for_dialog_dpi(3, dpi)),
         static_cast<Gdiplus::REAL>(box.top + scale_for_dialog_dpi(8, dpi))},
        {static_cast<Gdiplus::REAL>(box.left + scale_for_dialog_dpi(7, dpi)),
         static_cast<Gdiplus::REAL>(box.bottom - scale_for_dialog_dpi(3, dpi))},
        {static_cast<Gdiplus::REAL>(box.right - scale_for_dialog_dpi(3, dpi)),
         static_cast<Gdiplus::REAL>(box.top + scale_for_dialog_dpi(3, dpi))},
    };
    graphics.DrawLines(&pen, points, static_cast<INT>(std::size(points)));
}

void draw_selection_control_text(const DRAWITEMSTRUCT& draw, const RECT& glyph,
                                 UINT dpi, const DialogColors& colors, bool enabled) {
    wchar_t text[256]{};
    GetWindowTextW(draw.hwndItem, text,
                   static_cast<int>(sizeof(text) / sizeof(text[0])));
    const int saved_dc = SaveDC(draw.hDC);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0));
    if (font != nullptr) SelectObject(draw.hDC, font);
    // The control background has already been painted. Supplying that exact
    // color as an opaque ClearType composition surface makes owner-drawn
    // labels match adjacent native static labels pixel-for-pixel.
    SetBkMode(draw.hDC, OPAQUE);
    SetBkColor(draw.hDC, colors.background);
    SetTextColor(draw.hDC, enabled ? colors.text : colors.disabled_text);
    RECT text_rect = draw.rcItem;
    text_rect.left = glyph.right + scale_for_dialog_dpi(8, dpi);
    DrawTextW(draw.hDC, text, -1, &text_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RestoreDC(draw.hDC, saved_dc);
}

bool input_character_allowed(DialogInputFilter filter, wchar_t character) {
    if (character < L' ') return true;
    switch (filter) {
        case DialogInputFilter::unsigned_integer:
            return character >= L'0' && character <= L'9';
        case DialogInputFilter::byte_size:
            return (character >= L'0' && character <= L'9') ||
                   std::iswspace(character) ||
                   wcschr(L"bBkKmMgGtTiIyYeEsS", character) != nullptr;
        case DialogInputFilter::hexadecimal_color:
            return character == L'#' || std::iswxdigit(character);
    }
    return false;
}

bool reserved_windows_filename(std::wstring_view filename) {
    std::wstring stem(filename.substr(0, filename.find(L'.')));
    while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) {
        stem.pop_back();
    }
    std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towupper(ch));
    });
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" ||
        stem == L"NUL") {
        return true;
    }
    if (stem.size() == 4 &&
        (stem.rfind(L"COM", 0) == 0 || stem.rfind(L"LPT", 0) == 0) &&
        stem[3] >= L'1' && stem[3] <= L'9') {
        return true;
    }
    return false;
}

std::optional<std::wstring> invalid_windows_path_message(
    const std::filesystem::path& path) {
    const std::wstring raw = path.wstring();
    // Extended paths deliberately opt into Win32's verbatim rules. Let the
    // filesystem operation validate those rather than rejecting the prefix's
    // question mark here.
    if (raw.rfind(LR"(\\?\)", 0) == 0) return std::nullopt;

    for (const auto& component_path : path.relative_path()) {
        const std::wstring component = component_path.wstring();
        if (component.empty() || component == L"." || component == L"..") {
            continue;
        }
        if (component.back() == L' ' || component.back() == L'.') {
            return L"Path components cannot end with a space or period.";
        }
        for (wchar_t ch : component) {
            if (ch < L' ' || wcschr(L"<>:\"/\\|?*", ch) != nullptr) {
                return L"The path contains a character Windows does not allow in a file or folder name.";
            }
        }
        if (reserved_windows_filename(component)) {
            return L"The path uses a reserved Windows device name such as CON, NUL, COM1, or LPT1.";
        }
    }
    return std::nullopt;
}

std::filesystem::path nearest_existing_ancestor(
    std::filesystem::path path, std::error_code& error) {
    error.clear();
    const bool relative = path.is_relative();
    if (path.empty()) path = std::filesystem::current_path(error);
    while (!path.empty()) {
        if (std::filesystem::exists(path, error)) return error ? std::filesystem::path{} : path;
        if (error && error != std::errc::no_such_file_or_directory) {
            return {};
        }
        error.clear();
        const auto parent = path.parent_path();
        if (parent.empty() || parent == path) break;
        path = parent;
    }
    if (relative) {
        error.clear();
        const auto current = std::filesystem::current_path(error);
        if (!error && std::filesystem::is_directory(current, error) && !error) {
            return current;
        }
    }
    return {};
}

bool clipboard_matches_input_filter(HWND window, DialogInputFilter filter) {
    if (!OpenClipboard(window)) return false;
    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    const auto* text = handle == nullptr
        ? nullptr : static_cast<const wchar_t*>(GlobalLock(handle));
    bool valid = text != nullptr;
    if (text != nullptr) {
        for (const wchar_t* cursor = text; *cursor != L'\0'; ++cursor) {
            if (!input_character_allowed(filter, *cursor)) {
                valid = false;
                break;
            }
        }
        GlobalUnlock(handle);
    }
    CloseClipboard();
    return valid;
}

LRESULT CALLBACK typed_input_subclass_proc(HWND window, UINT message,
                                            WPARAM wparam, LPARAM lparam,
                                            UINT_PTR, DWORD_PTR reference) {
    const auto filter = static_cast<DialogInputFilter>(reference);
    if (message == WM_CHAR &&
        !input_character_allowed(filter, static_cast<wchar_t>(wparam))) {
        MessageBeep(MB_ICONWARNING);
        return 0;
    }
    if (message == WM_PASTE && !clipboard_matches_input_filter(window, filter)) {
        MessageBeep(MB_ICONWARNING);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, typed_input_subclass_proc,
                             kTypedInputSubclass);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

struct ModalOwnerState {
    HWND owner = nullptr;
    bool restored = false;
    bool activation_owned = false;
    bool close_requested = false;
    bool command_dispatch = false;
};

bool modal_owner_has_foreground(HWND owner, HWND dialog) {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        const HWND active = GetActiveWindow();
        return active == owner || (dialog != nullptr && active == dialog);
    }
    if (foreground == owner || foreground == dialog) return true;

    // Nested Axiom dialogs share the same root owner. Preserve activation when
    // one of them closes, but do not pull Axiom in front of an application the
    // user switched to while the modal window was open.
    const HWND owner_root = GetAncestor(owner, GA_ROOTOWNER);
    const HWND foreground_root = GetAncestor(foreground, GA_ROOTOWNER);
    return owner_root != nullptr && foreground_root == owner_root;
}

bool window_belongs_to_modal_chain(HWND owner, HWND window) {
    if (owner == nullptr || window == nullptr) return false;
    if (window == owner) return true;
    const HWND owner_root = GetAncestor(owner, GA_ROOTOWNER);
    return owner_root != nullptr &&
           GetAncestor(window, GA_ROOTOWNER) == owner_root;
}

void enable_modal_owner_for_close(HWND owner, bool restore_activation) {
    if (owner == nullptr || !IsWindow(owner)) return;
    EnableWindow(owner, TRUE);
    if (restore_activation) {
        // The public restore call runs after DestroyWindow has completely
        // returned. Remember that activation belonged to this modal chain so
        // the owner can be activated then, without repainting it underneath a
        // half-destroyed child.
        SetPropW(owner, kRestoreModalActivationProperty,
                 reinterpret_cast<HANDLE>(1));
    }
}

LRESULT CALLBACK modal_owner_subclass_proc(HWND window, UINT message,
                                            WPARAM wparam, LPARAM lparam,
                                            UINT_PTR subclass_id,
                                            DWORD_PTR reference_data) {
    auto* state = reinterpret_cast<ModalOwnerState*>(reference_data);
    const bool command_can_destroy =
        state != nullptr && message == WM_COMMAND;
    if (state != nullptr &&
        (message == WM_CLOSE ||
         (message == WM_SYSCOMMAND &&
          (wparam & 0xFFF0u) == SC_CLOSE))) {
        // Record the user's close request before DefSubclassProc enters the
        // dialog procedure. USER32 can nominate an unrelated window (often
        // Explorer beneath Axiom) during the nested DestroyWindow call.
        state->close_requested = true;
        state->activation_owned =
            state->activation_owned ||
            modal_owner_has_foreground(state->owner, window) ||
            GetActiveWindow() == window;
    }
    if (command_can_destroy) {
        // Most custom dialog buttons destroy their window synchronously from
        // WM_COMMAND. This flag is cleared after dispatch when the command was
        // non-closing, but protects activation throughout a nested destruction.
        state->command_dispatch = true;
        // A control notification carries its sender in lParam and therefore
        // represents interaction with this dialog before any close-time
        // activation messages have fired. Capture that stronger signal.
        if (lparam != 0 || GetActiveWindow() == window ||
            modal_owner_has_foreground(state->owner, window)) {
            state->activation_owned = true;
        }
    }
    if (state != nullptr && message == WM_ACTIVATE) {
        if (LOWORD(wparam) != WA_INACTIVE) {
            state->activation_owned = true;
        } else {
            const HWND next_active = reinterpret_cast<HWND>(lparam);
            // A close can deactivate the child directly to its owner (or
            // briefly report no successor). Preserve the recorded state in
            // those cases. A real switch to another top-level window clears it.
            if (!state->close_requested && !state->command_dispatch &&
                next_active != nullptr &&
                !window_belongs_to_modal_chain(state->owner, next_active)) {
                state->activation_owned = false;
            }
        }
    } else if (state != nullptr && message == WM_ACTIVATEAPP && wparam == FALSE &&
               !state->close_requested && !state->command_dispatch) {
        state->activation_owned = false;
    }
    if (message == WM_DESTROY && state != nullptr && !state->restored) {
        state->restored = true;
        enable_modal_owner_for_close(state->owner, state->activation_owned);
    }
    if (message == WM_NCDESTROY) {
        if (state != nullptr && !state->restored) {
            state->restored = true;
            enable_modal_owner_for_close(state->owner, state->activation_owned);
        }
        RemoveWindowSubclass(window, modal_owner_subclass_proc, subclass_id);
        delete state;
    }
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (command_can_destroy && IsWindow(window)) {
        DWORD_PTR current_reference = 0;
        if (GetWindowSubclass(window, modal_owner_subclass_proc, subclass_id,
                              &current_reference) &&
            current_reference == reference_data) {
            reinterpret_cast<ModalOwnerState*>(current_reference)
                ->command_dispatch = false;
        }
    }
    return result;
}

bool high_contrast_enabled() {
    return wimukthi::win32_theme::is_high_contrast();
}

COLORREF blend_color(COLORREF base, COLORREF overlay, int overlay_percent) {
    overlay_percent = std::clamp(overlay_percent, 0, 100);
    const int base_percent = 100 - overlay_percent;
    return RGB((GetRValue(base) * base_percent + GetRValue(overlay) * overlay_percent) / 100,
               (GetGValue(base) * base_percent + GetGValue(overlay) * overlay_percent) / 100,
               (GetBValue(base) * base_percent + GetBValue(overlay) * overlay_percent) / 100);
}

COLORREF readable_text_color(COLORREF background) {
    const int luminance = GetRValue(background) * 299 +
                          GetGValue(background) * 587 +
                          GetBValue(background) * 114;
    return luminance > 150000 ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

std::wstring lower_text(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return text;
}

bool contains_text(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

ToolbarIcon button_icon_for_label(HWND button) {
    wchar_t raw_text[256]{};
    GetWindowTextW(button, raw_text,
                   static_cast<int>(sizeof(raw_text) / sizeof(raw_text[0])));
    const std::wstring text = lower_text(raw_text);
    const int id = GetDlgCtrlID(button);
    if (id == IDCANCEL || contains_text(text, L"cancel") ||
        contains_text(text, L"close") || contains_text(text, L"no")) {
        return ToolbarIcon::cancel;
    }
    if (contains_text(text, L"browse") || contains_text(text, L"folder") ||
        contains_text(text, L"file") || contains_text(text, L"open")) {
        return ToolbarIcon::open;
    }
    if (contains_text(text, L"settings") || contains_text(text, L"options") ||
        contains_text(text, L"advanced")) {
        return ToolbarIcon::settings;
    }
    if (contains_text(text, L"default") || contains_text(text, L"refresh") ||
        contains_text(text, L"update")) {
        return ToolbarIcon::refresh;
    }
    if (contains_text(text, L"copy") || contains_text(text, L"info")) {
        return ToolbarIcon::info;
    }
    if (contains_text(text, L"pause")) {
        return ToolbarIcon::pause;
    }
    if (contains_text(text, L"start") || contains_text(text, L"resume") ||
        contains_text(text, L"yes") || id == IDYES) {
        return ToolbarIcon::resume;
    }
    if (id == IDOK || contains_text(text, L"ok") || contains_text(text, L"apply")) {
        return ToolbarIcon::test;
    }
    return ToolbarIcon::none;
}

COLORREF button_icon_color(COLORREF fallback, bool enabled) {
    if (!enabled) return fallback;
    if (dialog_icon_style() == 2) return dialog_accent_color();
    return fallback;
}

ToolbarIconStyle button_icon_style(bool enabled) {
    return enabled && dialog_icon_style() == 1
        ? ToolbarIconStyle::colorful
        : ToolbarIconStyle::monochrome;
}

void save_window_style(HWND window) {
    if (window == nullptr || GetPropW(window, kSavedComboStyleProperty) != nullptr) {
        return;
    }
    const auto style = static_cast<UINT_PTR>(GetWindowLongPtrW(window, GWL_STYLE));
    const auto ex_style = static_cast<UINT_PTR>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    SetPropW(window, kSavedComboStyleProperty,
             reinterpret_cast<HANDLE>(style + 1));
    SetPropW(window, kSavedComboExStyleProperty,
             reinterpret_cast<HANDLE>(ex_style + 1));
}

void strip_light_control_edges(HWND window) {
    if (window == nullptr) return;
    save_window_style(window);
    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    style &= ~static_cast<LONG_PTR>(WS_BORDER);
    ex_style &= ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE |
                                       WS_EX_WINDOWEDGE);
    SetWindowLongPtrW(window, GWL_STYLE, style);
    SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void restore_control_edges(HWND window) {
    if (window == nullptr) return;
    const HANDLE saved_style = GetPropW(window, kSavedComboStyleProperty);
    const HANDLE saved_ex_style = GetPropW(window, kSavedComboExStyleProperty);
    if (saved_style == nullptr || saved_ex_style == nullptr) return;

    SetWindowLongPtrW(window, GWL_STYLE,
                      static_cast<LONG_PTR>(
                          reinterpret_cast<UINT_PTR>(saved_style) - 1));
    SetWindowLongPtrW(window, GWL_EXSTYLE,
                      static_cast<LONG_PTR>(
                          reinterpret_cast<UINT_PTR>(saved_ex_style) - 1));
    RemovePropW(window, kSavedComboStyleProperty);
    RemovePropW(window, kSavedComboExStyleProperty);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

struct MonitorSearchState {
    RECT rect{};
    bool visible = false;
};

BOOL CALLBACK monitor_visibility_proc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* state = reinterpret_cast<MonitorSearchState*>(param);
    if (state == nullptr || state->visible) return FALSE;
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    RECT intersection{};
    if (IntersectRect(&intersection, &state->rect, &info.rcWork) &&
        intersection.right - intersection.left >= 64 &&
        intersection.bottom - intersection.top >= 64) {
        state->visible = true;
        return FALSE;
    }
    return TRUE;
}

bool rect_is_visible_on_connected_monitor(const RECT& rect) {
    if (rect.right - rect.left < 64 || rect.bottom - rect.top < 64) return false;
    MonitorSearchState state{rect, false};
    EnumDisplayMonitors(nullptr, nullptr, monitor_visibility_proc,
                        reinterpret_cast<LPARAM>(&state));
    return state.visible;
}

RECT owner_or_primary_work_area(HWND owner) {
    HMONITOR monitor = nullptr;
    if (owner != nullptr && IsWindow(owner)) {
        monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    }
    if (monitor == nullptr) {
        POINT origin{0, 0};
        monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }

    MONITORINFO info{sizeof(info)};
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

std::wstring layout_registry_path(std::wstring_view name) {
    std::wstring path = kWindowLayoutsRegistryPath;
    path.push_back(L'\\');
    path.append(name);
    return path;
}

bool read_window_placement(std::wstring_view name, WINDOWPLACEMENT& placement,
                           UINT& saved_dpi) {
    const std::wstring path = layout_registry_path(name);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    placement.length = sizeof(WINDOWPLACEMENT);
    DWORD type = 0;
    DWORD size = sizeof(WINDOWPLACEMENT);
    const LSTATUS status = RegQueryValueExW(
        key, L"WindowPlacement", nullptr, &type,
        reinterpret_cast<BYTE*>(&placement), &size);
    DWORD dpi_type = 0;
    DWORD dpi_size = sizeof(saved_dpi);
    if (RegQueryValueExW(key, L"WindowDpi", nullptr, &dpi_type,
                         reinterpret_cast<BYTE*>(&saved_dpi), &dpi_size) != ERROR_SUCCESS ||
        dpi_type != REG_DWORD || dpi_size != sizeof(saved_dpi)) {
        saved_dpi = 0;
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_BINARY || size != sizeof(WINDOWPLACEMENT)) {
        return false;
    }
    placement.length = sizeof(WINDOWPLACEMENT);
    return true;
}

void apply_combo_child_theme(HWND combo, bool dark) {
    COMBOBOXINFO info{sizeof(info)};
    if (!GetComboBoxInfo(combo, &info)) return;
    if (dark) {
        strip_light_control_edges(combo);
    } else {
        restore_control_edges(combo);
    }
    if (info.hwndList != nullptr) {
        if (dark) {
            wimukthi::win32_theme::apply_control(info.hwndList, nullptr);
            strip_light_control_edges(info.hwndList);
        } else {
            restore_control_edges(info.hwndList);
            wimukthi::win32_theme::apply_control(info.hwndList, nullptr);
        }
        if (IsWindowVisible(info.hwndList)) {
            RedrawWindow(info.hwndList, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                             RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
    }
    if (info.hwndItem != nullptr && info.hwndItem != combo) {
        if (dark) {
            wimukthi::win32_theme::apply_control(info.hwndItem, nullptr);
            strip_light_control_edges(info.hwndItem);
        } else {
            restore_control_edges(info.hwndItem);
            wimukthi::win32_theme::apply_control(info.hwndItem, nullptr);
        }
        if (IsWindowVisible(info.hwndItem)) {
            RedrawWindow(info.hwndItem, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                             RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
    }
}

bool combo_is_dropdown_list(HWND window) {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    return (style & CBS_DROPDOWNLIST) == CBS_DROPDOWNLIST;
}

RECT dark_combo_arrow_rect(HWND window) {
    RECT client{};
    if (!GetClientRect(window, &client)) return {};

    const UINT dpi = GetDpiForWindow(window);
    const int arrow_width = (std::max)(
        scale_for_dialog_dpi(18, dpi), GetSystemMetricsForDpi(SM_CXVSCROLL, dpi));
    return RECT{
        (std::max)(client.left + 1, client.right - arrow_width),
        client.top + 1,
        client.right - 1,
        client.bottom - 1,
    };
}

void paint_dark_combo(HWND window, HDC dc) {
    RECT client{};
    if (!GetClientRect(window, &client)) return;

    const DialogColors colors = dialog_colors(true);
    const UINT dpi = GetDpiForWindow(window);
    const RECT arrow = dark_combo_arrow_rect(window);

    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(window, &cursor);
    const bool hot = PtInRect(&arrow, cursor) != FALSE;
    const bool pressed = hot && (GetKeyState(VK_LBUTTON) & 0x8000) != 0;
    const COLORREF arrow_background = pressed
        ? RGB(58, 58, 59)
        : hot ? RGB(48, 48, 49) : colors.control_background;

    if (combo_is_dropdown_list(window)) {
        HBRUSH background = CreateSolidBrush(colors.control_background);
        FillRect(dc, &client, background);
        DeleteObject(background);

        std::wstring text;
        const LRESULT selected = SendMessageW(window, CB_GETCURSEL, 0, 0);
        if (selected != CB_ERR) {
            const LRESULT length = SendMessageW(window, CB_GETLBTEXTLEN,
                                                static_cast<WPARAM>(selected), 0);
            if (length >= 0) {
                text.resize(static_cast<std::size_t>(length) + 1);
                SendMessageW(window, CB_GETLBTEXT, static_cast<WPARAM>(selected),
                             reinterpret_cast<LPARAM>(text.data()));
                text.resize(static_cast<std::size_t>(length));
            }
        }
        if (text.empty()) {
            const int length = GetWindowTextLengthW(window);
            if (length > 0) {
                text.resize(static_cast<std::size_t>(length) + 1);
                GetWindowTextW(window, text.data(), length + 1);
                text.resize(static_cast<std::size_t>(length));
            }
        }

        HFONT font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        HGDIOBJ old_font = font != nullptr ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, IsWindowEnabled(window) ? colors.text : colors.disabled_text);
        RECT text_rect{
            client.left + scale_for_dialog_dpi(7, dpi),
            client.top + 1,
            arrow.left - scale_for_dialog_dpi(4, dpi),
            client.bottom - 1,
        };
        DrawTextW(dc, text.c_str(), -1, &text_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (old_font != nullptr) SelectObject(dc, old_font);
    }

    HBRUSH fill = CreateSolidBrush(arrow_background);
    FillRect(dc, &arrow, fill);
    DeleteObject(fill);

    HBRUSH control_fill = CreateSolidBrush(colors.control_background);
    RECT top_edge{client.left + 1, client.top + 1, arrow.left, client.top + 2};
    RECT left_edge{client.left + 1, client.top + 1, client.left + 2, client.bottom - 1};
    RECT bottom_edge{client.left + 1, client.bottom - 2, arrow.left, client.bottom - 1};
    RECT separator_erase{
        arrow.left - scale_for_dialog_dpi(2, dpi),
        client.top + 1,
        arrow.left + scale_for_dialog_dpi(2, dpi),
        client.bottom - 1,
    };
    FillRect(dc, &top_edge, control_fill);
    FillRect(dc, &left_edge, control_fill);
    FillRect(dc, &bottom_edge, control_fill);
    FillRect(dc, &separator_erase, control_fill);
    DeleteObject(control_fill);
    fill = CreateSolidBrush(arrow_background);
    RECT arrow_after_separator{
        arrow.left + 1,
        arrow.top,
        arrow.right,
        arrow.bottom,
    };
    FillRect(dc, &arrow_after_separator, fill);
    DeleteObject(fill);

    HPEN border_pen = CreatePen(PS_SOLID, 1, colors.border);
    HGDIOBJ old_pen = SelectObject(dc, border_pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, client.left, client.top, client.right, client.bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border_pen);

    HBRUSH separator_brush = CreateSolidBrush(colors.border);
    RECT separator{
        arrow.left,
        arrow.top,
        arrow.left + 1,
        arrow.bottom,
    };
    FillRect(dc, &separator, separator_brush);
    DeleteObject(separator_brush);

    const int half_width = scale_for_dialog_dpi(4, dpi);
    const int half_height = scale_for_dialog_dpi(2, dpi);
    const int center_x = arrow.left + (arrow.right - arrow.left) / 2;
    const int center_y = arrow.top + (arrow.bottom - arrow.top) / 2;
    POINT triangle[3]{
        {center_x - half_width, center_y - half_height},
        {center_x + half_width, center_y - half_height},
        {center_x, center_y + half_height},
    };
    HBRUSH arrow_brush = CreateSolidBrush(
        IsWindowEnabled(window) ? colors.text : colors.disabled_text);
    old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    old_brush = SelectObject(dc, arrow_brush);
    Polygon(dc, triangle, 3);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(arrow_brush);
}

void draw_dark_combo_frame(HWND window) {
    HDC dc = GetDC(window);
    if (dc == nullptr) return;
    paint_dark_combo(window, dc);
    ReleaseDC(window, dc);
}

void redraw_dark_combo_now(HWND window) {
    if (window == nullptr || !IsWindow(window) || !IsWindowVisible(window)) return;
    RedrawWindow(window, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW | RDW_NOERASE);
    draw_dark_combo_frame(window);
}

LRESULT CALLBACK dark_combo_subclass_proc(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam,
                                          UINT_PTR subclass_id,
                                          DWORD_PTR reference_data) {
    switch (message) {
        case CB_SHOWDROPDOWN: {
            const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
            apply_combo_child_theme(window, reference_data != 0);
            if (reference_data != 0) redraw_dark_combo_now(window);
            return result;
        }
        case WM_PAINT: {
            if (reference_data != 0 && combo_is_dropdown_list(window)) {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(window, &paint);
                if (dc != nullptr) paint_dark_combo(window, dc);
                EndPaint(window, &paint);
                return 0;
            }
            const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
            if (reference_data != 0) draw_dark_combo_frame(window);
            return result;
        }
        case WM_ERASEBKGND:
            if (reference_data != 0 && combo_is_dropdown_list(window)) {
                return 1;
            }
            break;
        case WM_NCPAINT: {
            if (reference_data != 0) {
                draw_dark_combo_frame(window);
                return 0;
            }
            const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
            return result;
        }
        case WM_MOUSEMOVE: {
            if (reference_data != 0) {
                if (GetPropW(window, kDarkComboTrackingProperty) == nullptr) {
                    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
                    if (TrackMouseEvent(&tracking)) {
                        SetPropW(window, kDarkComboTrackingProperty,
                                 reinterpret_cast<HANDLE>(1));
                    }
                }
                const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                const RECT arrow = dark_combo_arrow_rect(window);
                const bool hot = PtInRect(&arrow, point) != FALSE;
                const bool was_hot = GetPropW(window, kDarkComboHotProperty) != nullptr;
                if (hot != was_hot) {
                    if (hot) {
                        SetPropW(window, kDarkComboHotProperty,
                                 reinterpret_cast<HANDLE>(1));
                    } else {
                        RemovePropW(window, kDarkComboHotProperty);
                    }
                    InvalidateRect(window, &arrow, FALSE);
                }
                return 0;
            }
            break;
        }
        case WM_MOUSELEAVE: {
            const bool was_hot = RemovePropW(window, kDarkComboHotProperty) != nullptr;
            RemovePropW(window, kDarkComboTrackingProperty);
            if (reference_data != 0 && was_hot) {
                const RECT arrow = dark_combo_arrow_rect(window);
                InvalidateRect(window, &arrow, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_ENABLE:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_THEMECHANGED: {
            const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
            if (reference_data != 0) redraw_dark_combo_now(window);
            return result;
        }
        case WM_NCDESTROY:
            RemovePropW(window, kDarkComboHotProperty);
            RemovePropW(window, kDarkComboTrackingProperty);
            RemoveWindowSubclass(window, dark_combo_subclass_proc, subclass_id);
            break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

}  // namespace

int scale_for_dialog_dpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi),
                  USER_DEFAULT_SCREEN_DPI);
}

HFONT create_dialog_font(UINT dpi) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    BOOL loaded = SystemParametersInfoForDpi(
        SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
        dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi);
    if (!loaded) {
        loaded = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    }
    HFONT font = loaded ? CreateFontIndirectW(&metrics.lfMessageFont) : nullptr;
    return font != nullptr ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

void delete_dialog_font(HFONT font) {
    if (font != nullptr && font != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(font);
    }
}

HICON load_axiom_icon(HINSTANCE instance, int width, int height) {
    HICON icon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_AXIOM), IMAGE_ICON, width, height,
        LR_DEFAULTCOLOR | LR_SHARED));
    return icon != nullptr ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

void assign_axiom_window_class_icons(WNDCLASSEXW& window_class, HINSTANCE instance) {
    window_class.hIcon = load_axiom_icon(instance, GetSystemMetrics(SM_CXICON),
                                         GetSystemMetrics(SM_CYICON));
    window_class.hIconSm = load_axiom_icon(instance, GetSystemMetrics(SM_CXSMICON),
                                           GetSystemMetrics(SM_CYSMICON));
}

void apply_axiom_window_icons(HWND window, HINSTANCE instance) {
    const UINT dpi = GetDpiForWindow(window);
    SendMessageW(window, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(load_axiom_icon(
                     instance, GetSystemMetricsForDpi(SM_CXICON, dpi),
                     GetSystemMetricsForDpi(SM_CYICON, dpi))));
    SendMessageW(window, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(load_axiom_icon(
                     instance, GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                     GetSystemMetricsForDpi(SM_CYSMICON, dpi))));
}

DialogColors dialog_colors(bool dark) {
    if (high_contrast_enabled()) {
        return {
            GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT),
            GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_HIGHLIGHT),
            GetSysColor(COLOR_HIGHLIGHTTEXT), GetSysColor(COLOR_GRAYTEXT),
            GetSysColor(COLOR_WINDOWFRAME), GetSysColor(COLOR_HOTLIGHT),
        };
    }
    const COLORREF accent = dialog_accent_color();
    if (dark) {
        const COLORREF selection = blend_color(RGB(31, 31, 31), accent, 42);
        return {
            RGB(31, 31, 31), RGB(241, 241, 241), RGB(37, 37, 38),
            selection, readable_text_color(selection),
            RGB(150, 150, 150), RGB(64, 64, 64), accent,
        };
    }
    return {
        RGB(250, 250, 250), RGB(32, 32, 32), RGB(255, 255, 255),
        accent, readable_text_color(accent), RGB(105, 105, 105),
        RGB(200, 200, 200), accent,
    };
}

SIZE dialog_window_size_for_client(int logical_width, int logical_height,
                                   DWORD style, DWORD extended_style, UINT dpi) {
    const UINT effective_dpi = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
    RECT rect{0, 0,
              scale_for_dialog_dpi(logical_width, effective_dpi),
              scale_for_dialog_dpi(logical_height, effective_dpi)};
    AdjustWindowRectExForDpi(&rect, style, FALSE, extended_style, effective_dpi);
    return {rect.right - rect.left, rect.bottom - rect.top};
}

bool dialog_system_prefers_dark_mode() {
    return wimukthi::win32_theme::system_prefers_dark();
}

bool dialog_high_contrast_enabled() {
    return wimukthi::win32_theme::is_high_contrast();
}

namespace {

void configure_windows_theme() {
    using namespace wimukthi::win32_theme;

    const bool palette_dark =
        g_dialog_appearance.theme_mode == 1 ||
        (g_dialog_appearance.theme_mode == 0 && system_prefers_dark());
    const DialogColors colors = dialog_colors(palette_dark);
    const COLORREF hot_background =
        blend_color(colors.control_background, colors.focus_border,
                    palette_dark ? 12 : 8);

    Configuration configuration;
    configuration.mode =
        g_dialog_appearance.theme_mode == 1 ? Mode::dark
        : g_dialog_appearance.theme_mode == 2 ? Mode::light
                                               : Mode::system;
    configuration.use_custom_palette = !is_high_contrast();
    configuration.palette = {
        colors.background,
        colors.control_background,
        hot_background,
        colors.background,
        palette_dark ? RGB(82, 38, 38) : RGB(255, 240, 240),
        colors.text,
        colors.disabled_text,
        colors.disabled_text,
        colors.focus_border,
        colors.border,
        colors.focus_border,
        colors.border,
        colors.selection_background,
        colors.control_background,
        colors.text,
        colors.border,
        colors.control_background,
        hot_background,
        colors.text,
        colors.border,
    };
    configure(configuration);
}

}  // namespace

bool handle_dialog_theme_setting_change(LPARAM lparam) {
    return wimukthi::win32_theme::handle_setting_change(lparam);
}

void set_dialog_appearance(const DialogAppearance& appearance) {
    g_dialog_appearance = appearance;
    g_dialog_appearance.theme_mode = std::clamp(g_dialog_appearance.theme_mode, 0, 2);
    g_dialog_appearance.accent_color_mode =
        std::clamp(g_dialog_appearance.accent_color_mode, 0, 6);
    g_dialog_appearance.icon_style = std::clamp(g_dialog_appearance.icon_style, 0, 2);
    configure_windows_theme();
}

DialogAppearance dialog_appearance() {
    return g_dialog_appearance;
}

bool dialog_should_use_dark() {
    configure_windows_theme();
    return wimukthi::win32_theme::is_dark();
}

COLORREF resolve_dialog_accent_color(int mode, COLORREF custom_color) {
    if (high_contrast_enabled()) {
        return GetSysColor(COLOR_HIGHLIGHT);
    }
    switch (std::clamp(mode, 0, 6)) {
        case 1: return RGB(255, 185, 60);
        case 2: return RGB(83, 174, 255);
        case 3: return RGB(96, 205, 112);
        case 4: return RGB(180, 143, 255);
        case 5: return RGB(255, 99, 99);
        case 6: return custom_color;
        case 0:
        default:
            break;
    }

    DWORD colorization = 0;
    BOOL opaque_blend = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&colorization, &opaque_blend))) {
        return RGB((colorization >> 16) & 0xff,
                   (colorization >> 8) & 0xff,
                   colorization & 0xff);
    }
    return GetSysColor(COLOR_HIGHLIGHT);
}

COLORREF dialog_accent_color() {
    return resolve_dialog_accent_color(g_dialog_appearance.accent_color_mode,
                                       g_dialog_appearance.custom_accent_color);
}

int dialog_icon_style() {
    return g_dialog_appearance.icon_style;
}

void apply_dialog_dark_frame(HWND window, bool dark) {
    configure_windows_theme();

    // DWM owns the caption and resize border.  On Windows 11 it can animate
    // those surfaces from the system's default (usually light) colors even
    // when immersive dark mode was set before the first ShowWindow call.  Set
    // every non-client color explicitly and suppress that transition before
    // asking the shared theme layer to install its remaining attributes.  This
    // keeps the first composed frame dark instead of briefly exposing a white
    // title bar or border.
    const BOOL transitions_disabled = TRUE;
    DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED,
                          &transitions_disabled,
                          sizeof(transitions_disabled));

    if (!high_contrast_enabled()) {
        const DialogColors colors = dialog_colors(dark);
        const COLORREF caption_color = colors.background;
        const COLORREF border_color = colors.border;
        const COLORREF text_color = colors.text;
        DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR,
                              &caption_color, sizeof(caption_color));
        DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR,
                              &border_color, sizeof(border_color));
        DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR,
                              &text_color, sizeof(text_color));
    }

    wimukthi::win32_theme::apply_title_bar(window);
}

void apply_dialog_control_theme(HWND control, bool dark) {
    if (control != nullptr) {
        wchar_t class_name[32]{};
        GetClassNameW(control, class_name,
                      static_cast<int>(sizeof(class_name) / sizeof(class_name[0])));
        const bool combo_box = lstrcmpiW(class_name, L"ComboBox") == 0;
        if (combo_box) {
            if (dark) {
                wimukthi::win32_theme::apply_theme_class(control, L"", nullptr);
                SetWindowSubclass(control, dark_combo_subclass_proc,
                                  kDarkComboSubclass, 1);
                apply_combo_child_theme(control, true);
            } else {
                // Remove the custom painter before resetting the native theme.
                // Resetting the native theme synchronously sends
                // WM_THEMECHANGED. Remove the custom painter first so it cannot
                // paint one final dark frame over the restored light combo.
                SendMessageW(control, CB_SHOWDROPDOWN, FALSE, 0);
                RemoveWindowSubclass(control, dark_combo_subclass_proc,
                                     kDarkComboSubclass);
                RemovePropW(control, kDarkComboHotProperty);
                RemovePropW(control, kDarkComboTrackingProperty);
                apply_combo_child_theme(control, false);
                wimukthi::win32_theme::apply_theme_class(control, L"", nullptr);
            }
        } else {
            wimukthi::win32_theme::apply_control(control);
        }
        if (IsWindowVisible(control)) {
            RedrawWindow(control, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                             RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
    }
}

void set_dialog_control_font(HWND control, HFONT font) {
    if (control != nullptr && font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

TooltipManager::~TooltipManager() {
    destroy();
}

bool TooltipManager::create(HWND owner, UINT layout_dpi, bool dark) {
    destroy();
    if (owner == nullptr) return false;
    owner_ = owner;
    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (hwnd_ == nullptr) {
        owner_ = nullptr;
        return false;
    }
    SetWindowSubclass(owner_, &TooltipManager::owner_subclass_proc,
                      reinterpret_cast<UINT_PTR>(this),
                      reinterpret_cast<DWORD_PTR>(this));
    relay_windows_.push_back(owner_);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(hwnd_, TTM_SETDELAYTIME, TTDT_INITIAL, 500);
    SendMessageW(hwnd_, TTM_SETDELAYTIME, TTDT_RESHOW, 100);
    SendMessageW(hwnd_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
    update_dpi(layout_dpi);
    apply_dialog_control_theme(hwnd_, dark);
    return true;
}

bool TooltipManager::add(HWND control, const wchar_t* text) {
    if (hwnd_ == nullptr || control == nullptr || text == nullptr ||
        *text == L'\0') {
        return false;
    }
    // Plain STATIC controls normally return HTTRANSPARENT, so the parent
    // receives their mouse input and TTF_SUBCLASS never sees a hover. SS_NOTIFY
    // makes labels and status text behave as tooltip tools without repainting.
    wchar_t class_name[32]{};
    if (GetClassNameW(control, class_name,
                      static_cast<int>(std::size(class_name))) > 0 &&
        lstrcmpiW(class_name, L"STATIC") == 0) {
        const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
        SetWindowLongPtrW(control, GWL_STYLE, style | SS_NOTIFY);
    }

    TOOLINFOW tool{};
    // Axiom uses the system common-controls activation context. Its tooltip
    // control needs the documented v1 size, whose fields cover this manager.
    tool.cbSize = TTTOOLINFOW_V1_SIZE;
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = owner_;
    tool.uId = reinterpret_cast<UINT_PTR>(control);
    tool.lpszText = const_cast<wchar_t*>(text);
    const bool child_tool_added =
        SendMessageW(hwnd_, TTM_ADDTOOLW, 0,
                     reinterpret_cast<LPARAM>(&tool)) != FALSE;

    // Disabled child windows cannot activate HWND-based tools. Register the
    // same text as a rectangle on the enabled owner and relay owner mouse input
    // so unavailable controls remain self-explanatory.
    ToolEntry entry{control, overlay_id(control)};
    TOOLINFOW overlay{};
    overlay.cbSize = TTTOOLINFOW_V1_SIZE;
    overlay.hwnd = owner_;
    overlay.uId = entry.id;
    overlay.lpszText = const_cast<wchar_t*>(text);
    update_rect(overlay, control);
    const bool overlay_added =
        SendMessageW(hwnd_, TTM_ADDTOOLW, 0,
                     reinterpret_cast<LPARAM>(&overlay)) != FALSE;
    if (overlay_added) tools_.push_back(entry);
    attach_relay_chain(control);
    bool nested_edit_added = false;
    if (lstrcmpiW(class_name, L"COMBOBOX") == 0) {
        COMBOBOXINFO info{sizeof(info)};
        if (GetComboBoxInfo(control, &info) && info.hwndItem != nullptr &&
            info.hwndItem != control) {
            // Editable combo boxes route pointer input to their inner EDIT.
            // Register it with the same explanation so the tooltip does not
            // disappear over the part where the user actually types.
            nested_edit_added = add(info.hwndItem, text);
        }
    }
    return child_tool_added || overlay_added || nested_edit_added;
}

void TooltipManager::remove(HWND control) {
    if (hwnd_ == nullptr || control == nullptr) return;
    wchar_t class_name[32]{};
    if (GetClassNameW(control, class_name,
                      static_cast<int>(std::size(class_name))) > 0 &&
        lstrcmpiW(class_name, L"COMBOBOX") == 0) {
        COMBOBOXINFO info{sizeof(info)};
        if (GetComboBoxInfo(control, &info) && info.hwndItem != nullptr &&
            info.hwndItem != control) {
            remove(info.hwndItem);
        }
    }
    TOOLINFOW tool{};
    tool.cbSize = TTTOOLINFOW_V1_SIZE;
    tool.uFlags = TTF_IDISHWND;
    tool.hwnd = owner_;
    tool.uId = reinterpret_cast<UINT_PTR>(control);
    SendMessageW(hwnd_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));

    const auto entry = std::find_if(
        tools_.begin(), tools_.end(),
        [control](const ToolEntry& value) { return value.control == control; });
    if (entry != tools_.end()) {
        TOOLINFOW overlay{};
        overlay.cbSize = TTTOOLINFOW_V1_SIZE;
        overlay.hwnd = owner_;
        overlay.uId = entry->id;
        SendMessageW(hwnd_, TTM_DELTOOLW, 0,
                     reinterpret_cast<LPARAM>(&overlay));
        tools_.erase(entry);
    }
}

void TooltipManager::update_dpi(UINT layout_dpi) {
    if (hwnd_ == nullptr) return;
    const UINT dpi = layout_dpi == 0 ? USER_DEFAULT_SCREEN_DPI : layout_dpi;
    SendMessageW(hwnd_, TTM_SETMAXTIPWIDTH, 0,
                 static_cast<LPARAM>(MulDiv(460, static_cast<int>(dpi), 96)));
    update_layout();
}

void TooltipManager::update_layout() const {
    if (hwnd_ == nullptr || owner_ == nullptr) return;
    for (const ToolEntry& entry : tools_) {
        TOOLINFOW overlay{};
        overlay.cbSize = TTTOOLINFOW_V1_SIZE;
        overlay.hwnd = owner_;
        overlay.uId = entry.id;
        update_rect(overlay, entry.control);
        SendMessageW(hwnd_, TTM_NEWTOOLRECTW, 0,
                     reinterpret_cast<LPARAM>(&overlay));
    }
}

void TooltipManager::apply_theme(bool dark) {
    if (hwnd_ != nullptr) apply_dialog_control_theme(hwnd_, dark);
}

void TooltipManager::destroy() {
    for (HWND relay_window : relay_windows_) {
        if (relay_window != nullptr && IsWindow(relay_window)) {
            RemoveWindowSubclass(relay_window,
                                 &TooltipManager::owner_subclass_proc,
                                 reinterpret_cast<UINT_PTR>(this));
        }
    }
    if (hwnd_ != nullptr && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    owner_ = nullptr;
    tools_.clear();
    relay_windows_.clear();
}

UINT_PTR TooltipManager::overlay_id(HWND control) {
    constexpr UINT_PTR high_bit =
        static_cast<UINT_PTR>(1) << (sizeof(UINT_PTR) * 8 - 1);
    return reinterpret_cast<UINT_PTR>(control) ^ high_bit;
}

void TooltipManager::attach_relay_chain(HWND control) {
    if (control == nullptr || owner_ == nullptr) return;
    for (HWND parent = GetParent(control);
         parent != nullptr && parent != owner_;
         parent = GetParent(parent)) {
        if (std::find(relay_windows_.begin(), relay_windows_.end(), parent) !=
            relay_windows_.end()) {
            continue;
        }
        if (SetWindowSubclass(parent, &TooltipManager::owner_subclass_proc,
                              reinterpret_cast<UINT_PTR>(this),
                              reinterpret_cast<DWORD_PTR>(this))) {
            relay_windows_.push_back(parent);
        }
    }
}

void TooltipManager::update_rect(TOOLINFOW& tool, HWND control) const {
    RECT rect{};
    if (control != nullptr && IsWindow(control) &&
        (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0 &&
        GetWindowRect(control, &rect)) {
        MapWindowPoints(HWND_DESKTOP, owner_, reinterpret_cast<POINT*>(&rect), 2);
    }
    tool.rect = rect;
}

LRESULT CALLBACK TooltipManager::owner_subclass_proc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR subclass_id, DWORD_PTR reference) {
    auto* self = reinterpret_cast<TooltipManager*>(reference);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, &TooltipManager::owner_subclass_proc,
                             subclass_id);
        if (self != nullptr) {
            self->relay_windows_.erase(
                std::remove(self->relay_windows_.begin(),
                            self->relay_windows_.end(), hwnd),
                self->relay_windows_.end());
            if (self->owner_ == hwnd) self->owner_ = nullptr;
        }
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }
    if (message == WM_SIZE) {
        const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
        if (self != nullptr) self->update_layout();
        return result;
    }
    if (self != nullptr && self->hwnd_ != nullptr && IsWindow(self->hwnd_)) {
        switch (message) {
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP: {
                POINT client_point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                if (self->owner_ != nullptr && hwnd != self->owner_) {
                    MapWindowPoints(hwnd, self->owner_, &client_point, 1);
                }
                MSG relay{};
                relay.hwnd = self->owner_;
                relay.message = message;
                relay.wParam = wparam;
                relay.lParam = MAKELPARAM(
                    static_cast<short>(client_point.x),
                    static_cast<short>(client_point.y));
                relay.time = static_cast<DWORD>(GetMessageTime());
                const DWORD cursor = GetMessagePos();
                relay.pt.x = static_cast<short>(LOWORD(cursor));
                relay.pt.y = static_cast<short>(HIWORD(cursor));
                SendMessageW(self->hwnd_, TTM_RELAYEVENT, 0,
                             reinterpret_cast<LPARAM>(&relay));
                break;
            }
            default:
                break;
        }
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void apply_dialog_input_filter(HWND control, DialogInputFilter filter,
                               UINT maximum_characters) {
    if (control == nullptr) return;
    HWND edit = control;
    wchar_t class_name[32]{};
    GetClassNameW(control, class_name,
                  static_cast<int>(sizeof(class_name) / sizeof(class_name[0])));
    if (lstrcmpiW(class_name, L"ComboBox") == 0) {
        COMBOBOXINFO info{sizeof(info)};
        if (!GetComboBoxInfo(control, &info) || info.hwndItem == nullptr) return;
        edit = info.hwndItem;
    }
    if (maximum_characters != 0) {
        SendMessageW(edit, EM_SETLIMITTEXT, maximum_characters, 0);
    }
    SetWindowSubclass(edit, typed_input_subclass_proc, kTypedInputSubclass,
                      static_cast<DWORD_PTR>(filter));
}

std::wstring trim_dialog_input(std::wstring text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    }).base();
    if (first >= last) return {};
    return std::wstring(first, last);
}

DialogPathValidation validate_dialog_path(
    std::wstring text, DialogPathKind kind) {
    text = trim_dialog_input(std::move(text));
    if (text.empty()) {
        return {{}, L"Enter a path."};
    }

    std::filesystem::path path(text);
    if (const auto invalid = invalid_windows_path_message(path)) {
        return {std::move(path), *invalid};
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return {std::move(path),
                L"Windows could not inspect this path. Check that the location is accessible."};
    }
    error.clear();

    if (kind == DialogPathKind::existing_file) {
        if (!exists || !std::filesystem::is_regular_file(path, error) || error) {
            return {std::move(path), L"Choose an existing file."};
        }
        return {std::move(path), {}};
    }
    if (kind == DialogPathKind::existing_folder) {
        if (!exists || !std::filesystem::is_directory(path, error) || error) {
            return {std::move(path), L"Choose an existing folder."};
        }
        return {std::move(path), {}};
    }

    if (kind == DialogPathKind::output_file) {
        if (path.filename().empty()) {
            return {std::move(path),
                    L"Enter a file name as well as a destination folder."};
        }
        if (exists && std::filesystem::is_directory(path, error)) {
            return {std::move(path),
                    L"The output path names an existing folder, not a file."};
        }
        if (error) {
            return {std::move(path), L"Windows could not inspect the output path."};
        }
        if (exists) {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
                return {std::move(path),
                        L"The existing output file is read-only."};
            }
        }
        std::filesystem::path parent = path.parent_path();
        if (parent.empty()) parent = std::filesystem::current_path(error);
        if (error || parent.empty() ||
            !std::filesystem::is_directory(parent, error) || error) {
            return {std::move(path),
                    L"The output file's parent folder does not exist or cannot be accessed."};
        }
        return {std::move(path), {}};
    }

    if (exists) {
        if (!std::filesystem::is_directory(path, error) || error) {
            return {std::move(path),
                    L"The destination path names a file, not a folder."};
        }
        return {std::move(path), {}};
    }

    const auto ancestor = nearest_existing_ancestor(path, error);
    if (error || ancestor.empty() ||
        !std::filesystem::is_directory(ancestor, error) || error) {
        return {std::move(path),
                L"No accessible parent folder exists for this destination."};
    }
    return {std::move(path), {}};
}

void draw_dialog_button(const DRAWITEMSTRUCT& draw, bool dark) {
    if (draw.CtlType != ODT_BUTTON || draw.hDC == nullptr || draw.hwndItem == nullptr) {
        return;
    }

    const DialogColors colors = dialog_colors(dark);
    const bool enabled = (draw.itemState & ODS_DISABLED) == 0;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;
    COLORREF fill = colors.control_background;
    if (pressed) {
        fill = dark ? blend_color(colors.control_background, colors.focus_border, 18)
                    : blend_color(colors.control_background, colors.focus_border, 16);
    } else if (hot || focused) {
        fill = dark ? blend_color(colors.control_background, colors.focus_border, 10)
                    : blend_color(colors.control_background, colors.focus_border, 8);
    }

    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(draw.hDC, &draw.rcItem, brush);
    DeleteObject(brush);

    const COLORREF border_color = focused ? colors.focus_border : colors.border;
    HPEN pen = CreatePen(PS_SOLID, 1, border_color);
    HGDIOBJ old_pen = SelectObject(draw.hDC, pen);
    HGDIOBJ old_brush = SelectObject(draw.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(draw.hDC, draw.rcItem.left, draw.rcItem.top,
              draw.rcItem.right, draw.rcItem.bottom);
    SelectObject(draw.hDC, old_brush);
    SelectObject(draw.hDC, old_pen);
    DeleteObject(pen);

    wchar_t text[256]{};
    GetWindowTextW(draw.hwndItem, text,
                   static_cast<int>(sizeof(text) / sizeof(text[0])));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = font != nullptr ? SelectObject(draw.hDC, font) : nullptr;
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, enabled ? colors.text : colors.disabled_text);
    RECT text_rect = draw.rcItem;
    if (pressed) OffsetRect(&text_rect, 1, 1);
    const ToolbarIcon icon = button_icon_for_label(draw.hwndItem);
    const UINT dpi = GetDpiForWindow(draw.hwndItem);
    if (icon == ToolbarIcon::none) {
        DrawTextW(draw.hDC, text, -1, &text_rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        SIZE text_size{};
        GetTextExtentPoint32W(draw.hDC, text, static_cast<int>(wcslen(text)), &text_size);
        const int icon_size = scale_for_dialog_dpi(16, dpi);
        const int gap = scale_for_dialog_dpi(5, dpi);
        const int content_width = icon_size + gap + text_size.cx;
        const int available_width = text_rect.right - text_rect.left -
                                    scale_for_dialog_dpi(10, dpi);
        if (content_width <= available_width) {
            const int left = text_rect.left +
                             (text_rect.right - text_rect.left - content_width) / 2;
            RECT icon_rect{left, text_rect.top, left + icon_size, text_rect.bottom};
            const COLORREF icon_color = button_icon_color(
                enabled ? colors.text : colors.disabled_text, enabled);
            draw_toolbar_icon(draw.hDC, icon, icon_rect, icon_color, dpi, 16,
                              button_icon_style(enabled));
            text_rect.left = icon_rect.right + gap;
            text_rect.right = text_rect.left + text_size.cx;
            DrawTextW(draw.hDC, text, -1, &text_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            DrawTextW(draw.hDC, text, -1, &text_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }
    if (old_font != nullptr) SelectObject(draw.hDC, old_font);

}

void draw_dialog_checkbox(const DRAWITEMSTRUCT& draw, bool dark, bool checked) {
    if (draw.CtlType != ODT_BUTTON || draw.hDC == nullptr || draw.hwndItem == nullptr) {
        return;
    }
    const DialogColors colors = dialog_colors(dark);
    const bool enabled = (draw.itemState & ODS_DISABLED) == 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;
    HBRUSH background = CreateSolidBrush(colors.background);
    FillRect(draw.hDC, &draw.rcItem, background);
    DeleteObject(background);

    const UINT dpi = GetDpiForWindow(draw.hwndItem);
    const int box_size = scale_for_dialog_dpi(16, dpi);
    RECT box{
        draw.rcItem.left + scale_for_dialog_dpi(2, dpi),
        draw.rcItem.top + (draw.rcItem.bottom - draw.rcItem.top - box_size) / 2,
        draw.rcItem.left + scale_for_dialog_dpi(2, dpi) + box_size,
        draw.rcItem.top + (draw.rcItem.bottom - draw.rcItem.top + box_size) / 2,
    };
    COLORREF box_fill = colors.control_background;
    COLORREF box_border = focused ? colors.focus_border : colors.border;
    COLORREF mark_color = colors.selection_text;
    if (checked) {
        box_fill = enabled
            ? colors.focus_border
            : blend_color(colors.control_background, colors.disabled_text, 45);
        box_border = box_fill;
        mark_color = enabled ? readable_text_color(box_fill) : colors.control_background;
    } else if (enabled && (pressed || hot || focused)) {
        box_fill = blend_color(colors.control_background, colors.focus_border,
                               pressed ? 15 : 7);
        box_border = colors.focus_border;
    }
    HBRUSH box_brush = CreateSolidBrush(box_fill);
    FillRect(draw.hDC, &box, box_brush);
    DeleteObject(box_brush);
    HBRUSH border = CreateSolidBrush(box_border);
    FrameRect(draw.hDC, &box, border);
    DeleteObject(border);

    if (checked) {
        draw_antialiased_checkmark(draw.hDC, box, dpi, mark_color);
    }

    draw_selection_control_text(draw, box, dpi, colors, enabled);
}

void draw_dialog_radio_button(const DRAWITEMSTRUCT& draw, bool dark, bool checked) {
    if (draw.CtlType != ODT_BUTTON || draw.hDC == nullptr || draw.hwndItem == nullptr) {
        return;
    }
    const DialogColors colors = dialog_colors(dark);
    const bool enabled = (draw.itemState & ODS_DISABLED) == 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;
    HBRUSH background = CreateSolidBrush(colors.background);
    FillRect(draw.hDC, &draw.rcItem, background);
    DeleteObject(background);

    const UINT dpi = GetDpiForWindow(draw.hwndItem);
    const int diameter = scale_for_dialog_dpi(16, dpi);
    RECT circle{
        draw.rcItem.left + scale_for_dialog_dpi(2, dpi),
        draw.rcItem.top + (draw.rcItem.bottom - draw.rcItem.top - diameter) / 2,
        draw.rcItem.left + scale_for_dialog_dpi(2, dpi) + diameter,
        draw.rcItem.top + (draw.rcItem.bottom - draw.rcItem.top + diameter) / 2,
    };
    const COLORREF active_color = enabled ? colors.focus_border : colors.disabled_text;
    const COLORREF outline_color = checked || focused
        ? active_color
        : colors.border;
    const COLORREF fill_color = enabled && !checked && (pressed || hot)
        ? blend_color(colors.control_background, colors.focus_border,
                      pressed ? 15 : 7)
        : colors.control_background;
    if (gdiplus_ready()) {
        Gdiplus::Graphics graphics(draw.hDC);
        configure_antialiased_shape_drawing(graphics);
        Gdiplus::SolidBrush fill(gdiplus_color(fill_color));
        Gdiplus::Pen outline(gdiplus_color(outline_color),
                             checked ? 1.5f : 1.0f);
        const Gdiplus::RectF bounds(
            static_cast<Gdiplus::REAL>(circle.left) + 0.5f,
            static_cast<Gdiplus::REAL>(circle.top) + 0.5f,
            static_cast<Gdiplus::REAL>(diameter - 1),
            static_cast<Gdiplus::REAL>(diameter - 1));
        graphics.FillEllipse(&fill, bounds);
        graphics.DrawEllipse(&outline, bounds);
        if (checked) {
            const int inset = scale_for_dialog_dpi(4, dpi);
            Gdiplus::SolidBrush dot(gdiplus_color(active_color));
            graphics.FillEllipse(
                &dot, static_cast<Gdiplus::REAL>(circle.left + inset),
                static_cast<Gdiplus::REAL>(circle.top + inset),
                static_cast<Gdiplus::REAL>(diameter - inset * 2),
                static_cast<Gdiplus::REAL>(diameter - inset * 2));
        }
    } else {
        HBRUSH fill = CreateSolidBrush(fill_color);
        HPEN outline = CreatePen(PS_SOLID, 1, outline_color);
        HGDIOBJ old_brush = SelectObject(draw.hDC, fill);
        HGDIOBJ old_pen = SelectObject(draw.hDC, outline);
        Ellipse(draw.hDC, circle.left, circle.top, circle.right, circle.bottom);
        if (checked) {
            const int inset = scale_for_dialog_dpi(4, dpi);
            HBRUSH dot = CreateSolidBrush(active_color);
            SelectObject(draw.hDC, dot);
            Ellipse(draw.hDC, circle.left + inset, circle.top + inset,
                    circle.right - inset, circle.bottom - inset);
            SelectObject(draw.hDC, fill);
            DeleteObject(dot);
        }
        SelectObject(draw.hDC, old_pen);
        SelectObject(draw.hDC, old_brush);
        DeleteObject(outline);
        DeleteObject(fill);
    }
    draw_selection_control_text(draw, circle, dpi, colors, enabled);
}

void draw_dialog_combo_item(const DRAWITEMSTRUCT& draw, bool dark) {
    if (draw.CtlType != ODT_COMBOBOX || draw.hDC == nullptr || draw.hwndItem == nullptr) {
        return;
    }

    const DialogColors colors = dialog_colors(dark);
    const bool enabled = (draw.itemState & ODS_DISABLED) == 0;
    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    const COLORREF background = selected
        ? colors.selection_background
        : colors.control_background;
    const COLORREF text_color = !enabled
        ? colors.disabled_text
        : selected ? colors.selection_text : colors.text;

    HBRUSH brush = CreateSolidBrush(background);
    FillRect(draw.hDC, &draw.rcItem, brush);
    DeleteObject(brush);

    if (draw.itemID != static_cast<UINT>(-1)) {
        const LRESULT length = SendMessageW(draw.hwndItem, CB_GETLBTEXTLEN,
                                            draw.itemID, 0);
        if (length >= 0) {
            std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
            SendMessageW(draw.hwndItem, CB_GETLBTEXT, draw.itemID,
                         reinterpret_cast<LPARAM>(text.data()));
            text.resize(static_cast<std::size_t>(length));
            HFONT font = reinterpret_cast<HFONT>(
                SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0));
            HGDIOBJ old_font = font != nullptr ? SelectObject(draw.hDC, font) : nullptr;
            SetBkMode(draw.hDC, TRANSPARENT);
            SetTextColor(draw.hDC, text_color);
            RECT text_rect = draw.rcItem;
            text_rect.left += scale_for_dialog_dpi(7, GetDpiForWindow(draw.hwndItem));
            text_rect.right -= scale_for_dialog_dpi(4, GetDpiForWindow(draw.hwndItem));
            DrawTextW(draw.hDC, text.c_str(), -1, &text_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (old_font != nullptr) SelectObject(draw.hDC, old_font);
        }
    }

    if ((draw.itemState & ODS_FOCUS) != 0) {
        if ((draw.itemState & ODS_COMBOBOXEDIT) != 0) return;
        RECT focus = draw.rcItem;
        InflateRect(&focus, -2, -2);
        HBRUSH focus_brush = CreateSolidBrush(colors.focus_border);
        FrameRect(draw.hDC, &focus, focus_brush);
        DeleteObject(focus_brush);
    }
}

bool disable_dialog_owner(HWND owner, HWND dialog) {
    if (owner == nullptr || !IsWindow(owner)) return false;
    const bool was_enabled = IsWindowEnabled(owner) != FALSE;
    if (!was_enabled) return false;
    const bool activation_owned = modal_owner_has_foreground(owner, dialog);
    RemovePropW(owner, kRestoreModalActivationProperty);
    EnableWindow(owner, FALSE);
    if (dialog != nullptr && IsWindow(dialog)) {
        auto* state = new (std::nothrow) ModalOwnerState{
            owner, false, activation_owned, false, false};
        if (state != nullptr &&
            !SetWindowSubclass(dialog, modal_owner_subclass_proc,
                               kModalOwnerSubclass,
                               reinterpret_cast<DWORD_PTR>(state))) {
            delete state;
        }
    }
    return true;
}

void destroy_modal_dialog(HWND dialog) {
    if (dialog == nullptr || !IsWindow(dialog)) return;

    DWORD_PTR reference_data = 0;
    if (GetWindowSubclass(dialog, modal_owner_subclass_proc,
                          kModalOwnerSubclass, &reference_data) &&
        reference_data != 0) {
        auto* state = reinterpret_cast<ModalOwnerState*>(reference_data);
        state->close_requested = true;
        state->activation_owned =
            state->activation_owned ||
            modal_owner_has_foreground(state->owner, dialog) ||
            GetActiveWindow() == dialog;
        if (!state->restored) {
            state->restored = true;
            // Do this while the active owned popup still exists. USER32 can
            // then hand activation straight to its owner during DestroyWindow
            // instead of selecting (and painting) an unrelated window first.
            enable_modal_owner_for_close(state->owner,
                                         state->activation_owned);
        }
    }
    DestroyWindow(dialog);
}

void restore_dialog_owner(HWND owner, bool was_enabled) {
    if (!was_enabled || owner == nullptr || !IsWindow(owner)) return;
    bool restore_activation =
        RemovePropW(owner, kRestoreModalActivationProperty) != nullptr;
    // The modal-child subclass normally restores the owner during WM_DESTROY,
    // before Windows can activate an unrelated window beneath Axiom. If the
    // subclass could not be installed, perform that enable here as a fallback.
    if (!IsWindowEnabled(owner)) {
        restore_activation = modal_owner_has_foreground(owner, nullptr);
        EnableWindow(owner, TRUE);
    }
    if (!restore_activation || !IsWindowVisible(owner) || IsIconic(owner)) return;

    // Activation is intentionally deferred until the modal child is completely
    // gone. Usually USER32 has already selected the enabled owner, so these
    // calls are no-ops. The fallback only repairs a transient focus handoff; a
    // dialog closed after the user switched applications never requests it.
    HWND foreground = GetForegroundWindow();
    const HWND owner_root = GetAncestor(owner, GA_ROOTOWNER);
    if (foreground != nullptr && foreground != owner &&
        GetAncestor(foreground, GA_ROOTOWNER) == owner_root) {
        return;
    }
    if (GetActiveWindow() != owner) SetActiveWindow(owner);
    foreground = GetForegroundWindow();
    if (foreground != owner) {
        BringWindowToTop(owner);
        SetForegroundWindow(owner);
    }
}

bool message_targets_window(HWND window, const MSG& message) {
    return window != nullptr && (message.hwnd == window || IsChild(window, message.hwnd));
}

bool window_placement_is_visible(const WINDOWPLACEMENT& placement) {
    return rect_is_visible_on_connected_monitor(placement.rcNormalPosition);
}

POINT centered_window_position(HWND owner, int width, int height) {
    const RECT work = owner_or_primary_work_area(owner);
    RECT anchor = work;
    HWND owner_root = owner != nullptr && IsWindow(owner)
        ? GetAncestor(owner, GA_ROOTOWNER) : nullptr;
    if (owner_root != nullptr && IsWindowVisible(owner_root)) {
        RECT owner_rect{};
        if (GetWindowRect(owner_root, &owner_rect)) anchor = owner_rect;
    }
    int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
    int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;
    x = std::clamp(x, static_cast<int>(work.left),
                   (std::max)(static_cast<int>(work.left),
                              static_cast<int>(work.right) - width));
    y = std::clamp(y, static_cast<int>(work.top),
                   (std::max)(static_cast<int>(work.top),
                              static_cast<int>(work.bottom) - height));
    return POINT{x, y};
}

int restore_named_window_placement(HWND window, HWND owner, std::wstring_view name) {
    if (window == nullptr) return SW_SHOW;
    WINDOWPLACEMENT placement{sizeof(placement)};
    UINT saved_dpi = 0;
    if (read_window_placement(name, placement, saved_dpi)) {
        const long long saved_width =
            static_cast<long long>(placement.rcNormalPosition.right) -
            placement.rcNormalPosition.left;
        const long long saved_height =
            static_cast<long long>(placement.rcNormalPosition.bottom) -
            placement.rcNormalPosition.top;
        if (saved_width >= 64 && saved_height >= 64 &&
            saved_width <= INT_MAX && saved_height <= INT_MAX) {
            const int show_command = placement.showCmd == SW_SHOWMAXIMIZED
                ? SW_SHOWMAXIMIZED : SW_SHOW;
            const bool restore_saved_position =
                !g_dialog_appearance.center_child_windows &&
                window_placement_is_visible(placement);

            // WINDOWPLACEMENT stores physical pixels for a per-monitor-aware
            // window. Move first so Windows establishes the destination monitor
            // DPI, then convert the persisted normal size exactly once.
            if (restore_saved_position) {
                SetWindowPos(window, nullptr,
                             placement.rcNormalPosition.left,
                             placement.rcNormalPosition.top,
                             0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            const UINT target_dpi = GetDpiForWindow(window);
            int width = static_cast<int>(saved_width);
            int height = static_cast<int>(saved_height);
            if (saved_dpi >= 48 && saved_dpi <= 768 && target_dpi != 0 &&
                target_dpi != saved_dpi) {
                width = MulDiv(width, static_cast<int>(target_dpi),
                               static_cast<int>(saved_dpi));
                height = MulDiv(height, static_cast<int>(target_dpi),
                                static_cast<int>(saved_dpi));
            }

            const RECT work = owner_or_primary_work_area(
                restore_saved_position ? window : owner);
            const int work_width = (std::max)(64L, work.right - work.left);
            const int work_height = (std::max)(64L, work.bottom - work.top);
            width = std::clamp(width, 64, work_width);
            height = std::clamp(height, 64, work_height);

            int x = placement.rcNormalPosition.left;
            int y = placement.rcNormalPosition.top;
            if (!restore_saved_position) {
                const POINT position = centered_window_position(owner, width, height);
                x = position.x;
                y = position.y;
            } else {
                x = std::clamp(x, static_cast<int>(work.left),
                               (std::max)(static_cast<int>(work.left),
                                          static_cast<int>(work.right) - width));
                y = std::clamp(y, static_cast<int>(work.top),
                               (std::max)(static_cast<int>(work.top),
                                          static_cast<int>(work.bottom) - height));
            }
            // SetWindowPlacement also applies showCmd and can expose a hidden
            // window before its dark non-client frame and first client frame
            // are ready. Restore the normal geometry directly and let the
            // caller show the fully initialized window once.
            SetWindowPos(window, nullptr, x, y, width, height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            return show_command;
        }
    }

    RECT rect{};
    if (!GetWindowRect(window, &rect)) return SW_SHOW;
    const POINT position = centered_window_position(owner, rect.right - rect.left,
                                                    rect.bottom - rect.top);
    SetWindowPos(window, nullptr, position.x, position.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return SW_SHOW;
}

void save_named_window_placement(std::wstring_view name, HWND window) {
    if (window == nullptr || !IsWindow(window)) return;
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(window, &placement)) return;
    if (placement.showCmd == SW_SHOWMINIMIZED) placement.showCmd = SW_SHOWNORMAL;

    const std::wstring path = layout_registry_path(name);
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, L"WindowPlacement", 0, REG_BINARY,
                   reinterpret_cast<const BYTE*>(&placement), sizeof(placement));
    const DWORD dpi = GetDpiForWindow(window);
    RegSetValueExW(key, L"WindowDpi", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&dpi), sizeof(dpi));
    RegCloseKey(key);
}

std::wstring last_error_text(DWORD error) {
    if (error == ERROR_SUCCESS) {
        return L"No error.";
    }
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = length > 0 && buffer != nullptr
        ? std::wstring(buffer, length)
        : L"Unknown error.";
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

}  // namespace axiom::gui
