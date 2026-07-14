#define NOMINMAX
#include "gui/archive_feature_dialogs.hpp"

#include "gui/dialog_support.hpp"
#include "gui/message_dialog.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace axiom::gui {
namespace fs = std::filesystem;
namespace {

constexpr wchar_t kPasswordPromptClass[] = L"AxiomArchivePasswordPrompt";
constexpr wchar_t kCommentEditorClass[] = L"AxiomArchiveCommentEditor";
constexpr DWORD kSimpleDialogStyle =
    WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS |
    WS_THICKFRAME;
constexpr DWORD kSimpleDialogExStyle = WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT;

constexpr int kAccept = IDOK;
constexpr int kCancel = IDCANCEL;

constexpr int kSimplePasswordEdit = 4301;
constexpr int kSimpleShowPassword = 4302;
constexpr int kSimpleCommentEdit = 4311;

std::wstring control_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void clear_sensitive_text(std::wstring& text) {
    if (!text.empty()) {
        SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
        text.clear();
    }
}

void set_password_visible(HWND edit, bool visible) {
    if (edit == nullptr) return;
    SendMessageW(edit, EM_SETPASSWORDCHAR, visible ? 0 : static_cast<WPARAM>(L'\u25CF'), 0);
    InvalidateRect(edit, nullptr, TRUE);
}

struct SimplePasswordDialogState {
    HWND window{};
    HWND owner{};
    HWND label{};
    HWND edit{};
    HWND show{};
    HWND ok{};
    HWND cancel{};
    HINSTANCE instance{};
    HFONT font{};
    HBRUSH background_brush{};
    HBRUSH control_brush{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    bool dark{};
    bool accepted{};
    std::wstring password;
};

std::array<HWND, 5> simple_password_controls(SimplePasswordDialogState* state) {
    if (state == nullptr) return {};
    return {state->label, state->edit, state->show, state->ok, state->cancel};
}

void layout_simple_password_dialog(SimplePasswordDialogState* state) {
    if (state == nullptr || state->window == nullptr) return;
    RECT client{};
    GetClientRect(state->window, &client);
    const int margin = scale_for_dialog_dpi(24, state->dpi);
    const int label_width = scale_for_dialog_dpi(90, state->dpi);
    const int row_height = scale_for_dialog_dpi(30, state->dpi);
    const int button_width = scale_for_dialog_dpi(88, state->dpi);
    const int button_height = scale_for_dialog_dpi(30, state->dpi);
    const int gap = scale_for_dialog_dpi(10, state->dpi);
    const int edit_left = margin + label_width + scale_for_dialog_dpi(12, state->dpi);
    const int edit_width = client.right - edit_left - margin;
    MoveWindow(state->label, margin, margin + scale_for_dialog_dpi(4, state->dpi),
               label_width, row_height, TRUE);
    MoveWindow(state->edit, edit_left, margin, edit_width, row_height, TRUE);
    MoveWindow(state->show, edit_left, margin + scale_for_dialog_dpi(42, state->dpi),
               scale_for_dialog_dpi(180, state->dpi), scale_for_dialog_dpi(24, state->dpi),
               TRUE);
    const int button_top = client.bottom - margin - button_height;
    MoveWindow(state->cancel, client.right - margin - button_width, button_top,
               button_width, button_height, TRUE);
    MoveWindow(state->ok, client.right - margin - button_width * 2 - gap, button_top,
               button_width, button_height, TRUE);
    InvalidateRect(state->window, nullptr, TRUE);
}

void rebuild_simple_password_fonts(SimplePasswordDialogState* state) {
    delete_dialog_font(state->font);
    state->font = create_dialog_font(state->dpi);
    for (HWND control : simple_password_controls(state)) {
        set_dialog_control_font(control, state->font);
    }
}

void apply_simple_password_theme(SimplePasswordDialogState* state) {
    if (state == nullptr) return;
    apply_dialog_dark_frame(state->window, state->dark);
    for (HWND control : simple_password_controls(state)) {
        apply_dialog_control_theme(control, state->dark);
    }
}

LRESULT simple_dialog_control_color(HWND, HBRUSH background, HBRUSH control,
                                    bool dark, WPARAM wparam, bool edit) {
    const DialogColors colors = dialog_colors(dark);
    HDC dc = reinterpret_cast<HDC>(wparam);
    SetTextColor(dc, colors.text);
    SetBkColor(dc, edit ? colors.control_background : colors.background);
    SetBkMode(dc, edit ? OPAQUE : TRANSPARENT);
    return reinterpret_cast<LRESULT>(edit ? control : background);
}

LRESULT CALLBACK simple_password_dialog_proc(HWND hwnd, UINT message,
                                             WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<SimplePasswordDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = create == nullptr
                    ? nullptr
                    : static_cast<SimplePasswordDialogState*>(create->lpCreateParams);
        if (state == nullptr) return FALSE;
        state->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    switch (message) {
        case WM_CREATE: {
            const DialogColors colors = dialog_colors(state->dark);
            state->background_brush = CreateSolidBrush(colors.background);
            state->control_brush = CreateSolidBrush(colors.control_background);
            state->font = create_dialog_font(state->dpi);
            state->label = CreateWindowExW(
                0, L"STATIC", L"Password",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_NOPREFIX,
                0, 0, 0, 0, hwnd, nullptr, state->instance, nullptr);
            state->edit = CreateWindowExW(
                0, L"EDIT", state->password.c_str(),
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP |
                    ES_PASSWORD | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimplePasswordEdit)),
                state->instance, nullptr);
            state->show = CreateWindowExW(
                0, L"BUTTON", L"Show password",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP |
                    BS_OWNERDRAW | BS_CHECKBOX,
                0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleShowPassword)),
                state->instance, nullptr);
            state->ok = CreateWindowExW(
                0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDOK),
                state->instance, nullptr);
            state->cancel = CreateWindowExW(
                0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDCANCEL),
                state->instance, nullptr);
            for (HWND control : simple_password_controls(state)) {
                set_dialog_control_font(control, state->font);
            }
            apply_simple_password_theme(state);
            set_password_visible(state->edit, false);
            layout_simple_password_dialog(state);
            SendMessageW(hwnd, DM_SETDEFID, IDOK, 0);
            SetFocus(state->edit);
            SendMessageW(state->edit, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_SIZE:
            layout_simple_password_dialog(state);
            return 0;
        case WM_DPICHANGED: {
            state->dpi = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            apply_axiom_window_icons(hwnd, state->instance);
            rebuild_simple_password_fonts(state);
            layout_simple_password_dialog(state);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const SIZE minimum = dialog_window_size_for_client(
                420, 178, kSimpleDialogStyle, kSimpleDialogExStyle, state->dpi);
            info->ptMinTrackSize.x = minimum.cx;
            info->ptMinTrackSize.y = minimum.cy;
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == kSimpleShowPassword) {
                const bool checked =
                    SendMessageW(state->show, BM_GETCHECK, 0, 0) != BST_CHECKED;
                SendMessageW(state->show, BM_SETCHECK,
                             checked ? BST_CHECKED : BST_UNCHECKED, 0);
                set_password_visible(state->edit, checked);
                InvalidateRect(state->show, nullptr, FALSE);
                return 0;
            }
            if (LOWORD(wparam) == IDOK) {
                state->password = control_text(state->edit);
                state->accepted = true;
                SetWindowTextW(state->edit, L"");
                save_named_window_placement(L"ArchivePasswordPrompt", hwnd);
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                SetWindowTextW(state->edit, L"");
                save_named_window_placement(L"ArchivePasswordPrompt", hwnd);
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
            return simple_dialog_control_color(hwnd, state->background_brush,
                                               state->control_brush, state->dark,
                                               wparam, false);
        case WM_CTLCOLOREDIT:
            return simple_dialog_control_color(hwnd, state->background_brush,
                                               state->control_brush, state->dark,
                                               wparam, true);
        case WM_DRAWITEM:
            if (lparam != 0) {
                const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
                if (draw.CtlID == kSimpleShowPassword) {
                    draw_dialog_checkbox(
                        draw, state->dark,
                        SendMessageW(state->show, BM_GETCHECK, 0, 0) == BST_CHECKED);
                } else {
                    draw_dialog_button(draw, state->dark);
                }
                return TRUE;
            }
            break;
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client, state->background_brush);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            FillRect(dc, &paint.rcPaint, state->background_brush);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_CLOSE:
            SetWindowTextW(state->edit, L"");
            save_named_window_placement(L"ArchivePasswordPrompt", hwnd);
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (state != nullptr) {
                delete_dialog_font(state->font);
                if (state->background_brush != nullptr) DeleteObject(state->background_brush);
                if (state->control_brush != nullptr) DeleteObject(state->control_brush);
                state->window = nullptr;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

struct SimpleCommentDialogState {
    HWND window{};
    HWND owner{};
    HWND label{};
    HWND edit{};
    HWND ok{};
    HWND cancel{};
    HINSTANCE instance{};
    HFONT font{};
    HBRUSH background_brush{};
    HBRUSH control_brush{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    bool dark{};
    bool accepted{};
    std::wstring comment;
};

std::array<HWND, 4> simple_comment_controls(SimpleCommentDialogState* state) {
    if (state == nullptr) return {};
    return {state->label, state->edit, state->ok, state->cancel};
}

void layout_simple_comment_dialog(SimpleCommentDialogState* state) {
    if (state == nullptr || state->window == nullptr) return;
    RECT client{};
    GetClientRect(state->window, &client);
    const int margin = scale_for_dialog_dpi(20, state->dpi);
    const int label_height = scale_for_dialog_dpi(24, state->dpi);
    const int button_width = scale_for_dialog_dpi(88, state->dpi);
    const int button_height = scale_for_dialog_dpi(30, state->dpi);
    const int gap = scale_for_dialog_dpi(10, state->dpi);
    const int button_top = client.bottom - margin - button_height;
    MoveWindow(state->label, margin, margin, client.right - margin * 2,
               label_height, TRUE);
    MoveWindow(state->edit, margin, margin + label_height + scale_for_dialog_dpi(8, state->dpi),
               client.right - margin * 2,
               button_top - margin - label_height - scale_for_dialog_dpi(20, state->dpi),
               TRUE);
    MoveWindow(state->cancel, client.right - margin - button_width, button_top,
               button_width, button_height, TRUE);
    MoveWindow(state->ok, client.right - margin - button_width * 2 - gap, button_top,
               button_width, button_height, TRUE);
    InvalidateRect(state->window, nullptr, TRUE);
}

void rebuild_simple_comment_fonts(SimpleCommentDialogState* state) {
    delete_dialog_font(state->font);
    state->font = create_dialog_font(state->dpi);
    for (HWND control : simple_comment_controls(state)) {
        set_dialog_control_font(control, state->font);
    }
}

void apply_simple_comment_theme(SimpleCommentDialogState* state) {
    if (state == nullptr) return;
    apply_dialog_dark_frame(state->window, state->dark);
    for (HWND control : simple_comment_controls(state)) {
        apply_dialog_control_theme(control, state->dark);
    }
}

LRESULT CALLBACK simple_comment_dialog_proc(HWND hwnd, UINT message,
                                            WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<SimpleCommentDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = create == nullptr
                    ? nullptr
                    : static_cast<SimpleCommentDialogState*>(create->lpCreateParams);
        if (state == nullptr) return FALSE;
        state->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    switch (message) {
        case WM_CREATE: {
            const DialogColors colors = dialog_colors(state->dark);
            state->background_brush = CreateSolidBrush(colors.background);
            state->control_brush = CreateSolidBrush(colors.control_background);
            state->font = create_dialog_font(state->dpi);
            state->label = CreateWindowExW(
                0, L"STATIC", L"Archive comment",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_NOPREFIX,
                0, 0, 0, 0, hwnd, nullptr, state->instance, nullptr);
            state->edit = CreateWindowExW(
                0, L"EDIT", state->comment.c_str(),
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP |
                    ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
                0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleCommentEdit)),
                state->instance, nullptr);
            state->ok = CreateWindowExW(
                0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDOK),
                state->instance, nullptr);
            state->cancel = CreateWindowExW(
                0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDCANCEL),
                state->instance, nullptr);
            for (HWND control : simple_comment_controls(state)) {
                set_dialog_control_font(control, state->font);
            }
            apply_simple_comment_theme(state);
            layout_simple_comment_dialog(state);
            SendMessageW(hwnd, DM_SETDEFID, IDOK, 0);
            SetFocus(state->edit);
            SendMessageW(state->edit, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_SIZE:
            layout_simple_comment_dialog(state);
            return 0;
        case WM_DPICHANGED: {
            state->dpi = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            apply_axiom_window_icons(hwnd, state->instance);
            rebuild_simple_comment_fonts(state);
            layout_simple_comment_dialog(state);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const SIZE minimum = dialog_window_size_for_client(
                520, 300, kSimpleDialogStyle, kSimpleDialogExStyle, state->dpi);
            info->ptMinTrackSize.x = minimum.cx;
            info->ptMinTrackSize.y = minimum.cy;
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK) {
                state->comment = control_text(state->edit);
                state->accepted = true;
                save_named_window_placement(L"ArchiveCommentEditor", hwnd);
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                save_named_window_placement(L"ArchiveCommentEditor", hwnd);
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
            return simple_dialog_control_color(hwnd, state->background_brush,
                                               state->control_brush, state->dark,
                                               wparam, false);
        case WM_CTLCOLOREDIT:
            return simple_dialog_control_color(hwnd, state->background_brush,
                                               state->control_brush, state->dark,
                                               wparam, true);
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
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            FillRect(dc, &paint.rcPaint, state->background_brush);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_CLOSE:
            save_named_window_placement(L"ArchiveCommentEditor", hwnd);
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (state != nullptr) {
                delete_dialog_font(state->font);
                if (state->background_brush != nullptr) DeleteObject(state->background_brush);
                if (state->control_brush != nullptr) DeleteObject(state->control_brush);
                state->window = nullptr;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool run_simple_password_dialog(HWND owner, std::wstring& password) {
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (instance == nullptr) instance = GetModuleHandleW(nullptr);
    const UINT dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    SimplePasswordDialogState state{};
    state.owner = owner;
    state.instance = instance;
    state.dpi = dpi;
    state.dark = dialog_system_prefers_dark_mode();
    state.password = password;

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &simple_password_dialog_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kPasswordPromptClass;
    assign_axiom_window_class_icons(window_class, instance);
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        show_message_dialog(owner, instance, dpi, state.dark, L"Archive password",
                            last_error_text(), MessageDialogIcon::error);
        return false;
    }

    const SIZE window_size = dialog_window_size_for_client(
        430, 188, kSimpleDialogStyle, kSimpleDialogExStyle, dpi);
    const int width = window_size.cx;
    const int height = window_size.cy;
    const POINT position = centered_window_position(owner, width, height);
    HWND dialog = CreateWindowExW(
        kSimpleDialogExStyle, kPasswordPromptClass, L"Archive password",
        kSimpleDialogStyle,
        position.x, position.y, width, height, owner, nullptr, instance, &state);
    if (dialog == nullptr) {
        show_message_dialog(owner, instance, dpi, state.dark, L"Archive password",
                            last_error_text(), MessageDialogIcon::error);
        return false;
    }
    apply_axiom_window_icons(dialog, instance);
    restore_named_window_placement(dialog, owner, L"ArchivePasswordPrompt");
    const bool owner_was_enabled = disable_dialog_owner(owner, dialog);
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
            save_named_window_placement(L"ArchivePasswordPrompt", dialog);
            DestroyWindow(dialog);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    restore_dialog_owner(owner, owner_was_enabled);
    if (!state.accepted) {
        clear_sensitive_text(state.password);
        return false;
    }
    password = std::move(state.password);
    return true;
}

bool run_simple_comment_dialog(HWND owner, std::wstring& comment) {
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (instance == nullptr) instance = GetModuleHandleW(nullptr);
    const UINT dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    SimpleCommentDialogState state{};
    state.owner = owner;
    state.instance = instance;
    state.dpi = dpi;
    state.dark = dialog_system_prefers_dark_mode();
    state.comment = comment;

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &simple_comment_dialog_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kCommentEditorClass;
    assign_axiom_window_class_icons(window_class, instance);
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        show_message_dialog(owner, instance, dpi, state.dark, L"Archive comment",
                            last_error_text(), MessageDialogIcon::error);
        return false;
    }

    const SIZE window_size = dialog_window_size_for_client(
        580, 380, kSimpleDialogStyle, kSimpleDialogExStyle, dpi);
    const int width = window_size.cx;
    const int height = window_size.cy;
    const POINT position = centered_window_position(owner, width, height);
    HWND dialog = CreateWindowExW(
        kSimpleDialogExStyle, kCommentEditorClass, L"Archive comment",
        kSimpleDialogStyle,
        position.x, position.y, width, height, owner, nullptr, instance, &state);
    if (dialog == nullptr) {
        show_message_dialog(owner, instance, dpi, state.dark, L"Archive comment",
                            last_error_text(), MessageDialogIcon::error);
        return false;
    }
    apply_axiom_window_icons(dialog, instance);
    restore_named_window_placement(dialog, owner, L"ArchiveCommentEditor");
    const bool owner_was_enabled = disable_dialog_owner(owner, dialog);
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
            save_named_window_placement(L"ArchiveCommentEditor", dialog);
            DestroyWindow(dialog);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    restore_dialog_owner(owner, owner_was_enabled);
    if (!state.accepted) return false;
    comment = std::move(state.comment);
    return true;
}

constexpr wchar_t kArchiveSummaryDialogClass[] = L"AxiomArchiveSummaryDialog";
constexpr DWORD kArchiveSummaryDialogStyle =
    WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
constexpr DWORD kArchiveSummaryDialogExStyle =
    WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT;

struct ArchiveSummaryCapabilityRow {
    std::wstring label;
    bool supported = false;
};

struct ArchiveSummaryDialogData {
    std::wstring title;
    std::wstring heading;
    std::wstring subheading;
    ArchiveSummaryRows details;
    std::wstring archive_comment;
    std::vector<ArchiveSummaryCapabilityRow> capabilities;
};

struct ArchiveSummaryDialogState {
    HWND hwnd{};
    HWND owner{};
    HWND ok{};
    HINSTANCE instance{};
    HFONT font{};
    HFONT title_font{};
    HFONT section_font{};
    HBRUSH background_brush{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    bool dark{};
    int scroll_y{};
    int content_height{};
    bool dragging_scrollbar{};
    int drag_offset_y{};
    HDC buffer_dc{};
    HBITMAP buffer_bitmap{};
    HGDIOBJ buffer_old_bitmap{};
    int buffer_width{};
    int buffer_height{};
    ArchiveSummaryDialogData data;
};

HFONT create_scaled_font(HFONT base, UINT dpi, int point_height, int weight) {
    LOGFONTW description{};
    if (base == nullptr || GetObjectW(base, sizeof(description), &description) == 0) {
        return nullptr;
    }
    description.lfHeight = -scale_for_dialog_dpi(point_height, dpi);
    description.lfWeight = weight;
    return CreateFontIndirectW(&description);
}

std::array<HWND, 1> summary_controls(ArchiveSummaryDialogState* state) {
    if (state == nullptr) return {};
    return {state->ok};
}

int summary_button_height(const ArchiveSummaryDialogState* state) {
    return scale_for_dialog_dpi(30, state->dpi);
}

struct ArchiveSummaryPalette {
    COLORREF window;
    COLORREF panel;
    COLORREF panel_alt;
    COLORREF header;
    COLORREF border;
    COLORREF text;
    COLORREF muted;
    COLORREF supported;
    COLORREF scrollbar_track;
    COLORREF scrollbar_thumb;
    COLORREF scrollbar_thumb_hot;
};

ArchiveSummaryPalette archive_summary_palette(bool dark) {
    if (dark) {
        return {
            RGB(31, 31, 31), RGB(37, 37, 38), RGB(34, 34, 35),
            RGB(45, 45, 48), RGB(67, 67, 70), RGB(242, 242, 242),
            RGB(190, 190, 190), RGB(96, 210, 132), RGB(38, 38, 40),
            RGB(91, 91, 95), RGB(122, 122, 126),
        };
    }
    return {
        RGB(248, 248, 248), RGB(255, 255, 255), RGB(246, 246, 246),
        RGB(238, 238, 238), RGB(190, 190, 190), RGB(20, 20, 20),
        RGB(90, 90, 90), RGB(0, 128, 64), RGB(230, 230, 230),
        RGB(150, 150, 150), RGB(110, 110, 110),
    };
}

void fill_summary_rect(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

int summary_scrollbar_width(const ArchiveSummaryDialogState* state) {
    return scale_for_dialog_dpi(8, state->dpi);
}

int summary_scrollbar_gap(const ArchiveSummaryDialogState* state) {
    return scale_for_dialog_dpi(8, state->dpi);
}

bool summary_has_comment(const ArchiveSummaryDialogState* state) {
    return state != nullptr && !state->data.archive_comment.empty();
}

int summary_comment_box_height(const ArchiveSummaryDialogState* state) {
    const int line_height = scale_for_dialog_dpi(18, state->dpi);
    int line_count = 1;
    std::size_t current_line_chars = 0;
    for (const wchar_t ch : state->data.archive_comment) {
        if (ch == L'\r') continue;
        if (ch == L'\n') {
            ++line_count;
            current_line_chars = 0;
            continue;
        }
        ++current_line_chars;
        if (current_line_chars >= 84) {
            ++line_count;
            current_line_chars = 0;
        }
    }
    line_count = std::clamp(line_count, 3, 14);
    return scale_for_dialog_dpi(18, state->dpi) + line_count * line_height;
}

bool ensure_summary_buffer(ArchiveSummaryDialogState* state, HDC reference_dc,
                           int width, int height) {
    if (state == nullptr || reference_dc == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    if (state->buffer_dc == nullptr) {
        state->buffer_dc = CreateCompatibleDC(reference_dc);
        if (state->buffer_dc == nullptr) return false;
    }
    if (state->buffer_bitmap == nullptr || width > state->buffer_width ||
        height > state->buffer_height) {
        const int next_width = std::max(width, state->buffer_width);
        const int next_height = std::max(height, state->buffer_height);
        HBITMAP next_bitmap = CreateCompatibleBitmap(reference_dc, next_width, next_height);
        if (next_bitmap == nullptr) return false;
        if (state->buffer_bitmap != nullptr) {
            SelectObject(state->buffer_dc, state->buffer_old_bitmap);
            DeleteObject(state->buffer_bitmap);
        }
        state->buffer_bitmap = next_bitmap;
        state->buffer_old_bitmap = SelectObject(state->buffer_dc, state->buffer_bitmap);
        state->buffer_width = next_width;
        state->buffer_height = next_height;
    }
    return state->buffer_dc != nullptr && state->buffer_bitmap != nullptr;
}

void release_summary_buffer(ArchiveSummaryDialogState* state) {
    if (state == nullptr) return;
    if (state->buffer_dc != nullptr) {
        if (state->buffer_bitmap != nullptr) {
            SelectObject(state->buffer_dc, state->buffer_old_bitmap);
            DeleteObject(state->buffer_bitmap);
        }
        DeleteDC(state->buffer_dc);
    }
    state->buffer_dc = nullptr;
    state->buffer_bitmap = nullptr;
    state->buffer_old_bitmap = nullptr;
    state->buffer_width = 0;
    state->buffer_height = 0;
}

int summary_content_bottom(const ArchiveSummaryDialogState* state, const RECT& client) {
    const int margin = scale_for_dialog_dpi(20, state->dpi);
    return client.bottom - margin - summary_button_height(state) -
           scale_for_dialog_dpi(14, state->dpi);
}

int summary_content_height(const ArchiveSummaryDialogState* state) {
    const int margin = scale_for_dialog_dpi(20, state->dpi);
    const int table_header = scale_for_dialog_dpi(28, state->dpi);
    const int row_height = scale_for_dialog_dpi(26, state->dpi);
    const int rows = static_cast<int>(state->data.details.size()) +
                     static_cast<int>(state->data.capabilities.size()) + 1;

    int y = margin + scale_for_dialog_dpi(68, state->dpi);
    y += table_header + rows * row_height;
    if (summary_has_comment(state)) {
        y += scale_for_dialog_dpi(10, state->dpi);
        y += row_height + summary_comment_box_height(state);
    }
    y += margin;
    return y;
}

void clamp_summary_scroll(ArchiveSummaryDialogState* state) {
    RECT client{};
    GetClientRect(state->hwnd, &client);
    const int page = std::max(1, summary_content_bottom(state, client));
    const int max_scroll = std::max(0, state->content_height - page);
    state->scroll_y = std::clamp(state->scroll_y, 0, max_scroll);
}

int summary_max_scroll(const ArchiveSummaryDialogState* state, const RECT& client) {
    const int page = std::max(1, summary_content_bottom(state, client));
    return std::max(0, state->content_height - page);
}

bool summary_needs_scrollbar(const ArchiveSummaryDialogState* state,
                             const RECT& client) {
    return summary_max_scroll(state, client) > 0;
}

RECT summary_scrollbar_track_rect(const ArchiveSummaryDialogState* state,
                                  const RECT& client) {
    const int width = summary_scrollbar_width(state);
    const int margin = scale_for_dialog_dpi(6, state->dpi);
    const int top = scale_for_dialog_dpi(40, state->dpi);
    const int bottom = summary_content_bottom(state, client) - margin;
    return {client.right - margin - width, top, client.right - margin, bottom};
}

RECT summary_scrollbar_thumb_rect(const ArchiveSummaryDialogState* state,
                                  const RECT& client) {
    RECT track = summary_scrollbar_track_rect(state, client);
    const int track_height = std::max(1, static_cast<int>(track.bottom - track.top));
    const int page = std::max(1, summary_content_bottom(state, client));
    const int content = std::max(page, state->content_height);
    const int min_thumb = std::min(scale_for_dialog_dpi(28, state->dpi), track_height);
    const int thumb_height =
        std::clamp(track_height * page / content, min_thumb, track_height);
    const int max_scroll = summary_max_scroll(state, client);
    const int travel = std::max(0, track_height - thumb_height);
    const int thumb_top =
        track.top + (max_scroll > 0 ? (travel * state->scroll_y) / max_scroll : 0);
    return {track.left, thumb_top, track.right, thumb_top + thumb_height};
}

void set_summary_scroll_from_thumb(ArchiveSummaryDialogState* state, int thumb_top) {
    RECT client{};
    GetClientRect(state->hwnd, &client);
    const RECT track = summary_scrollbar_track_rect(state, client);
    const RECT thumb = summary_scrollbar_thumb_rect(state, client);
    const int thumb_height = thumb.bottom - thumb.top;
    const int travel =
        std::max(1, static_cast<int>(track.bottom - track.top) - thumb_height);
    const int max_scroll = summary_max_scroll(state, client);
    const int clamped_top =
        std::clamp(thumb_top, static_cast<int>(track.top),
                   static_cast<int>(track.bottom) - thumb_height);
    state->scroll_y = max_scroll > 0
                          ? ((clamped_top - track.top) * max_scroll) / travel
                          : 0;
    clamp_summary_scroll(state);
    InvalidateRect(state->hwnd, nullptr, FALSE);
}

void update_summary_scrollbar(ArchiveSummaryDialogState* state) {
    if (state == nullptr || state->hwnd == nullptr) return;
    state->content_height = summary_content_height(state);
    clamp_summary_scroll(state);
}

void scroll_summary_by(ArchiveSummaryDialogState* state, int delta) {
    if (state == nullptr || state->hwnd == nullptr) return;
    state->scroll_y += delta;
    update_summary_scrollbar(state);
    InvalidateRect(state->hwnd, nullptr, FALSE);
}

void rebuild_summary_fonts(ArchiveSummaryDialogState* state) {
    delete_dialog_font(state->font);
    if (state->title_font != nullptr) DeleteObject(state->title_font);
    if (state->section_font != nullptr) DeleteObject(state->section_font);
    state->font = create_dialog_font(state->dpi);
    state->title_font = create_scaled_font(state->font, state->dpi, 20, FW_SEMIBOLD);
    state->section_font = create_scaled_font(state->font, state->dpi, 13, FW_SEMIBOLD);
    for (HWND control : summary_controls(state)) {
        set_dialog_control_font(control, state->font);
    }
}

void layout_summary(ArchiveSummaryDialogState* state) {
    if (state == nullptr || state->hwnd == nullptr) return;
    RECT client{};
    GetClientRect(state->hwnd, &client);
    const int margin = scale_for_dialog_dpi(20, state->dpi);
    const int button_width = scale_for_dialog_dpi(96, state->dpi);
    const int button_height = scale_for_dialog_dpi(30, state->dpi);
    MoveWindow(state->ok, client.right - margin - button_width,
               client.bottom - margin - button_height, button_width, button_height, TRUE);
    update_summary_scrollbar(state);
    InvalidateRect(state->hwnd, nullptr, FALSE);
}

void apply_summary_theme(ArchiveSummaryDialogState* state) {
    apply_dialog_dark_frame(state->hwnd, state->dark);
    for (HWND control : summary_controls(state)) {
        apply_dialog_control_theme(control, state->dark);
    }
}

void draw_text_in_rect(HDC dc, const std::wstring& text, RECT rect, UINT format) {
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | DT_NOPREFIX);
}

void draw_summary_scrollbar(ArchiveSummaryDialogState* state, HDC dc,
                            const RECT& client) {
    if (!summary_needs_scrollbar(state, client)) return;
    const ArchiveSummaryPalette palette = archive_summary_palette(state->dark);
    const RECT track = summary_scrollbar_track_rect(state, client);
    const RECT thumb = summary_scrollbar_thumb_rect(state, client);
    fill_summary_rect(dc, track, palette.scrollbar_track);
    fill_summary_rect(dc, thumb,
                      state->dragging_scrollbar ? palette.scrollbar_thumb_hot
                                                : palette.scrollbar_thumb);
}

void paint_archive_summary(ArchiveSummaryDialogState* state) {
    PAINTSTRUCT paint_state{};
    HDC window_dc = BeginPaint(state->hwnd, &paint_state);
    const ArchiveSummaryPalette palette = archive_summary_palette(state->dark);

    RECT client{};
    GetClientRect(state->hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HDC dc = window_dc;
    if (ensure_summary_buffer(state, window_dc, width, height)) {
        dc = state->buffer_dc;
    }
    FillRect(dc, &client, state->background_brush);
    SetBkMode(dc, TRANSPARENT);

    const int content_bottom = summary_content_bottom(state, client);
    HPEN footer_pen = CreatePen(PS_SOLID, 1, palette.border);
    HGDIOBJ footer_old_pen = SelectObject(dc, footer_pen);
    MoveToEx(dc, 0, content_bottom, nullptr);
    LineTo(dc, client.right, content_bottom);
    SelectObject(dc, footer_old_pen);
    DeleteObject(footer_pen);

    SaveDC(dc);
    IntersectClipRect(dc, 0, 0, client.right, content_bottom);
    SetViewportOrgEx(dc, 0, -state->scroll_y, nullptr);

    const int margin = scale_for_dialog_dpi(20, state->dpi);
    const int icon_size = scale_for_dialog_dpi(40, state->dpi);
    const int header_left = margin + icon_size + scale_for_dialog_dpi(14, state->dpi);
    const bool needs_scrollbar = summary_needs_scrollbar(state, client);
    const int content_right =
        client.right - margin -
        (needs_scrollbar ? summary_scrollbar_width(state) + summary_scrollbar_gap(state) : 0);
    const int content_width = std::max(1, content_right - margin);

    HICON icon = load_axiom_icon(state->instance, icon_size, icon_size);
    if (icon != nullptr) {
        DrawIconEx(dc, margin, margin, icon, icon_size, icon_size, 0, nullptr, DI_NORMAL);
    }

    HGDIOBJ old_font = SelectObject(dc, state->title_font != nullptr ? state->title_font : state->font);
    SetTextColor(dc, palette.text);
    RECT heading_rect{header_left, margin - scale_for_dialog_dpi(2, state->dpi),
                      content_right, margin + scale_for_dialog_dpi(26, state->dpi)};
    draw_text_in_rect(dc, state->data.heading, heading_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, state->font);
    SetTextColor(dc, palette.muted);
    RECT subheading_rect{header_left, margin + scale_for_dialog_dpi(28, state->dpi),
                         content_right, margin + scale_for_dialog_dpi(56, state->dpi)};
    draw_text_in_rect(dc, state->data.subheading, subheading_rect,
                      DT_LEFT | DT_TOP | DT_END_ELLIPSIS);

    int y = margin + scale_for_dialog_dpi(68, state->dpi);
    const int row_height = scale_for_dialog_dpi(26, state->dpi);
    const int header_height = scale_for_dialog_dpi(28, state->dpi);
    const int label_width = std::min(scale_for_dialog_dpi(230, state->dpi),
                                     content_width / 2);
    const int value_left = margin + label_width;
    const int text_pad = scale_for_dialog_dpi(10, state->dpi);

    HPEN table_pen = CreatePen(PS_SOLID, 1, palette.border);
    HGDIOBJ old_pen = SelectObject(dc, table_pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));

    RECT header_rect{margin, y, content_right, y + header_height};
    fill_summary_rect(dc, header_rect, palette.header);
    Rectangle(dc, header_rect.left, header_rect.top, header_rect.right, header_rect.bottom);
    MoveToEx(dc, value_left, header_rect.top, nullptr);
    LineTo(dc, value_left, header_rect.bottom);
    HGDIOBJ old_section_font = SelectObject(
        dc, state->section_font != nullptr ? state->section_font : state->font);
    SetTextColor(dc, palette.text);
    RECT property_header{margin + text_pad, y, value_left - text_pad, y + header_height};
    RECT value_header{value_left + text_pad, y,
                      content_right - text_pad, y + header_height};
    draw_text_in_rect(dc, L"Property", property_header,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text_in_rect(dc, L"Value", value_header,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_section_font);
    y += header_height;

    auto draw_row = [&](const std::wstring& label, const std::wstring& value,
                        bool section, bool alternate, COLORREF value_color) {
        RECT row_rect{margin, y, content_right, y + row_height};
        fill_summary_rect(dc, row_rect,
                          section ? palette.header
                                  : alternate ? palette.panel_alt : palette.panel);
        Rectangle(dc, row_rect.left, row_rect.top, row_rect.right, row_rect.bottom);
        if (!section) {
            MoveToEx(dc, value_left, row_rect.top, nullptr);
            LineTo(dc, value_left, row_rect.bottom);
        }

        if (section) {
            HGDIOBJ old = SelectObject(
                dc, state->section_font != nullptr ? state->section_font : state->font);
            SetTextColor(dc, palette.text);
            RECT text_rect{row_rect.left + text_pad, row_rect.top,
                           row_rect.right - text_pad, row_rect.bottom};
            draw_text_in_rect(dc, label, text_rect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
        } else {
            RECT label_rect{row_rect.left + text_pad, row_rect.top,
                            value_left - text_pad, row_rect.bottom};
            RECT value_rect{value_left + text_pad, row_rect.top,
                            row_rect.right - text_pad, row_rect.bottom};
            SetTextColor(dc, palette.muted);
            draw_text_in_rect(dc, label, label_rect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SetTextColor(dc, value_color);
            draw_text_in_rect(dc, value, value_rect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        y += row_height;
    };

    bool alternate = false;
    for (const auto& row : state->data.details) {
        draw_row(row.first, row.second, false, alternate, palette.text);
        alternate = !alternate;
    }

    if (summary_has_comment(state)) {
        y += scale_for_dialog_dpi(10, state->dpi);
        draw_row(L"Archive comment", L"", true, false, palette.text);
        const int comment_height = summary_comment_box_height(state);
        RECT comment_rect{margin, y, content_right, y + comment_height};
        fill_summary_rect(dc, comment_rect, palette.panel_alt);
        Rectangle(dc, comment_rect.left, comment_rect.top,
                  comment_rect.right, comment_rect.bottom);
        RECT text_rect = comment_rect;
        InflateRect(&text_rect, -scale_for_dialog_dpi(10, state->dpi),
                    -scale_for_dialog_dpi(8, state->dpi));
        SetTextColor(dc, palette.text);
        draw_text_in_rect(dc, state->data.archive_comment, text_rect,
                          DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL);
        y += comment_height;
    }

    draw_row(L"Capabilities", L"", true, false, palette.text);
    alternate = false;
    for (const auto& capability : state->data.capabilities) {
        draw_row(capability.label,
                 capability.supported ? L"Supported" : L"Not supported",
                 false,
                 alternate,
                 capability.supported ? palette.supported : palette.muted);
        alternate = !alternate;
    }

    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(table_pen);

    SelectObject(dc, old_font);
    RestoreDC(dc, -1);
    draw_summary_scrollbar(state, dc, client);
    if (dc != window_dc) {
        BitBlt(window_dc, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    }
    EndPaint(state->hwnd, &paint_state);
}

LRESULT summary_control_color(ArchiveSummaryDialogState* state, WPARAM wparam) {
    if (state == nullptr) return 0;
    const ArchiveSummaryPalette palette = archive_summary_palette(state->dark);
    HDC dc = reinterpret_cast<HDC>(wparam);
    SetTextColor(dc, palette.text);
    SetBkColor(dc, palette.window);
    SetBkMode(dc, TRANSPARENT);
    return reinterpret_cast<LRESULT>(state->background_brush);
}

LRESULT CALLBACK archive_summary_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<ArchiveSummaryDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<ArchiveSummaryDialogState*>(create->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    switch (message) {
        case WM_CREATE:
            state->background_brush =
                CreateSolidBrush(archive_summary_palette(state->dark).window);
            rebuild_summary_fonts(state);
            state->ok = CreateWindowExW(
                0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDOK),
                state->instance, nullptr);
            for (HWND control : summary_controls(state)) {
                set_dialog_control_font(control, state->font);
            }
            apply_summary_theme(state);
            layout_summary(state);
            SendMessageW(hwnd, DM_SETDEFID, IDOK, 0);
            SetFocus(state->ok);
            return 0;
        case WM_SIZE:
            layout_summary(state);
            return 0;
        case WM_DPICHANGED: {
            const UINT previous_dpi = state->dpi;
            state->dpi = HIWORD(wparam);
            if (previous_dpi != 0) {
                state->scroll_y = MulDiv(state->scroll_y,
                                         static_cast<int>(state->dpi),
                                         static_cast<int>(previous_dpi));
            }
            // The owner-drawn table caches its physical content height; keeping
            // the old value makes its scrollbar range wrong after a monitor move.
            state->content_height = summary_content_height(state);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            apply_axiom_window_icons(hwnd, state->instance);
            rebuild_summary_fonts(state);
            layout_summary(state);
            return 0;
        }
        case WM_PAINT:
            paint_archive_summary(state);
            return 0;
        case WM_KEYDOWN: {
            RECT client{};
            GetClientRect(hwnd, &client);
            const int page = std::max(1, summary_content_bottom(state, client));
            switch (wparam) {
                case VK_UP:
                    scroll_summary_by(state, -scale_for_dialog_dpi(24, state->dpi));
                    return 0;
                case VK_DOWN:
                    scroll_summary_by(state, scale_for_dialog_dpi(24, state->dpi));
                    return 0;
                case VK_PRIOR:
                    scroll_summary_by(state, -page);
                    return 0;
                case VK_NEXT:
                    scroll_summary_by(state, page);
                    return 0;
                case VK_HOME:
                    state->scroll_y = 0;
                    update_summary_scrollbar(state);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                case VK_END:
                    state->scroll_y = summary_max_scroll(state, client);
                    update_summary_scrollbar(state);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_MOUSEWHEEL: {
            const int wheel = GET_WHEEL_DELTA_WPARAM(wparam);
            if (wheel != 0) {
                scroll_summary_by(state, -(wheel / WHEEL_DELTA) *
                                             scale_for_dialog_dpi(72, state->dpi));
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            RECT client{};
            GetClientRect(hwnd, &client);
            if (summary_needs_scrollbar(state, client)) {
                POINT point{static_cast<short>(LOWORD(lparam)),
                            static_cast<short>(HIWORD(lparam))};
                const RECT thumb = summary_scrollbar_thumb_rect(state, client);
                const RECT track = summary_scrollbar_track_rect(state, client);
                if (PtInRect(&thumb, point)) {
                    state->dragging_scrollbar = true;
                    state->drag_offset_y = point.y - thumb.top;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (PtInRect(&track, point)) {
                    scroll_summary_by(state, point.y < thumb.top
                                                 ? -summary_content_bottom(state, client)
                                                 : summary_content_bottom(state, client));
                    return 0;
                }
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (state != nullptr && state->dragging_scrollbar) {
                POINT point{static_cast<short>(LOWORD(lparam)),
                            static_cast<short>(HIWORD(lparam))};
                set_summary_scroll_from_thumb(state, point.y - state->drag_offset_y);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (state != nullptr && state->dragging_scrollbar) {
                state->dragging_scrollbar = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            if (state != nullptr && state->dragging_scrollbar) {
                state->dragging_scrollbar = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
                save_named_window_placement(L"ArchiveSummaryDialog", hwnd);
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return summary_control_color(state, wparam);
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
            save_named_window_placement(L"ArchiveSummaryDialog", hwnd);
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (state != nullptr) {
                delete_dialog_font(state->font);
                if (state->title_font != nullptr) DeleteObject(state->title_font);
                if (state->section_font != nullptr) DeleteObject(state->section_font);
                if (state->background_brush != nullptr) DeleteObject(state->background_brush);
                release_summary_buffer(state);
                state->hwnd = nullptr;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

std::vector<ArchiveSummaryCapabilityRow> summary_capabilities(
    const ArchiveCapabilities& capabilities) {
    return {
        {L"Browse directory", capabilities.list},
        {L"Extract", capabilities.extract},
        {L"Test integrity", capabilities.test},
        {L"Selective extraction", capabilities.selective_extract},
        {L"Create archive", capabilities.create},
        {L"Update entries", capabilities.update},
        {L"Delete entries", capabilities.delete_entries},
        {L"Move entries", capabilities.move_entries},
        {L"Encryption", capabilities.encryption || capabilities.encrypted},
        {L"Comments", capabilities.comments},
        {L"Recovery data", capabilities.recovery_records},
        {L"Create split volumes", capabilities.can_create_volumes},
        {L"Multi-volume archive", capabilities.is_multi_volume},
        {L"Signatures", capabilities.authenticity},
        {L"Self-extractor", capabilities.sfx},
        {L"Metadata", capabilities.metadata},
        {L"Links", capabilities.links},
    };
}

std::wstring widen_ascii(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

std::wstring provider_name_for_archive(const fs::path& archive_path) {
    const auto* provider = axiom::archive_provider_for_path(archive_path);
    if (provider == nullptr) return L"Unsupported archive";
    return widen_ascii(provider->info().display_name);
}

void show_archive_summary_dialog(HWND owner, ArchiveSummaryDialogData data) {
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    const bool dark = dialog_system_prefers_dark_mode();
    const UINT dpi = GetDpiForWindow(owner);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &archive_summary_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kArchiveSummaryDialogClass;
    assign_axiom_window_class_icons(window_class, instance);
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        show_message_dialog(owner, instance, dpi, dark, data.title,
                            last_error_text(), MessageDialogIcon::error);
        return;
    }

    RECT owner_rect{};
    GetWindowRect(owner, &owner_rect);
    ArchiveSummaryDialogState state{};
    state.owner = owner;
    state.instance = instance;
    state.dpi = dpi;
    state.dark = dark;
    state.data = std::move(data);
    state.content_height = summary_content_height(&state);

    const int desired_client_height = state.content_height +
                                      scale_for_dialog_dpi(20 + 30 + 14 + 48, dpi);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    HMONITOR target_monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    const int fallback_max_height = scale_for_dialog_dpi(760, dpi);
    int max_height = fallback_max_height;
    if (GetMonitorInfoW(target_monitor, &monitor)) {
        const int work_height = static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top);
        max_height = std::max(scale_for_dialog_dpi(380, dpi),
                              work_height - scale_for_dialog_dpi(48, dpi));
    }
    const int logical_client_height = std::clamp(
        MulDiv(desired_client_height, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi)),
        380, 520);
    const SIZE window_size = dialog_window_size_for_client(
        640, logical_client_height, kArchiveSummaryDialogStyle,
        kArchiveSummaryDialogExStyle, dpi);
    const int width = window_size.cx;
    const int height = std::min(static_cast<int>(window_size.cy), max_height);

    HWND dialog = CreateWindowExW(
        kArchiveSummaryDialogExStyle, kArchiveSummaryDialogClass,
        state.data.title.c_str(),
        kArchiveSummaryDialogStyle,
        owner_rect.left + (owner_rect.right - owner_rect.left - width) / 2,
        owner_rect.top + (owner_rect.bottom - owner_rect.top - height) / 2,
        width, height, owner, nullptr, instance, &state);
    if (dialog == nullptr) {
        show_message_dialog(owner, instance, dpi, dark, state.data.title,
                            last_error_text(), MessageDialogIcon::error);
        return;
    }
    apply_axiom_window_icons(dialog, instance);
    restore_named_window_placement(dialog, owner, L"ArchiveSummaryDialog");
    const bool owner_was_enabled = disable_dialog_owner(owner, dialog);
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
            save_named_window_placement(L"ArchiveSummaryDialog", dialog);
            DestroyWindow(dialog);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    restore_dialog_owner(owner, owner_was_enabled);
}

}  // namespace

bool show_archive_password_dialog(HWND owner, std::wstring& password) {
    return run_simple_password_dialog(owner, password);
}

bool show_archive_comment_dialog(HWND owner, std::wstring& comment) {
    return run_simple_comment_dialog(owner, comment);
}

void show_archive_information_dialog(
    HWND owner,
    const std::filesystem::path& archive_path,
    const ArchiveSummaryRows& details,
    const ArchiveCapabilities& capabilities,
    std::wstring archive_comment) {
    ArchiveSummaryDialogData data;
    data.title = L"Archive information";
    data.heading = archive_path.filename().wstring();
    data.subheading = provider_name_for_archive(archive_path) + L"  -  " +
                      archive_path.wstring();
    data.details = details;
    data.archive_comment = std::move(archive_comment);
    data.capabilities = summary_capabilities(capabilities);
    show_archive_summary_dialog(owner, std::move(data));
}

void show_archive_feature_summary_dialog(
    HWND owner,
    const std::filesystem::path& archive_path,
    const ArchiveCapabilities& capabilities) {
    ArchiveSummaryDialogData data;
    data.title = L"Archive capabilities";
    data.heading = archive_path.filename().wstring();
    data.subheading = provider_name_for_archive(archive_path) + L"  -  " +
                      archive_path.wstring();
    data.details = {
        {L"Format", provider_name_for_archive(archive_path)},
        {L"Provider mode", capabilities.create ? L"Full read/write" : L"Read-only"},
        {L"Encryption state", capabilities.encrypted ? L"Encrypted" : L"Not encrypted"},
        {L"Lock state", capabilities.locked ? L"Locked" : L"Not locked"},
    };
    data.capabilities = summary_capabilities(capabilities);
    show_archive_summary_dialog(owner, std::move(data));
}

}  // namespace axiom::gui
