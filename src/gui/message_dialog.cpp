#define NOMINMAX
#include "gui/message_dialog.hpp"

#include "gui/dialog_support.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace axiom::gui {
namespace {

constexpr wchar_t kMessageDialogClass[] = L"AxiomMessageDialog";
constexpr int kContentIconPixels = 32;
constexpr int kContentIconSlotPixels = 40;
constexpr int kContentIconGapPixels = 12;

struct ButtonSpec {
    int id;
    const wchar_t* text;
};

struct MessageDialogState {
    HWND hwnd{};
    HWND owner{};
    HWND message_control{};
    std::array<HWND, 3> buttons{};
    HINSTANCE instance{};
    HICON content_icon{};
    HFONT font{};
    HBRUSH background_brush{};
    std::wstring title;
    std::wstring message;
    std::vector<ButtonSpec> button_specs;
    MessageDialogIcon icon{MessageDialogIcon::information};
    MessageDialogButtons button_set{MessageDialogButtons::ok};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    int message_height{};
    int result{IDCANCEL};
    int default_result{IDOK};
    bool dark{};
};

std::vector<ButtonSpec> button_specs_for(MessageDialogButtons buttons) {
    switch (buttons) {
        case MessageDialogButtons::ok_cancel:
            return {{IDOK, L"OK"}, {IDCANCEL, L"Cancel"}};
        case MessageDialogButtons::yes_no:
            return {{IDYES, L"Yes"}, {IDNO, L"No"}};
        case MessageDialogButtons::yes_no_cancel:
            return {{IDYES, L"Yes"}, {IDNO, L"No"}, {IDCANCEL, L"Cancel"}};
        case MessageDialogButtons::ok:
        default:
            return {{IDOK, L"OK"}};
    }
}

int close_result_for(MessageDialogButtons buttons) {
    switch (buttons) {
        case MessageDialogButtons::ok:
            return IDOK;
        case MessageDialogButtons::ok_cancel:
        case MessageDialogButtons::yes_no_cancel:
            return IDCANCEL;
        case MessageDialogButtons::yes_no:
        default:
            return 0;
    }
}

LPCWSTR system_icon_for(MessageDialogIcon icon) {
    switch (icon) {
        case MessageDialogIcon::warning: return IDI_WARNING;
        case MessageDialogIcon::error: return IDI_ERROR;
        case MessageDialogIcon::question: return IDI_QUESTION;
        case MessageDialogIcon::information: return IDI_INFORMATION;
        case MessageDialogIcon::none:
        default: return nullptr;
    }
}

void refresh_content_icon(MessageDialogState* state) {
    if (state == nullptr) {
        return;
    }
    LPCWSTR icon_name = system_icon_for(state->icon);
    if (icon_name == nullptr) {
        return;
    }
    const int icon_size = scale_for_dialog_dpi(kContentIconPixels, state->dpi);
    HICON replacement = static_cast<HICON>(LoadImageW(
        nullptr, icon_name, IMAGE_ICON, icon_size, icon_size, LR_DEFAULTCOLOR));
    if (replacement == nullptr) {
        HICON shared = LoadIconW(nullptr, icon_name);
        replacement = shared != nullptr ? CopyIcon(shared) : nullptr;
    }
    if (replacement == nullptr) return;
    if (state->content_icon != nullptr) {
        DestroyIcon(state->content_icon);
    }
    state->content_icon = replacement;
}

int measure_message_height(std::wstring_view message, int text_width, HFONT font) {
    HDC dc = GetDC(nullptr);
    if (dc == nullptr) {
        return USER_DEFAULT_SCREEN_DPI;
    }
    HGDIOBJ old_font = font != nullptr ? SelectObject(dc, font) : nullptr;
    RECT rect{0, 0, std::max(1, text_width), 0};
    const std::wstring text(message);
    DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    if (old_font != nullptr) {
        SelectObject(dc, old_font);
    }
    ReleaseDC(nullptr, dc);
    return std::max<int>(1, static_cast<int>(rect.bottom - rect.top));
}

RECT owner_or_monitor_rect(HWND owner) {
    RECT rect{};
    if (owner != nullptr && GetWindowRect(owner, &rect)) {
        return rect;
    }
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
        return monitor.rcWork;
    }
    return {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

void complete_dialog(MessageDialogState* state, int result) {
    if (state != nullptr && state->hwnd != nullptr) {
        state->result = result;
        DestroyWindow(state->hwnd);
    }
}

int message_text_width(const MessageDialogState& state, int client_width) {
    const int margin = scale_for_dialog_dpi(18, state.dpi);
    const int icon_width = state.icon == MessageDialogIcon::none
        ? 0
        : scale_for_dialog_dpi(kContentIconSlotPixels + kContentIconGapPixels, state.dpi);
    return std::max(1, client_width - (margin * 2) - icon_width);
}

void refresh_font_and_measurement(MessageDialogState* state) {
    if (state == nullptr) {
        return;
    }
    delete_dialog_font(state->font);
    state->font = create_dialog_font(state->dpi);
    RECT client{};
    GetClientRect(state->hwnd, &client);
    state->message_height = measure_message_height(
        state->message, message_text_width(*state, client.right - client.left), state->font);
    set_dialog_control_font(state->message_control, state->font);
    for (HWND button : state->buttons) {
        set_dialog_control_font(button, state->font);
    }
}

void layout_dialog(MessageDialogState* state) {
    if (state == nullptr || state->hwnd == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(state->hwnd, &client);
    const int margin = scale_for_dialog_dpi(18, state->dpi);
    const int icon_size = scale_for_dialog_dpi(kContentIconSlotPixels, state->dpi);
    const int icon_gap = scale_for_dialog_dpi(kContentIconGapPixels, state->dpi);
    const int button_width = scale_for_dialog_dpi(88, state->dpi);
    const int button_height = scale_for_dialog_dpi(30, state->dpi);
    const int button_gap = scale_for_dialog_dpi(10, state->dpi);
    const int text_left = margin +
        (state->icon != MessageDialogIcon::none ? icon_size + icon_gap : 0);
    const int text_width = std::max<int>(
        1, static_cast<int>(client.right) - margin - text_left);

    MoveWindow(state->message_control, text_left, margin, text_width,
               state->message_height, TRUE);

    const int count = static_cast<int>(state->button_specs.size());
    const int total_width = count * button_width + std::max(0, count - 1) * button_gap;
    int left = client.right - margin - total_width;
    const int top = client.bottom - margin - button_height;
    for (int index = 0; index < count; ++index) {
        MoveWindow(state->buttons[static_cast<std::size_t>(index)], left, top,
                   button_width, button_height, TRUE);
        left += button_width + button_gap;
    }
}

LRESULT control_color(MessageDialogState* state, WPARAM wparam) {
    if (state == nullptr) {
        return 0;
    }
    const DialogColors colors = dialog_colors(state->dark);
    HDC dc = reinterpret_cast<HDC>(wparam);
    SetTextColor(dc, colors.text);
    SetBkColor(dc, colors.background);
    SetBkMode(dc, TRANSPARENT);
    return reinterpret_cast<LRESULT>(state->background_brush);
}

LRESULT CALLBACK message_dialog_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<MessageDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<MessageDialogState*>(create->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
        case WM_CREATE: {
            const DialogColors colors = dialog_colors(state->dark);
            state->background_brush = CreateSolidBrush(colors.background);
            state->font = create_dialog_font(state->dpi);
            apply_dialog_dark_frame(hwnd, state->dark);
            apply_axiom_window_icons(hwnd, state->instance);

            state->message_control = CreateWindowExW(
                0, L"STATIC", state->message.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                0, 0, 0, 0, hwnd, nullptr, state->instance, nullptr);

            refresh_content_icon(state);

            for (std::size_t index = 0; index < state->button_specs.size(); ++index) {
                const ButtonSpec& spec = state->button_specs[index];
                const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
                state->buttons[index] = CreateWindowExW(
                    0, L"BUTTON", spec.text, style, 0, 0, 0, 0, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(spec.id)),
                    state->instance, nullptr);
            }

            set_dialog_control_font(state->message_control, state->font);
            for (HWND button : state->buttons) {
                set_dialog_control_font(button, state->font);
                apply_dialog_control_theme(button, state->dark);
            }
            if (close_result_for(state->button_set) == 0) {
                EnableMenuItem(GetSystemMenu(hwnd, FALSE), SC_CLOSE,
                               MF_BYCOMMAND | MF_GRAYED);
            }
            layout_dialog(state);
            if (HWND default_button = GetDlgItem(hwnd, state->default_result)) {
                SendMessageW(hwnd, DM_SETDEFID, state->default_result, 0);
                SetFocus(default_button);
            }
            return 0;
        }
        case WM_SIZE:
            layout_dialog(state);
            return 0;
        case WM_DPICHANGED: {
            state->dpi = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            apply_axiom_window_icons(hwnd, state->instance);
            refresh_font_and_measurement(state);
            refresh_content_icon(state);
            layout_dialog(state);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            if (state->content_icon != nullptr) {
                const int margin = scale_for_dialog_dpi(18, state->dpi);
                const int slot_size = scale_for_dialog_dpi(kContentIconSlotPixels,
                                                           state->dpi);
                const int icon_size = scale_for_dialog_dpi(kContentIconPixels,
                                                           state->dpi);
                const int inset = std::max(0, (slot_size - icon_size) / 2);
                DrawIconEx(dc, margin + inset, margin + inset,
                           state->content_icon, icon_size, icon_size,
                           0, nullptr, DI_NORMAL);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return control_color(state, wparam);
        case WM_DRAWITEM:
            if (lparam != 0) {
                draw_dialog_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam), state->dark);
                return TRUE;
            }
            break;
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client, state->background_brush);
            return 1;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            for (const ButtonSpec& spec : state->button_specs) {
                if (id == spec.id) {
                    complete_dialog(state, id);
                    return 0;
                }
            }
            break;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                const int result = close_result_for(state->button_set);
                if (result != 0) {
                    complete_dialog(state, result);
                }
                return 0;
            }
            break;
        case WM_CLOSE: {
            const int result = close_result_for(state->button_set);
            if (result != 0) {
                complete_dialog(state, result);
            }
            return 0;
        }
        case WM_NCDESTROY:
            if (state != nullptr) {
                delete_dialog_font(state->font);
                if (state->background_brush != nullptr) {
                    DeleteObject(state->background_brush);
                }
                if (state->content_icon != nullptr) {
                    DestroyIcon(state->content_icon);
                }
                state->content_icon = nullptr;
                state->font = nullptr;
                state->background_brush = nullptr;
                state->hwnd = nullptr;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool register_message_dialog_class(HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &message_dialog_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kMessageDialogClass;
    assign_axiom_window_class_icons(window_class, instance);
    return RegisterClassExW(&window_class) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace

int show_message_dialog(HWND owner, HINSTANCE instance, UINT dpi, bool dark,
                        std::wstring_view title, std::wstring_view message,
                        MessageDialogIcon icon, MessageDialogButtons buttons,
                        int default_result) {
    if (instance == nullptr) {
        instance = reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr));
    }
    const UINT owner_dpi = owner != nullptr ? GetDpiForWindow(owner) : 0;
    const UINT effective_dpi = owner_dpi != 0
        ? owner_dpi
        : (dpi != 0 ? dpi : GetDpiForSystem());
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    const DWORD ex_style = WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT;
    const int client_width = scale_for_dialog_dpi(430, effective_dpi);
    const int margin = scale_for_dialog_dpi(18, effective_dpi);
    const int icon_width = icon == MessageDialogIcon::none
        ? 0
        : scale_for_dialog_dpi(kContentIconSlotPixels + kContentIconGapPixels,
                               effective_dpi);
    HFONT measure_font = create_dialog_font(effective_dpi);
    const int message_height = measure_message_height(
        message, client_width - (margin * 2) - icon_width, measure_font);
    delete_dialog_font(measure_font);
    const int content_height = std::max(
        icon == MessageDialogIcon::none
            ? 0
            : scale_for_dialog_dpi(kContentIconSlotPixels, effective_dpi),
        message_height);
    const int client_height = margin + content_height +
        scale_for_dialog_dpi(22 + 30, effective_dpi) + margin;

    RECT window_rect{0, 0, client_width, client_height};
    AdjustWindowRectExForDpi(&window_rect, style, FALSE, ex_style, effective_dpi);
    const int width = window_rect.right - window_rect.left;
    const int height = window_rect.bottom - window_rect.top;
    const RECT anchor = owner_or_monitor_rect(owner);
    const int x = anchor.left + (anchor.right - anchor.left - width) / 2;
    const int y = anchor.top + (anchor.bottom - anchor.top - height) / 2;

    const int safe_result = close_result_for(buttons) != 0
        ? close_result_for(buttons)
        : IDNO;
    if (!register_message_dialog_class(instance)) {
        OutputDebugStringW(L"Axiom could not register its message dialog class.\n");
        return safe_result;
    }

    MessageDialogState state{};
    state.owner = owner;
    state.instance = instance;
    state.title = std::wstring(title);
    state.message = std::wstring(message);
    state.button_specs = button_specs_for(buttons);
    state.icon = icon;
    state.button_set = buttons;
    state.dpi = effective_dpi;
    state.message_height = message_height;
    state.result = safe_result;
    state.default_result = default_result;
    state.dark = dark;

    HWND dialog = CreateWindowExW(
        ex_style, kMessageDialogClass, state.title.c_str(), style,
        x, y, width, height, owner, nullptr, instance, &state);
    if (dialog == nullptr) {
        OutputDebugStringW(L"Axiom could not create its message dialog.\n");
        return safe_result;
    }

    const bool owner_was_enabled = disable_dialog_owner(owner);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    MSG msg{};
    while (IsWindow(dialog)) {
        const BOOL status = GetMessageW(&msg, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) {
                PostQuitMessage(static_cast<int>(msg.wParam));
            }
            break;
        }
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    restore_dialog_owner(owner, owner_was_enabled);
    return state.result;
}

}  // namespace axiom::gui
