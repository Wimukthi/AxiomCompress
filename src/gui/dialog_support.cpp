#include "gui/dialog_support.hpp"

#include "gui/resource.hpp"

#include <algorithm>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

namespace axiom::gui {
namespace {

constexpr DWORD kOlderImmersiveDarkMode = 19;
constexpr UINT_PTR kDarkComboSubclass = 1;
constexpr const wchar_t* kSavedComboStyleProperty = L"AxiomSavedComboStyle";
constexpr const wchar_t* kSavedComboExStyleProperty = L"AxiomSavedComboExStyle";

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

void apply_combo_child_theme(HWND combo, bool dark) {
    COMBOBOXINFO info{sizeof(info)};
    if (!GetComboBoxInfo(combo, &info)) return;
    const wchar_t* theme = dark ? L"DarkMode_Explorer" : nullptr;
    if (dark) {
        strip_light_control_edges(combo);
    } else {
        restore_control_edges(combo);
    }
    if (info.hwndList != nullptr) {
        SetWindowTheme(info.hwndList, theme, nullptr);
        if (dark) {
            strip_light_control_edges(info.hwndList);
        } else {
            restore_control_edges(info.hwndList);
        }
        RedrawWindow(info.hwndList, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
    if (info.hwndItem != nullptr && info.hwndItem != combo) {
        SetWindowTheme(info.hwndItem, theme, nullptr);
        if (dark) {
            strip_light_control_edges(info.hwndItem);
        } else {
            restore_control_edges(info.hwndItem);
        }
        RedrawWindow(info.hwndItem, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
}

bool combo_is_dropdown_list(HWND window) {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    return (style & CBS_DROPDOWNLIST) == CBS_DROPDOWNLIST;
}

void paint_dark_combo(HWND window, HDC dc) {
    RECT client{};
    if (!GetClientRect(window, &client)) return;

    const DialogColors colors = dialog_colors(true);
    const UINT dpi = GetDpiForWindow(window);
    const int arrow_width = (std::max)(
        scale_for_dialog_dpi(18, dpi), GetSystemMetricsForDpi(SM_CXVSCROLL, dpi));
    RECT arrow{
        (std::max)(client.left + 1, client.right - arrow_width),
        client.top + 1,
        client.right - 1,
        client.bottom - 1,
    };

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
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
            if (reference_data != 0) redraw_dark_combo_now(window);
            return result;
        }
        case WM_MOUSELEAVE:
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
        wchar_t class_name[32]{};
        GetClassNameW(control, class_name,
                      static_cast<int>(sizeof(class_name) / sizeof(class_name[0])));
        const bool combo_box = lstrcmpiW(class_name, L"ComboBox") == 0;
        SetWindowTheme(control,
                       dark ? (combo_box ? L"" : L"DarkMode_Explorer")
                            : nullptr,
                       dark && combo_box ? L"" : nullptr);
        if (combo_box) {
            if (dark) {
                SetWindowSubclass(control, dark_combo_subclass_proc,
                                  kDarkComboSubclass, 1);
            } else {
                RemoveWindowSubclass(control, dark_combo_subclass_proc,
                                     kDarkComboSubclass);
            }
            apply_combo_child_theme(control, dark);
        }
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

bool disable_dialog_owner(HWND owner) {
    if (owner == nullptr || !IsWindow(owner)) return false;
    const bool was_enabled = IsWindowEnabled(owner) != FALSE;
    if (!was_enabled) return false;
    EnableWindow(owner, FALSE);
    return true;
}

void restore_dialog_owner(HWND owner, bool was_enabled) {
    if (!was_enabled || owner == nullptr || !IsWindow(owner)) return;
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    SetFocus(owner);
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
