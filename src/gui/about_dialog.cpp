#include "gui/about_dialog.hpp"

#include "gui/dialog_support.hpp"
#include "gui/message_dialog.hpp"
#include "gui/update_checker.hpp"

#include <array>
#include <string>

namespace axiom::gui {
namespace {

constexpr wchar_t kAboutDialogClass[] = L"AxiomAboutDialog";
constexpr wchar_t kAuthor[] = L"Wimukthi Bandara";
constexpr wchar_t kLicense[] = L"GNU GPLv3";
constexpr int kAutoUpdateControl = 201;
constexpr int kCheckUpdatesControl = 202;

#define AXIOM_WIDEN2(value) L##value
#define AXIOM_WIDEN(value) AXIOM_WIDEN2(value)
constexpr wchar_t kBuildTimestamp[] = AXIOM_WIDEN(__DATE__) L" " AXIOM_WIDEN(__TIME__);
#undef AXIOM_WIDEN
#undef AXIOM_WIDEN2

struct AboutDialogState {
    HWND hwnd{};
    HWND owner{};
    HWND icon{};
    HWND title{};
    HWND description{};
    std::array<HWND, 4> metadata{};
    HWND auto_update{};
    HWND check_updates{};
    HWND ok{};
    HINSTANCE instance{};
    HFONT font{};
    HFONT title_font{};
    HBRUSH background_brush{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    UINT check_updates_command{};
    bool dark{};
};

std::array<HWND, 9> controls(AboutDialogState* state) {
    if (state == nullptr) return {};
    return {
        state->title, state->description,
        state->metadata[0], state->metadata[1], state->metadata[2], state->metadata[3],
        state->check_updates, state->auto_update, state->ok,
    };
}

HFONT create_title_font(HFONT base, UINT dpi) {
    LOGFONTW description{};
    if (base == nullptr || GetObjectW(base, sizeof(description), &description) == 0) {
        return nullptr;
    }
    description.lfHeight = -scale_for_dialog_dpi(24, dpi);
    description.lfWeight = FW_SEMIBOLD;
    return CreateFontIndirectW(&description);
}

void rebuild_fonts(AboutDialogState* state) {
    delete_dialog_font(state->font);
    if (state->title_font != nullptr) DeleteObject(state->title_font);
    state->font = create_dialog_font(state->dpi);
    state->title_font = create_title_font(state->font, state->dpi);
    for (HWND control : controls(state)) {
        set_dialog_control_font(control, state->font);
    }
    set_dialog_control_font(state->title,
                            state->title_font != nullptr ? state->title_font : state->font);
}

void layout(AboutDialogState* state) {
    if (state == nullptr || state->hwnd == nullptr) return;
    RECT client{};
    GetClientRect(state->hwnd, &client);
    const int margin = scale_for_dialog_dpi(20, state->dpi);
    const int icon_size = scale_for_dialog_dpi(56, state->dpi);
    const int title_left = margin + icon_size + scale_for_dialog_dpi(16, state->dpi);
    const int title_height = scale_for_dialog_dpi(32, state->dpi);
    const int description_top = margin + title_height + scale_for_dialog_dpi(4, state->dpi);
    const int description_height = scale_for_dialog_dpi(42, state->dpi);
    const int metadata_top = margin + scale_for_dialog_dpi(102, state->dpi);
    const int metadata_height = scale_for_dialog_dpi(22, state->dpi);
    const int button_width = scale_for_dialog_dpi(92, state->dpi);
    const int update_width = scale_for_dialog_dpi(138, state->dpi);
    const int button_height = scale_for_dialog_dpi(30, state->dpi);
    const int button_gap = scale_for_dialog_dpi(12, state->dpi);
    const int button_top = client.bottom - margin - button_height -
                           scale_for_dialog_dpi(8, state->dpi);

    MoveWindow(state->icon, margin, margin + scale_for_dialog_dpi(2, state->dpi),
               icon_size, icon_size, TRUE);
    MoveWindow(state->title, title_left, margin, client.right - title_left - margin,
               title_height, TRUE);
    MoveWindow(state->description, title_left, description_top,
               client.right - title_left - margin, description_height, TRUE);
    for (std::size_t index = 0; index < state->metadata.size(); ++index) {
        MoveWindow(state->metadata[index], margin,
                   metadata_top + static_cast<int>(index) * metadata_height,
                   client.right - margin * 2, metadata_height, TRUE);
    }
    MoveWindow(state->auto_update, margin, button_top + scale_for_dialog_dpi(4, state->dpi),
               scale_for_dialog_dpi(190, state->dpi), scale_for_dialog_dpi(22, state->dpi), TRUE);
    MoveWindow(state->check_updates,
               client.right - margin - button_width - button_gap - update_width,
               button_top, update_width, button_height, TRUE);
    MoveWindow(state->ok, client.right - margin - button_width, button_top,
               button_width, button_height, TRUE);
    InvalidateRect(state->hwnd, nullptr, TRUE);
}

void apply_theme(AboutDialogState* state) {
    apply_dialog_dark_frame(state->hwnd, state->dark);
    for (HWND control : controls(state)) {
        apply_dialog_control_theme(control, state->dark);
    }
}

LRESULT control_color(AboutDialogState* state, WPARAM wparam) {
    if (state == nullptr) return 0;
    const DialogColors colors = dialog_colors(state->dark);
    HDC dc = reinterpret_cast<HDC>(wparam);
    SetTextColor(dc, colors.text);
    SetBkColor(dc, colors.background);
    SetBkMode(dc, TRANSPARENT);
    return reinterpret_cast<LRESULT>(state->background_brush);
}

void paint(AboutDialogState* state) {
    PAINTSTRUCT paint_state{};
    HDC dc = BeginPaint(state->hwnd, &paint_state);
    FillRect(dc, &paint_state.rcPaint, state->background_brush);
    const DialogColors colors = dialog_colors(state->dark);
    RECT client{};
    GetClientRect(state->hwnd, &client);
    const int margin = scale_for_dialog_dpi(20, state->dpi);
    const int y = scale_for_dialog_dpi(92, state->dpi);
    HPEN pen = CreatePen(PS_SOLID, 1, colors.border);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    MoveToEx(dc, margin, y, nullptr);
    LineTo(dc, client.right - margin, y);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    EndPaint(state->hwnd, &paint_state);
}

LRESULT CALLBACK about_dialog_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AboutDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<AboutDialogState*>(create->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    switch (message) {
        case WM_CREATE: {
            state->background_brush = CreateSolidBrush(dialog_colors(state->dark).background);
            state->font = create_dialog_font(state->dpi);
            state->title_font = create_title_font(state->font, state->dpi);
            state->icon = CreateWindowExW(
                0, L"STATIC", nullptr,
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_ICON | SS_CENTERIMAGE,
                0, 0, 0, 0, hwnd, nullptr, state->instance, nullptr);
            SendMessageW(state->icon, STM_SETICON,
                         reinterpret_cast<WPARAM>(load_axiom_icon(
                             state->instance, scale_for_dialog_dpi(64, state->dpi),
                             scale_for_dialog_dpi(64, state->dpi))), 0);
            state->title = CreateWindowExW(
                0, L"STATIC", L"Axiom",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_NOPREFIX,
                0, 0, 0, 0, hwnd, nullptr, state->instance, nullptr);
            state->description = CreateWindowExW(
                0, L"STATIC",
                L"A native C++ Win32 archive manager and compression frontend focused on speed, integrity, and a dark-first file browser.",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_NOPREFIX,
                0, 0, 0, 0, hwnd, nullptr, state->instance, nullptr);
            const std::array<std::wstring, 4> metadata_text{
                L"Version: " + current_executable_version(state->instance),
                L"Build: " + std::wstring(kBuildTimestamp),
                L"Author: " + std::wstring(kAuthor),
                L"Licence: " + std::wstring(kLicense),
            };
            for (std::size_t index = 0; index < state->metadata.size(); ++index) {
                state->metadata[index] = CreateWindowExW(
                    0, L"STATIC", metadata_text[index].c_str(),
                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_NOPREFIX,
                    0, 0, 0, 0, hwnd, nullptr, state->instance, nullptr);
            }
            state->auto_update = CreateWindowExW(
                0, L"BUTTON", L"Check automatically",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_AUTOCHECKBOX,
                0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAutoUpdateControl)),
                state->instance, nullptr);
            SendMessageW(state->auto_update, BM_SETCHECK,
                         automatic_update_checks_enabled() ? BST_CHECKED : BST_UNCHECKED, 0);
            state->check_updates = CreateWindowExW(
                0, L"BUTTON", L"Check for Updates...",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckUpdatesControl)),
                state->instance, nullptr);
            state->ok = CreateWindowExW(
                0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDOK), state->instance, nullptr);
            for (HWND control : controls(state)) set_dialog_control_font(control, state->font);
            set_dialog_control_font(state->title,
                                    state->title_font != nullptr ? state->title_font : state->font);
            apply_theme(state);
            layout(state);
            SendMessageW(hwnd, DM_SETDEFID, IDOK, 0);
            SetFocus(state->ok);
            return 0;
        }
        case WM_SIZE:
            layout(state);
            return 0;
        case WM_DPICHANGED: {
            state->dpi = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            rebuild_fonts(state);
            SendMessageW(state->icon, STM_SETICON,
                         reinterpret_cast<WPARAM>(load_axiom_icon(
                             state->instance, scale_for_dialog_dpi(64, state->dpi),
                             scale_for_dialog_dpi(64, state->dpi))), 0);
            layout(state);
            return 0;
        }
        case WM_PAINT:
            paint(state);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wparam) == kAutoUpdateControl) {
                set_automatic_update_checks_enabled(
                    SendMessageW(state->auto_update, BM_GETCHECK, 0, 0) == BST_CHECKED);
                return 0;
            }
            if (LOWORD(wparam) == kCheckUpdatesControl) {
                PostMessageW(state->owner, WM_COMMAND,
                             MAKEWPARAM(state->check_updates_command, 0), 0);
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
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
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (state != nullptr) {
                delete_dialog_font(state->font);
                if (state->title_font != nullptr) DeleteObject(state->title_font);
                if (state->background_brush != nullptr) DeleteObject(state->background_brush);
                state->hwnd = nullptr;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

void show_about_dialog(HWND owner, HINSTANCE instance, UINT dpi, bool dark,
                       UINT check_updates_command) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &about_dialog_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kAboutDialogClass;
    assign_axiom_window_class_icons(window_class, instance);
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        show_message_dialog(owner, instance, dpi, dark, L"Axiom - About",
                            last_error_text(), MessageDialogIcon::error);
        return;
    }

    const UINT effective_dpi = dpi == 0 ? GetDpiForWindow(owner) : dpi;
    RECT owner_rect{};
    GetWindowRect(owner, &owner_rect);
    const int width = scale_for_dialog_dpi(500, effective_dpi);
    const int height = scale_for_dialog_dpi(324, effective_dpi);
    AboutDialogState state{};
    state.owner = owner;
    state.instance = instance;
    state.dpi = effective_dpi;
    state.check_updates_command = check_updates_command;
    state.dark = dark;
    HWND dialog = CreateWindowExW(
        WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT, kAboutDialogClass, L"About Axiom",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        owner_rect.left + (owner_rect.right - owner_rect.left - width) / 2,
        owner_rect.top + (owner_rect.bottom - owner_rect.top - height) / 2,
        width, height, owner, nullptr, instance, &state);
    if (dialog == nullptr) {
        show_message_dialog(owner, instance, effective_dpi, dark, L"Axiom - About",
                            last_error_text(), MessageDialogIcon::error);
        return;
    }
    apply_axiom_window_icons(dialog, instance);
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    MSG message{};
    while (IsWindow(dialog)) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (message_targets_window(dialog, message) &&
            message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            DestroyWindow(dialog);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
        SetFocus(owner);
    }
}

}  // namespace axiom::gui
