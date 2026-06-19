#include "gui/dialog_support.hpp"

#include "gui/resource.hpp"

#include <dwmapi.h>
#include <uxtheme.h>

namespace axiom::gui {
namespace {

constexpr DWORD kOlderImmersiveDarkMode = 19;

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
    if (dark) {
        return {
            RGB(31, 31, 31), RGB(241, 241, 241), RGB(37, 37, 38),
            RGB(55, 78, 112), RGB(255, 255, 255), RGB(150, 150, 150),
            RGB(64, 64, 64), RGB(78, 115, 158),
        };
    }
    return {
        RGB(240, 240, 240), RGB(0, 0, 0), RGB(255, 255, 255),
        RGB(0, 120, 215), RGB(255, 255, 255), RGB(120, 120, 120),
        RGB(170, 170, 170), RGB(0, 120, 215),
    };
}

bool dialog_system_prefers_dark_mode() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0) {
        return false;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && value == 0;
}

void apply_dialog_dark_frame(HWND window, bool dark) {
    BOOL enabled = dark ? TRUE : FALSE;
    constexpr DWORD immersive_dark_mode = 20;
    if (FAILED(DwmSetWindowAttribute(window, immersive_dark_mode, &enabled, sizeof(enabled)))) {
        DwmSetWindowAttribute(window, kOlderImmersiveDarkMode, &enabled, sizeof(enabled));
    }
}

void apply_dialog_control_theme(HWND control, bool dark) {
    if (control != nullptr) {
        SetWindowTheme(control, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
        RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
}

void set_dialog_control_font(HWND control, HFONT font) {
    if (control != nullptr && font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
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
    COLORREF fill = dark ? colors.control_background : GetSysColor(COLOR_BTNFACE);
    if (pressed) {
        fill = dark ? RGB(48, 48, 48) : GetSysColor(COLOR_3DSHADOW);
    } else if (hot || focused) {
        fill = dark ? RGB(45, 45, 46) : GetSysColor(COLOR_3DLIGHT);
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
    DrawTextW(draw.hDC, text, -1, &text_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (old_font != nullptr) SelectObject(draw.hDC, old_font);

    if (focused) {
        RECT focus_rect = draw.rcItem;
        InflateRect(&focus_rect, -3, -3);
        DrawFocusRect(draw.hDC, &focus_rect);
    }
}

void draw_dialog_checkbox(const DRAWITEMSTRUCT& draw, bool dark, bool checked) {
    if (draw.CtlType != ODT_BUTTON || draw.hDC == nullptr || draw.hwndItem == nullptr) {
        return;
    }
    const DialogColors colors = dialog_colors(dark);
    const bool enabled = (draw.itemState & ODS_DISABLED) == 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
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
    HBRUSH box_brush = CreateSolidBrush(colors.control_background);
    FillRect(draw.hDC, &box, box_brush);
    DeleteObject(box_brush);
    HBRUSH border = CreateSolidBrush(focused ? colors.focus_border : colors.border);
    FrameRect(draw.hDC, &box, border);
    DeleteObject(border);

    if (checked) {
        HPEN pen = CreatePen(PS_SOLID, scale_for_dialog_dpi(2, dpi),
                             enabled ? colors.text : colors.disabled_text);
        HGDIOBJ old_pen = SelectObject(draw.hDC, pen);
        MoveToEx(draw.hDC, box.left + scale_for_dialog_dpi(3, dpi),
                 box.top + scale_for_dialog_dpi(8, dpi), nullptr);
        LineTo(draw.hDC, box.left + scale_for_dialog_dpi(7, dpi),
               box.bottom - scale_for_dialog_dpi(3, dpi));
        LineTo(draw.hDC, box.right - scale_for_dialog_dpi(3, dpi),
               box.top + scale_for_dialog_dpi(3, dpi));
        SelectObject(draw.hDC, old_pen);
        DeleteObject(pen);
    }

    wchar_t text[256]{};
    GetWindowTextW(draw.hwndItem, text,
                   static_cast<int>(sizeof(text) / sizeof(text[0])));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = font != nullptr ? SelectObject(draw.hDC, font) : nullptr;
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, enabled ? colors.text : colors.disabled_text);
    RECT text_rect = draw.rcItem;
    text_rect.left = box.right + scale_for_dialog_dpi(8, dpi);
    DrawTextW(draw.hDC, text, -1, &text_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (old_font != nullptr) SelectObject(draw.hDC, old_font);
}

bool message_targets_window(HWND window, const MSG& message) {
    return window != nullptr && (message.hwnd == window || IsChild(window, message.hwnd));
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
