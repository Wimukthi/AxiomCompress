#define NOMINMAX
#include "gui/custom_menu.hpp"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>

namespace axiom::gui {

namespace {

constexpr wchar_t kMenuBarClass[] = L"AxiomCustomMenuBar";
constexpr wchar_t kPopupClass[] = L"AxiomCustomPopupMenu";
constexpr wchar_t kShadowClass[] = L"AxiomCustomPopupShadow";

int scale_for_dpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi),
                  USER_DEFAULT_SCREEN_DPI);
}

std::wstring display_text(std::wstring text) {
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] != L'&') {
            ++index;
            continue;
        }
        if (index + 1 < text.size() && text[index + 1] == L'&') {
            text.erase(index, 1);
            ++index;
        } else {
            text.erase(index, 1);
        }
    }
    return text;
}

struct PopupItemLayout : CustomMenuItem {
    RECT rect{};
};

struct PopupState {
    HWND hwnd{};
    HWND owner{};
    HWND shadow{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    HFONT font{};
    CustomMenuTheme theme{};
    std::vector<PopupItemLayout> items;
    int hot_index{-1};
    UINT command{};
};

struct ShadowMetrics {
    int margin{};
    int offset_x{};
    int offset_y{};
    int radius{};
    BYTE max_alpha{};
    SIZE window_size{};
    RECT popup_rect{};
    RECT caster_rect{};
};

ShadowMetrics shadow_metrics(SIZE popup_size, UINT dpi) {
    ShadowMetrics metrics{};
    metrics.radius = scale_for_dpi(14, dpi);
    metrics.margin = scale_for_dpi(12, dpi);
    metrics.offset_x = scale_for_dpi(2, dpi);
    metrics.offset_y = scale_for_dpi(4, dpi);
    metrics.max_alpha = 58;
    metrics.window_size = {
        popup_size.cx + metrics.margin * 2 + metrics.offset_x,
        popup_size.cy + metrics.margin * 2 + metrics.offset_y,
    };
    metrics.popup_rect = {metrics.margin, metrics.margin,
                          metrics.margin + popup_size.cx, metrics.margin + popup_size.cy};
    metrics.caster_rect = {metrics.margin + metrics.offset_x,
                           metrics.margin + metrics.offset_y,
                           metrics.margin + metrics.offset_x + popup_size.cx,
                           metrics.margin + metrics.offset_y + popup_size.cy};
    return metrics;
}

BYTE shadow_alpha(const ShadowMetrics& metrics, int x, int y) {
    if (x >= metrics.popup_rect.left && x < metrics.popup_rect.right &&
        y >= metrics.popup_rect.top && y < metrics.popup_rect.bottom) {
        return 0;
    }
    const int caster_left = static_cast<int>(metrics.caster_rect.left);
    const int caster_top = static_cast<int>(metrics.caster_rect.top);
    const int caster_right = static_cast<int>(metrics.caster_rect.right);
    const int caster_bottom = static_cast<int>(metrics.caster_rect.bottom);
    const int dx = std::max(std::max(caster_left - x, 0), x - (caster_right - 1));
    const int dy = std::max(std::max(caster_top - y, 0), y - (caster_bottom - 1));
    const double distance = std::sqrt(static_cast<double>(dx * dx + dy * dy));
    if (distance > metrics.radius) {
        return 0;
    }
    const double falloff = 1.0 - distance / static_cast<double>(metrics.radius);
    return static_cast<BYTE>(metrics.max_alpha * falloff * falloff);
}

LRESULT CALLBACK shadow_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool register_window_class(HINSTANCE instance, const wchar_t* name, WNDPROC procedure) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = name;
    return RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND create_shadow(HWND owner, HINSTANCE instance, POINT position, SIZE popup_size, UINT dpi) {
    if (!register_window_class(instance, kShadowClass, shadow_proc)) return nullptr;
    const ShadowMetrics metrics = shadow_metrics(popup_size, dpi);
    HWND shadow = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        kShadowClass, nullptr, WS_POPUP,
        position.x - metrics.margin, position.y - metrics.margin,
        metrics.window_size.cx, metrics.window_size.cy,
        owner, nullptr, instance, nullptr);
    if (shadow == nullptr) return nullptr;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = metrics.window_size.cx;
    info.bmiHeader.biHeight = -metrics.window_size.cy;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen_dc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP bitmap = screen_dc != nullptr
        ? CreateDIBSection(screen_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0)
        : nullptr;
    if (bitmap == nullptr || bits == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (screen_dc != nullptr) ReleaseDC(nullptr, screen_dc);
        DestroyWindow(shadow);
        return nullptr;
    }

    auto* pixels = static_cast<std::uint32_t*>(bits);
    for (int y = 0; y < metrics.window_size.cy; ++y) {
        for (int x = 0; x < metrics.window_size.cx; ++x) {
            pixels[static_cast<std::size_t>(y) * metrics.window_size.cx + x] =
                static_cast<std::uint32_t>(shadow_alpha(metrics, x, y)) << 24;
        }
    }

    HDC memory_dc = CreateCompatibleDC(screen_dc);
    HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
    POINT source{};
    SIZE size = metrics.window_size;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL updated = UpdateLayeredWindow(shadow, screen_dc, nullptr, &size,
                                             memory_dc, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory_dc, old_bitmap);
    DeleteDC(memory_dc);
    DeleteObject(bitmap);
    ReleaseDC(nullptr, screen_dc);
    if (!updated) {
        DestroyWindow(shadow);
        return nullptr;
    }
    ShowWindow(shadow, SW_SHOWNOACTIVATE);
    return shadow;
}

int popup_hit_test(const PopupState* state, POINT point) {
    if (state == nullptr) return -1;
    for (std::size_t index = 0; index < state->items.size(); ++index) {
        if (!state->items[index].separator && PtInRect(&state->items[index].rect, point)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void select_adjacent(PopupState* state, int direction) {
    if (state == nullptr || state->items.empty()) return;
    int index = state->hot_index;
    for (std::size_t attempts = 0; attempts < state->items.size(); ++attempts) {
        index += direction;
        if (index < 0) index = static_cast<int>(state->items.size()) - 1;
        if (index >= static_cast<int>(state->items.size())) index = 0;
        const auto& item = state->items[static_cast<std::size_t>(index)];
        if (!item.separator && item.enabled) {
            state->hot_index = index;
            InvalidateRect(state->hwnd, nullptr, FALSE);
            return;
        }
    }
}

void paint_popup(PopupState* state) {
    if (state == nullptr) return;
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(state->hwnd, &paint);
    if (dc == nullptr) return;
    RECT client{};
    GetClientRect(state->hwnd, &client);
    HBRUSH brush = CreateSolidBrush(state->theme.border);
    FillRect(dc, &client, brush);
    DeleteObject(brush);
    RECT body = client;
    InflateRect(&body, -1, -1);
    brush = CreateSolidBrush(state->theme.background);
    FillRect(dc, &body, brush);
    DeleteObject(brush);

    HGDIOBJ old_font = SelectObject(dc, state->font != nullptr
        ? state->font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    for (std::size_t index = 0; index < state->items.size(); ++index) {
        const auto& item = state->items[index];
        if (item.separator) {
            const int y = item.rect.top + (item.rect.bottom - item.rect.top) / 2;
            HPEN pen = CreatePen(PS_SOLID, 1, state->theme.separator);
            HGDIOBJ old_pen = SelectObject(dc, pen);
            MoveToEx(dc, item.rect.left + scale_for_dpi(30, state->dpi), y, nullptr);
            LineTo(dc, item.rect.right - scale_for_dpi(8, state->dpi), y);
            SelectObject(dc, old_pen);
            DeleteObject(pen);
            continue;
        }

        const bool selected = static_cast<int>(index) == state->hot_index && item.enabled;
        brush = CreateSolidBrush(selected ? state->theme.hot : state->theme.background);
        FillRect(dc, &item.rect, brush);
        DeleteObject(brush);
        SetTextColor(dc, item.enabled ? state->theme.text : state->theme.disabled_text);

        if (item.checked) {
            HPEN pen = CreatePen(PS_SOLID, std::max(1, scale_for_dpi(2, state->dpi)),
                                 item.enabled ? state->theme.text : state->theme.disabled_text);
            HGDIOBJ old_pen = SelectObject(dc, pen);
            const int left = item.rect.left + scale_for_dpi(9, state->dpi);
            const int middle = item.rect.top + (item.rect.bottom - item.rect.top) / 2;
            MoveToEx(dc, left, middle, nullptr);
            LineTo(dc, left + scale_for_dpi(4, state->dpi), middle + scale_for_dpi(4, state->dpi));
            LineTo(dc, left + scale_for_dpi(12, state->dpi), middle - scale_for_dpi(5, state->dpi));
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }

        RECT text_rect = item.rect;
        text_rect.left += scale_for_dpi(30, state->dpi);
        text_rect.right -= scale_for_dpi(12, state->dpi);
        DrawTextW(dc, item.label.c_str(), -1, &text_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (!item.shortcut.empty()) {
            DrawTextW(dc, item.shortcut.c_str(), -1, &text_rect,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    SelectObject(dc, old_font);
    EndPaint(state->hwnd, &paint);
}

LRESULT CALLBACK popup_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<PopupState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<PopupState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    switch (message) {
        case WM_PAINT: paint_popup(state); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const int hit = popup_hit_test(state, point);
            if (state != nullptr && state->hot_index != hit) {
                state->hot_index = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (popup_hit_test(state, point) < 0) DestroyWindow(hwnd);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const int hit = popup_hit_test(state, point);
            if (state != nullptr && hit >= 0) {
                const auto& item = state->items[static_cast<std::size_t>(hit)];
                if (item.enabled) state->command = item.command;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_CANCELMODE:
            DestroyWindow(hwnd);
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
            if (wparam == VK_UP) { select_adjacent(state, -1); return 0; }
            if (wparam == VK_DOWN) { select_adjacent(state, 1); return 0; }
            if (wparam == VK_RETURN && state != nullptr && state->hot_index >= 0) {
                const auto& item = state->items[static_cast<std::size_t>(state->hot_index)];
                if (item.enabled) state->command = item.command;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_DESTROY:
            if (state != nullptr && state->shadow != nullptr) {
                DestroyWindow(state->shadow);
                state->shadow = nullptr;
            }
            if (GetCapture() == hwnd) ReleaseCapture();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

SIZE layout_popup_items(HWND owner, UINT dpi, HFONT font, std::vector<PopupItemLayout>& items) {
    HDC dc = GetDC(owner);
    HGDIOBJ old_font = dc != nullptr
        ? SelectObject(dc, font != nullptr ? font : GetStockObject(DEFAULT_GUI_FONT)) : nullptr;
    int width = scale_for_dpi(190, dpi);
    for (const auto& item : items) {
        if (item.separator) continue;
        SIZE label_size{};
        SIZE shortcut_size{};
        if (dc != nullptr) {
            GetTextExtentPoint32W(dc, item.label.c_str(), static_cast<int>(item.label.size()), &label_size);
            GetTextExtentPoint32W(dc, item.shortcut.c_str(), static_cast<int>(item.shortcut.size()), &shortcut_size);
        }
        const int measured_width = scale_for_dpi(66, dpi) +
            static_cast<int>(label_size.cx + shortcut_size.cx);
        width = std::max(width, measured_width);
    }
    if (dc != nullptr) {
        SelectObject(dc, old_font);
        ReleaseDC(owner, dc);
    }
    int y = 1;
    for (auto& item : items) {
        const int height = scale_for_dpi(item.separator ? 9 : 30, dpi);
        item.rect = {1, y, width - 1, y + height};
        y += height;
    }
    return {width, y + 1};
}

POINT clamp_popup(POINT point, SIZE size) {
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return point;
    point.x = std::clamp(point.x, info.rcWork.left, info.rcWork.right - size.cx);
    point.y = std::clamp(point.y, info.rcWork.top, info.rcWork.bottom - size.cy);
    return point;
}

UINT show_popup(HWND owner, HINSTANCE instance, UINT dpi, HFONT font,
                const CustomMenuTheme& theme, std::vector<CustomMenuItem> source,
                POINT point) {
    if (source.empty() || !register_window_class(instance, kPopupClass, popup_proc)) return 0;
    PopupState state{};
    state.owner = owner;
    state.dpi = dpi;
    state.font = font;
    state.theme = theme;
    state.items.reserve(source.size());
    for (auto& item : source) {
        item.label = display_text(std::move(item.label));
        PopupItemLayout layout{};
        static_cast<CustomMenuItem&>(layout) = std::move(item);
        state.items.push_back(std::move(layout));
    }
    const SIZE size = layout_popup_items(owner, dpi, font, state.items);
    point = clamp_popup(point, size);
    state.shadow = create_shadow(owner, instance, point, size, dpi);
    HWND popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kPopupClass, nullptr, WS_POPUP | WS_CLIPSIBLINGS,
        point.x, point.y, size.cx, size.cy, owner, nullptr, instance, &state);
    if (popup == nullptr) {
        if (state.shadow != nullptr) DestroyWindow(state.shadow);
        return 0;
    }
    if (state.shadow != nullptr) {
        SetWindowPos(state.shadow, popup, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    // Keep the owner active, as a native menu would. Keyboard messages are
    // routed by this local loop because the popup deliberately never activates.
    ShowWindow(popup, SW_SHOWNOACTIVATE);
    UpdateWindow(popup);
    SetCapture(popup);
    MSG message{};
    while (IsWindow(popup)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) &&
            (message.wParam == VK_ESCAPE || message.wParam == VK_UP ||
             message.wParam == VK_DOWN || message.wParam == VK_RETURN)) {
            SendMessageW(popup, WM_KEYDOWN, message.wParam, message.lParam);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return state.command;
}

} // namespace

bool CustomMenuBar::create(HWND parent,
                           HINSTANCE instance,
                           std::vector<std::pair<UINT, std::wstring>> entries,
                           ItemProvider item_provider,
                           CommandHandler command_handler) {
    parent_ = parent;
    instance_ = instance;
    item_provider_ = std::move(item_provider);
    command_handler_ = std::move(command_handler);
    entries_.reserve(entries.size());
    for (auto& [id, text] : entries) entries_.push_back({id, std::move(text), {}});
    if (!register_window_class(instance, kMenuBarClass, window_proc)) return false;
    hwnd_ = CreateWindowExW(0, kMenuBarClass, nullptr,
                            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                            0, 0, 0, 0, parent, nullptr, instance, this);
    return hwnd_ != nullptr;
}

int CustomMenuBar::preferred_height() const { return scale_for_dpi(25, dpi_); }

void CustomMenuBar::set_dpi(UINT dpi) {
    dpi_ = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void CustomMenuBar::set_font(HFONT font) {
    font_ = font;
    if (hwnd_ != nullptr) SendMessageW(hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void CustomMenuBar::set_theme(const CustomMenuTheme& theme) {
    theme_ = theme;
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void CustomMenuBar::move(int x, int y, int width, int height) {
    MoveWindow(hwnd_, x, y, width, height, TRUE);
}

void CustomMenuBar::layout_entries(HDC dc, int width) {
    int left = 0;
    const int padding = scale_for_dpi(12, dpi_);
    const int height = preferred_height();
    for (auto& entry : entries_) {
        const std::wstring text = display_text(entry.text);
        SIZE size{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        const int item_width = size.cx + padding * 2;
        entry.rect = {left, 0, std::min(width, left + item_width), height};
        left += item_width;
    }
}

void CustomMenuBar::paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    if (dc == nullptr) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    HBRUSH brush = CreateSolidBrush(theme_.background);
    FillRect(dc, &client, brush);
    DeleteObject(brush);
    HGDIOBJ old_font = SelectObject(dc, font_ != nullptr ? font_ : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    layout_entries(dc, client.right);
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const auto& entry = entries_[index];
        const COLORREF background = active_index_ == static_cast<int>(index) ? theme_.pressed
            : hot_index_ == static_cast<int>(index) ? theme_.hot : theme_.background;
        brush = CreateSolidBrush(background);
        FillRect(dc, &entry.rect, brush);
        DeleteObject(brush);
        SetTextColor(dc, theme_.text);
        RECT text_rect = entry.rect;
        DrawTextW(dc, entry.text.c_str(), -1, &text_rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                  (keyboard_cues_ ? 0u : DT_HIDEPREFIX));
    }
    HPEN pen = CreatePen(PS_SOLID, 1, theme_.border);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    MoveToEx(dc, client.left, client.bottom - 1, nullptr);
    LineTo(dc, client.right, client.bottom - 1);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    SelectObject(dc, old_font);
    EndPaint(hwnd_, &paint);
}

int CustomMenuBar::hit_test(POINT point) const {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (PtInRect(&entries_[index].rect, point)) return static_cast<int>(index);
    }
    return -1;
}

void CustomMenuBar::track_mouse_leave() {
    if (mouse_tracking_) return;
    TRACKMOUSEEVENT event{sizeof(event), TME_LEAVE, hwnd_, 0};
    mouse_tracking_ = TrackMouseEvent(&event) != FALSE;
}

int CustomMenuBar::mnemonic_index(WPARAM key) const {
    const wchar_t target = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(key)));
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const std::wstring& text = entries_[index].text;
        const auto marker = text.find(L'&');
        if (marker != std::wstring::npos && marker + 1 < text.size() &&
            std::towupper(text[marker + 1]) == target) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void CustomMenuBar::set_keyboard_index(int index) {
    if (entries_.empty()) return;
    const int count = static_cast<int>(entries_.size());
    while (index < 0) index += count;
    keyboard_index_ = index % count;
    hot_index_ = keyboard_index_;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void CustomMenuBar::enter_keyboard_mode(int index) {
    if (!keyboard_cues_) previous_focus_ = GetFocus();
    keyboard_cues_ = true;
    set_keyboard_index(index);
    SetFocus(hwnd_);
}

void CustomMenuBar::exit_keyboard_mode(bool restore_focus) {
    keyboard_cues_ = false;
    keyboard_index_ = -1;
    if (active_index_ < 0) hot_index_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (restore_focus && previous_focus_ != nullptr && IsWindow(previous_focus_)) {
        SetFocus(previous_focus_);
    }
    previous_focus_ = nullptr;
}

void CustomMenuBar::show_menu(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size()) || !item_provider_) return;
    active_index_ = index;
    hot_index_ = index;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    RECT anchor = entries_[static_cast<std::size_t>(index)].rect;
    MapWindowPoints(hwnd_, HWND_DESKTOP, reinterpret_cast<POINT*>(&anchor), 2);
    SetForegroundWindow(parent_);
    const UINT command = show_popup(parent_, instance_, dpi_, font_, theme_,
                                    item_provider_(entries_[static_cast<std::size_t>(index)].id),
                                    {anchor.left, anchor.bottom});
    active_index_ = -1;
    hot_index_ = -1;
    mouse_tracking_ = false;
    exit_keyboard_mode(true);
    if (command != 0 && command_handler_) command_handler_(command);
}

bool CustomMenuBar::handle_menu_key(WPARAM key) {
    if (key == VK_F10) {
        enter_keyboard_mode(keyboard_index_ >= 0 ? keyboard_index_ : 0);
        return true;
    }
    if (!keyboard_cues_) return false;
    switch (key) {
        case VK_ESCAPE: exit_keyboard_mode(true); return true;
        case VK_LEFT: set_keyboard_index((keyboard_index_ >= 0 ? keyboard_index_ : 0) - 1); return true;
        case VK_RIGHT: set_keyboard_index((keyboard_index_ >= 0 ? keyboard_index_ : 0) + 1); return true;
        case VK_DOWN:
        case VK_RETURN:
        case VK_SPACE: show_menu(keyboard_index_ >= 0 ? keyboard_index_ : 0); return true;
        default: {
            const int index = mnemonic_index(key);
            if (index >= 0) { show_menu(index); return true; }
            return GetFocus() == hwnd_;
        }
    }
}

bool CustomMenuBar::translate_message(const MSG& message) {
    if (message.message == WM_KEYDOWN && message.wParam == VK_F10) {
        enter_keyboard_mode(keyboard_index_ >= 0 ? keyboard_index_ : 0);
        return true;
    }
    if (message.message == WM_SYSKEYDOWN) {
        if (message.wParam == VK_MENU || message.wParam == VK_F10) {
            enter_keyboard_mode(keyboard_index_ >= 0 ? keyboard_index_ : 0);
            return true;
        }
        const int index = mnemonic_index(message.wParam);
        if ((GetKeyState(VK_MENU) & 0x8000) != 0 && index >= 0) {
            keyboard_cues_ = true;
            keyboard_index_ = index;
            show_menu(index);
            return true;
        }
    }
    if (message.message == WM_SYSKEYUP && message.wParam == VK_MENU && keyboard_cues_) return true;
    if (message.message == WM_KEYDOWN && GetFocus() == hwnd_) return handle_menu_key(message.wParam);
    return false;
}

UINT CustomMenuBar::show_context_menu(std::vector<CustomMenuItem> items, POINT screen_point) {
    SetForegroundWindow(parent_);
    return show_popup(parent_, instance_, dpi_, font_, theme_, std::move(items), screen_point);
}

LRESULT CustomMenuBar::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_PAINT: paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN: if (handle_menu_key(wparam)) return 0; break;
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP: {
            MSG current{hwnd_, message, wparam, lparam, 0, {}};
            if (translate_message(current)) return 0;
            break;
        }
        case WM_KILLFOCUS: if (active_index_ < 0) exit_keyboard_mode(false); return 0;
        case WM_MOUSEMOVE: {
            track_mouse_leave();
            const int hit = hit_test({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            if (hit != hot_index_) { hot_index_ = hit; InvalidateRect(hwnd_, nullptr, FALSE); }
            return 0;
        }
        case WM_MOUSELEAVE:
            mouse_tracking_ = false;
            hot_index_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = hit_test({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            if (hit >= 0) show_menu(hit);
            return 0;
        }
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

LRESULT CALLBACK CustomMenuBar::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    CustomMenuBar* menu = nullptr;
    if (message == WM_NCCREATE) {
        menu = static_cast<CustomMenuBar*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        menu->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(menu));
    } else {
        menu = reinterpret_cast<CustomMenuBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return menu != nullptr ? menu->handle_message(message, wparam, lparam)
                           : DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace axiom::gui
