#define NOMINMAX
#include "gui/sfx_dialog.hpp"

#include "core/cpu.hpp"
#include "gui/dialog_support.hpp"
#include "gui/message_dialog.hpp"

#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace axiom::gui {
namespace fs = std::filesystem;
namespace {

constexpr wchar_t kWindowClass[] = L"AxiomSfxExtractDialog";
constexpr int kDestinationEdit = 4101;
constexpr int kBrowseButton = 4102;
constexpr int kOverwriteCombo = 4103;
constexpr int kThreadsCombo = 4104;
constexpr int kRestoreTime = 4105;
constexpr int kOpenDestination = 4106;

template <typename T>
class ComPtr {
public:
    ~ComPtr() { reset(); }
    T** put() {
        reset();
        return &value_;
    }
    T* operator->() const { return value_; }
private:
    void reset() {
        if (value_ != nullptr) value_->Release();
        value_ = nullptr;
    }
    T* value_{};
};

std::wstring window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) GetWindowTextW(window, result.data(), length + 1);
    result.resize(static_cast<std::size_t>(length));
    return result;
}

std::wstring format_size(std::uint64_t bytes) {
    constexpr std::array<const wchar_t*, 5> units{L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    std::wstringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(unit == 0 ? 0 : 1);
    stream << value << L' ' << units[unit];
    return stream.str();
}

std::optional<fs::path> browse_folder(HWND owner, const fs::path& initial) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        return std::nullopt;
    }
    FILEOPENDIALOGOPTIONS flags{};
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Choose extraction destination");
    if (!initial.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
                                                  IID_PPV_ARGS(folder.put())))) {
            dialog->SetFolder(folder.operator->());
        }
    }
    if (dialog->Show(owner) != S_OK) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

class SfxDialog {
public:
    SfxDialog(const SfxArchiveSummary& summary, SfxExtractDialogOptions options)
        : summary_(summary), options_(std::move(options)) {}

    ~SfxDialog() {
        delete_dialog_font(font_);
        if (background_brush_ != nullptr) DeleteObject(background_brush_);
        if (control_brush_ != nullptr) DeleteObject(control_brush_);
    }

    bool show(HWND owner, HINSTANCE instance) {
        owner_ = owner;
        instance_ = instance;
        dpi_ = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
        dark_ = dialog_system_prefers_dark_mode();
        if (!register_class()) return false;

        RECT window_rect{0, 0, scale(680), scale(470)};
        constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        AdjustWindowRectExForDpi(&window_rect, style, FALSE, WS_EX_DLGMODALFRAME, dpi_);
        const int width = window_rect.right - window_rect.left;
        const int height = window_rect.bottom - window_rect.top;
        RECT anchor{};
        if (owner == nullptr || !GetWindowRect(owner, &anchor)) {
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
        }
        const int x = anchor.left + (anchor.right - anchor.left - width) / 2;
        const int y = anchor.top + (anchor.bottom - anchor.top - height) / 2;
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kWindowClass,
                                  L"Axiom Self-Extractor", style,
                                  x, y, width, height, owner, nullptr, instance, this);
        if (window_ == nullptr) return false;
        restore_named_window_placement(window_, owner, L"SfxExtractDialog");
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

    const SfxExtractDialogOptions& options() const { return options_; }

private:
    bool register_class() const {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &SfxDialog::window_proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kWindowClass;
        assign_axiom_window_class_icons(wc, instance_);
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    int scale(int value) const { return scale_for_dialog_dpi(value, dpi_); }

    HWND create_control(const wchar_t* class_name, const wchar_t* text,
                        DWORD style, int id) {
        HWND control = CreateWindowExW(0, class_name, text,
                                       WS_CHILD | WS_VISIBLE | style,
                                       0, 0, 0, 0, window_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       instance_, nullptr);
        set_dialog_control_font(control, font_);
        apply_dialog_control_theme(control, dark_);
        controls_.push_back(control);
        return control;
    }

    HWND create_label(const wchar_t* text) {
        return create_control(L"STATIC", text, SS_LEFT | SS_NOPREFIX, -1);
    }

    void create_controls() {
        font_ = create_dialog_font(dpi_);
        tooltip_ = create_dialog_tooltip(window_);
        heading_ = create_label((L"Extract " + summary_.archive_name).c_str());

        const std::wstring item_summary =
            std::to_wstring(summary_.file_count) +
            (summary_.file_count == 1 ? L" file" : L" files") + L", " +
            std::to_wstring(summary_.directory_count) +
            (summary_.directory_count == 1 ? L" folder" : L" folders") +
            L"  |  " + format_size(summary_.unpacked_size) + L" unpacked";
        summary_label_ = create_label(item_summary.c_str());

        std::wstring security = summary_.encrypted ? L"Encrypted archive" : L"Not encrypted";
        security += summary_.signature_present
            ? (summary_.signature_valid ? L"  |  Valid EdDSA signature" : L"  |  Invalid signature")
            : L"  |  Not signed";
        security += L"  |  File integrity is verified while extracting";
        security_label_ = create_label(security.c_str());

        destination_label_ = create_label(L"Destination folder path");
        destination_edit_ = create_control(L"EDIT", options_.destination.c_str(),
                                           WS_TABSTOP | ES_AUTOHSCROLL, kDestinationEdit);
        SendMessageW(destination_edit_, EM_SETSEL, 0, 0);
        SendMessageW(destination_edit_, EM_SETLIMITTEXT, 32767, 0);
        browse_button_ = create_control(L"BUTTON", L"Browse...",
                                        WS_TABSTOP | BS_OWNERDRAW, kBrowseButton);

        overwrite_label_ = create_label(L"Existing files");
        overwrite_combo_ = create_control(
            WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                                   CBS_HASSTRINGS | WS_VSCROLL,
            kOverwriteCombo);
        for (const wchar_t* choice : {L"Replace existing files", L"Skip existing files",
                                      L"Stop if a file exists"}) {
            SendMessageW(overwrite_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice));
        }
        const int overwrite_index = options_.overwrite == ExtractOptions::Overwrite::overwrite ? 0
            : options_.overwrite == ExtractOptions::Overwrite::skip ? 1 : 2;
        SendMessageW(overwrite_combo_, CB_SETCURSEL, overwrite_index, 0);

        threads_label_ = create_label(L"CPU threads");
        threads_combo_ = create_control(
            WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                                   CBS_HASSTRINGS | WS_VSCROLL,
            kThreadsCombo);
        SendMessageW(threads_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Automatic"));
        thread_values_.push_back(0);
        const auto hardware = static_cast<unsigned>(
            axiom::core::logical_processor_count());
        for (unsigned value = 1; value <= hardware; value *= 2) {
            const std::wstring label = std::to_wstring(value);
            SendMessageW(threads_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            thread_values_.push_back(value);
            if (value > hardware / 2) break;
        }
        if (thread_values_.back() != hardware) {
            const std::wstring label = std::to_wstring(hardware);
            SendMessageW(threads_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            thread_values_.push_back(hardware);
        }
        const auto selected_thread = std::find(thread_values_.begin(), thread_values_.end(),
                                               options_.thread_count);
        SendMessageW(threads_combo_, CB_SETCURSEL,
                     selected_thread == thread_values_.end()
                         ? 0 : static_cast<int>(selected_thread - thread_values_.begin()), 0);

        restore_checkbox_ = create_control(L"BUTTON", L"Preserve file modification times",
                                           WS_TABSTOP | BS_OWNERDRAW, kRestoreTime);
        open_checkbox_ = create_control(L"BUTTON", L"Open destination when extraction finishes",
                                        WS_TABSTOP | BS_OWNERDRAW, kOpenDestination);

        if (!summary_.comment.empty()) {
            comment_heading_ = create_label(L"Archive comment");
            std::wstring comment = summary_.comment;
            if (comment.size() > 220) comment = comment.substr(0, 217) + L"...";
            comment_label_ = create_label(comment.c_str());
        }

        extract_button_ = create_control(L"BUTTON", L"Extract",
                                         WS_TABSTOP | BS_OWNERDRAW, IDOK);
        cancel_button_ = create_control(L"BUTTON", L"Cancel",
                                         WS_TABSTOP | BS_OWNERDRAW, IDCANCEL);
        add_dialog_tooltip(tooltip_, destination_edit_,
                           L"Windows folder path that will receive the extracted files.");
        add_dialog_tooltip(tooltip_, browse_button_,
                           L"Choose an existing destination folder.");
        add_dialog_tooltip(tooltip_, overwrite_combo_,
                           L"Choose how extraction handles files that already exist at the destination.");
        add_dialog_tooltip(tooltip_, threads_combo_,
                           L"Select an unsigned CPU thread count, or Automatic to use Axiom's default.");
        add_dialog_tooltip(tooltip_, restore_checkbox_,
                           L"Restore each extracted file's stored last-modified timestamp.");
        add_dialog_tooltip(tooltip_, open_checkbox_,
                           L"Open the destination folder in Axiom after extraction succeeds.");
        SendMessageW(window_, DM_SETDEFID, IDOK, 0);
    }

    void rebuild_font() {
        delete_dialog_font(font_);
        font_ = create_dialog_font(dpi_);
        for (HWND control : controls_) set_dialog_control_font(control, font_);
    }

    void recreate_brushes() {
        if (background_brush_ != nullptr) DeleteObject(background_brush_);
        if (control_brush_ != nullptr) DeleteObject(control_brush_);
        const DialogColors colors = dialog_colors(dark_);
        background_brush_ = CreateSolidBrush(colors.background);
        control_brush_ = CreateSolidBrush(colors.control_background);
    }

    void apply_theme() {
        dark_ = dialog_system_prefers_dark_mode();
        recreate_brushes();
        apply_dialog_dark_frame(window_, dark_);
        for (HWND control : controls_) apply_dialog_control_theme(control, dark_);
        InvalidateRect(window_, nullptr, TRUE);
    }

    void layout() const {
        RECT client{};
        GetClientRect(window_, &client);
        const int margin = scale(24);
        const int width = client.right - margin * 2;
        MoveWindow(heading_, margin, scale(22), width, scale(26), TRUE);
        MoveWindow(summary_label_, margin, scale(52), width, scale(22), TRUE);
        MoveWindow(security_label_, margin, scale(76), width, scale(22), TRUE);

        MoveWindow(destination_label_, margin, scale(119), width, scale(20), TRUE);
        const int browse_width = scale(104);
        MoveWindow(destination_edit_, margin, scale(143),
                   width - browse_width - scale(10), scale(31), TRUE);
        MoveWindow(browse_button_, client.right - margin - browse_width, scale(143),
                   browse_width, scale(31), TRUE);

        const int column_gap = scale(24);
        const int column_width = (width - column_gap) / 2;
        MoveWindow(overwrite_label_, margin, scale(196), column_width, scale(20), TRUE);
        MoveWindow(overwrite_combo_, margin, scale(220), column_width, scale(250), TRUE);
        const int right_column = margin + column_width + column_gap;
        MoveWindow(threads_label_, right_column, scale(196), column_width, scale(20), TRUE);
        MoveWindow(threads_combo_, right_column, scale(220), column_width, scale(250), TRUE);

        MoveWindow(restore_checkbox_, margin, scale(272), width, scale(28), TRUE);
        MoveWindow(open_checkbox_, margin, scale(305), width, scale(28), TRUE);

        if (comment_heading_ != nullptr) {
            MoveWindow(comment_heading_, margin, scale(345), width, scale(20), TRUE);
            MoveWindow(comment_label_, margin, scale(369), width, scale(42), TRUE);
        }

        const int button_width = scale(100);
        const int button_height = scale(32);
        const int button_y = client.bottom - margin - button_height;
        MoveWindow(cancel_button_, client.right - margin - button_width, button_y,
                   button_width, button_height, TRUE);
        MoveWindow(extract_button_, client.right - margin - button_width * 2 - scale(10), button_y,
                   button_width, button_height, TRUE);
    }

    void toggle_checkbox(int id) {
        bool* value = id == kRestoreTime ? &options_.restore_mtime : &options_.open_destination;
        *value = !*value;
        InvalidateRect(id == kRestoreTime ? restore_checkbox_ : open_checkbox_, nullptr, TRUE);
    }

    void accept() {
        std::wstring destination = window_text(destination_edit_);
        const auto first = destination.find_first_not_of(L" \t\r\n");
        const auto last = destination.find_last_not_of(L" \t\r\n");
        if (first == std::wstring::npos) destination.clear();
        else destination = destination.substr(first, last - first + 1);
        if (destination.empty()) {
            show_message_dialog(window_, instance_, dpi_, dark_, L"Axiom Self-Extractor",
                                L"Choose a destination folder before extracting.",
                                MessageDialogIcon::warning);
            SetFocus(destination_edit_);
            return;
        }
        options_.destination = destination;
        const int overwrite = static_cast<int>(SendMessageW(overwrite_combo_, CB_GETCURSEL, 0, 0));
        options_.overwrite = overwrite == 1 ? ExtractOptions::Overwrite::skip
            : overwrite == 2 ? ExtractOptions::Overwrite::fail
                             : ExtractOptions::Overwrite::overwrite;
        const int threads = static_cast<int>(SendMessageW(threads_combo_, CB_GETCURSEL, 0, 0));
        options_.thread_count = threads >= 0 && static_cast<std::size_t>(threads) < thread_values_.size()
            ? thread_values_[static_cast<std::size_t>(threads)] : 0;
        accepted_ = true;
        close_dialog();
    }

    void close_dialog() {
        save_named_window_placement(L"SfxExtractDialog", window_);
        if (window_ != nullptr && IsWindow(window_)) DestroyWindow(window_);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE:
                create_controls();
                apply_axiom_window_icons(window_, instance_);
                apply_theme();
                layout();
                SetFocus(destination_edit_);
                return 0;
            case WM_SIZE: layout(); return 0;
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wparam);
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                apply_axiom_window_icons(window_, instance_);
                rebuild_font();
                layout();
                return 0;
            }
            case WM_COMMAND:
                switch (LOWORD(wparam)) {
                    case kBrowseButton:
                        if (const auto selected = browse_folder(window_, window_text(destination_edit_))) {
                            SetWindowTextW(destination_edit_, selected->c_str());
                        }
                        return 0;
                    case kRestoreTime:
                    case kOpenDestination:
                        toggle_checkbox(LOWORD(wparam));
                        return 0;
                    case IDOK: accept(); return 0;
                    case IDCANCEL: close_dialog(); return 0;
                }
                break;
            case WM_DRAWITEM: {
                const auto& draw = *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
                if (draw.CtlID == kOverwriteCombo || draw.CtlID == kThreadsCombo) {
                    draw_dialog_combo_item(draw, dark_);
                } else if (draw.CtlID == kRestoreTime) {
                    draw_dialog_checkbox(draw, dark_, options_.restore_mtime);
                } else if (draw.CtlID == kOpenDestination) {
                    draw_dialog_checkbox(draw, dark_, options_.open_destination);
                } else {
                    draw_dialog_button(draw, dark_);
                }
                return TRUE;
            }
            case WM_CTLCOLORSTATIC: {
                const DialogColors colors = dialog_colors(dark_);
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetBkColor(dc, colors.background);
                SetTextColor(dc, colors.text);
                return reinterpret_cast<LRESULT>(background_brush_);
            }
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX: {
                const DialogColors colors = dialog_colors(dark_);
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetBkColor(dc, colors.control_background);
                SetTextColor(dc, colors.text);
                return reinterpret_cast<LRESULT>(control_brush_);
            }
            case WM_ERASEBKGND: {
                RECT client{};
                GetClientRect(window_, &client);
                FillRect(reinterpret_cast<HDC>(wparam), &client, background_brush_);
                return 1;
            }
            case WM_SETTINGCHANGE:
            case WM_THEMECHANGED: apply_theme(); return 0;
            case WM_CLOSE: close_dialog(); return 0;
            case WM_NCDESTROY: window_ = nullptr; return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        SfxDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            dialog = static_cast<SfxDialog*>(
                reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            dialog->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        } else {
            dialog = reinterpret_cast<SfxDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return dialog != nullptr ? dialog->handle_message(message, wparam, lparam)
                                 : DefWindowProcW(window, message, wparam, lparam);
    }

    const SfxArchiveSummary& summary_;
    SfxExtractDialogOptions options_;
    HWND owner_{};
    HWND window_{};
    HINSTANCE instance_{};
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};
    bool dark_{};
    bool accepted_{};
    HFONT font_{};
    HBRUSH background_brush_{};
    HBRUSH control_brush_{};
    HWND tooltip_{};
    std::vector<HWND> controls_;
    std::vector<std::size_t> thread_values_;
    HWND heading_{};
    HWND summary_label_{};
    HWND security_label_{};
    HWND destination_label_{};
    HWND destination_edit_{};
    HWND browse_button_{};
    HWND overwrite_label_{};
    HWND overwrite_combo_{};
    HWND threads_label_{};
    HWND threads_combo_{};
    HWND restore_checkbox_{};
    HWND open_checkbox_{};
    HWND comment_heading_{};
    HWND comment_label_{};
    HWND extract_button_{};
    HWND cancel_button_{};
};

}  // namespace

bool show_sfx_extract_dialog(HWND owner,
                             HINSTANCE instance,
                             const SfxArchiveSummary& summary,
                             SfxExtractDialogOptions& options) {
    SfxDialog dialog(summary, options);
    if (!dialog.show(owner, instance)) return false;
    options = dialog.options();
    return true;
}

}  // namespace axiom::gui
