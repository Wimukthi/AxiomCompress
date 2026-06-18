#define NOMINMAX
#include "gui/archive_dialogs.hpp"

#include <dwmapi.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace axiom::gui {
namespace fs = std::filesystem;
namespace {

constexpr int kPathEdit = 2001;
constexpr int kBrowse = 2002;
constexpr int kLevelDown = 2003;
constexpr int kLevelValue = 2004;
constexpr int kLevelUp = 2005;
constexpr int kThreads = 2006;
constexpr int kOverwrite = 2007;
constexpr int kRestoreTime = 2008;
constexpr int kConfirmDelete = 2009;
constexpr int kShowHidden = 2010;
constexpr int kAccept = IDOK;
constexpr int kCancel = IDCANCEL;

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
        if (value_) value_->Release();
        value_ = nullptr;
    }
    T* value_ = nullptr;
};

bool use_dark_theme() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0) {
        return false;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value == 0;
    }
    return false;
}

struct Palette {
    bool dark = false;
    COLORREF window = GetSysColor(COLOR_WINDOW);
    COLORREF edit = GetSysColor(COLOR_WINDOW);
    COLORREF button = GetSysColor(COLOR_BTNFACE);
    COLORREF hot = GetSysColor(COLOR_BTNHIGHLIGHT);
    COLORREF pressed = GetSysColor(COLOR_BTNSHADOW);
    COLORREF border = GetSysColor(COLOR_ACTIVEBORDER);
    COLORREF text = GetSysColor(COLOR_WINDOWTEXT);
    COLORREF muted = GetSysColor(COLOR_GRAYTEXT);
    COLORREF focus = GetSysColor(COLOR_HIGHLIGHT);
};

Palette make_palette() {
    Palette result;
    result.dark = use_dark_theme();
    if (result.dark) {
        result.window = RGB(30, 30, 30);
        result.edit = RGB(36, 36, 36);
        result.button = RGB(45, 45, 48);
        result.hot = RGB(58, 58, 61);
        result.pressed = RGB(72, 72, 76);
        result.border = RGB(63, 63, 67);
        result.text = RGB(242, 242, 242);
        result.muted = RGB(176, 176, 176);
        result.focus = RGB(78, 115, 158);
    }
    return result;
}

void set_dark_title(HWND window, bool dark) {
    BOOL enabled = dark ? TRUE : FALSE;
    constexpr DWORD immersive_dark_mode = 20;
    if (FAILED(DwmSetWindowAttribute(window, immersive_dark_mode, &enabled, sizeof(enabled)))) {
        constexpr DWORD older_immersive_dark_mode = 19;
        DwmSetWindowAttribute(window, older_immersive_dark_mode, &enabled, sizeof(enabled));
    }
}

std::wstring window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(window, result.data(), length + 1);
    result.resize(static_cast<std::size_t>(length));
    return result;
}

void set_window_text(HWND window, const std::wstring& text) {
    SetWindowTextW(window, text.c_str());
}

std::optional<fs::path> browse_save_archive(HWND owner) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) return std::nullopt;
    const COMDLG_FILTERSPEC filters[] = {{L"Axiom archives", L"*.axar"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(2, filters);
    dialog->SetDefaultExtension(L"axar");
    if (dialog->Show(owner) != S_OK) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::optional<fs::path> browse_folder(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) return std::nullopt;
    FILEOPENDIALOGOPTIONS flags{};
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (dialog->Show(owner) != S_OK) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

enum class DialogMode { create_archive, extract_archive, settings };

class OptionsDialog {
public:
    explicit OptionsDialog(DialogMode mode) : mode_(mode) {}
    ~OptionsDialog() {
        if (font_) DeleteObject(font_);
        if (window_brush_) DeleteObject(window_brush_);
        if (edit_brush_) DeleteObject(edit_brush_);
    }

    bool show(HWND owner) {
        owner_ = owner;
        instance_ = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
        dpi_ = GetDpiForWindow(owner);
        if (!register_class()) return false;
        const int width = scale(540);
        const int height = scale(mode_ == DialogMode::settings ? 260 : 290);
        RECT owner_rect{};
        GetWindowRect(owner, &owner_rect);
        const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
        const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
        const wchar_t* title = mode_ == DialogMode::create_archive ? L"Add to archive"
            : (mode_ == DialogMode::extract_archive ? L"Extract archive" : L"Axiom settings");
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, class_name(), title,
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  x, y, width, height, owner, nullptr, instance_, this);
        if (!window_) return false;
        EnableWindow(owner, FALSE);
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        MSG message{};
        while (IsWindow(window_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
        return accepted_;
    }

    CreateArchiveDialogOptions create_options;
    ExtractArchiveDialogOptions extract_options;
    ApplicationDialogOptions application_options;
    std::size_t input_count = 0;
    fs::path archive_path;

private:
    static const wchar_t* class_name() { return L"AxiomDarkOptionsDialog"; }

    bool register_class() {
        static ATOM atom = 0;
        if (atom) return true;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &OptionsDialog::window_proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = class_name();
        atom = RegisterClassExW(&wc);
        return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    int scale(int value) const {
        return MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
    }

    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id) {
        HWND result = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
                                      0, 0, 0, 0, window_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                      instance_, nullptr);
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SetWindowTheme(result, palette_.dark ? L"DarkMode_Explorer" : nullptr, nullptr);
        return result;
    }

    HWND label(const wchar_t* text) {
        return control(L"STATIC", text, SS_LEFT, 0);
    }

    void create_controls() {
        palette_ = make_palette();
        set_dark_title(window_, palette_.dark);
        window_brush_ = CreateSolidBrush(palette_.window);
        edit_brush_ = CreateSolidBrush(palette_.edit);
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        }
        font_ = CreateFontIndirectW(&metrics.lfMessageFont);

        if (mode_ != DialogMode::settings) {
            summary_ = label(mode_ == DialogMode::create_archive ? L"Selected items" : L"Archive");
            path_label_ = label(mode_ == DialogMode::create_archive ? L"Archive path" : L"Destination");
            path_edit_ = control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kPathEdit);
            browse_ = control(L"BUTTON", L"Browse...", WS_TABSTOP | BS_OWNERDRAW, kBrowse);
        }
        level_label_ = label(L"Compression level");
        level_down_ = control(L"BUTTON", L"-", WS_TABSTOP | BS_OWNERDRAW, kLevelDown);
        level_value_ = control(L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, kLevelValue);
        level_up_ = control(L"BUTTON", L"+", WS_TABSTOP | BS_OWNERDRAW, kLevelUp);
        threads_label_ = label(L"Threads (0 = all)");
        threads_edit_ = control(L"EDIT", L"0", WS_TABSTOP | ES_NUMBER, kThreads);
        if (mode_ == DialogMode::extract_archive) {
            overwrite_ = control(L"BUTTON", L"Overwrite existing files",
                                 WS_TABSTOP | BS_AUTOCHECKBOX | BS_OWNERDRAW, kOverwrite);
            restore_time_ = control(L"BUTTON", L"Restore modified times",
                                    WS_TABSTOP | BS_AUTOCHECKBOX | BS_OWNERDRAW, kRestoreTime);
        } else if (mode_ == DialogMode::settings) {
            confirm_delete_ = control(L"BUTTON", L"Confirm before deleting",
                                      WS_TABSTOP | BS_AUTOCHECKBOX | BS_OWNERDRAW, kConfirmDelete);
            show_hidden_ = control(L"BUTTON", L"Show hidden and system items",
                                   WS_TABSTOP | BS_AUTOCHECKBOX | BS_OWNERDRAW, kShowHidden);
        }
        accept_ = control(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW, kAccept);
        cancel_ = control(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, kCancel);
        SendMessageW(path_edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(scale(6), scale(6)));
        SendMessageW(threads_edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(scale(6), scale(6)));
        load_values();
        layout();
    }

    void load_values() {
        int level = 5;
        std::size_t threads = 0;
        if (mode_ == DialogMode::create_archive) {
            level = create_options.level;
            threads = create_options.thread_count;
            set_window_text(path_edit_, create_options.archive_path.wstring());
            set_window_text(summary_, std::to_wstring(input_count) +
                            (input_count == 1 ? L" item" : L" items"));
        } else if (mode_ == DialogMode::extract_archive) {
            threads = extract_options.thread_count;
            set_window_text(path_edit_, extract_options.destination.wstring());
            set_window_text(summary_, archive_path.filename().wstring());
            overwrite_checked_ = extract_options.overwrite;
            restore_time_checked_ = extract_options.restore_mtime;
        } else {
            level = application_options.default_level;
            threads = application_options.default_thread_count;
            confirm_delete_checked_ = application_options.confirm_delete;
            show_hidden_checked_ = application_options.show_hidden;
        }
        level_ = std::clamp(level, 1, 9);
        update_level();
        set_window_text(threads_edit_, std::to_wstring(threads));
    }

    void layout() {
        RECT client{};
        GetClientRect(window_, &client);
        const int margin = scale(18);
        const int label_width = scale(125);
        const int row_height = scale(30);
        const int gap = scale(12);
        int y = margin;
        if (mode_ != DialogMode::settings) {
            MoveWindow(summary_, margin, y, client.right - margin * 2, row_height, TRUE);
            y += row_height + scale(6);
            MoveWindow(path_label_, margin, y + scale(6), label_width, row_height, TRUE);
            const int browse_width = scale(86);
            MoveWindow(path_edit_, margin + label_width, y,
                       client.right - margin * 2 - label_width - browse_width - scale(6), row_height, TRUE);
            MoveWindow(browse_, client.right - margin - browse_width, y, browse_width, row_height, TRUE);
            y += row_height + gap;
        }
        if (mode_ != DialogMode::extract_archive) {
            MoveWindow(level_label_, margin, y + scale(6), label_width, row_height, TRUE);
            int x = margin + label_width;
            MoveWindow(level_down_, x, y, scale(34), row_height, TRUE);
            x += scale(38);
            MoveWindow(level_value_, x, y, scale(96), row_height, TRUE);
            x += scale(100);
            MoveWindow(level_up_, x, y, scale(34), row_height, TRUE);
            y += row_height + gap;
        } else {
            ShowWindow(level_label_, SW_HIDE);
            ShowWindow(level_down_, SW_HIDE);
            ShowWindow(level_value_, SW_HIDE);
            ShowWindow(level_up_, SW_HIDE);
        }
        MoveWindow(threads_label_, margin, y + scale(6), label_width, row_height, TRUE);
        MoveWindow(threads_edit_, margin + label_width, y, scale(90), row_height, TRUE);
        y += row_height + scale(8);
        if (mode_ == DialogMode::extract_archive) {
            MoveWindow(overwrite_, margin + label_width, y, scale(220), row_height, TRUE);
            y += row_height;
            MoveWindow(restore_time_, margin + label_width, y, scale(220), row_height, TRUE);
        } else if (mode_ == DialogMode::settings) {
            MoveWindow(confirm_delete_, margin + label_width, y, scale(230), row_height, TRUE);
            y += row_height;
            MoveWindow(show_hidden_, margin + label_width, y, scale(250), row_height, TRUE);
        }
        const int button_width = scale(86);
        const int button_y = client.bottom - margin - row_height;
        MoveWindow(cancel_, client.right - margin - button_width, button_y, button_width, row_height, TRUE);
        MoveWindow(accept_, client.right - margin - button_width * 2 - scale(8),
                   button_y, button_width, row_height, TRUE);
    }

    void update_level() {
        set_window_text(level_value_, L"Level " + std::to_wstring(level_));
        InvalidateRect(level_value_, nullptr, TRUE);
    }

    void browse() {
        const auto path = mode_ == DialogMode::create_archive
            ? browse_save_archive(window_)
            : browse_folder(window_);
        if (path) set_window_text(path_edit_, path->wstring());
    }

    std::size_t thread_count() const {
        try {
            return static_cast<std::size_t>(std::stoull(window_text(threads_edit_)));
        } catch (...) {
            return 0;
        }
    }

    void accept() {
        if (mode_ != DialogMode::settings && window_text(path_edit_).empty()) {
            MessageBoxW(window_, L"Choose a path first.", L"Axiom", MB_ICONINFORMATION);
            return;
        }
        if (mode_ == DialogMode::create_archive) {
            create_options.archive_path = window_text(path_edit_);
            create_options.level = level_;
            create_options.thread_count = thread_count();
        } else if (mode_ == DialogMode::extract_archive) {
            extract_options.destination = window_text(path_edit_);
            extract_options.thread_count = thread_count();
            extract_options.overwrite = overwrite_checked_;
            extract_options.restore_mtime = restore_time_checked_;
        } else {
            application_options.default_level = level_;
            application_options.default_thread_count = thread_count();
            application_options.confirm_delete = confirm_delete_checked_;
            application_options.show_hidden = show_hidden_checked_;
        }
        accepted_ = true;
        DestroyWindow(window_);
    }

    bool checkbox_checked(int id) const {
        switch (id) {
            case kOverwrite: return overwrite_checked_;
            case kRestoreTime: return restore_time_checked_;
            case kConfirmDelete: return confirm_delete_checked_;
            case kShowHidden: return show_hidden_checked_;
            default: return false;
        }
    }

    void toggle(int id, HWND checkbox) {
        switch (id) {
            case kOverwrite: overwrite_checked_ = !overwrite_checked_; break;
            case kRestoreTime: restore_time_checked_ = !restore_time_checked_; break;
            case kConfirmDelete: confirm_delete_checked_ = !confirm_delete_checked_; break;
            case kShowHidden: show_hidden_checked_ = !show_hidden_checked_; break;
        }
        InvalidateRect(checkbox, nullptr, TRUE);
    }

    void draw_button(const DRAWITEMSTRUCT& draw) const {
        RECT rect = draw.rcItem;
        const int id = GetDlgCtrlID(draw.hwndItem);
        const bool checkbox = id == kOverwrite || id == kRestoreTime ||
                              id == kConfirmDelete || id == kShowHidden;
        const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
        const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
        const bool focused = (draw.itemState & ODS_FOCUS) != 0;
        HBRUSH background = CreateSolidBrush(checkbox ? palette_.window
            : (pressed ? palette_.pressed : palette_.button));
        FillRect(draw.hDC, &rect, background);
        DeleteObject(background);
        std::wstring text = window_text(draw.hwndItem);
        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, disabled ? palette_.muted : palette_.text);
        HGDIOBJ old_font = SelectObject(draw.hDC, font_);
        if (checkbox) {
            const int box_size = scale(16);
            RECT box{rect.left + scale(2), rect.top + (rect.bottom - rect.top - box_size) / 2,
                     rect.left + scale(2) + box_size, rect.top + (rect.bottom - rect.top + box_size) / 2};
            HBRUSH box_brush = CreateSolidBrush(palette_.edit);
            FillRect(draw.hDC, &box, box_brush);
            DeleteObject(box_brush);
            HBRUSH border_brush = CreateSolidBrush(focused ? palette_.focus : palette_.border);
            FrameRect(draw.hDC, &box, border_brush);
            DeleteObject(border_brush);
            if (checkbox_checked(id)) {
                HPEN pen = CreatePen(PS_SOLID, scale(2), palette_.text);
                HGDIOBJ old_pen = SelectObject(draw.hDC, pen);
                MoveToEx(draw.hDC, box.left + scale(3), box.top + scale(8), nullptr);
                LineTo(draw.hDC, box.left + scale(7), box.bottom - scale(3));
                LineTo(draw.hDC, box.right - scale(3), box.top + scale(3));
                SelectObject(draw.hDC, old_pen);
                DeleteObject(pen);
            }
            rect.left = box.right + scale(8);
            DrawTextW(draw.hDC, text.c_str(), -1, &rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            HBRUSH border = CreateSolidBrush(focused ? palette_.focus : palette_.border);
            FrameRect(draw.hDC, &rect, border);
            DeleteObject(border);
            DrawTextW(draw.hDC, text.c_str(), -1, &rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(draw.hDC, old_font);
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE: create_controls(); return 0;
            case WM_SIZE: layout(); return 0;
            case WM_ERASEBKGND: {
                RECT rect{};
                GetClientRect(window_, &rect);
                FillRect(reinterpret_cast<HDC>(wparam), &rect, window_brush_);
                return 1;
            }
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN:
                SetBkColor(reinterpret_cast<HDC>(wparam), palette_.window);
                SetTextColor(reinterpret_cast<HDC>(wparam), palette_.text);
                return reinterpret_cast<LRESULT>(window_brush_);
            case WM_CTLCOLOREDIT:
                SetBkColor(reinterpret_cast<HDC>(wparam), palette_.edit);
                SetTextColor(reinterpret_cast<HDC>(wparam), palette_.text);
                return reinterpret_cast<LRESULT>(edit_brush_);
            case WM_DRAWITEM:
                draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
                return TRUE;
            case WM_COMMAND:
                switch (LOWORD(wparam)) {
                    case kBrowse: browse(); return 0;
                    case kLevelDown: level_ = std::max(1, level_ - 1); update_level(); return 0;
                    case kLevelUp: level_ = std::min(9, level_ + 1); update_level(); return 0;
                    case kAccept: accept(); return 0;
                    case kCancel: DestroyWindow(window_); return 0;
                    case kOverwrite: toggle(kOverwrite, overwrite_); return 0;
                    case kRestoreTime: toggle(kRestoreTime, restore_time_); return 0;
                    case kConfirmDelete: toggle(kConfirmDelete, confirm_delete_); return 0;
                    case kShowHidden: toggle(kShowHidden, show_hidden_); return 0;
                }
                break;
            case WM_CLOSE: DestroyWindow(window_); return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        OptionsDialog* self = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<OptionsDialog*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<OptionsDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return self ? self->handle(message, wparam, lparam)
                    : DefWindowProcW(window, message, wparam, lparam);
    }

    DialogMode mode_;
    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    Palette palette_;
    HBRUSH window_brush_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    HFONT font_ = nullptr;
    bool accepted_ = false;
    int level_ = 5;
    bool overwrite_checked_ = false;
    bool restore_time_checked_ = true;
    bool confirm_delete_checked_ = true;
    bool show_hidden_checked_ = true;
    HWND summary_ = nullptr;
    HWND path_label_ = nullptr;
    HWND path_edit_ = nullptr;
    HWND browse_ = nullptr;
    HWND level_label_ = nullptr;
    HWND level_down_ = nullptr;
    HWND level_value_ = nullptr;
    HWND level_up_ = nullptr;
    HWND threads_label_ = nullptr;
    HWND threads_edit_ = nullptr;
    HWND overwrite_ = nullptr;
    HWND restore_time_ = nullptr;
    HWND confirm_delete_ = nullptr;
    HWND show_hidden_ = nullptr;
    HWND accept_ = nullptr;
    HWND cancel_ = nullptr;
};

}  // namespace

bool show_create_archive_dialog(HWND owner,
                                std::size_t input_count,
                                CreateArchiveDialogOptions& options) {
    OptionsDialog dialog(DialogMode::create_archive);
    dialog.create_options = options;
    dialog.input_count = input_count;
    if (!dialog.show(owner)) return false;
    options = std::move(dialog.create_options);
    return true;
}

bool show_extract_archive_dialog(HWND owner,
                                 const fs::path& archive_path,
                                 ExtractArchiveDialogOptions& options) {
    OptionsDialog dialog(DialogMode::extract_archive);
    dialog.archive_path = archive_path;
    dialog.extract_options = options;
    if (!dialog.show(owner)) return false;
    options = std::move(dialog.extract_options);
    return true;
}

bool show_application_settings_dialog(HWND owner, ApplicationDialogOptions& options) {
    OptionsDialog dialog(DialogMode::settings);
    dialog.application_options = options;
    if (!dialog.show(owner)) return false;
    options = std::move(dialog.application_options);
    return true;
}

}  // namespace axiom::gui
