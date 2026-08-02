#define NOMINMAX
#include "gui/license_dialog.hpp"

#include "gui/dialog_support.hpp"

#include <commctrl.h>

#include <string>
#include <vector>

namespace axiom::gui {
namespace {

constexpr wchar_t kWindowClass[] = L"AxiomSfxLicenseDialog";
constexpr int kLicenseEdit = 4201;
constexpr int kAcceptCheck = 4202;
constexpr int kContinueButton = 4203;
constexpr int kDeclineButton = 4204;

// Edit controls expect CRLF; an authored license almost certainly uses LF.
std::wstring to_windows_newlines(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size() + text.size() / 16);
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\n' && (index == 0 || text[index - 1] != L'\r')) {
            result.push_back(L'\r');
        }
        result.push_back(text[index]);
    }
    return result;
}

class LicenseDialog {
public:
    LicenseDialog(std::wstring title, std::wstring license, bool dark)
        : title_(std::move(title)), license_(std::move(license)), dark_(dark) {}

    ~LicenseDialog() {
        delete_dialog_font(font_);
        if (background_brush_ != nullptr) DeleteObject(background_brush_);
        if (control_brush_ != nullptr) DeleteObject(control_brush_);
    }

    bool show(HWND owner, HINSTANCE instance) {
        instance_ = instance;
        dpi_ = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();

        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &LicenseDialog::window_proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        assign_axiom_window_class_icons(wc, instance_);
        if (RegisterClassExW(&wc) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        RECT rect{0, 0, scale(620), scale(520)};
        constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_DLGMODALFRAME, dpi_);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        const POINT position = centered_window_position(owner, width, height);
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass, title_.c_str(),
                                  style, position.x, position.y, width, height,
                                  owner, nullptr, instance_, this);
        if (window_ == nullptr) return false;

        const bool owner_was_enabled = disable_dialog_owner(owner, window_);
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        MSG message{};
        while (IsWindow(window_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        restore_dialog_owner(owner, owner_was_enabled);
        return accepted_;
    }

private:
    int scale(int value) const { return scale_for_dialog_dpi(value, dpi_); }

    HWND create_control(const wchar_t* class_name, const wchar_t* text,
                        DWORD style, int id) {
        HWND control = CreateWindowExW(
            0, class_name, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_, nullptr);
        set_dialog_control_font(control, font_);
        apply_dialog_control_theme(control, dark_);
        return control;
    }

    void create_controls() {
        font_ = create_dialog_font(dpi_);
        heading_ = create_control(
            L"STATIC", L"Review the license agreement before continuing.",
            SS_LEFT | SS_NOPREFIX, -1);
        license_view_ = create_control(
            L"EDIT", to_windows_newlines(license_).c_str(),
            WS_BORDER | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                ES_AUTOVSCROLL,
            kLicenseEdit);
        // Owner-drawn like every other Axiom dialog control: stock Win32
        // buttons and checkboxes ignore the dark palette and would render as
        // light chrome on a dark surface.
        accept_ = create_control(L"BUTTON", L"I accept the terms of this agreement",
                                 BS_OWNERDRAW | WS_TABSTOP, kAcceptCheck);
        continue_ = create_control(L"BUTTON", L"Continue",
                                   BS_OWNERDRAW | WS_TABSTOP, kContinueButton);
        decline_ = create_control(L"BUTTON", L"Decline", BS_OWNERDRAW | WS_TABSTOP,
                                  kDeclineButton);
        // Acceptance must be an action, never a default.
        EnableWindow(continue_, FALSE);
        apply_theme();
    }

    void layout() {
        RECT client{};
        GetClientRect(window_, &client);
        const int margin = scale(16);
        const int spacing = scale(10);
        const int button_width = scale(110);
        const int button_height = scale(30);
        const int heading_height = scale(22);
        const int check_height = scale(24);
        const int width = client.right - 2 * margin;

        int y = margin;
        MoveWindow(heading_, margin, y, width, heading_height, TRUE);
        y += heading_height + spacing;
        const int view_height = client.bottom - y - margin - button_height -
                                check_height - 2 * spacing;
        MoveWindow(license_view_, margin, y, width, view_height, TRUE);
        y += view_height + spacing;
        MoveWindow(accept_, margin, y, width, check_height, TRUE);
        y += check_height + spacing;
        MoveWindow(continue_, client.right - margin - button_width, y,
                   button_width, button_height, TRUE);
        MoveWindow(decline_, client.right - margin - 2 * button_width - spacing, y,
                   button_width, button_height, TRUE);
    }

    void apply_theme() {
        const auto colors = dialog_colors(dark_);
        if (background_brush_ != nullptr) DeleteObject(background_brush_);
        if (control_brush_ != nullptr) DeleteObject(control_brush_);
        background_brush_ = CreateSolidBrush(colors.background);
        control_brush_ = CreateSolidBrush(colors.control_background);
        apply_dialog_dark_frame(window_, dark_);
        for (HWND control : {heading_, license_view_, accept_, continue_, decline_}) {
            apply_dialog_control_theme(control, dark_);
        }
        InvalidateRect(window_, nullptr, TRUE);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE:
                create_controls();
                layout();
                return 0;
            case WM_SIZE:
                layout();
                return 0;
            case WM_COMMAND: {
                const int id = LOWORD(wparam);
                if (id == kAcceptCheck) {
                    // An owner-drawn checkbox has no automatic state, so the
                    // dialog owns it and repaints on each toggle.
                    checked_ = !checked_;
                    EnableWindow(continue_, checked_ ? TRUE : FALSE);
                    InvalidateRect(accept_, nullptr, TRUE);
                    return 0;
                }
                if (id == kContinueButton) {
                    if (!checked_) return 0;
                    accepted_ = true;
                    destroy_modal_dialog(window_);
                    return 0;
                }
                if (id == kDeclineButton || id == IDCANCEL) {
                    accepted_ = false;
                    destroy_modal_dialog(window_);
                    return 0;
                }
                return 0;
            }
            case WM_DRAWITEM: {
                const auto& draw = *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
                if (draw.CtlID == kAcceptCheck) {
                    draw_dialog_checkbox(draw, dark_, checked_);
                } else {
                    draw_dialog_button(draw, dark_);
                }
                return TRUE;
            }
            case WM_CTLCOLORSTATIC: {
                const auto colors = dialog_colors(dark_);
                SetTextColor(reinterpret_cast<HDC>(wparam), colors.text);
                SetBkColor(reinterpret_cast<HDC>(wparam), colors.background);
                return reinterpret_cast<LRESULT>(background_brush_);
            }
            case WM_CTLCOLOREDIT: {
                const auto colors = dialog_colors(dark_);
                SetTextColor(reinterpret_cast<HDC>(wparam), colors.text);
                SetBkColor(reinterpret_cast<HDC>(wparam), colors.control_background);
                return reinterpret_cast<LRESULT>(control_brush_);
            }
            case WM_ERASEBKGND: {
                RECT client{};
                GetClientRect(window_, &client);
                FillRect(reinterpret_cast<HDC>(wparam), &client, background_brush_);
                return 1;
            }
            case WM_SETTINGCHANGE:
            case WM_THEMECHANGED:
                // The extractor has no settings store, so it follows the
                // system the same way the other SFX dialogs do.
                dark_ = dialog_system_prefers_dark_mode();
                apply_theme();
                return 0;
            case WM_CLOSE:
                accepted_ = false;
                destroy_modal_dialog(window_);
                return 0;
            case WM_NCDESTROY:
                window_ = nullptr;
                return 0;
            default:
                break;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                                        LPARAM lparam) {
        LicenseDialog* self = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<LicenseDialog*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<LicenseDialog*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        if (self == nullptr) return DefWindowProcW(window, message, wparam, lparam);
        return self->handle_message(message, wparam, lparam);
    }

    std::wstring title_;
    std::wstring license_;
    HINSTANCE instance_{};
    HWND window_{};
    HWND heading_{};
    HWND license_view_{};
    HWND accept_{};
    HWND continue_{};
    HWND decline_{};
    HFONT font_{};
    HBRUSH background_brush_{};
    HBRUSH control_brush_{};
    UINT dpi_ = 96;
    bool dark_ = false;
    bool checked_ = false;   // the accept box; owner-drawn, so we track it
    bool accepted_ = false;
};

}  // namespace

bool show_license_dialog(HWND owner, HINSTANCE instance, const std::wstring& title,
                         const std::wstring& license_text, bool dark) {
    LicenseDialog dialog(title.empty() ? L"License agreement" : title, license_text,
                         dark);
    return dialog.show(owner, instance);
}

}  // namespace axiom::gui
