#define NOMINMAX
#include "gui/archive_feature_dialogs.hpp"

#include "gui/dialog_support.hpp"
#include "gui/message_dialog.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace axiom::gui {
namespace fs = std::filesystem;
namespace {

constexpr wchar_t kFeatureDialogClass[] = L"AxiomArchiveFeatureOptionsDialog";
constexpr wchar_t kPasswordPromptClass[] = L"AxiomArchivePasswordPrompt";
constexpr wchar_t kCommentEditorClass[] = L"AxiomArchiveCommentEditor";

constexpr int kPageMetadata = 0;
constexpr int kPageUpdate = 1;
constexpr int kPageSecurity = 2;
constexpr int kPageRecovery = 3;
constexpr int kPageAuthenticity = 4;

constexpr int kNavBase = 4100;
constexpr int kAccept = IDOK;
constexpr int kCancel = IDCANCEL;

constexpr int kUpdateMode = 4220;
constexpr int kLockArchive = 4222;
constexpr int kComment = 4223;
constexpr int kRepackAfterUpdate = 4224;
constexpr int kEncryptData = 4230;
constexpr int kEncryptNames = 4231;
constexpr int kPassword = 4232;
constexpr int kConfirmPassword = 4233;
constexpr int kShowPassword = 4234;
constexpr int kVolumeSize = 4240;
constexpr int kVolumeUnit = 4241;
constexpr int kRecoveryPercent = 4242;
constexpr int kRecoveryVolumes = 4243;
constexpr int kAttemptRecovery = 4245;
constexpr int kSignArchive = 4250;
constexpr int kSigningKey = 4251;
constexpr int kCreateSfx = 4252;
constexpr int kSfxDestination = 4253;
constexpr int kBrowseSigningKey = 4254;
constexpr int kVerifySignature = 4255;
constexpr int kBrowseSfxDestination = 4256;
constexpr int kSimplePasswordEdit = 4301;
constexpr int kSimpleShowPassword = 4302;
constexpr int kSimpleCommentEdit = 4311;

constexpr std::array<const wchar_t*, 5> kUpdateModeNames{
    L"Create a new archive", L"Add or replace entries",
    L"Update entries that are newer", L"Freshen existing entries",
    L"Synchronize with source"};
constexpr std::array<const wchar_t*, 4> kVolumeUnitNames{
    L"KiB", L"MiB", L"GiB", L"TiB"};

struct PlacedControl {
    HWND window{};
    int page{};
    int x{};
    int y{};
    int width{};
    int height{};
    bool wrapped{};
};

template <typename T>
class ComPtr {
public:
    ~ComPtr() { reset(); }
    T** put() {
        reset();
        return &value_;
    }
    T* get() const { return value_; }
    T* operator->() const { return value_; }

private:
    void reset() {
        if (value_ != nullptr) value_->Release();
        value_ = nullptr;
    }
    T* value_ = nullptr;
};

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
            rebuild_simple_password_fonts(state);
            layout_simple_password_dialog(state);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = scale_for_dialog_dpi(420, state->dpi);
            info->ptMinTrackSize.y = scale_for_dialog_dpi(178, state->dpi);
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
            rebuild_simple_comment_fonts(state);
            layout_simple_comment_dialog(state);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = scale_for_dialog_dpi(520, state->dpi);
            info->ptMinTrackSize.y = scale_for_dialog_dpi(300, state->dpi);
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

    const int width = scale_for_dialog_dpi(430, dpi);
    const int height = scale_for_dialog_dpi(188, dpi);
    const POINT position = centered_window_position(owner, width, height);
    HWND dialog = CreateWindowExW(
        WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT, kPasswordPromptClass, L"Archive password",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS |
            WS_THICKFRAME,
        position.x, position.y, width, height, owner, nullptr, instance, &state);
    if (dialog == nullptr) {
        show_message_dialog(owner, instance, dpi, state.dark, L"Archive password",
                            last_error_text(), MessageDialogIcon::error);
        return false;
    }
    apply_axiom_window_icons(dialog, instance);
    restore_named_window_placement(dialog, owner, L"ArchivePasswordPrompt");
    const bool owner_was_enabled = disable_dialog_owner(owner);
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

    const int width = scale_for_dialog_dpi(580, dpi);
    const int height = scale_for_dialog_dpi(380, dpi);
    const POINT position = centered_window_position(owner, width, height);
    HWND dialog = CreateWindowExW(
        WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT, kCommentEditorClass, L"Archive comment",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS |
            WS_THICKFRAME,
        position.x, position.y, width, height, owner, nullptr, instance, &state);
    if (dialog == nullptr) {
        show_message_dialog(owner, instance, dpi, state.dark, L"Archive comment",
                            last_error_text(), MessageDialogIcon::error);
        return false;
    }
    apply_axiom_window_icons(dialog, instance);
    restore_named_window_placement(dialog, owner, L"ArchiveCommentEditor");
    const bool owner_was_enabled = disable_dialog_owner(owner);
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

std::optional<fs::path> shell_item_path(IShellItem* item) {
    if (item == nullptr) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

void set_initial_folder(IFileDialog* dialog, const fs::path& path) {
    if (dialog == nullptr || path.empty()) return;
    std::error_code error;
    fs::path folder = fs::is_directory(path, error) ? path : path.parent_path();
    if (folder.empty() || !fs::is_directory(folder, error)) return;
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr,
                                              IID_PPV_ARGS(item.put())))) {
        dialog->SetFolder(item.get());
    }
}

std::optional<fs::path> browse_signing_key(HWND owner, const fs::path& initial = {}) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) return std::nullopt;
    const COMDLG_FILTERSPEC filters[] = {
        {L"Axiom signing keys", L"*.key"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(static_cast<UINT>(sizeof(filters) / sizeof(filters[0])), filters);
    dialog->SetTitle(L"Choose signing key");
    FILEOPENDIALOGOPTIONS flags{};
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    set_initial_folder(dialog.get(), initial);
    if (dialog->Show(owner) != S_OK) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    return shell_item_path(item.get());
}

std::optional<fs::path> browse_save_sfx(HWND owner, const fs::path& initial = {}) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) return std::nullopt;
    const COMDLG_FILTERSPEC filters[] = {
        {L"Axiom self-extractors", L"*.exe"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(static_cast<UINT>(sizeof(filters) / sizeof(filters[0])), filters);
    dialog->SetDefaultExtension(L"exe");
    dialog->SetTitle(L"Choose SFX output path");
    set_initial_folder(dialog.get(), initial);
    if (!initial.filename().empty()) {
        dialog->SetFileName(initial.filename().c_str());
    }
    if (dialog->Show(owner) != S_OK) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    return shell_item_path(item.get());
}

class ArchiveFeatureDialog {
public:
    ArchiveFeatureDialog(ArchiveFeatureDialogContext context,
                         ArchiveFeatureOptions archive_options,
                         ExtractFeatureOptions extract_options,
                         ArchiveFeatureAvailability availability,
                         std::wstring suggested_sfx_output = {})
        : context_(context),
          archive_options_(std::move(archive_options)),
          extract_options_(std::move(extract_options)),
          availability_(availability),
          suggested_sfx_output_(std::move(suggested_sfx_output)) {}

    ~ArchiveFeatureDialog() {
        delete_dialog_font(font_);
        if (background_brush_ != nullptr) DeleteObject(background_brush_);
        if (edit_brush_ != nullptr) DeleteObject(edit_brush_);
    }

    bool show(HWND owner) {
        owner_ = owner;
        instance_ = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
        dpi_ = GetDpiForWindow(owner);
        dark_ = dialog_system_prefers_dark_mode();
        if (!register_class()) return false;

        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        const DWORD ex_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
        RECT rect{0, 0, scale(760), scale(550)};
        AdjustWindowRectExForDpi(&rect, style, FALSE, ex_style, dpi_);
        RECT owner_rect{};
        GetWindowRect(owner, &owner_rect);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        const int x = owner_rect.left + (owner_rect.right - owner_rect.left - width) / 2;
        const int y = owner_rect.top + (owner_rect.bottom - owner_rect.top - height) / 2;
        const wchar_t* title = context_ == ArchiveFeatureDialogContext::create_or_update
            ? L"Advanced archive options"
            : context_ == ArchiveFeatureDialogContext::comment
                ? L"Archive comment"
                : context_ == ArchiveFeatureDialogContext::password_prompt
                    ? L"Archive password"
                    : L"Advanced extraction options";
        window_ = CreateWindowExW(ex_style, kFeatureDialogClass, title, style,
                                  x, y, width, height, owner, nullptr, instance_, this);
        if (window_ == nullptr) return false;
        apply_axiom_window_icons(window_, instance_);
        restore_named_window_placement(window_, owner, layout_name());
        const bool owner_was_enabled = disable_dialog_owner(owner);
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);

        MSG message{};
        while (IsWindow(window_)) {
            const BOOL status = GetMessageW(&message, nullptr, 0, 0);
            if (status <= 0) {
                if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        restore_dialog_owner(owner, owner_was_enabled);
        return accepted_;
    }

    const ArchiveFeatureOptions& archive_options() const { return archive_options_; }
    const ExtractFeatureOptions& extract_options() const { return extract_options_; }

private:
    bool register_class() const {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &ArchiveFeatureDialog::window_proc;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = kFeatureDialogClass;
        assign_axiom_window_class_icons(window_class, instance_);
        return RegisterClassExW(&window_class) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    int scale(int value) const { return scale_for_dialog_dpi(value, dpi_); }

    const wchar_t* layout_name() const {
        switch (context_) {
            case ArchiveFeatureDialogContext::create_or_update:
                return L"ArchiveFeatureOptionsDialog";
            case ArchiveFeatureDialogContext::extract:
                return L"ExtractFeatureOptionsDialog";
            case ArchiveFeatureDialogContext::password_prompt:
                return L"ArchivePasswordDialog";
            case ArchiveFeatureDialogContext::comment:
                return L"ArchiveCommentDialog";
        }
        return L"ArchiveFeatureDialog";
    }

    void close_dialog() {
        save_named_window_placement(layout_name(), window_);
        if (window_ != nullptr && IsWindow(window_)) {
            DestroyWindow(window_);
        }
    }

    HWND make_control(const wchar_t* type, const wchar_t* text, DWORD style, int id,
                      bool enabled = true) {
        HWND control = CreateWindowExW(
            0, type, text, WS_CHILD | style, 0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        set_dialog_control_font(control, font_);
        apply_dialog_control_theme(control, dark_);
        EnableWindow(control, enabled ? TRUE : FALSE);
        return control;
    }

    HWND place(int page, const wchar_t* type, const wchar_t* text, DWORD style, int id,
               int x, int y, int width, int height, bool enabled = true,
               bool wrapped = false) {
        HWND control = make_control(type, text, style, id, enabled);
        placed_.push_back({control, page, x, y, width, height, wrapped});
        return control;
    }

    HWND page_label(int page, const wchar_t* text, int x, int y, int width,
                    bool enabled = true, bool wrapped = false) {
        return place(page, L"STATIC", text,
                     SS_LEFT | SS_NOPREFIX | (wrapped ? SS_EDITCONTROL : 0), 0,
                     x, y, width, wrapped ? 48 : 24, enabled, wrapped);
    }

    HWND page_edit(int page, const wchar_t* text, int id, int x, int y, int width,
                   int height, bool enabled, DWORD extra_style = ES_AUTOHSCROLL) {
        HWND edit = place(page, L"EDIT", text, WS_TABSTOP | WS_BORDER | extra_style, id,
                          x, y, width, height, enabled);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(scale(6), scale(6)));
        return edit;
    }

    HWND page_combo(int page, int id, int x, int y, int width, int drop_height,
                    const wchar_t* const* items, std::size_t item_count,
                    int selection, bool enabled) {
        HWND combo = place(page, L"COMBOBOX", L"",
                           WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                               CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                           id, x, y, width, drop_height, enabled);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, scale(24));
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scale(24));
        for (std::size_t i = 0; i < item_count; ++i) {
            SendMessageW(combo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(items[i]));
        }
        SendMessageW(combo, CB_SETCURSEL,
                     static_cast<WPARAM>(std::clamp(selection, 0,
                         static_cast<int>(item_count) - 1)), 0);
        return combo;
    }

    void page_checkbox(int page, int id, const wchar_t* text, bool& value,
                       int y, bool enabled) {
        HWND checkbox = place(page, L"BUTTON", text,
                              WS_TABSTOP | BS_AUTOCHECKBOX | BS_OWNERDRAW,
                              id, 8, y, 470, 28, enabled);
        checkbox_values_[id] = &value;
        checkbox_windows_[id] = checkbox;
    }

    void page_heading(int page, const wchar_t* title, const wchar_t* description) {
        page_label(page, title, 8, 0, 500);
        page_label(page, description, 8, 28, 510, true, true);
    }

    void create_metadata_page() {
        if (context_ == ArchiveFeatureDialogContext::create_or_update) {
            page_heading(kPageMetadata, L"Metadata and links",
                         L"Supported filesystem metadata is captured automatically.");
            page_label(kPageMetadata,
                       L"Axiom stores Windows attributes and full-precision timestamps, NTFS "
                       L"alternate data streams, symbolic and hard links, and POSIX mode and "
                       L"ownership where the host exposes them.",
                       8, 76, 500, true, true);
            page_label(kPageMetadata,
                       L"Unsupported special files are skipped; metadata that the destination "
                       L"filesystem cannot represent is restored on a best-effort basis.",
                       8, 150, 500, true, true);
        } else {
            page_heading(kPageMetadata, L"Restore metadata and links",
                         L"Stored filesystem metadata is restored automatically.");
            page_label(kPageMetadata,
                       L"Attributes, creation/access times, alternate data streams, links, and "
                       L"POSIX metadata are restored when supported by the destination.",
                       8, 76, 500, true, true);
            page_label(kPageMetadata,
                       L"Modification-time restoration remains available in the main extraction "
                       L"dialog because it is the one metadata policy exposed by the engine.",
                       8, 150, 500, true, true);
        }
    }

    void create_update_page() {
        if (context_ == ArchiveFeatureDialogContext::comment) {
            page_heading(kPageUpdate, L"Archive comment",
                         L"Add, replace, or remove the comment stored in this archive.");
            page_label(kPageUpdate, L"Comment", 8, 75, 160, true);
            comment_ = page_edit(kPageUpdate, archive_options_.comment.c_str(), kComment,
                                 8, 104, 500, 240, true,
                                 ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN);
            return;
        }

        page_heading(kPageUpdate, L"Archive update behavior",
                     L"Configure in-place editing, synchronization, and archive service data.");
        page_label(kPageUpdate, L"Update mode", 8, 75, 120, availability_.update);
        update_mode_ = page_combo(
            kPageUpdate, kUpdateMode, 145, 70, 290, 190,
            kUpdateModeNames.data(), kUpdateModeNames.size(),
            static_cast<int>(archive_options_.update_mode), availability_.update);
        page_label(kPageUpdate,
                   L"Axiom always writes a self-locating central directory for immediate listing.",
                   8, 116, 500, true, true);
        page_checkbox(kPageUpdate, kLockArchive, L"Lock archive against further changes",
                      archive_options_.lock_archive, 150, availability_.lock);
        page_checkbox(kPageUpdate, kRepackAfterUpdate,
                      L"Repack affected solid runs after updating",
                      archive_options_.repack_after_update, 184, availability_.update);
        page_label(kPageUpdate, L"Archive comment", 8, 231, 160, availability_.comments);
        comment_ = page_edit(kPageUpdate, archive_options_.comment.c_str(), kComment,
                             8, 260, 500, 100, availability_.comments,
                             ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN);
    }

    void create_security_page() {
        page_heading(kPageSecurity, L"Encryption",
                     L"Passwords are never saved in GUI settings and will be cleared after use.");
        if (context_ == ArchiveFeatureDialogContext::create_or_update) {
            page_checkbox(kPageSecurity, kEncryptData, L"Encrypt file data",
                          archive_options_.encrypt_data, 72, availability_.encryption);
            page_checkbox(kPageSecurity, kEncryptNames, L"Encrypt file names and headers",
                          archive_options_.encrypt_names, 106,
                          availability_.header_encryption);
            page_label(kPageSecurity, L"Password", 8, 157, 130, availability_.encryption);
            password_ = page_edit(kPageSecurity, archive_options_.password.c_str(), kPassword,
                                  145, 150, 300, 30, availability_.encryption,
                                  ES_PASSWORD | ES_AUTOHSCROLL);
            page_label(kPageSecurity, L"Confirm password", 8, 199, 130,
                       availability_.encryption);
            confirm_password_ = page_edit(
                kPageSecurity, archive_options_.password.c_str(), kConfirmPassword,
                145, 192, 300, 30, availability_.encryption,
                ES_PASSWORD | ES_AUTOHSCROLL);
            page_checkbox(kPageSecurity, kShowPassword, L"Show password",
                          show_password_, 231, availability_.encryption);
            page_label(kPageSecurity,
                       L"Argon2id uses the current format defaults; parameters are stored in the "
                       L"archive for deterministic decoding.",
                       8, 283, 500, true, true);
        } else {
            page_label(kPageSecurity, L"Password", 8, 79, 130, availability_.encryption);
            password_ = page_edit(kPageSecurity, extract_options_.password.c_str(), kPassword,
                                  145, 72, 300, 30, availability_.encryption,
                                  ES_PASSWORD | ES_AUTOHSCROLL);
            page_checkbox(kPageSecurity, kShowPassword, L"Show password",
                          show_password_, 114, availability_.encryption);
        }
    }

    void create_recovery_page() {
        page_heading(kPageRecovery, L"Recovery and volumes",
                     L"Configure split archives, recovery records, and missing-volume behavior.");
        if (context_ == ArchiveFeatureDialogContext::create_or_update) {
            page_label(kPageRecovery, L"Split volume size", 8, 77, 135, availability_.volumes);
            volume_size_ = page_edit(kPageRecovery, archive_options_.volume_size.c_str(),
                                     kVolumeSize, 155, 70, 160, 30, availability_.volumes);
            volume_unit_ = page_combo(
                kPageRecovery, kVolumeUnit, 325, 70, 110, 150,
                kVolumeUnitNames.data(), kVolumeUnitNames.size(),
                archive_options_.volume_unit, availability_.volumes);
            page_label(kPageRecovery, L"Recovery record", 8, 125, 135, availability_.recovery);
            recovery_percent_ = page_edit(
                kPageRecovery, std::to_wstring(archive_options_.recovery_percent).c_str(),
                kRecoveryPercent, 155, 118, 90, 30, availability_.recovery, ES_NUMBER);
            page_label(kPageRecovery, L"% of archive data", 255, 125, 150,
                       availability_.recovery);
            page_checkbox(kPageRecovery, kRecoveryVolumes,
                          L"Create recovery volumes for missing-volume reconstruction",
                          archive_options_.create_recovery_volumes, 168,
                          availability_.recovery && availability_.volumes);
        } else {
            page_checkbox(kPageRecovery, kAttemptRecovery,
                          L"Attempt recovery before reporting damaged data",
                          extract_options_.attempt_recovery, 72, availability_.recovery);
            page_label(kPageRecovery,
                       L"Split sets are joined before browsing or extraction. Opening any surviving "
                       L"data or recovery volume discovers adjacent members and reconstructs the "
                       L"complete archive when enough shards remain.",
                       8, 126, 500, availability_.volumes, true);
        }
    }

    void create_authenticity_page() {
        if (context_ == ArchiveFeatureDialogContext::create_or_update) {
            page_heading(kPageAuthenticity, L"Authenticity and self-extraction",
                         L"Sign completed archives or package them with the native SFX stub.");
            page_checkbox(kPageAuthenticity, kSignArchive, L"Sign archive with Monocypher EdDSA",
                          archive_options_.sign_archive, 72, availability_.authenticity);
            page_label(kPageAuthenticity, L"Signing key", 8, 121, 125,
                       availability_.authenticity);
            signing_key_ = page_edit(kPageAuthenticity,
                                     archive_options_.signing_key.wstring().c_str(),
                                      kSigningKey, 145, 114, 260, 30,
                                      availability_.authenticity);
            browse_signing_key_ = place(kPageAuthenticity, L"BUTTON", L"Browse...",
                                         WS_TABSTOP | BS_OWNERDRAW, kBrowseSigningKey,
                                         415, 114, 88, 30, availability_.authenticity);
            page_checkbox(kPageAuthenticity, kCreateSfx,
                           L"Create a self-extracting Windows executable",
                           archive_options_.create_sfx, 166, availability_.sfx);
            page_label(kPageAuthenticity, L"SFX output path", 8, 215, 125,
                       availability_.sfx);
            const std::wstring displayed_sfx_output =
                archive_options_.sfx_destination.empty()
                    ? suggested_sfx_output_
                    : archive_options_.sfx_destination;
            sfx_destination_uses_default_ = archive_options_.sfx_destination.empty();
            sfx_destination_ = page_edit(kPageAuthenticity,
                                          displayed_sfx_output.c_str(),
                                          kSfxDestination, 145, 208, 260, 30,
                                          availability_.sfx);
            browse_sfx_destination_ = place(kPageAuthenticity, L"BUTTON", L"Browse...",
                                            WS_TABSTOP | BS_OWNERDRAW,
                                            kBrowseSfxDestination,
                                            415, 208, 88, 30, availability_.sfx);
            page_label(kPageAuthenticity,
                       L"The .exe embeds the completed .axar and is created beside it by default.",
                       8, 244, 500, availability_.sfx, true);
            page_label(kPageAuthenticity,
                       L"Runtime destination, overwrite policy, thread count, timestamp restore, "
                       L"and destination-folder opening are selected by the self-extractor.",
                       8, 280, 500, availability_.sfx, true);
        } else {
            page_heading(kPageAuthenticity, L"Signature verification",
                         L"Control how authenticity signatures are handled during extraction.");
            page_checkbox(kPageAuthenticity, kVerifySignature,
                          L"Verify signed archives before extraction",
                          extract_options_.verify_signature, 72, availability_.authenticity);
            page_label(kPageAuthenticity,
                       L"Invalid signatures block extraction. Unsigned archives remain allowed; "
                       L"trusted-key pinning is available through the CLI verify command.",
                       8, 126, 500, availability_.authenticity, true);
        }
    }

    void create_controls() {
        const DialogColors colors = dialog_colors(dark_);
        background_brush_ = CreateSolidBrush(colors.background);
        edit_brush_ = CreateSolidBrush(colors.control_background);
        font_ = create_dialog_font(dpi_);
        apply_dialog_dark_frame(window_, dark_);

        heading_ = make_control(L"STATIC",
            context_ == ArchiveFeatureDialogContext::create_or_update
                ? L"Archive feature options"
                : context_ == ArchiveFeatureDialogContext::comment
                    ? L"Edit archive comment"
                    : context_ == ArchiveFeatureDialogContext::password_prompt
                        ? L"Password required"
                        : L"Extraction feature options",
            SS_LEFT | SS_NOPREFIX, 0);
        banner_ = make_control(
            L"STATIC",
            context_ == ArchiveFeatureDialogContext::comment
                ? L"Archive comments are optional; an empty comment removes the current one."
                : context_ == ArchiveFeatureDialogContext::password_prompt
                    ? L"The password is retained only in memory while this archive is open."
                    : context_ == ArchiveFeatureDialogContext::create_or_update
                        ? L"Configure archive metadata, security, recovery, volumes, and packaging."
                        : L"Configure restoration, security verification, and recovery behavior.",
            SS_LEFT | SS_NOPREFIX, 0);
        ShowWindow(heading_, SW_SHOW);
        ShowWindow(banner_, SW_SHOW);

        if (context_ == ArchiveFeatureDialogContext::comment) {
            logical_pages_ = {kPageUpdate};
        } else if (context_ == ArchiveFeatureDialogContext::password_prompt) {
            logical_pages_ = {kPageSecurity};
        } else if (context_ == ArchiveFeatureDialogContext::create_or_update) {
            logical_pages_ = {kPageMetadata, kPageUpdate, kPageSecurity,
                              kPageRecovery, kPageAuthenticity};
        } else {
            logical_pages_ = {kPageMetadata, kPageSecurity,
                              kPageRecovery, kPageAuthenticity};
        }
        const std::array<const wchar_t*, 5> create_names{
            L"Metadata", L"Update", L"Security", L"Recovery & volumes", L"Authenticity & SFX"};
        const std::array<const wchar_t*, 5> extract_names{
            L"Restore", L"", L"Security", L"Recovery & volumes", L"Authenticity"};
        for (int page : logical_pages_) {
            const wchar_t* text = context_ == ArchiveFeatureDialogContext::comment
                ? L"Comment"
                : context_ == ArchiveFeatureDialogContext::password_prompt
                    ? L"Security"
                    : context_ == ArchiveFeatureDialogContext::create_or_update
                        ? create_names[static_cast<std::size_t>(page)]
                        : extract_names[static_cast<std::size_t>(page)];
            nav_buttons_[page] = make_control(L"BUTTON", text,
                                              WS_TABSTOP | BS_OWNERDRAW,
                                              kNavBase + page);
            ShowWindow(nav_buttons_[page], SW_SHOW);
        }

        create_metadata_page();
        if (context_ == ArchiveFeatureDialogContext::create_or_update ||
            context_ == ArchiveFeatureDialogContext::comment) {
            create_update_page();
        }
        create_security_page();
        create_recovery_page();
        create_authenticity_page();

        accept_ = make_control(L"BUTTON", L"OK", WS_TABSTOP | BS_OWNERDRAW, kAccept);
        cancel_ = make_control(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, kCancel);
        ShowWindow(accept_, SW_SHOW);
        ShowWindow(cancel_, SW_SHOW);
        SendMessageW(window_, DM_SETDEFID, IDOK, 0);
        select_page(logical_pages_.front());
        layout();
    }

    void select_page(int page) {
        current_page_ = page;
        for (const PlacedControl& control : placed_) {
            ShowWindow(control.window, control.page == current_page_ ? SW_SHOW : SW_HIDE);
        }
        for (const auto& [logical_page, button] : nav_buttons_) {
            InvalidateRect(button, nullptr, TRUE);
        }
    }

    void layout() {
        if (window_ == nullptr) return;
        RECT client{};
        GetClientRect(window_, &client);
        const int margin = scale(18);
        const int nav_width = scale(166);
        const int top = scale(76);
        const int content_left = margin + nav_width + scale(20);
        MoveWindow(heading_, margin, scale(16), client.right - margin * 2, scale(26), TRUE);
        MoveWindow(banner_, margin, scale(43), client.right - margin * 2, scale(24), TRUE);

        int nav_y = top;
        for (int page : logical_pages_) {
            MoveWindow(nav_buttons_[page], margin, nav_y, nav_width, scale(36), TRUE);
            nav_y += scale(42);
        }
        for (const PlacedControl& control : placed_) {
            const int available_width = std::max(
                scale(120), static_cast<int>(client.right) - margin -
                                content_left - scale(control.x));
            const int width = std::min(scale(control.width), available_width);
            int height = scale(control.height);
            if (control.wrapped) {
                HDC dc = GetDC(control.window);
                HGDIOBJ old_font = SelectObject(dc, font_);
                RECT measured{0, 0, width, 0};
                const std::wstring text = control_text(control.window);
                DrawTextW(dc, text.c_str(), -1, &measured,
                          DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
                SelectObject(dc, old_font);
                ReleaseDC(control.window, dc);
                height = std::max(height, static_cast<int>(measured.bottom) + scale(3));
            }
            MoveWindow(control.window,
                       content_left + scale(control.x), top + scale(control.y),
                       width, height, TRUE);
        }
        const int button_width = scale(88);
        const int button_height = scale(30);
        const int button_y = client.bottom - margin - button_height;
        MoveWindow(cancel_, client.right - margin - button_width, button_y,
                   button_width, button_height, TRUE);
        MoveWindow(accept_, client.right - margin - button_width * 2 - scale(10),
                   button_y, button_width, button_height, TRUE);
        InvalidateRect(window_, nullptr, TRUE);
    }

    void rebuild_font() {
        delete_dialog_font(font_);
        font_ = create_dialog_font(dpi_);
        set_dialog_control_font(heading_, font_);
        set_dialog_control_font(banner_, font_);
        set_dialog_control_font(accept_, font_);
        set_dialog_control_font(cancel_, font_);
        for (const auto& [page, button] : nav_buttons_) set_dialog_control_font(button, font_);
        for (const PlacedControl& control : placed_) set_dialog_control_font(control.window, font_);
    }

    void draw_nav_button(const DRAWITEMSTRUCT& draw, int page) const {
        if (page != current_page_) {
            draw_dialog_button(draw, dark_);
            return;
        }
        const DialogColors colors = dialog_colors(dark_);
        HBRUSH background = CreateSolidBrush(colors.selection_background);
        FillRect(draw.hDC, &draw.rcItem, background);
        DeleteObject(background);
        HBRUSH border = CreateSolidBrush(colors.focus_border);
        FrameRect(draw.hDC, &draw.rcItem, border);
        DeleteObject(border);
        wchar_t text[128]{};
        GetWindowTextW(draw.hwndItem, text,
                       static_cast<int>(sizeof(text) / sizeof(text[0])));
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0));
        HGDIOBJ old_font = SelectObject(draw.hDC, font);
        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, colors.selection_text);
        RECT text_rect = draw.rcItem;
        text_rect.left += scale(12);
        DrawTextW(draw.hDC, text, -1, &text_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(draw.hDC, old_font);
    }

    void draw_item(const DRAWITEMSTRUCT& draw) const {
        if (draw.CtlType == ODT_COMBOBOX) {
            draw_dialog_combo_item(draw, dark_);
            return;
        }
        const int id = GetDlgCtrlID(draw.hwndItem);
        if (id >= kNavBase && id < kNavBase + 5) {
            draw_nav_button(draw, id - kNavBase);
            return;
        }
        if (auto found = checkbox_values_.find(id); found != checkbox_values_.end()) {
            draw_dialog_checkbox(draw, dark_, *found->second);
            return;
        }
        draw_dialog_button(draw, dark_);
    }

    void toggle_checkbox(int id) {
        auto found = checkbox_values_.find(id);
        if (found == checkbox_values_.end()) return;
        HWND checkbox = checkbox_windows_[id];
        if (!IsWindowEnabled(checkbox)) return;
        *found->second = !*found->second;
        if (id == kEncryptNames && archive_options_.encrypt_names) {
            archive_options_.encrypt_data = true;
            if (const auto data = checkbox_windows_.find(kEncryptData);
                data != checkbox_windows_.end()) {
                InvalidateRect(data->second, nullptr, TRUE);
            }
        } else if (id == kEncryptData && !archive_options_.encrypt_data) {
            archive_options_.encrypt_names = false;
            if (const auto names = checkbox_windows_.find(kEncryptNames);
                names != checkbox_windows_.end()) {
                InvalidateRect(names->second, nullptr, TRUE);
            }
        }
        if (id == kShowPassword) {
            const WPARAM password_character = show_password_ ? 0 : static_cast<WPARAM>(L'\x25cf');
            SendMessageW(password_, EM_SETPASSWORDCHAR, password_character, 0);
            if (confirm_password_ != nullptr) {
                SendMessageW(confirm_password_, EM_SETPASSWORDCHAR, password_character, 0);
            }
            InvalidateRect(password_, nullptr, TRUE);
            if (confirm_password_ != nullptr) InvalidateRect(confirm_password_, nullptr, TRUE);
        }
        InvalidateRect(checkbox, nullptr, TRUE);
    }

    void browse_signing_key_path() {
        if (signing_key_ == nullptr || !IsWindowEnabled(signing_key_)) return;
        if (const auto selected = browse_signing_key(window_, control_text(signing_key_))) {
            SetWindowTextW(signing_key_, selected->c_str());
        }
    }

    void browse_sfx_output_path() {
        if (sfx_destination_ == nullptr || !IsWindowEnabled(sfx_destination_)) return;
        if (const auto selected = browse_save_sfx(window_, control_text(sfx_destination_))) {
            SetWindowTextW(sfx_destination_, selected->c_str());
            sfx_destination_uses_default_ = false;
        }
    }

    void accept() {
        if (update_mode_ != nullptr && IsWindowEnabled(update_mode_)) {
            const LRESULT selection = SendMessageW(update_mode_, CB_GETCURSEL, 0, 0);
            if (selection != CB_ERR) {
                archive_options_.update_mode = static_cast<ArchiveUpdateMode>(selection);
            }
        }
        if (volume_unit_ != nullptr && IsWindowEnabled(volume_unit_)) {
            const LRESULT selection = SendMessageW(volume_unit_, CB_GETCURSEL, 0, 0);
            if (selection != CB_ERR) archive_options_.volume_unit = static_cast<int>(selection);
        }

        if (password_ != nullptr && IsWindowEnabled(password_)) {
            const std::wstring password = control_text(password_);
            if (context_ == ArchiveFeatureDialogContext::create_or_update) {
                if (archive_options_.encrypt_data) {
                    const bool has_comment = comment_ != nullptr &&
                                             !control_text(comment_).empty();
                    if (archive_options_.encrypt_names &&
                        (has_comment || archive_options_.lock_archive)) {
                        show_message_dialog(
                            window_, instance_, dpi_, dark_, L"Axiom",
                            L"Comments and locking cannot currently be changed after file-name "
                            L"encryption is applied. Clear those options or encrypt file data only.",
                            MessageDialogIcon::warning);
                        return;
                    }
                    if (password.empty()) {
                        show_message_dialog(window_, instance_, dpi_, dark_, L"Axiom",
                                            L"Enter a password to encrypt the archive.",
                                            MessageDialogIcon::warning);
                        return;
                    }
                    if (confirm_password_ == nullptr ||
                        password != control_text(confirm_password_)) {
                        show_message_dialog(window_, instance_, dpi_, dark_, L"Axiom",
                                            L"The passwords do not match.",
                                            MessageDialogIcon::warning);
                        return;
                    }
                    archive_options_.password = password;
                } else {
                    archive_options_.password.clear();
                }
            } else {
                if (password.empty()) {
                    show_message_dialog(window_, instance_, dpi_, dark_, L"Axiom",
                                        L"Enter the archive password.",
                                        MessageDialogIcon::warning);
                    return;
                }
                extract_options_.password = password;
            }
        }
        if (comment_ != nullptr && IsWindowEnabled(comment_)) {
            archive_options_.comment = control_text(comment_);
        }
        if (volume_size_ != nullptr && IsWindowEnabled(volume_size_)) {
            archive_options_.volume_size = control_text(volume_size_);
        }
        if (recovery_percent_ != nullptr && IsWindowEnabled(recovery_percent_)) {
            try {
                archive_options_.recovery_percent = std::clamp(
                    std::stoi(control_text(recovery_percent_)), 0, 100);
            } catch (...) {
                archive_options_.recovery_percent = 0;
            }
        }
        if (signing_key_ != nullptr && IsWindowEnabled(signing_key_)) {
            archive_options_.signing_key = control_text(signing_key_);
        }
        if (sfx_destination_ != nullptr && IsWindowEnabled(sfx_destination_)) {
            std::wstring output = control_text(sfx_destination_);
            if (!output.empty()) {
                std::filesystem::path output_path(output);
                if (lstrcmpiW(output_path.extension().c_str(), L".exe") != 0) {
                    output_path.replace_extension(L".exe");
                }
                output = output_path.wstring();
            }
            archive_options_.sfx_destination =
                sfx_destination_uses_default_ && output == suggested_sfx_output_
                    ? std::wstring{}
                    : std::move(output);
        }
        if (password_ != nullptr) {
            SetWindowTextW(password_, L"");
            if (confirm_password_ != nullptr) SetWindowTextW(confirm_password_, L"");
        }
        accepted_ = true;
        close_dialog();
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE:
                create_controls();
                return 0;
            case WM_SIZE:
                layout();
                return 0;
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wparam);
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                rebuild_font();
                layout();
                return 0;
            }
            case WM_ERASEBKGND: {
                RECT client{};
                GetClientRect(window_, &client);
                FillRect(reinterpret_cast<HDC>(wparam), &client, background_brush_);
                return 1;
            }
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(window_, &paint);
                const DialogColors colors = dialog_colors(dark_);
                const int x = scale(194);
                HPEN pen = CreatePen(PS_SOLID, 1, colors.border);
                HGDIOBJ old_pen = SelectObject(dc, pen);
                MoveToEx(dc, x, scale(76), nullptr);
                LineTo(dc, x, scale(480));
                SelectObject(dc, old_pen);
                DeleteObject(pen);
                EndPaint(window_, &paint);
                return 0;
            }
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN: {
                const DialogColors colors = dialog_colors(dark_);
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lparam))
                                     ? colors.text
                                     : colors.disabled_text);
                return reinterpret_cast<LRESULT>(background_brush_);
            }
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX: {
                const DialogColors colors = dialog_colors(dark_);
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetBkColor(dc, colors.control_background);
                SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lparam))
                                     ? colors.text
                                     : colors.disabled_text);
                return reinterpret_cast<LRESULT>(edit_brush_);
            }
            case WM_DRAWITEM:
                if (lparam != 0) {
                    draw_item(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
                    return TRUE;
                }
                break;
            case WM_COMMAND: {
                const int id = LOWORD(wparam);
                if (id >= kNavBase && id < kNavBase + 5) {
                    select_page(id - kNavBase);
                    return 0;
                }
                if (checkbox_values_.contains(id)) {
                    toggle_checkbox(id);
                    return 0;
                }
                if (id == kBrowseSigningKey) {
                    browse_signing_key_path();
                    return 0;
                }
                if (id == kBrowseSfxDestination) {
                    browse_sfx_output_path();
                    return 0;
                }
                if (id == kAccept) {
                    accept();
                    return 0;
                }
                if (id == kCancel) {
                    close_dialog();
                    return 0;
                }
                break;
            }
            case WM_CLOSE:
                if (password_ != nullptr) {
                    SetWindowTextW(password_, L"");
                    if (confirm_password_ != nullptr) SetWindowTextW(confirm_password_, L"");
                }
                close_dialog();
                return 0;
            case WM_NCDESTROY:
                window_ = nullptr;
                return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        ArchiveFeatureDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            dialog = static_cast<ArchiveFeatureDialog*>(create->lpCreateParams);
            dialog->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        } else {
            dialog = reinterpret_cast<ArchiveFeatureDialog*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return dialog != nullptr
            ? dialog->handle_message(message, wparam, lparam)
            : DefWindowProcW(window, message, wparam, lparam);
    }

    ArchiveFeatureDialogContext context_;
    ArchiveFeatureOptions archive_options_;
    ExtractFeatureOptions extract_options_;
    ArchiveFeatureAvailability availability_;
    std::wstring suggested_sfx_output_;
    HWND owner_{};
    HWND window_{};
    HINSTANCE instance_{};
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};
    bool dark_{};
    bool accepted_{};
    bool show_password_{};
    bool sfx_destination_uses_default_{};
    HFONT font_{};
    HBRUSH background_brush_{};
    HBRUSH edit_brush_{};
    HWND heading_{};
    HWND banner_{};
    HWND accept_{};
    HWND cancel_{};
    HWND update_mode_{};
    HWND comment_{};
    HWND password_{};
    HWND confirm_password_{};
    HWND volume_size_{};
    HWND volume_unit_{};
    HWND recovery_percent_{};
    HWND signing_key_{};
    HWND browse_signing_key_{};
    HWND sfx_destination_{};
    HWND browse_sfx_destination_{};
    int current_page_{kPageMetadata};
    std::vector<int> logical_pages_;
    std::unordered_map<int, HWND> nav_buttons_;
    std::vector<PlacedControl> placed_;
    std::unordered_map<int, bool*> checkbox_values_;
    std::unordered_map<int, HWND> checkbox_windows_;
};

constexpr wchar_t kArchiveSummaryDialogClass[] = L"AxiomArchiveSummaryDialog";

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
            state->dpi = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
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
        {L"Split volumes", capabilities.multi_volume},
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

    const int width = scale_for_dialog_dpi(640, dpi);
    const int desired_height = state.content_height +
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
    const int dialog_max_height = std::min(max_height, scale_for_dialog_dpi(520, dpi));
    const int height = std::clamp(desired_height, scale_for_dialog_dpi(380, dpi),
                                  dialog_max_height);
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;

    HWND dialog = CreateWindowExW(
        WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT, kArchiveSummaryDialogClass,
        state.data.title.c_str(),
        style,
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
    const bool owner_was_enabled = disable_dialog_owner(owner);
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

bool show_archive_feature_options_dialog(
    HWND owner,
    ArchiveFeatureDialogContext context,
    ArchiveFeatureOptions& archive_options,
    ExtractFeatureOptions& extract_options,
    const ArchiveFeatureAvailability& availability,
    std::wstring suggested_sfx_output) {
    ArchiveFeatureDialog dialog(context, archive_options, extract_options, availability,
                                std::move(suggested_sfx_output));
    if (!dialog.show(owner)) return false;
    archive_options = dialog.archive_options();
    extract_options = dialog.extract_options();
    return true;
}

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
