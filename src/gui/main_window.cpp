#define NOMINMAX
#include "axiom/archive.hpp"
#include "axiom/axiom.hpp"
#include "gui/about_dialog.hpp"
#include "gui/app.hpp"
#include "gui/archive_dialogs.hpp"
#include "gui/archive_feature_dialogs.hpp"
#include "gui/browser_model.hpp"
#include "gui/custom_menu.hpp"
#include "gui/dialog_support.hpp"
#include "gui/directory_watcher.hpp"
#include "gui/message_dialog.hpp"
#include "gui/operation_runner.hpp"
#include "gui/operation_progress_window.hpp"
#include "gui/settings_store.hpp"
#include "gui/toolbar_icons.hpp"
#include "gui/update_checker.hpp"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Uxtheme.lib")

namespace {

namespace fs = std::filesystem;

constexpr UINT kOperationDoneMessage = WM_APP + 1;
constexpr UINT kOperationProgressMessage = WM_APP + 2;
constexpr UINT kTableActivateMessage = WM_APP + 3;
constexpr UINT kTableSelectionChangedMessage = WM_APP + 4;
constexpr UINT kBrowserLoadedMessage = WM_APP + 5;
constexpr UINT kTableParentMessage = WM_APP + 6;
constexpr UINT kTableSortMessage = WM_APP + 7;
constexpr UINT kDirectoryChangedMessage = WM_APP + 8;
constexpr UINT_PTR kDirectoryRefreshTimer = 41;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
#endif

enum ControlId : int {
    kAddFiles = 1005,
    kOpenArchive = 1008,
    kExtract = 1010,
    kTest = 1011,
    kList = 1015,
    kStatus = 1017,
    kNavigateBack = 1022,
    kNavigateForward = 1023,
    kNavigateUp = 1024,
    kNavigateRefresh = 1025,
    kAddressEdit = 1026,
    kAddressGo = 1027,
    kView = 1028,
    kDelete = 1029,
    kInfo = 1030,
    kSettings = 1031,
    kMenuFile = 1100,
    kMenuCommands = 1101,
    kMenuTools = 1102,
    kMenuOptions = 1103,
    kMenuHelp = 1104,
    kExitApplication = 1110,
    kSelectAll = 1111,
    kAbout = 1112,
    kCheckUpdates = 1113,
    kArchiveFeatures = 1120,
    kUpdateArchive = 1121,
    kSynchronizeArchive = 1122,
    kDeleteArchiveEntries = 1123,
    kRepackArchive = 1124,
    kEditArchiveComment = 1125,
    kLockArchive = 1126,
    kRepairArchive = 1127,
    kCreateRecoveryVolumes = 1128,
    kVerifyArchiveSignature = 1129,
    kCreateSfx = 1130,
    kFreshenArchive = 1131,
};

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() {
        reset();
    }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** put() {
        reset();
        return &ptr_;
    }

    T* get() const {
        return ptr_;
    }

    T* operator->() const {
        return ptr_;
    }

    explicit operator bool() const {
        return ptr_ != nullptr;
    }

private:
    void reset() {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    T* ptr_ = nullptr;
};

std::wstring widen(std::string_view text, UINT code_page = CP_UTF8) {
    if (text.empty()) {
        return {};
    }

    const auto flags = code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    const int needed = MultiByteToWideChar(code_page, flags, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0 && code_page == CP_UTF8) {
        return widen(text, CP_ACP);
    }
    if (needed <= 0) {
        return L"(text conversion failed)";
    }

    std::wstring output(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(code_page, flags, text.data(), static_cast<int>(text.size()),
                        output.data(), needed);
    return output;
}

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                            text.data(), static_cast<int>(text.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string output(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                        text.data(), static_cast<int>(text.size()),
                        output.data(), needed, nullptr, nullptr);
    return output;
}

void secure_clear(std::wstring& text) {
    if (!text.empty()) {
        SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
        text.clear();
    }
}

std::wstring get_text(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void set_text(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(hwnd, text.c_str());
}

std::wstring quote_count(std::size_t count, const wchar_t* singular, const wchar_t* plural) {
    std::wstringstream stream;
    stream << count << L' ' << (count == 1 ? singular : plural);
    return stream.str();
}

std::wstring format_size(std::uint64_t size) {
    constexpr std::uint64_t kib = 1024;
    constexpr std::uint64_t mib = kib * 1024;
    constexpr std::uint64_t gib = mib * 1024;

    std::wstringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(1);
    if (size >= gib) {
        stream << static_cast<double>(size) / static_cast<double>(gib) << L" GB";
    } else if (size >= mib) {
        stream << static_cast<double>(size) / static_cast<double>(mib) << L" MB";
    } else if (size >= kib) {
        stream << static_cast<double>(size) / static_cast<double>(kib) << L" KB";
    } else {
        stream << size << L" B";
    }
    return stream.str();
}

std::wstring format_crc32(const std::optional<std::uint32_t>& crc) {
    if (!crc) return {};
    wchar_t text[9]{};
    swprintf_s(text, L"%08X", *crc);
    return text;
}

struct ShellIconRef {
    HIMAGELIST image_list = nullptr;
    int index = -1;
};

ShellIconRef shell_icon_for_item(const axiom::gui::BrowserItem& item) {
    SHFILEINFOW info{};
    DWORD attributes = item.kind == axiom::gui::BrowserItemKind::drive ||
                               item.kind == axiom::gui::BrowserItemKind::directory ||
                               item.kind == axiom::gui::BrowserItemKind::parent
        ? FILE_ATTRIBUTE_DIRECTORY
        : FILE_ATTRIBUTE_NORMAL;
    std::wstring query = item.filesystem_path.empty()
        ? (item.kind == axiom::gui::BrowserItemKind::directory || item.is_parent()
               ? L"folder"
               : item.name)
        : item.filesystem_path.wstring();
    UINT flags = SHGFI_SYSICONINDEX | SHGFI_SMALLICON;
    if (item.kind != axiom::gui::BrowserItemKind::drive) flags |= SHGFI_USEFILEATTRIBUTES;
    const auto image_list = reinterpret_cast<HIMAGELIST>(
        SHGetFileInfoW(query.c_str(), attributes, &info, sizeof(info), flags));
    return {image_list, image_list != nullptr ? info.iIcon : -1};
}

std::optional<fs::path> shell_item_path(IShellItem* item) {
    PWSTR raw_path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) || raw_path == nullptr) {
        return std::nullopt;
    }
    fs::path path(raw_path);
    CoTaskMemFree(raw_path);
    return path;
}

void set_axar_filter(IFileDialog* dialog) {
    COMDLG_FILTERSPEC filters[] = {
        {L"Axiom archives (*.axar)", L"*.axar"},
        {L"All files (*.*)", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(sizeof(filters) / sizeof(filters[0])), filters);
    dialog->SetDefaultExtension(L"axar");
}

std::vector<fs::path> pick_files(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        return {};
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);

    if (FAILED(dialog->Show(owner))) {
        return {};
    }

    ComPtr<IShellItemArray> items;
    if (FAILED(dialog->GetResults(items.put()))) {
        return {};
    }

    DWORD count = 0;
    items->GetCount(&count);
    std::vector<fs::path> paths;
    paths.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(items->GetItemAt(i, item.put()))) {
            if (auto path = shell_item_path(item.get())) {
                paths.push_back(std::move(*path));
            }
        }
    }
    return paths;
}

std::optional<fs::path> pick_open_archive(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        return std::nullopt;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    set_axar_filter(dialog.get());

    if (FAILED(dialog->Show(owner))) {
        return std::nullopt;
    }

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) {
        return std::nullopt;
    }
    return shell_item_path(item.get());
}

void apply_level(axiom::CompressionOptions& options, int level) {
    level = std::clamp(level, 1, 9);
    options.use_tree_matcher = false;
    options.use_fast_lz = false;
    options.enable_optimal_parser = false;
    options.auto_block_size_for_threads = true;

    switch (level) {
        case 1:
            options.max_chain_depth = 8;
            options.nice_length = 64;
            options.lazy_matching = false;
            options.fast_entropy = true;
            options.use_fast_lz = true;
            break;
        case 2:
            options.max_chain_depth = 16;
            options.nice_length = 64;
            options.lazy_matching = true;
            options.fast_entropy = true;
            break;
        case 3:
            options.max_chain_depth = 32;
            options.nice_length = 128;
            options.lazy_matching = true;
            options.fast_entropy = true;
            break;
        case 4:
            options.max_chain_depth = 64;
            options.nice_length = 128;
            options.lazy_matching = true;
            options.fast_entropy = false;
            break;
        case 5:
            options.max_chain_depth = 128;
            options.nice_length = 128;
            options.lazy_matching = true;
            options.fast_entropy = false;
            break;
        case 6:
            options.max_chain_depth = 256;
            options.nice_length = 192;
            options.lazy_matching = true;
            options.fast_entropy = false;
            break;
        case 7:
            options.use_tree_matcher = true;
            options.max_chain_depth = 128;
            options.block_size = 8u << 20;
            options.window_size = 8u << 20;
            options.fast_entropy = false;
            options.auto_block_size_for_threads = false;
            break;
        case 8:
            options.use_tree_matcher = true;
            options.max_chain_depth = 256;
            options.block_size = 32u << 20;
            options.window_size = 32u << 20;
            options.fast_entropy = false;
            options.auto_block_size_for_threads = false;
            break;
        case 9:
            options.use_tree_matcher = true;
            options.max_chain_depth = 512;
            options.block_size = 512u << 20;
            options.window_size = 512u << 20;
            options.fast_entropy = false;
            options.auto_block_size_for_threads = false;
            break;
    }
}

struct ThemePalette {
    bool dark = false;
    COLORREF window = GetSysColor(COLOR_WINDOW);
    COLORREF panel = GetSysColor(COLOR_BTNFACE);
    COLORREF edit = GetSysColor(COLOR_WINDOW);
    COLORREF text = GetSysColor(COLOR_WINDOWTEXT);
    COLORREF muted_text = GetSysColor(COLOR_GRAYTEXT);
    COLORREF border = GetSysColor(COLOR_ACTIVEBORDER);
    COLORREF button = GetSysColor(COLOR_BTNFACE);
    COLORREF button_hot = GetSysColor(COLOR_BTNHIGHLIGHT);
    COLORREF button_pressed = GetSysColor(COLOR_BTNSHADOW);
    COLORREF selection = GetSysColor(COLOR_HIGHLIGHT);
    COLORREF selection_text = GetSysColor(COLOR_HIGHLIGHTTEXT);
    COLORREF focus = GetSysColor(COLOR_HOTLIGHT);
    COLORREF scrollbar_track = GetSysColor(COLOR_BTNFACE);
    COLORREF scrollbar_thumb = GetSysColor(COLOR_BTNSHADOW);
    COLORREF scrollbar_thumb_pressed = GetSysColor(COLOR_3DDKSHADOW);
};

bool high_contrast_enabled() {
    HIGHCONTRASTW high_contrast{};
    high_contrast.cbSize = sizeof(high_contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(high_contrast),
                                 &high_contrast, 0) &&
           (high_contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool system_prefers_dark_mode() {
    if (high_contrast_enabled()) {
        return false;
    }

    DWORD apps_use_light_theme = 1;
    DWORD size = sizeof(apps_use_light_theme);
    const auto result = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &apps_use_light_theme,
        &size);
    return result == ERROR_SUCCESS && apps_use_light_theme == 0;
}

ThemePalette make_theme() {
    ThemePalette theme;
    theme.dark = system_prefers_dark_mode();
    if (theme.dark) {
        theme.window = RGB(32, 32, 32);
        theme.panel = RGB(45, 45, 48);
        theme.edit = RGB(37, 37, 38);
        theme.text = RGB(241, 241, 241);
        theme.muted_text = RGB(180, 180, 180);
        theme.border = RGB(64, 64, 64);
        theme.button = RGB(45, 45, 48);
        theme.button_hot = RGB(58, 58, 61);
        theme.button_pressed = RGB(72, 72, 76);
        theme.selection = RGB(55, 78, 112);
        theme.selection_text = RGB(255, 255, 255);
        theme.focus = RGB(78, 115, 158);
        theme.scrollbar_track = RGB(45, 45, 48);
        theme.scrollbar_thumb = RGB(92, 92, 96);
        theme.scrollbar_thumb_pressed = RGB(122, 122, 126);
    }
    return theme;
}

axiom::gui::OperationWindowTheme make_operation_window_theme(const ThemePalette& theme) {
    return {
        theme.dark,
        theme.window,
        theme.panel,
        theme.text,
        theme.muted_text,
        theme.border,
        theme.button,
        theme.button_hot,
        theme.button_pressed,
        theme.edit,
        theme.selection,
    };
}

void set_dark_title_bar(HWND hwnd, bool dark) {
    BOOL value = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                     &value, sizeof(value)))) {
        constexpr DWORD kOlderDarkModeAttribute = 19;
        (void)DwmSetWindowAttribute(hwnd, kOlderDarkModeAttribute, &value, sizeof(value));
    }
}

bool is_button_id(UINT id) {
    switch (id) {
        case kAddFiles:
        case kOpenArchive:
        case kExtract:
        case kTest:
        case kNavigateBack:
        case kNavigateForward:
        case kNavigateUp:
        case kNavigateRefresh:
        case kAddressGo:
        case kView:
        case kDelete:
        case kInfo:
        case kSettings:
            return true;
        default:
            return false;
    }
}

axiom::gui::ToolbarIcon toolbar_icon_for_button(UINT id) {
    using axiom::gui::ToolbarIcon;
    switch (id) {
        case kAddFiles: return ToolbarIcon::archive;
        case kOpenArchive: return ToolbarIcon::open;
        case kExtract: return ToolbarIcon::extract;
        case kTest: return ToolbarIcon::test;
        case kNavigateBack: return ToolbarIcon::back;
        case kNavigateForward: return ToolbarIcon::forward;
        case kNavigateUp: return ToolbarIcon::up;
        case kNavigateRefresh: return ToolbarIcon::refresh;
        case kAddressGo: return ToolbarIcon::forward;
        case kView: return ToolbarIcon::view;
        case kDelete: return ToolbarIcon::delete_item;
        case kInfo: return ToolbarIcon::info;
        case kSettings: return ToolbarIcon::settings;
        default: return ToolbarIcon::none;
    }
}

bool is_icon_only_button(UINT id) {
    return id == kNavigateBack || id == kNavigateForward ||
           id == kNavigateUp || id == kNavigateRefresh;
}

int scale_for_dpi(UINT dpi, int value) {
    return MulDiv(value, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi),
                  USER_DEFAULT_SCREEN_DPI);
}

void fill_solid_rect(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void frame_solid_rect(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}

void draw_solid_line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

struct TableColumn {
    std::wstring title;
    int logical_width = 0;
};

// Native report/list controls still leak light header and scrollbar pixels on some
// Windows builds, so Axiom owns every table pixel through this lightweight view.
class DarkTableView {
public:
    bool create(HWND parent, HINSTANCE instance, int id) {
        if (!register_class(instance)) {
            return false;
        }

        hwnd_ = CreateWindowExW(0, class_name(), L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                0, 0, 0, 0,
                                parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                instance,
                                this);
        return hwnd_ != nullptr;
    }

    HWND hwnd() const {
        return hwnd_;
    }

    void set_font(HFONT font) {
        font_ = font;
        invalidate();
    }

    void set_dpi(UINT dpi) {
        dpi_ = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
        clamp_scroll();
        invalidate();
    }

    void set_theme(const ThemePalette& theme) {
        theme_ = theme;
        invalidate();
    }

    void set_columns(std::vector<TableColumn> columns) {
        columns_ = std::move(columns);
        clamp_scroll();
        invalidate();
    }

    void set_image_list(HIMAGELIST image_list) {
        image_list_ = image_list;
        invalidate();
    }

    void set_sort_indicator(int column, bool ascending) {
        sort_column_ = column;
        sort_ascending_ = ascending;
        invalidate();
    }

    void clear() {
        rows_.clear();
        icon_indices_.clear();
        selected_.clear();
        selected_row_ = -1;
        selection_anchor_ = -1;
        scroll_y_ = 0;
        scroll_x_ = 0;
        invalidate();
    }

    void append_row(std::vector<std::wstring> row, int icon_index = -1) {
        rows_.push_back(std::move(row));
        icon_indices_.push_back(icon_index);
        selected_.push_back(false);
        clamp_scroll();
        invalidate();
    }

    std::vector<int> selected_indices() const {
        std::vector<int> result;
        for (int index = 0; index < static_cast<int>(selected_.size()); ++index) {
            if (selected_[index]) result.push_back(index);
        }
        return result;
    }

    int focused_index() const {
        return selected_row_;
    }

    void select_all() {
        std::fill(selected_.begin(), selected_.end(), true);
        selected_row_ = selected_.empty() ? -1 : 0;
        selection_anchor_ = selected_row_;
        notify_parent(kTableSelectionChangedMessage);
        invalidate();
    }

private:
    static const wchar_t* class_name() {
        return L"AxiomDarkTableView";
    }

    static bool register_class(HINSTANCE instance) {
        static ATOM atom = 0;
        if (atom != 0) {
            return true;
        }

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.hInstance = instance;
        wc.lpfnWndProc = &DarkTableView::window_proc;
        wc.lpszClassName = class_name();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        atom = RegisterClassExW(&wc);
        return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    int scale(int value) const {
        return scale_for_dpi(dpi_, value);
    }

    int header_height() const {
        return scale(28);
    }

    int row_height() const {
        return scale(24);
    }

    int scrollbar_width() const {
        return scale(8);
    }

    int scrollbar_gap() const {
        return scale(2);
    }

    int content_height() const {
        return row_height() * static_cast<int>(rows_.size());
    }

    int requested_content_width() const {
        int width = 0;
        for (const auto& column : columns_) {
            width += scale(column.logical_width);
        }
        return width;
    }

    struct ScrollbarVisibility {
        bool vertical = false;
        bool horizontal = false;
    };

    ScrollbarVisibility scrollbar_visibility(const RECT& client) const {
        ScrollbarVisibility visibility;
        const int base_width = std::max(
            0, static_cast<int>(client.right - client.left) - scale(2));
        const int base_height = std::max(
            0, static_cast<int>(client.bottom - client.top) - header_height() - scale(1));

        // Each scrollbar consumes space that can make the other one necessary.
        for (int pass = 0; pass < 3; ++pass) {
            const int width = std::max(
                0, base_width - (visibility.vertical
                                      ? scrollbar_width() + scrollbar_gap()
                                      : 0));
            if (requested_content_width() > width) {
                visibility.horizontal = true;
            }

            const int height = std::max(
                0, base_height - (visibility.horizontal
                                       ? scrollbar_width() + scrollbar_gap()
                                       : 0));
            if (content_height() > height) {
                visibility.vertical = true;
            }
        }
        return visibility;
    }

    int rows_bottom(const RECT& client) const {
        const auto visibility = scrollbar_visibility(client);
        int bottom = client.bottom - scale(1);
        if (visibility.horizontal) {
            bottom -= scrollbar_width() + scrollbar_gap();
        }
        return std::max(static_cast<int>(client.top) + header_height(), bottom);
    }

    int viewport_height(const RECT& client) const {
        return std::max(0, rows_bottom(client) -
                               (static_cast<int>(client.top) + header_height()));
    }

    int viewport_height() const {
        RECT client{};
        GetClientRect(hwnd_, &client);
        return viewport_height(client);
    }

    int max_scroll() const {
        return std::max(0, content_height() - viewport_height());
    }

    void clamp_scroll() {
        scroll_y_ = std::clamp(scroll_y_, 0, max_scroll());
        scroll_x_ = std::clamp(scroll_x_, 0, max_scroll_x());
    }

    void invalidate() const {
        if (hwnd_ != nullptr) {
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
    }

    std::vector<int> column_widths(int available_width) const {
        std::vector<int> widths;
        widths.reserve(columns_.size());
        int used = 0;
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            const int requested = scale(columns_[i].logical_width);
            if (i + 1 == columns_.size()) {
                widths.push_back(std::max(requested, available_width - used));
            } else {
                widths.push_back(requested);
                used += requested;
            }
        }
        return widths;
    }

    int table_left(const RECT& client) const {
        return client.left + scale(1);
    }

    int table_right(const RECT& client) const {
        int right = client.right - scale(1);
        if (scrollbar_visibility(client).vertical) {
            right -= scrollbar_width() + scrollbar_gap();
        }
        return std::max(table_left(client), right);
    }

    std::vector<int> column_widths(const RECT& client) const {
        return column_widths(std::max(0, table_right(client) - table_left(client)));
    }

    int max_scroll_x(const RECT& client) const {
        const auto widths = column_widths(client);
        int content_width = 0;
        for (const int width : widths) {
            content_width += width;
        }
        return std::max(0, content_width - (table_right(client) - table_left(client)));
    }

    int max_scroll_x() const {
        RECT client{};
        GetClientRect(hwnd_, &client);
        return max_scroll_x(client);
    }

    int column_separator_at(POINT point, const RECT& client) const {
        if (point.y < client.top + scale(1) ||
            point.y >= client.top + header_height()) {
            return -1;
        }

        const int right = table_right(client);
        const auto widths = column_widths(client);
        int x = table_left(client) - scroll_x_;
        for (int column = 0; column < static_cast<int>(widths.size()); ++column) {
            const int next_x = x + widths[static_cast<std::size_t>(column)];
            if (next_x >= table_left(client) && next_x < right &&
                std::abs(point.x - next_x) <= scale(5)) {
                return column;
            }
            x = next_x;
            if (x >= right) {
                break;
            }
        }
        return -1;
    }

    RECT scrollbar_track_rect(const RECT& client) const {
        return RECT{client.right - scrollbar_width() - scale(1),
                    client.top + header_height() + scale(1),
                    client.right - scale(1),
                    rows_bottom(client)};
    }

    RECT scrollbar_thumb_rect(const RECT& client) const {
        RECT track = scrollbar_track_rect(client);
        const int track_height = std::max(0, static_cast<int>(track.bottom - track.top));
        const int content = content_height();
        const int viewport = viewport_height();
        if (content <= viewport || track_height <= 0) {
            return RECT{track.left, track.top, track.right, track.top};
        }

        const int thumb_height = std::clamp(
            MulDiv(viewport, track_height, content), scale(24), track_height);
        const int travel = std::max(0, track_height - thumb_height);
        const int top = track.top + (max_scroll() == 0 ? 0 : MulDiv(scroll_y_, travel, max_scroll()));
        return RECT{track.left, top, track.right, top + thumb_height};
    }

    RECT horizontal_scrollbar_track_rect(const RECT& client) const {
        return RECT{table_left(client),
                    rows_bottom(client) + scrollbar_gap(),
                    table_right(client),
                    client.bottom - scale(1)};
    }

    RECT horizontal_scrollbar_thumb_rect(const RECT& client) const {
        const RECT track = horizontal_scrollbar_track_rect(client);
        const int track_width = std::max(0, static_cast<int>(track.right - track.left));
        const int viewport = std::max(0, table_right(client) - table_left(client));
        const int maximum = max_scroll_x(client);
        const int content = viewport + maximum;
        if (maximum <= 0 || track_width <= 0 || content <= 0) {
            return RECT{track.left, track.top, track.left, track.bottom};
        }

        const int thumb_width = std::clamp(
            MulDiv(viewport, track_width, content), scale(24), track_width);
        const int travel = std::max(0, track_width - thumb_width);
        const int left = track.left + MulDiv(scroll_x_, travel, maximum);
        return RECT{left, track.top, left + thumb_width, track.bottom};
    }

    bool point_in_rect(POINT point, const RECT& rect) const {
        return point.x >= rect.left && point.x < rect.right &&
               point.y >= rect.top && point.y < rect.bottom;
    }

    void set_scroll_from_thumb(POINT point, const RECT& client) {
        RECT track = scrollbar_track_rect(client);
        RECT thumb = scrollbar_thumb_rect(client);
        const int thumb_height = thumb.bottom - thumb.top;
        const int travel = std::max(1, static_cast<int>(track.bottom - track.top) - thumb_height);
        const int thumb_top = std::clamp(static_cast<int>(point.y) - drag_offset_y_,
                                         static_cast<int>(track.top),
                                         static_cast<int>(track.bottom) - thumb_height);
        scroll_y_ = MulDiv(thumb_top - track.top, max_scroll(), travel);
        clamp_scroll();
        invalidate();
    }

    void set_horizontal_scroll_from_thumb(POINT point, const RECT& client) {
        const RECT track = horizontal_scrollbar_track_rect(client);
        const RECT thumb = horizontal_scrollbar_thumb_rect(client);
        const int thumb_width = thumb.right - thumb.left;
        const int travel = std::max(1, static_cast<int>(track.right - track.left) - thumb_width);
        const int thumb_left = std::clamp(static_cast<int>(point.x) - drag_offset_x_,
                                          static_cast<int>(track.left),
                                          static_cast<int>(track.right) - thumb_width);
        scroll_x_ = MulDiv(thumb_left - track.left, max_scroll_x(client), travel);
        clamp_scroll();
        invalidate();
    }

    void scroll_by(int pixels) {
        scroll_y_ += pixels;
        clamp_scroll();
        invalidate();
    }

    void scroll_horizontally_by(int pixels) {
        scroll_x_ += pixels;
        clamp_scroll();
        invalidate();
    }

    void notify_parent(UINT message) const {
        SendMessageW(GetParent(hwnd_), message,
                     static_cast<WPARAM>(std::max(selected_row_, 0)), 0);
    }

    void select_row(int row, bool extend, bool toggle) {
        if (row < 0 || row >= static_cast<int>(rows_.size())) {
            if (!extend && !toggle) std::fill(selected_.begin(), selected_.end(), false);
            selected_row_ = -1;
            selection_anchor_ = -1;
        } else if (extend && selection_anchor_ >= 0) {
            std::fill(selected_.begin(), selected_.end(), false);
            const int first = std::min(selection_anchor_, row);
            const int last = std::max(selection_anchor_, row);
            for (int index = first; index <= last; ++index) selected_[index] = true;
            selected_row_ = row;
        } else if (toggle) {
            selected_[row] = !selected_[row];
            selected_row_ = row;
            selection_anchor_ = row;
        } else {
            std::fill(selected_.begin(), selected_.end(), false);
            selected_[row] = true;
            selected_row_ = row;
            selection_anchor_ = row;
        }
        ensure_selected_visible();
        notify_parent(kTableSelectionChangedMessage);
        invalidate();
    }

    void ensure_selected_visible() {
        if (selected_row_ < 0) {
            return;
        }
        const int row_top = selected_row_ * row_height();
        const int row_bottom = row_top + row_height();
        if (row_top < scroll_y_) {
            scroll_y_ = row_top;
        } else if (row_bottom > scroll_y_ + viewport_height()) {
            scroll_y_ = row_bottom - viewport_height();
        }
        clamp_scroll();
    }

    void paint_content(HDC dc, const RECT& client) {
        fill_solid_rect(dc, client, theme_.edit);
        frame_solid_rect(dc, client, theme_.border);

        HFONT font = font_ != nullptr ? font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);

        const auto visibility = scrollbar_visibility(client);
        const int content_left = table_left(client);
        const int content_right = table_right(client);
        RECT header{content_left, client.top + scale(1),
                    client.right - scale(1), client.top + header_height()};
        fill_solid_rect(dc, header, theme_.panel);

        const auto widths = column_widths(client);
        const int saved_header_dc = SaveDC(dc);
        IntersectClipRect(dc, content_left, header.top, content_right, header.bottom);
        int x = content_left - scroll_x_;
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            const int next_x = x + widths[i];
            if (next_x > content_left && x < content_right) {
                RECT cell{x + scale(7), header.top, next_x - scale(7), header.bottom};
                SetTextColor(dc, theme_.text);
                std::wstring title = columns_[i].title;
                if (static_cast<int>(i) == sort_column_) {
                    title += sort_ascending_ ? L"  ^" : L"  v";
                }
                DrawTextW(dc, title.c_str(), -1, &cell,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                draw_solid_line(dc, next_x, header.top, next_x, header.bottom, theme_.border);
            }
            x = next_x;
            if (x >= content_right) {
                break;
            }
        }
        RestoreDC(dc, saved_header_dc);
        draw_solid_line(dc, content_left, header.bottom - scale(1),
                        content_right, header.bottom - scale(1), theme_.border);

        RECT rows_clip{content_left, header.bottom, content_right, rows_bottom(client)};
        const int saved_rows_dc = SaveDC(dc);
        IntersectClipRect(dc, rows_clip.left, rows_clip.top, rows_clip.right, rows_clip.bottom);

        const int row_h = row_height();
        const int first_row = row_h > 0 ? scroll_y_ / row_h : 0;
        int y = rows_clip.top - (row_h > 0 ? scroll_y_ % row_h : 0);
        for (int row = first_row; row < static_cast<int>(rows_.size()) && y < rows_clip.bottom; ++row) {
            RECT row_rect{rows_clip.left, y, rows_clip.right, y + row_h};
            const bool selected = row < static_cast<int>(selected_.size()) && selected_[row];
            fill_solid_rect(dc, row_rect, selected ? theme_.selection : theme_.edit);

            x = content_left - scroll_x_;
            for (std::size_t col = 0; col < columns_.size(); ++col) {
                const int next_x = x + widths[col];
                if (next_x > rows_clip.left && x < rows_clip.right) {
                    RECT cell{x + scale(7), row_rect.top, next_x - scale(7), row_rect.bottom};
                    if (col == 0 && image_list_ != nullptr &&
                        row < static_cast<int>(icon_indices_.size()) && icon_indices_[row] >= 0) {
                        int icon_width = 0;
                        int icon_height = 0;
                        ImageList_GetIconSize(image_list_, &icon_width, &icon_height);
                        const int icon_y = row_rect.top + std::max(0, (row_h - icon_height) / 2);
                        ImageList_Draw(image_list_, icon_indices_[row], dc, cell.left, icon_y,
                                       ILD_TRANSPARENT);
                        cell.left += icon_width + scale(5);
                    }
                    const std::wstring empty;
                    const std::wstring& text =
                        col < rows_[row].size() ? rows_[row][col] : empty;
                    SetTextColor(dc, selected ? theme_.selection_text : theme_.text);
                    DrawTextW(dc, text.c_str(), -1, &cell,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                                  DT_NOPREFIX);
                }
                x = next_x;
                if (x >= rows_clip.right) {
                    break;
                }
            }
            draw_solid_line(dc, rows_clip.left, row_rect.bottom - scale(1),
                            rows_clip.right, row_rect.bottom - scale(1), theme_.panel);
            y += row_h;
        }

        // Draw grid lines last so they remain continuous across selection fills and
        // the empty area below the final row.
        x = content_left - scroll_x_;
        for (const int width : widths) {
            const int next_x = x + width;
            if (next_x > rows_clip.left && next_x <= rows_clip.right) {
                draw_solid_line(dc, next_x, rows_clip.top, next_x, rows_clip.bottom,
                                theme_.border);
            }
            x = next_x;
            if (x >= rows_clip.right) {
                break;
            }
        }
        RestoreDC(dc, saved_rows_dc);

        if (visibility.vertical) {
            const RECT track = scrollbar_track_rect(client);
            const RECT thumb = scrollbar_thumb_rect(client);
            fill_solid_rect(dc, track, theme_.scrollbar_track);
            fill_solid_rect(dc, thumb, dragging_scrollbar_
                                           ? theme_.scrollbar_thumb_pressed
                                           : theme_.scrollbar_thumb);
        }
        if (visibility.horizontal) {
            const RECT track = horizontal_scrollbar_track_rect(client);
            const RECT thumb = horizontal_scrollbar_thumb_rect(client);
            fill_solid_rect(dc, track, theme_.scrollbar_track);
            fill_solid_rect(dc, thumb, dragging_horizontal_scrollbar_
                                           ? theme_.scrollbar_thumb_pressed
                                           : theme_.scrollbar_thumb);
        }
        if (visibility.vertical && visibility.horizontal) {
            RECT corner{table_right(client) + scrollbar_gap(),
                        rows_bottom(client) + scrollbar_gap(),
                        client.right - scale(1), client.bottom - scale(1)};
            fill_solid_rect(dc, corner, theme_.scrollbar_track);
        }

        SelectObject(dc, old_font);
    }

    void on_paint() {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd_, &paint);
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width > 0 && height > 0) {
            HDC memory_dc = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
            HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
            RECT buffer{0, 0, width, height};
            paint_content(memory_dc, buffer);
            BitBlt(dc, 0, 0, width, height, memory_dc, 0, 0, SRCCOPY);
            SelectObject(memory_dc, old_bitmap);
            DeleteObject(bitmap);
            DeleteDC(memory_dc);
        }
        EndPaint(hwnd_, &paint);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
                on_paint();
                return 0;
            case WM_SETFONT:
                font_ = reinterpret_cast<HFONT>(wparam);
                invalidate();
                return 0;
            case WM_SIZE:
                clamp_scroll();
                invalidate();
                return 0;
            case WM_MOUSEWHEEL:
                if ((GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) != 0) {
                    scroll_horizontally_by(
                        -GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * scale(96));
                } else {
                    scroll_by(-GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * row_height() * 3);
                }
                return 0;
            case WM_MOUSEHWHEEL:
                scroll_horizontally_by(
                    GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * scale(96));
                return 0;
            case WM_LBUTTONDOWN: {
                SetFocus(hwnd_);
                RECT client{};
                GetClientRect(hwnd_, &client);
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                if (point.y < client.top + header_height() &&
                    point.x >= table_left(client) && point.x < table_right(client)) {
                    const int separator = column_separator_at(point, client);
                    if (separator >= 0) {
                        resizing_column_ = separator;
                        resize_start_x_ = point.x;
                        resize_start_width_ =
                            columns_[static_cast<std::size_t>(separator)].logical_width;
                        SetCapture(hwnd_);
                        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                        return 0;
                    }

                    const auto widths = column_widths(client);
                    int boundary = table_left(client) - scroll_x_;
                    for (int column = 0; column < static_cast<int>(widths.size()); ++column) {
                        const int left = boundary;
                        boundary += widths[column];
                        if (point.x >= left && point.x < boundary) {
                            SendMessageW(GetParent(hwnd_), kTableSortMessage,
                                         static_cast<WPARAM>(column), 0);
                            break;
                        }
                    }
                    return 0;
                }
                const auto visibility = scrollbar_visibility(client);
                if (visibility.vertical) {
                    const RECT thumb = scrollbar_thumb_rect(client);
                    const RECT track = scrollbar_track_rect(client);
                    if (point_in_rect(point, thumb)) {
                        dragging_scrollbar_ = true;
                        drag_offset_y_ = point.y - thumb.top;
                        SetCapture(hwnd_);
                        return 0;
                    }
                    if (point_in_rect(point, track)) {
                        drag_offset_y_ = (thumb.bottom - thumb.top) / 2;
                        set_scroll_from_thumb(point, client);
                        return 0;
                    }
                }
                if (visibility.horizontal) {
                    const RECT thumb = horizontal_scrollbar_thumb_rect(client);
                    const RECT track = horizontal_scrollbar_track_rect(client);
                    if (point_in_rect(point, thumb)) {
                        dragging_horizontal_scrollbar_ = true;
                        drag_offset_x_ = point.x - thumb.left;
                        SetCapture(hwnd_);
                        return 0;
                    }
                    if (point_in_rect(point, track)) {
                        drag_offset_x_ = (thumb.right - thumb.left) / 2;
                        set_horizontal_scroll_from_thumb(point, client);
                        return 0;
                    }
                }
                if (point.x < table_left(client) || point.x >= table_right(client) ||
                    point.y >= rows_bottom(client)) {
                    return 0;
                }
                const int y = point.y - header_height() + scroll_y_;
                const int row = row_height() > 0 ? y / row_height() : -1;
                select_row(row >= 0 && row < static_cast<int>(rows_.size()) ? row : -1,
                           (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                           (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                return 0;
            }
            case WM_LBUTTONDBLCLK: {
                RECT client{};
                GetClientRect(hwnd_, &client);
                const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                if (point.y < header_height() || point.y >= rows_bottom(client) ||
                    point.x < table_left(client) || point.x >= table_right(client)) {
                    return 0;
                }
                notify_parent(kTableActivateMessage);
                return 0;
            }
            case WM_RBUTTONDOWN: {
                SetFocus(hwnd_);
                RECT client{};
                GetClientRect(hwnd_, &client);
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                if (point.y < header_height() || point.y >= rows_bottom(client) ||
                    point.x < table_left(client) || point.x >= table_right(client)) {
                    return 0;
                }
                const int y = point.y - header_height() + scroll_y_;
                const int row = row_height() > 0 ? y / row_height() : -1;
                if (row >= 0 && row < static_cast<int>(rows_.size()) &&
                    !selected_[static_cast<std::size_t>(row)]) {
                    select_row(row, false, false);
                }
                break;
            }
            case WM_CONTEXTMENU:
                // Custom child windows do not get the standard list-view
                // context-menu forwarding, so preserve the originating HWND
                // and screen point explicitly for the application shell.
                SendMessageW(GetParent(hwnd_), WM_CONTEXTMENU,
                             reinterpret_cast<WPARAM>(hwnd_), lparam);
                return 0;
            case WM_MOUSEMOVE:
                if (resizing_column_ >= 0) {
                    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                    const int requested = scale(resize_start_width_) +
                                          point.x - resize_start_x_;
                    const int width = std::max(requested, scale(48));
                    columns_[static_cast<std::size_t>(resizing_column_)].logical_width =
                        std::max(48, MulDiv(width, USER_DEFAULT_SCREEN_DPI,
                                           static_cast<int>(dpi_)));
                    clamp_scroll();
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    invalidate();
                    return 0;
                }
                if (dragging_scrollbar_) {
                    RECT client{};
                    GetClientRect(hwnd_, &client);
                    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                    set_scroll_from_thumb(point, client);
                    return 0;
                }
                if (dragging_horizontal_scrollbar_) {
                    RECT client{};
                    GetClientRect(hwnd_, &client);
                    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                    set_horizontal_scroll_from_thumb(point, client);
                    return 0;
                }
                break;
            case WM_SETCURSOR:
                if (LOWORD(lparam) == HTCLIENT) {
                    POINT point{};
                    GetCursorPos(&point);
                    ScreenToClient(hwnd_, &point);
                    RECT client{};
                    GetClientRect(hwnd_, &client);
                    if (resizing_column_ >= 0 || column_separator_at(point, client) >= 0) {
                        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                        return TRUE;
                    }
                }
                break;
            case WM_LBUTTONUP:
                if (resizing_column_ >= 0) {
                    resizing_column_ = -1;
                    ReleaseCapture();
                    invalidate();
                    return 0;
                }
                if (dragging_scrollbar_) {
                    dragging_scrollbar_ = false;
                    ReleaseCapture();
                    invalidate();
                    return 0;
                }
                if (dragging_horizontal_scrollbar_) {
                    dragging_horizontal_scrollbar_ = false;
                    ReleaseCapture();
                    invalidate();
                    return 0;
                }
                break;
            case WM_KEYDOWN:
                if (!rows_.empty()) {
                    if (wparam == VK_DOWN) {
                        select_row(std::min(static_cast<int>(rows_.size()) - 1, selected_row_ + 1),
                                   (GetKeyState(VK_SHIFT) & 0x8000) != 0, false);
                        return 0;
                    }
                    if (wparam == VK_UP) {
                        select_row(std::max(0, selected_row_ < 0 ? 0 : selected_row_ - 1),
                                   (GetKeyState(VK_SHIFT) & 0x8000) != 0, false);
                        return 0;
                    }
                    if (wparam == VK_RETURN) {
                        notify_parent(kTableActivateMessage);
                        return 0;
                    }
                    if (wparam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                        std::fill(selected_.begin(), selected_.end(), true);
                        selected_row_ = 0;
                        selection_anchor_ = 0;
                        notify_parent(kTableSelectionChangedMessage);
                        invalidate();
                        return 0;
                    }
                }
                if (wparam == VK_BACK) {
                    notify_parent(kTableParentMessage);
                    return 0;
                }
                break;
            case WM_CANCELMODE:
                if (GetCapture() == hwnd_) {
                    ReleaseCapture();
                }
                resizing_column_ = -1;
                dragging_scrollbar_ = false;
                dragging_horizontal_scrollbar_ = false;
                invalidate();
                return 0;
            case WM_CAPTURECHANGED:
                resizing_column_ = -1;
                dragging_scrollbar_ = false;
                dragging_horizontal_scrollbar_ = false;
                invalidate();
                return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        DarkTableView* view = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            view = static_cast<DarkTableView*>(create->lpCreateParams);
            view->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
        } else {
            view = reinterpret_cast<DarkTableView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (view != nullptr) {
            return view->handle_message(message, wparam, lparam);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    ThemePalette theme_ = make_theme();
    HFONT font_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    int scroll_y_ = 0;
    int scroll_x_ = 0;
    int selected_row_ = -1;
    int selection_anchor_ = -1;
    bool dragging_scrollbar_ = false;
    bool dragging_horizontal_scrollbar_ = false;
    int drag_offset_y_ = 0;
    int drag_offset_x_ = 0;
    std::vector<TableColumn> columns_;
    std::vector<std::vector<std::wstring>> rows_;
    std::vector<bool> selected_;
    std::vector<int> icon_indices_;
    HIMAGELIST image_list_ = nullptr;
    int sort_column_ = 0;
    bool sort_ascending_ = true;
    int resizing_column_ = -1;
    int resize_start_x_ = 0;
    int resize_start_width_ = 0;
};

class MainWindow {
public:
    ~MainWindow() {
        reset_theme_brushes();
        reset_font();
    }

    bool create(HINSTANCE instance, int show_command, std::wstring initial_path) {
        instance_ = instance;
        persisted_settings_ = axiom::gui::load_gui_settings();
        application_options_ = persisted_settings_.application;
        selected_level_ = application_options_.default_level;
        selected_thread_count_ = application_options_.default_thread_count;
        sort_column_ = persisted_settings_.sort_column;
        sort_ascending_ = persisted_settings_.sort_ascending;
        initial_path_ = initial_path.empty() ? persisted_settings_.last_location
                                             : std::move(initial_path);
        const UINT system_dpi = GetDpiForSystem();
        const int initial_width = MulDiv(1080, static_cast<int>(system_dpi), USER_DEFAULT_SCREEN_DPI);
        const int initial_height = MulDiv(720, static_cast<int>(system_dpi), USER_DEFAULT_SCREEN_DPI);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance;
        wc.lpfnWndProc = &MainWindow::window_proc;
        wc.lpszClassName = L"AxiomGuiWindow";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        axiom::gui::assign_axiom_window_class_icons(wc, instance);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

        if (RegisterClassExW(&wc) == 0) {
            return false;
        }

        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Axiom", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, initial_width, initial_height,
                                nullptr, nullptr, instance, this);
        if (hwnd_ == nullptr) {
            return false;
        }
        axiom::gui::apply_axiom_window_icons(hwnd_, instance);

        if (persisted_settings_.has_placement) {
            if (persisted_settings_.placement.showCmd == SW_SHOWMINIMIZED) {
                persisted_settings_.placement.showCmd = SW_SHOWNORMAL;
            }
            SetWindowPlacement(hwnd_, &persisted_settings_.placement);
        }

        ShowWindow(hwnd_, persisted_settings_.has_placement
                              ? persisted_settings_.placement.showCmd
                              : show_command);
        UpdateWindow(hwnd_);
        return true;
    }

    bool translate_menu_message(const MSG& message) {
        return menu_bar_.translate_message(message);
    }

private:
    int scale(int value) const {
        return MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
    }

    void reset_font() {
        if (ui_font_ != nullptr) {
            DeleteObject(ui_font_);
            ui_font_ = nullptr;
        }
    }

    void rebuild_font() {
        reset_font();

        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                        &metrics, 0, dpi_)) {
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        }
        ui_font_ = CreateFontIndirectW(&metrics.lfMessageFont);
    }

    void set_control_font(HWND control) const {
        if (control != nullptr && ui_font_ != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
        }
    }

    void apply_fonts() {
        HWND controls[] = {
            add_files_, open_archive_, extract_, test_,
            list_, status_,
            navigate_back_, navigate_forward_, navigate_up_, navigate_refresh_,
            address_edit_, address_go_, view_, delete_, info_, settings_,
            tooltip_,
        };
        for (HWND control : controls) {
            set_control_font(control);
        }
        menu_bar_.set_font(ui_font_);
        table_.set_font(ui_font_);
        for (HWND label : transient_labels_) {
            set_control_font(label);
        }
    }

    void update_dpi(UINT dpi) {
        if (dpi == 0) {
            dpi = USER_DEFAULT_SCREEN_DPI;
        }
        dpi_ = dpi;
        rebuild_font();
        menu_bar_.set_dpi(dpi_);
        table_.set_dpi(dpi_);
        apply_fonts();
        apply_edit_margins();
    }

    void reset_theme_brushes() {
        if (window_brush_ != nullptr) {
            DeleteObject(window_brush_);
            window_brush_ = nullptr;
        }
        if (panel_brush_ != nullptr) {
            DeleteObject(panel_brush_);
            panel_brush_ = nullptr;
        }
        if (edit_brush_ != nullptr) {
            DeleteObject(edit_brush_);
            edit_brush_ = nullptr;
        }
    }

    void rebuild_theme_brushes() {
        reset_theme_brushes();
        window_brush_ = CreateSolidBrush(theme_.window);
        panel_brush_ = CreateSolidBrush(theme_.panel);
        edit_brush_ = CreateSolidBrush(theme_.edit);
    }

    void apply_theme_to_control(HWND control) const {
        if (control == nullptr) {
            return;
        }
        SetWindowTheme(control, theme_.dark ? L"DarkMode_Explorer" : nullptr, nullptr);
        InvalidateRect(control, nullptr, TRUE);
    }

    void apply_theme() {
        theme_ = make_theme();
        rebuild_theme_brushes();
        set_dark_title_bar(hwnd_, theme_.dark);

        HWND controls[] = {
            add_files_, open_archive_, extract_, test_,
            list_, status_,
            navigate_back_, navigate_forward_, navigate_up_, navigate_refresh_,
            address_edit_, address_go_, view_, delete_, info_, settings_,
            tooltip_,
        };
        for (HWND control : controls) {
            apply_theme_to_control(control);
        }
        menu_bar_.set_theme({
            theme_.dark ? RGB(31, 31, 31) : RGB(250, 250, 250),
            theme_.dark ? RGB(49, 49, 49) : RGB(229, 241, 251),
            theme_.dark ? RGB(64, 64, 64) : RGB(204, 228, 247),
            theme_.text,
            theme_.muted_text,
            theme_.dark ? RGB(42, 42, 42) : RGB(210, 210, 210),
            theme_.dark ? RGB(54, 54, 54) : RGB(210, 210, 210),
        });
        table_.set_theme(theme_);
        operation_window_.set_theme(make_operation_window_theme(theme_));

        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    LRESULT paint_control_background(HWND control, HDC dc, UINT message) {
        SetTextColor(dc, theme_.text);

        if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
            SetBkColor(dc, theme_.edit);
            return reinterpret_cast<LRESULT>(edit_brush_);
        }

        if (message == WM_CTLCOLORBTN) {
            SetBkColor(dc, theme_.panel);
            return reinterpret_cast<LRESULT>(panel_brush_);
        }

        if (control == status_) {
            SetBkColor(dc, theme_.window);
            return reinterpret_cast<LRESULT>(window_brush_);
        }

        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(window_brush_);
    }

    void fill_rect(HDC dc, const RECT& rect, COLORREF color) const {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    }

    void frame_rect(HDC dc, const RECT& rect, COLORREF color) const {
        HBRUSH brush = CreateSolidBrush(color);
        FrameRect(dc, &rect, brush);
        DeleteObject(brush);
    }

    void draw_owner_button(const DRAWITEMSTRUCT& draw) const {
        if (!is_button_id(draw.CtlID)) {
            return;
        }

        const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
        const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
        const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;
        const bool focused = (draw.itemState & ODS_FOCUS) != 0;
        RECT rect = draw.rcItem;
        const COLORREF button_color = pressed ? theme_.button_pressed :
            (hot ? theme_.button_hot : theme_.button);
        fill_rect(draw.hDC, rect, button_color);

        HFONT font = ui_font_ != nullptr ? ui_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(draw.hDC, font);
        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, disabled ? theme_.muted_text : theme_.text);

        // Buttons communicate focus through a restrained border; a blue focus
        // ring is visually too loud in the compact dark toolbar.
        const COLORREF border = (focused || hot || pressed) ? theme_.button_hot : theme_.border;
        frame_rect(draw.hDC, rect, border);

        std::wstring text = get_text(draw.hwndItem);
        if (pressed) {
            OffsetRect(&rect, scale(1), scale(1));
        }

        const auto icon = toolbar_icon_for_button(draw.CtlID);
        const COLORREF content_color = disabled ? theme_.muted_text : theme_.text;
        if (icon == axiom::gui::ToolbarIcon::none) {
            DrawTextW(draw.hDC, text.c_str(), -1, &rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else if (is_icon_only_button(draw.CtlID)) {
            axiom::gui::draw_toolbar_icon(draw.hDC, icon, rect, content_color, dpi_);
        } else {
            SIZE text_size{};
            GetTextExtentPoint32W(draw.hDC, text.c_str(), static_cast<int>(text.size()), &text_size);
            const int icon_size = scale(18);
            const int gap = scale(5);
            const int content_width = icon_size + gap + static_cast<int>(text_size.cx);
            const int content_left = rect.left + (rect.right - rect.left - content_width) / 2;
            RECT icon_rect{content_left, rect.top, content_left + icon_size, rect.bottom};
            axiom::gui::draw_toolbar_icon(draw.hDC, icon, icon_rect, content_color, dpi_);

            RECT text_rect{icon_rect.right + gap, rect.top,
                           icon_rect.right + gap + static_cast<int>(text_size.cx), rect.bottom};
            DrawTextW(draw.hDC, text.c_str(), -1, &text_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        SelectObject(draw.hDC, old_font);
    }

    HWND make_control(const wchar_t* class_name,
                      const wchar_t* text,
                      DWORD style,
                      int id,
                      DWORD ex_style = 0) {
        return CreateWindowExW(ex_style, class_name, text, style | WS_CHILD | WS_VISIBLE,
                               0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               instance_, nullptr);
    }

    void apply_edit_margins() const {
        const LPARAM margins = MAKELPARAM(scale(2), scale(2));
        if (address_edit_ != nullptr) {
            SendMessageW(address_edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, margins);
        }
    }

    void move_framed_edit(HWND edit, RECT& frame, int x, int y, int width, int height) {
        frame = RECT{x, y, x + width, y + height};
        const int inset_x = scale(4);
        const int inset_y = scale(3);
        MoveWindow(edit,
                   frame.left + inset_x,
                   frame.top + inset_y,
                   std::max(0, width - inset_x * 2),
                   std::max(0, height - inset_y * 2),
                   TRUE);
    }

    void draw_edit_frame(HDC dc, HWND edit, const RECT& frame) const {
        if (IsRectEmpty(&frame)) {
            return;
        }
        fill_rect(dc, frame, theme_.edit);
        const bool focused = GetFocus() == edit;
        frame_rect(dc, frame, focused ? theme_.focus : theme_.border);
    }

    void add_tooltip(HWND control, const wchar_t* text) const {
        if (tooltip_ == nullptr || control == nullptr) return;
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = hwnd_;
        tool.uId = reinterpret_cast<UINT_PTR>(control);
        tool.lpszText = const_cast<wchar_t*>(text);
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }

    int show_app_message(
        std::wstring_view message,
        axiom::gui::MessageDialogIcon icon,
        std::wstring_view title = L"Axiom",
        axiom::gui::MessageDialogButtons buttons = axiom::gui::MessageDialogButtons::ok,
        int default_result = IDOK) const {
        return axiom::gui::show_message_dialog(
            hwnd_, instance_, dpi_, theme_.dark, title, message, icon, buttons, default_result);
    }

    std::vector<axiom::gui::CustomMenuItem> menu_items(UINT menu_id) const {
        const bool has_selection = !selected_browser_indices().empty();
        const bool has_archive = active_archive_path().has_value();
        const bool browsing_archive =
            history_.current().kind == axiom::gui::BrowserLocationKind::archive;
        const axiom::gui::ArchiveCapabilities capabilities = active_archive_capabilities();
        const bool archive_editable = capabilities.update && !capabilities.locked &&
                                      !capabilities.encrypted;
        switch (menu_id) {
            case kMenuFile:
                return {
                    {kOpenArchive, L"&Open archive...", L"Ctrl+O", !busy_},
                    {0, L"", L"", false, true},
                    {kExitApplication, L"E&xit", L"Alt+F4"},
                };
            case kMenuCommands:
                return {
                    {kAddFiles, L"&Add to archive...", L"Ctrl+N",
                     !busy_ && (!browsing_archive || archive_editable)},
                    {kExtract, L"&Extract...", L"Ctrl+E", !busy_ && has_archive},
                    {kTest, L"&Test archive", L"Ctrl+T", !busy_ && has_archive},
                    {0, L"", L"", false, true},
                    {kUpdateArchive, L"&Update archive...", L"",
                      !busy_ && has_archive && archive_editable},
                    {kFreshenArchive, L"&Freshen archive...", L"",
                     !busy_ && has_archive && archive_editable},
                    {kSynchronizeArchive, L"S&ynchronize archive...", L"",
                      !busy_ && has_archive && archive_editable},
                    {kDeleteArchiveEntries, L"Delete from archive...", L"",
                      !busy_ && browsing_archive && has_selection && archive_editable},
                    {kRepackArchive, L"&Repack archive...", L"",
                      !busy_ && has_archive && archive_editable},
                    {0, L"", L"", false, true},
                    {kView, L"&View", L"Enter", has_selection},
                    {kDelete, browsing_archive ? L"&Delete from archive" : L"&Delete",
                     L"Delete", !busy_ && has_selection &&
                          (!browsing_archive || archive_editable)},
                    {kSelectAll, L"Select &all", L"Ctrl+A", !browser_items_.empty()},
                };
            case kMenuTools:
                return {
                    {kInfo, L"Archive &information", L"Ctrl+I"},
                    {kArchiveFeatures, L"Archive &features...", L"", has_archive},
                    {0, L"", L"", false, true},
                    {kEditArchiveComment, L"Edit archive &comment...", L"",
                      !busy_ && has_archive && capabilities.comments && archive_editable},
                    {kLockArchive, L"&Lock archive...", L"",
                      !busy_ && has_archive && capabilities.lock && archive_editable},
                    {kRepairArchive, L"&Repair archive...", L"",
                     !busy_ && has_archive && capabilities.recovery_records},
                    {kCreateRecoveryVolumes, L"Create recovery &volumes...", L"",
                     !busy_ && has_archive && capabilities.recovery_records &&
                         capabilities.multi_volume},
                    {kVerifyArchiveSignature, L"Verify &signature...", L"",
                     !busy_ && has_archive && capabilities.authenticity},
                    {kCreateSfx, L"Create &self-extracting archive...", L"", false},
                };
            case kMenuOptions:
                return {{kSettings, L"&Settings...", L"", !busy_}};
            case kMenuHelp:
                return {
                    {kCheckUpdates, L"Check for &Updates...", L""},
                    {0, L"", L"", false, true},
                    {kAbout, L"&About Axiom", L"F1"},
                };
            default:
                return {};
        }
    }

    void show_browser_context_menu(POINT point) {
        const bool has_selection = !selected_browser_indices().empty();
        const bool has_archive = active_archive_path().has_value();
        const bool browsing_archive =
            history_.current().kind == axiom::gui::BrowserLocationKind::archive;
        const axiom::gui::ArchiveCapabilities capabilities = active_archive_capabilities();
        const bool archive_editable = capabilities.update && !capabilities.locked &&
                                      !capabilities.encrypted;
        if (point.x == -1 && point.y == -1) {
            RECT list_rect{};
            GetWindowRect(list_, &list_rect);
            point = {list_rect.left + scale(24), list_rect.top + scale(24)};
        }
        std::vector<axiom::gui::CustomMenuItem> items{
            {kView, L"&View", L"Enter", has_selection},
            {0, L"", L"", false, true},
            {kAddFiles, L"&Add to archive...", L"Ctrl+N",
             !busy_ && has_selection && (!browsing_archive || archive_editable)},
            {kExtract, L"&Extract...", L"Ctrl+E", !busy_ && has_archive},
            {kTest, L"&Test archive", L"Ctrl+T", !busy_ && has_archive},
            {0, L"", L"", false, true},
            {kDelete, browsing_archive ? L"&Delete from archive" : L"&Delete", L"Delete",
             !busy_ && has_selection && (!browsing_archive || archive_editable)},
            {kInfo, L"&Information", L"Ctrl+I", has_selection || has_archive},
            {kSelectAll, L"Select &all", L"Ctrl+A", !browser_items_.empty()},
        };
        const UINT command = menu_bar_.show_context_menu(std::move(items), point);
        if (command != 0) SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    }

    void paint_shell() {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd_, &paint);
        draw_edit_frame(dc, address_edit_, address_edit_frame_);
        EndPaint(hwnd_, &paint);
    }

    void on_create() {
        update_dpi(GetDpiForWindow(hwnd_));

        menu_bar_.create(
            hwnd_, instance_,
            {{kMenuFile, L"&File"}, {kMenuCommands, L"&Commands"},
             {kMenuTools, L"&Tools"}, {kMenuOptions, L"&Options"},
             {kMenuHelp, L"&Help"}},
            [this](UINT menu_id) { return menu_items(menu_id); },
            [this](UINT command) {
                SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
            });

        add_files_ = make_control(L"BUTTON", L"Add", WS_TABSTOP | BS_OWNERDRAW, kAddFiles);
        open_archive_ = make_control(L"BUTTON", L"Open archive", WS_TABSTOP | BS_OWNERDRAW, kOpenArchive);
        extract_ = make_control(L"BUTTON", L"Extract", WS_TABSTOP | BS_OWNERDRAW, kExtract);
        test_ = make_control(L"BUTTON", L"Test", WS_TABSTOP | BS_OWNERDRAW, kTest);

        navigate_back_ = make_control(L"BUTTON", L"<", WS_TABSTOP | BS_OWNERDRAW, kNavigateBack);
        navigate_forward_ = make_control(L"BUTTON", L">", WS_TABSTOP | BS_OWNERDRAW, kNavigateForward);
        navigate_up_ = make_control(L"BUTTON", L"Up", WS_TABSTOP | BS_OWNERDRAW, kNavigateUp);
        navigate_refresh_ = make_control(L"BUTTON", L"Refresh", WS_TABSTOP | BS_OWNERDRAW, kNavigateRefresh);
        address_edit_ = make_control(L"EDIT", L"This PC",
                                     WS_TABSTOP | ES_AUTOHSCROLL, kAddressEdit);
        address_go_ = make_control(L"BUTTON", L"Go", WS_TABSTOP | BS_OWNERDRAW, kAddressGo);
        view_ = make_control(L"BUTTON", L"View", WS_TABSTOP | BS_OWNERDRAW, kView);
        delete_ = make_control(L"BUTTON", L"Delete", WS_TABSTOP | BS_OWNERDRAW, kDelete);
        info_ = make_control(L"BUTTON", L"Info", WS_TABSTOP | BS_OWNERDRAW, kInfo);
        settings_ = make_control(L"BUTTON", L"Settings", WS_TABSTOP | BS_OWNERDRAW, kSettings);

        tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   hwnd_, nullptr, instance_, nullptr);
        add_tooltip(navigate_back_, L"Back");
        add_tooltip(navigate_forward_, L"Forward");
        add_tooltip(navigate_up_, L"Up one level");
        add_tooltip(navigate_refresh_, L"Refresh");

        table_.create(hwnd_, instance_, kList);
        list_ = table_.hwnd();
        table_.set_columns({
            {L"Name", 300},
            {L"Size", 105},
            {L"Packed", 105},
            {L"Type", 140},
            {L"Modified", 150},
            {L"CRC-32", 90},
            {L"Attributes", 90},
        });

        status_ = make_control(L"STATIC", L"Ready", SS_LEFT, kStatus);
        // Custom controls do not expose painted content to accessibility tools, so give
        // each HWND a stable semantic name for UI Automation and screen readers.
        SetWindowTextW(list_, L"Files and archive contents");

        apply_edit_margins();
        apply_fonts();
        DragAcceptFiles(hwnd_, TRUE);
        apply_theme();
        set_busy(false);
        history_.reset(axiom::gui::BrowserLocation::computer());
        if (initial_path_.empty()) {
            navigate_to(history_.current(), false);
        } else {
            set_text(address_edit_, initial_path_);
            on_address_go();
        }
        maybe_start_automatic_update_check();
    }

    void layout() {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int margin = scale(8);
        const int button_height = scale(30);
        const int edit_height = scale(30);
        const int gap = scale(6);
        const int width = client.right - client.left;
        const int right = width - margin;

        for (HWND child : transient_labels_) {
            DestroyWindow(child);
        }
        transient_labels_.clear();

        const int menu_height = menu_bar_.preferred_height();
        menu_bar_.move(0, 0, width, menu_height);

        int y = menu_height + margin;
        int x = margin;
        auto place = [&](HWND child, int logical_width) {
            const int width_px = scale(logical_width);
            ShowWindow(child, SW_SHOW);
            MoveWindow(child, x, y, width_px, button_height, TRUE);
            x += width_px + gap;
        };

        place(add_files_, 72);
        place(extract_, 88);
        place(test_, 64);
        place(view_, 64);
        place(delete_, 70);
        place(info_, 60);
        place(open_archive_, 104);
        place(settings_, 82);
        y += button_height + gap;

        x = margin;
        place(navigate_back_, 36);
        place(navigate_forward_, 36);
        place(navigate_up_, 36);
        place(navigate_refresh_, 36);
        const int go_width = scale(48);
        const int address_x = x;
        const int address_width = std::max(scale(100), right - go_width - gap - address_x);
        move_framed_edit(address_edit_, address_edit_frame_,
                         address_x, y, address_width, edit_height);
        ShowWindow(address_edit_, SW_SHOW);
        MoveWindow(address_go_, right - go_width, y, go_width, button_height, TRUE);
        ShowWindow(address_go_, SW_SHOW);
        y += edit_height + gap;

        const int bottom_height = scale(28);
        const int list_height = std::max(scale(80), static_cast<int>(client.bottom) - y - bottom_height - margin);
        MoveWindow(list_, margin, y, width - 2 * margin,
                   list_height, TRUE);
        const int bottom_y = client.bottom - bottom_height;
        MoveWindow(status_, margin, bottom_y + scale(2), width - margin * 2, scale(20), TRUE);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void set_status(const std::wstring& text) {
        set_text(status_, text);
    }

    void set_busy(bool busy) {
        busy_ = busy;
        EnableWindow(add_files_, !busy);
        EnableWindow(open_archive_, !busy);
        EnableWindow(extract_, !busy);
        EnableWindow(test_, !busy);
        EnableWindow(delete_, !busy);
        EnableWindow(settings_, !busy);
        operation_paused_ = false;
    }

    int selected_level() const {
        return selected_level_;
    }

    axiom::CompressionOptions compression_options() const {
        axiom::CompressionOptions options;
        apply_level(options, selected_level());
        options.thread_count = selected_thread_count_;
        return options;
    }

    void update_navigation_buttons() {
        EnableWindow(navigate_back_, history_.can_back());
        EnableWindow(navigate_forward_, history_.can_forward());
        EnableWindow(navigate_up_, parent_location(history_.current()).has_value());
    }

    std::vector<int> selected_browser_indices() const {
        std::vector<int> result;
        for (int index : table_.selected_indices()) {
            if (index >= 0 && index < static_cast<int>(browser_items_.size())) result.push_back(index);
        }
        return result;
    }

    std::vector<fs::path> selected_filesystem_paths() const {
        std::vector<fs::path> result;
        for (int index : selected_browser_indices()) {
            const auto& item = browser_items_[index];
            if (!item.filesystem_path.empty() && !item.is_parent()) {
                result.push_back(item.filesystem_path);
            }
        }
        return result;
    }

    std::optional<fs::path> active_archive_path() const {
        if (history_.current().kind == axiom::gui::BrowserLocationKind::archive) {
            return history_.current().archive_path;
        }
        for (int index : selected_browser_indices()) {
            const auto& item = browser_items_[index];
            if (item.kind == axiom::gui::BrowserItemKind::archive) return item.filesystem_path;
        }
        return std::nullopt;
    }

    axiom::gui::ArchiveCapabilities active_archive_capabilities() const {
        const auto archive = active_archive_path();
        if (archive && archive_catalog_ && archive_catalog_->path() == *archive) {
            return archive_catalog_->capabilities();
        }
        return {};
    }

    static axiom::gui::ArchiveFeatureAvailability implemented_feature_availability() {
        axiom::gui::ArchiveFeatureAvailability availability;
        // Metadata, ADS, and links are automatic; the API does not expose per-item toggles.
        availability.metadata = false;
        availability.update = true;
        availability.comments = true;
        availability.lock = true;
        availability.encryption = true;
        // Header encryption and custom KDF presets are not exposed by the current API.
        availability.header_encryption = false;
        availability.kdf_presets = false;
        return availability;
    }

    void navigate_to(axiom::gui::BrowserLocation location, bool record_history = true) {
        directory_watcher_.stop();
        KillTimer(hwnd_, kDirectoryRefreshTimer);
        if (record_history) history_.navigate(location);
        update_navigation_buttons();
        set_text(address_edit_, location.display_name());
        table_.clear();
        browser_items_.clear();
        set_status(L"Loading " + location.display_name() + L"...");

        const std::uint64_t generation = ++browser_generation_;
        auto catalog = archive_catalog_;
        HWND target = hwnd_;
        browser_thread_ = std::jthread(
            [target, generation, location = std::move(location), catalog = std::move(catalog)](
                std::stop_token stop) mutable {
                auto result = std::make_unique<axiom::gui::BrowserLoadResult>(
                    axiom::gui::load_browser_location(location, generation, std::move(catalog), stop));
                if (stop.stop_requested()) return;
                auto* payload = result.release();
                if (!PostMessageW(target, kBrowserLoadedMessage, 0,
                                  reinterpret_cast<LPARAM>(payload))) {
                    delete payload;
                }
            });
    }

    void populate_browser_table() {
        const auto rank = [](const axiom::gui::BrowserItem& item) {
            if (item.kind == axiom::gui::BrowserItemKind::parent) return 0;
            if (item.kind == axiom::gui::BrowserItemKind::drive ||
                item.kind == axiom::gui::BrowserItemKind::directory) return 1;
            return 2;
        };
        std::stable_sort(browser_items_.begin(), browser_items_.end(),
                         [&](const auto& left, const auto& right) {
            if (rank(left) != rank(right)) return rank(left) < rank(right);
            int comparison = 0;
            switch (sort_column_) {
                case 1:
                    comparison = left.size < right.size ? -1 : (left.size > right.size ? 1 : 0);
                    break;
                case 2: {
                    const auto left_size = left.packed_size.value_or(0);
                    const auto right_size = right.packed_size.value_or(0);
                    comparison = left_size < right_size ? -1 : (left_size > right_size ? 1 : 0);
                    break;
                }
                case 3: comparison = _wcsicmp(left.type.c_str(), right.type.c_str()); break;
                case 4: comparison = _wcsicmp(left.modified.c_str(), right.modified.c_str()); break;
                case 5:
                    comparison = left.crc32.value_or(0) < right.crc32.value_or(0) ? -1
                        : (left.crc32.value_or(0) > right.crc32.value_or(0) ? 1 : 0);
                    break;
                case 6: comparison = _wcsicmp(left.attributes.c_str(), right.attributes.c_str()); break;
                default: comparison = _wcsicmp(left.name.c_str(), right.name.c_str()); break;
            }
            if (comparison == 0) comparison = _wcsicmp(left.name.c_str(), right.name.c_str());
            return sort_ascending_ ? comparison < 0 : comparison > 0;
        });

        table_.clear();
        table_.set_sort_indicator(sort_column_, sort_ascending_);
        for (const auto& item : browser_items_) {
            const bool show_size = item.kind == axiom::gui::BrowserItemKind::file ||
                                   item.kind == axiom::gui::BrowserItemKind::archive ||
                                   item.kind == axiom::gui::BrowserItemKind::drive;
            const ShellIconRef icon = shell_icon_for_item(item);
            if (icon.image_list != nullptr) table_.set_image_list(icon.image_list);
            table_.append_row({
                item.name,
                show_size ? format_size(item.size) : L"",
                item.packed_size ? format_size(*item.packed_size) : L"",
                item.type,
                item.modified,
                format_crc32(item.crc32),
                item.attributes,
            }, icon.index);
        }
    }

    void on_table_sort(int column) {
        if (column == sort_column_) {
            sort_ascending_ = !sort_ascending_;
        } else {
            sort_column_ = column;
            sort_ascending_ = true;
        }
        populate_browser_table();
    }

    void on_browser_loaded(LPARAM lparam) {
        std::unique_ptr<axiom::gui::BrowserLoadResult> result(
            reinterpret_cast<axiom::gui::BrowserLoadResult*>(lparam));
        if (!result || result->snapshot.generation != browser_generation_) return;

        if (result->archive_catalog) archive_catalog_ = std::move(result->archive_catalog);
        browser_items_ = std::move(result->snapshot.items);
        if (!application_options_.show_hidden) {
            std::erase_if(browser_items_, [](const auto& item) {
                return item.attributes.find(L'H') != std::wstring::npos ||
                       item.attributes.find(L'S') != std::wstring::npos;
            });
        }
        populate_browser_table();
        if (!result->snapshot.error.empty()) {
            set_status(L"Cannot read location: " + result->snapshot.error);
        } else {
            set_status(quote_count(browser_items_.size(), L"item", L"items"));
            if (result->snapshot.location.kind == axiom::gui::BrowserLocationKind::filesystem) {
                directory_watcher_.start(result->snapshot.location.filesystem_path, hwnd_,
                                         kDirectoryChangedMessage);
            }
        }
        update_navigation_buttons();
    }

    void on_navigate_back() {
        if (auto location = history_.back()) navigate_to(*location, false);
    }

    void on_navigate_forward() {
        if (auto location = history_.forward()) navigate_to(*location, false);
    }

    void on_navigate_up() {
        if (auto location = parent_location(history_.current())) navigate_to(*location);
    }

    void on_navigate_refresh() {
        navigate_to(history_.current(), false);
    }

    void on_address_go() {
        std::wstring text = get_text(address_edit_);
        if (_wcsicmp(text.c_str(), L"This PC") == 0) {
            navigate_to(axiom::gui::BrowserLocation::computer());
            return;
        }
        const std::wstring separator = L" :: /";
        if (const auto split = text.find(separator); split != std::wstring::npos) {
            const fs::path archive = text.substr(0, split);
            const std::wstring inner = text.substr(split + separator.size());
            const int needed = WideCharToMultiByte(CP_UTF8, 0, inner.data(),
                                                   static_cast<int>(inner.size()),
                                                   nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<std::size_t>(std::max(needed, 0)), '\0');
            if (needed > 0) {
                WideCharToMultiByte(CP_UTF8, 0, inner.data(), static_cast<int>(inner.size()),
                                    utf8.data(), needed, nullptr, nullptr);
            }
            navigate_to(axiom::gui::BrowserLocation::archive(archive, std::move(utf8)));
            return;
        }
        const fs::path path(text);
        std::error_code error;
        if (fs::is_regular_file(path, error) && axiom::gui::is_axiom_archive(path)) {
            navigate_to(axiom::gui::BrowserLocation::archive(path));
        } else if (fs::is_directory(path, error)) {
            navigate_to(axiom::gui::BrowserLocation::filesystem(path));
        } else {
            show_app_message(L"The location does not exist or cannot be opened.",
                             axiom::gui::MessageDialogIcon::warning);
            set_text(address_edit_, history_.current().display_name());
        }
    }

    void activate_browser_item(int index) {
        if (index < 0 || index >= static_cast<int>(browser_items_.size())) return;
        const auto& item = browser_items_[index];
        if (item.is_parent()) {
            on_navigate_up();
        } else if (item.kind == axiom::gui::BrowserItemKind::drive ||
                   item.kind == axiom::gui::BrowserItemKind::directory) {
            if (history_.current().kind == axiom::gui::BrowserLocationKind::archive) {
                navigate_to(axiom::gui::BrowserLocation::archive(
                    history_.current().archive_path, item.archive_path));
            } else {
                navigate_to(axiom::gui::BrowserLocation::filesystem(item.filesystem_path));
            }
        } else if (item.kind == axiom::gui::BrowserItemKind::archive) {
            navigate_to(axiom::gui::BrowserLocation::archive(item.filesystem_path));
        } else if (!item.filesystem_path.empty()) {
            ShellExecuteW(hwnd_, L"open", item.filesystem_path.c_str(), nullptr,
                          item.filesystem_path.parent_path().c_str(), SW_SHOWNORMAL);
        } else {
            show_app_message(
                L"Viewing a file directly from an archive requires selective extraction.",
                axiom::gui::MessageDialogIcon::information);
        }
    }

    void on_table_activate() {
        activate_browser_item(table_.focused_index());
    }

    void on_table_selection_changed() {
        const auto indices = selected_browser_indices();
        std::uint64_t bytes = 0;
        for (int index : indices) bytes += browser_items_[index].size;
        if (indices.empty()) {
            set_status(quote_count(browser_items_.size(), L"item", L"items"));
        } else {
            set_status(quote_count(indices.size(), L"item selected", L"items selected") +
                       (bytes != 0 ? L" (" + format_size(bytes) + L")" : L""));
        }
    }

    void create_archive_from_paths(
        std::vector<fs::path> paths,
        std::optional<fs::path> target_archive = std::nullopt,
        axiom::gui::ArchiveUpdateMode update_mode =
            axiom::gui::ArchiveUpdateMode::create_new) {
        if (paths.empty()) return;

        axiom::gui::CreateArchiveDialogOptions dialog_options;
        dialog_options.level = application_options_.default_level;
        dialog_options.thread_count = application_options_.default_thread_count;
        dialog_options.feature_availability = implemented_feature_availability();
        dialog_options.features.update_mode = update_mode;
        const fs::path base = history_.current().kind == axiom::gui::BrowserLocationKind::filesystem
            ? history_.current().filesystem_path
            : paths.front().parent_path();
        dialog_options.archive_path = target_archive.value_or(base / L"Archive.axar");
        if (target_archive) {
            try {
                dialog_options.features.comment = widen(axiom::archive_comment(*target_archive));
            } catch (...) {
                // The operation itself will report a precise archive error if necessary.
            }
        }
        if (!axiom::gui::show_create_archive_dialog(hwnd_, paths.size(), dialog_options)) return;

        inputs_ = std::move(paths);
        selected_level_ = dialog_options.level;
        selected_thread_count_ = dialog_options.thread_count;
        pending_archive_path_ = std::move(dialog_options.archive_path);
        pending_archive_features_ = std::move(dialog_options.features);
        on_compress();
    }

    void on_add_to_archive() {
        if (history_.current().kind == axiom::gui::BrowserLocationKind::archive) {
            const auto archive = active_archive_path();
            const auto capabilities = active_archive_capabilities();
            if (!archive || capabilities.locked || capabilities.encrypted) {
                show_app_message(
                    capabilities.locked
                        ? L"This archive is locked and cannot be changed."
                        : L"Editing encrypted archives is not supported yet.",
                    axiom::gui::MessageDialogIcon::information);
                return;
            }
            auto paths = pick_files(hwnd_);
            if (!paths.empty()) {
                create_archive_from_paths(std::move(paths), *archive,
                                          axiom::gui::ArchiveUpdateMode::add_or_replace);
            }
            return;
        }
        auto paths = selected_filesystem_paths();
        if (paths.empty()) paths = pick_files(hwnd_);
        create_archive_from_paths(std::move(paths));
    }

    void on_update_archive(axiom::gui::ArchiveUpdateMode mode) {
        const auto archive = active_archive_path();
        const auto capabilities = active_archive_capabilities();
        if (!archive) {
            show_app_message(L"Open an archive first.",
                             axiom::gui::MessageDialogIcon::information);
            return;
        }
        if (capabilities.locked || capabilities.encrypted) {
            show_app_message(
                capabilities.locked
                    ? L"This archive is locked and cannot be changed."
                    : L"Editing encrypted archives is not supported yet.",
                axiom::gui::MessageDialogIcon::information);
            return;
        }
        if (mode == axiom::gui::ArchiveUpdateMode::synchronize &&
            show_app_message(
                L"Synchronize will mirror the selected source into the archive.\n\n"
                L"Archived entries missing from the source will be permanently removed. Continue?",
                axiom::gui::MessageDialogIcon::warning, L"Synchronize archive",
                axiom::gui::MessageDialogButtons::yes_no, IDNO) != IDYES) {
            return;
        }
        auto paths = pick_files(hwnd_);
        if (!paths.empty()) create_archive_from_paths(std::move(paths), *archive, mode);
    }

    bool active_archive_is_editable() {
        const auto capabilities = active_archive_capabilities();
        if (!capabilities.locked && !capabilities.encrypted) return true;
        show_app_message(
            capabilities.locked
                ? L"This archive is locked and cannot be changed."
                : L"Editing encrypted archives is not supported yet.",
            axiom::gui::MessageDialogIcon::information);
        return false;
    }

    void on_view() {
        on_table_activate();
    }

    void on_delete_from_archive() {
        const auto archive = active_archive_path();
        if (!archive || history_.current().kind != axiom::gui::BrowserLocationKind::archive) {
            show_app_message(L"Open an archive and select entries to delete.",
                             axiom::gui::MessageDialogIcon::information);
            return;
        }
        if (!active_archive_is_editable()) return;
        std::vector<std::string> paths;
        for (const int index : selected_browser_indices()) {
            const auto& item = browser_items_[static_cast<std::size_t>(index)];
            if (!item.is_parent() && !item.archive_path.empty()) {
                paths.push_back(item.archive_path);
            }
        }
        if (paths.empty()) {
            show_app_message(L"Select one or more archive entries to delete.",
                             axiom::gui::MessageDialogIcon::information);
            return;
        }
        const std::wstring prompt = L"Permanently remove " +
            quote_count(paths.size(), L"selected archive entry", L"selected archive entries") +
            L"?\n\nThe archive will be rebuilt to reclaim their stored data.";
        if (show_app_message(prompt, axiom::gui::MessageDialogIcon::warning,
                             L"Delete from archive",
                             axiom::gui::MessageDialogButtons::yes_no, IDNO) != IDYES) {
            return;
        }
        const auto options = compression_options();
        operation_archive_output_ = *archive;
        start_operation(
            L"Deleting archive entries...", L"Selected entries were removed.",
            [archive = *archive, paths = std::move(paths), options](
                std::shared_ptr<axiom::OperationControl> operation) mutable {
                auto run_options = options;
                run_options.operation = std::move(operation);
                axiom::delete_from_archive(archive, paths, run_options);
            });
    }

    void on_repack_archive() {
        const auto archive = active_archive_path();
        if (!archive) return;
        if (!active_archive_is_editable()) return;
        if (show_app_message(
                L"Rebuild this archive to reclaim dead space?\n\n"
                L"All live files will be recompressed into fresh solid blocks.",
                axiom::gui::MessageDialogIcon::question, L"Repack archive",
                axiom::gui::MessageDialogButtons::yes_no, IDNO) != IDYES) {
            return;
        }
        const auto options = compression_options();
        operation_archive_output_ = *archive;
        start_operation(
            L"Repacking archive...", L"Archive repacked successfully.",
            [archive = *archive, options](
                std::shared_ptr<axiom::OperationControl> operation) mutable {
                auto run_options = options;
                run_options.operation = std::move(operation);
                axiom::repack_archive(archive, run_options);
            });
    }

    void on_edit_archive_comment() {
        const auto archive = active_archive_path();
        if (!archive) return;
        if (!active_archive_is_editable()) return;
        std::wstring comment;
        try {
            comment = widen(axiom::archive_comment(*archive));
        } catch (const std::exception& error) {
            show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                             L"Archive comment");
            return;
        }
        if (!axiom::gui::show_archive_comment_dialog(hwnd_, comment)) return;
        const auto options = compression_options();
        const std::string encoded_comment = utf8(comment);
        operation_archive_output_ = *archive;
        start_operation(
            L"Updating archive comment...", L"Archive comment updated.",
            [archive = *archive, encoded_comment, options](
                std::shared_ptr<axiom::OperationControl> operation) mutable {
                auto run_options = options;
                run_options.operation = std::move(operation);
                axiom::set_archive_comment(archive, encoded_comment, run_options);
            });
    }

    void on_lock_archive() {
        const auto archive = active_archive_path();
        if (!archive) return;
        if (!active_archive_is_editable()) return;
        if (show_app_message(
                L"Lock this archive permanently?\n\n"
                L"A locked archive remains readable, but it cannot be updated, deleted from, "
                L"repacked, commented, or unlocked.",
                axiom::gui::MessageDialogIcon::warning, L"Lock archive",
                axiom::gui::MessageDialogButtons::yes_no, IDNO) != IDYES) {
            return;
        }
        const auto options = compression_options();
        operation_archive_output_ = *archive;
        start_operation(
            L"Locking archive...", L"Archive locked successfully.",
            [archive = *archive, options](
                std::shared_ptr<axiom::OperationControl> operation) mutable {
                auto run_options = options;
                run_options.operation = std::move(operation);
                axiom::lock_archive(archive, run_options);
            });
    }

    void on_delete_selected() {
        if (history_.current().kind == axiom::gui::BrowserLocationKind::archive) {
            on_delete_from_archive();
            return;
        }
        const auto paths = selected_filesystem_paths();
        if (paths.empty()) {
            show_app_message(L"Select one or more filesystem items to delete.",
                             axiom::gui::MessageDialogIcon::information);
            return;
        }
        const std::wstring prompt = L"Move " +
            quote_count(paths.size(), L"selected item", L"selected items") +
            L" to the Recycle Bin?";
        if (application_options_.confirm_delete &&
            show_app_message(prompt, axiom::gui::MessageDialogIcon::warning, L"Axiom",
                             axiom::gui::MessageDialogButtons::yes_no, IDNO) != IDYES) {
            return;
        }

        ComPtr<IFileOperation> operation;
        HRESULT result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(operation.put()));
        if (SUCCEEDED(result)) {
            operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR |
                                         FOF_SILENT | FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE);
            for (const auto& path : paths) {
                ComPtr<IShellItem> item;
                result = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put()));
                if (FAILED(result)) break;
                result = operation->DeleteItem(item.get(), nullptr);
                if (FAILED(result)) break;
            }
            if (SUCCEEDED(result)) result = operation->PerformOperations();
        }
        if (FAILED(result)) {
            show_app_message(L"Windows could not move the selected items to the Recycle Bin.",
                             axiom::gui::MessageDialogIcon::error, L"Delete failed");
        }
        on_navigate_refresh();
    }

    void on_info() {
        const auto indices = selected_browser_indices();
        std::uint64_t bytes = 0;
        for (int index : indices) bytes += browser_items_[index].size;
        std::wstring message = L"Location: " + history_.current().display_name() + L"\n\n";
        message += indices.empty()
            ? quote_count(browser_items_.size(), L"item", L"items")
            : quote_count(indices.size(), L"selected item", L"selected items");
        if (bytes != 0) message += L"\nSize: " + format_size(bytes);
        if (const auto archive = active_archive_path()) {
            message += L"\nArchive: " + archive->wstring();
            try {
                message += axiom::archive_is_encrypted(*archive)
                    ? L"\nEncryption: File data encrypted"
                    : L"\nEncryption: None";
                message += axiom::archive_is_locked(*archive)
                    ? L"\nState: Locked (read-only)"
                    : L"\nState: Editable";
                const std::wstring comment = widen(axiom::archive_comment(*archive));
                if (!comment.empty()) message += L"\nComment: " + comment;
            } catch (...) {
                message += L"\nState: Could not read archive metadata";
            }
        }
        show_app_message(message, axiom::gui::MessageDialogIcon::information, L"Information");
    }

    void on_archive_features() {
        const auto archive = active_archive_path();
        if (!archive) {
            show_app_message(L"Open or select an Axiom archive first.",
                             axiom::gui::MessageDialogIcon::information);
            return;
        }
        const axiom::gui::ArchiveCapabilities capabilities = active_archive_capabilities();
        axiom::gui::show_archive_feature_summary_dialog(hwnd_, *archive, capabilities);
    }

    void on_pending_archive_command(std::wstring_view feature) {
        show_app_message(
            std::wstring(feature) +
                L" is represented in the GUI but is waiting for the archive API connection.",
            axiom::gui::MessageDialogIcon::information,
            L"Archive feature");
    }

    void on_about() {
        axiom::gui::show_about_dialog(hwnd_, instance_, dpi_, theme_.dark, kCheckUpdates);
    }

    void maybe_start_automatic_update_check() {
        if (axiom::gui::automatic_update_check_due()) {
            begin_update_check(axiom::gui::UpdateCheckKind::automatic);
        }
    }

    void begin_update_check(axiom::gui::UpdateCheckKind kind) {
        if (update_check_in_progress_) {
            if (kind == axiom::gui::UpdateCheckKind::manual) {
                show_app_message(L"An update check is already running.",
                                 axiom::gui::MessageDialogIcon::information);
            }
            return;
        }
        update_check_in_progress_ = true;
        if (kind == axiom::gui::UpdateCheckKind::manual) {
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        }
        axiom::gui::start_update_check(hwnd_, instance_, kind);
    }

    void begin_update_download(const axiom::gui::UpdateInfo& update) {
        if (update_download_in_progress_) {
            show_app_message(L"An update download is already running.",
                             axiom::gui::MessageDialogIcon::information);
            return;
        }
        update_download_in_progress_ = true;
        show_app_message(L"Axiom will download the installer in the background.",
                         axiom::gui::MessageDialogIcon::information,
                         L"Axiom Update");
        axiom::gui::start_update_download(hwnd_, update);
    }

    void on_update_check_complete(LPARAM lparam) {
        std::unique_ptr<axiom::gui::UpdateCheckResult> result(
            reinterpret_cast<axiom::gui::UpdateCheckResult*>(lparam));
        update_check_in_progress_ = false;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        if (!result) return;

        const bool manual = result->kind == axiom::gui::UpdateCheckKind::manual;
        if (!result->success) {
            if (manual) {
                show_app_message(L"Could not check for updates:\n\n" + result->message,
                                 axiom::gui::MessageDialogIcon::error,
                                 L"Axiom Update");
            }
            return;
        }
        if (!result->update_available) {
            if (manual) {
                show_app_message(
                    L"Axiom is up to date.\n\nInstalled version: " +
                        axiom::gui::current_executable_version(instance_),
                    axiom::gui::MessageDialogIcon::information,
                    L"Axiom Update");
            }
            return;
        }

        std::wstring message = L"Axiom " + result->update.version +
                               L" is available.\n\nDownload and install it now?";
        if (show_app_message(message, axiom::gui::MessageDialogIcon::question,
                             L"Axiom Update", axiom::gui::MessageDialogButtons::yes_no,
                             IDYES) == IDYES) {
            begin_update_download(result->update);
        }
    }

    void on_update_download_complete(LPARAM lparam) {
        std::unique_ptr<axiom::gui::UpdateDownloadResult> result(
            reinterpret_cast<axiom::gui::UpdateDownloadResult*>(lparam));
        update_download_in_progress_ = false;
        if (!result) return;
        if (!result->success) {
            show_app_message(L"Could not download the Axiom update:\n\n" + result->message,
                             axiom::gui::MessageDialogIcon::error,
                             L"Axiom Update");
            return;
        }
        if (busy_) {
            show_app_message(
                L"The update was downloaded to:\n\n" + result->installer_path +
                    L"\n\nFinish or cancel the active archive operation before running it.",
                axiom::gui::MessageDialogIcon::information,
                L"Axiom Update");
            return;
        }
        const std::wstring message = L"Axiom " + result->update.version +
            L" has been downloaded.\n\nRun the installer now? Axiom will close if it starts successfully.";
        if (show_app_message(message, axiom::gui::MessageDialogIcon::question,
                             L"Axiom Update", axiom::gui::MessageDialogButtons::yes_no,
                             IDYES) != IDYES) {
            return;
        }
        SetLastError(ERROR_SUCCESS);
        const auto launch = reinterpret_cast<INT_PTR>(ShellExecuteW(
            hwnd_, L"runas", result->installer_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (launch <= 32) {
            show_app_message(L"Could not launch the installer:\n\n" +
                                 axiom::gui::last_error_text(),
                             axiom::gui::MessageDialogIcon::error,
                             L"Axiom Update");
            return;
        }
        SendMessageW(hwnd_, WM_CLOSE, 0, 0);
    }

    void on_select_all() {
        table_.select_all();
    }

    void on_settings() {
        if (axiom::gui::show_application_settings_dialog(hwnd_, application_options_)) {
            selected_level_ = application_options_.default_level;
            selected_thread_count_ = application_options_.default_thread_count;
            save_current_settings();
            on_navigate_refresh();
        }
    }

    void on_open_archive() {
        auto path = pick_open_archive(hwnd_);
        if (path) navigate_to(axiom::gui::BrowserLocation::archive(*path));
    }

    void on_drop_files(HDROP drop) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<fs::path> paths;
        paths.reserve(count);
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            path.resize(length);
            paths.emplace_back(std::move(path));
        }
        DragFinish(drop);
        if (paths.size() == 1 && axiom::gui::is_axiom_archive(paths.front())) {
            navigate_to(axiom::gui::BrowserLocation::archive(std::move(paths.front())));
            return;
        }
        create_archive_from_paths(std::move(paths));
    }

    void save_current_settings() {
        persisted_settings_.application = application_options_;
        persisted_settings_.sort_column = sort_column_;
        persisted_settings_.sort_ascending = sort_ascending_;
        persisted_settings_.last_location = history_.current().display_name();
        persisted_settings_.placement.length = sizeof(WINDOWPLACEMENT);
        persisted_settings_.has_placement =
            GetWindowPlacement(hwnd_, &persisted_settings_.placement) != FALSE;
        axiom::gui::save_gui_settings(persisted_settings_);
    }

    void start_operation(std::wstring running,
                         std::wstring success,
                         std::function<void(std::shared_ptr<axiom::OperationControl>)> work) {
        if (busy_) {
            return;
        }

        // The archive engine writes its temporary output in the browsed directory.
        // Suppress any refresh already queued for those writes; completion performs
        // one authoritative reload after success, failure, or cancellation.
        KillTimer(hwnd_, kDirectoryRefreshTimer);
        set_busy(true);
        set_status(running);
        if (!operation_window_.create(
                hwnd_, instance_, running, operation_archive_output_,
                make_operation_window_theme(theme_),
                [this](bool paused) {
                    if (!busy_ || !operation_runner_.running()) return;
                    operation_paused_ = paused;
                    operation_runner_.set_paused(paused);
                    set_status(paused ? L"Operation paused." : L"Operation resumed.");
                },
                [this] {
                    if (!busy_ || !operation_runner_.running()) return;
                    operation_runner_.cancel();
                    set_status(L"Cancelling operation...");
                })) {
            set_busy(false);
            set_status(L"Could not create the operation progress window.");
            return;
        }
        // The operation's temporary output can generate thousands of directory
        // notifications. Completion reloads the current location and restarts the
        // watcher, so keeping it active here only consumes I/O and UI resources.
        directory_watcher_.stop();
        if (!operation_runner_.start(hwnd_, kOperationDoneMessage, kOperationProgressMessage,
                                     std::move(running), std::move(success), std::move(work))) {
            operation_window_.close();
            set_busy(false);
            set_status(L"Another operation is already running.");
            on_navigate_refresh();
        }
    }

    void on_compress() {
        if (inputs_.empty()) {
            show_app_message(L"Add at least one file or folder first.",
                             axiom::gui::MessageDialogIcon::information);
            return;
        }

        const auto archive = pending_archive_path_;
        if (archive.empty()) {
            show_app_message(L"Choose an output .axar archive path.",
                             axiom::gui::MessageDialogIcon::information);
            return;
        }

        const auto inputs = inputs_;
        auto options = compression_options();
        const auto mode = pending_archive_features_.update_mode;
        options.password = pending_archive_features_.encrypt_data
            ? utf8(pending_archive_features_.password)
            : std::string{};
        const std::string comment = utf8(pending_archive_features_.comment);
        const bool set_comment = mode != axiom::gui::ArchiveUpdateMode::create_new ||
                                 !comment.empty();
        const bool repack_after = pending_archive_features_.repack_after_update &&
                                  mode != axiom::gui::ArchiveUpdateMode::create_new;
        const bool lock_after = pending_archive_features_.lock_archive;
        secure_clear(pending_archive_features_.password);

        std::wstring running = L"Compressing...";
        std::wstring success = L"Archive created: " + archive.wstring();
        switch (mode) {
            case axiom::gui::ArchiveUpdateMode::add_or_replace:
                running = L"Adding to archive...";
                success = L"Archive updated: " + archive.wstring();
                break;
            case axiom::gui::ArchiveUpdateMode::update_newer:
                running = L"Updating archive...";
                success = L"Archive updated: " + archive.wstring();
                break;
            case axiom::gui::ArchiveUpdateMode::fresh_existing:
                running = L"Freshening archive...";
                success = L"Archive freshened: " + archive.wstring();
                break;
            case axiom::gui::ArchiveUpdateMode::synchronize:
                running = L"Synchronizing archive...";
                success = L"Archive synchronized: " + archive.wstring();
                break;
            default:
                break;
        }
        operation_archive_output_ = archive;
        start_operation(std::move(running), std::move(success),
                        [inputs, archive, options, mode, comment, set_comment,
                         repack_after, lock_after](
                            std::shared_ptr<axiom::OperationControl> operation) mutable {
                            auto run_options = options;
                            run_options.operation = operation;
                            switch (mode) {
                                case axiom::gui::ArchiveUpdateMode::add_or_replace:
                                    axiom::add_to_archive(inputs, archive, run_options);
                                    break;
                                case axiom::gui::ArchiveUpdateMode::update_newer:
                                    axiom::update_archive(inputs, archive, run_options, false);
                                    break;
                                case axiom::gui::ArchiveUpdateMode::fresh_existing:
                                    axiom::update_archive(inputs, archive, run_options, true);
                                    break;
                                case axiom::gui::ArchiveUpdateMode::synchronize:
                                    axiom::sync_archive(inputs, archive, run_options);
                                    break;
                                default:
                                    axiom::create_archive(inputs, archive, run_options);
                                    break;
                            }
                            if (set_comment) {
                                axiom::set_archive_comment(archive, comment, run_options);
                            }
                            if (repack_after) {
                                axiom::repack_archive(archive, run_options);
                            }
                            if (lock_after) {
                                axiom::lock_archive(archive, run_options);
                            }
                        });
    }

    void on_extract() {
        const auto archive = active_archive_path();
        if (!archive) {
            show_app_message(L"Open or select an Axiom archive first.",
                             axiom::gui::MessageDialogIcon::information);
            return;
        }
        axiom::gui::ExtractArchiveDialogOptions dialog_options;
        dialog_options.thread_count = application_options_.default_thread_count;
        dialog_options.destination = archive->parent_path() / archive->stem();
        dialog_options.feature_availability = implemented_feature_availability();
        bool encrypted = false;
        try {
            encrypted = axiom::archive_is_encrypted(*archive);
        } catch (const std::exception& error) {
            show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                             L"Open archive");
            return;
        }
        dialog_options.feature_availability.encryption = encrypted;
        if (!axiom::gui::show_extract_archive_dialog(hwnd_, *archive, dialog_options)) return;

        if (encrypted && dialog_options.features.password.empty() &&
            !axiom::gui::show_archive_password_dialog(
                hwnd_, dialog_options.features.password)) {
            return;
        }

        axiom::ExtractOptions options;
        options.thread_count = dialog_options.thread_count;
        options.restore_mtime = dialog_options.restore_mtime;
        options.overwrite = dialog_options.overwrite
            ? axiom::ExtractOptions::Overwrite::overwrite
            : axiom::ExtractOptions::Overwrite::fail;
        options.password = utf8(dialog_options.features.password);
        secure_clear(dialog_options.features.password);

        operation_archive_output_.clear();
        start_operation(L"Extracting...",
                        L"Extracted to: " + dialog_options.destination.wstring(),
                        [archive = *archive, output = dialog_options.destination, options](std::shared_ptr<axiom::OperationControl> operation) mutable {
                            auto run_options = options;
                            run_options.operation = std::move(operation);
                            axiom::extract_archive(archive, output, run_options);
                        });
    }

    void on_test() {
        const auto archive = active_archive_path();
        if (!archive) {
            show_app_message(L"Open or select an Axiom archive first.",
                             axiom::gui::MessageDialogIcon::information);
            return;
        }

        axiom::DecompressionOptions options;
        options.thread_count = application_options_.default_thread_count;
        try {
            if (axiom::archive_is_encrypted(*archive)) {
                std::wstring password;
                if (!axiom::gui::show_archive_password_dialog(hwnd_, password)) return;
                options.password = utf8(password);
                secure_clear(password);
            }
        } catch (const std::exception& error) {
            show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                             L"Open archive");
            return;
        }
        operation_archive_output_.clear();
        start_operation(L"Testing archive...",
                        L"Archive integrity test passed.",
                        [archive = *archive, options](std::shared_ptr<axiom::OperationControl> operation) mutable {
                            auto run_options = options;
                            run_options.operation = std::move(operation);
                            axiom::test_archive(archive, run_options);
                        });
    }

    void on_operation_progress(LPARAM lparam) {
        std::unique_ptr<axiom::OperationProgress> progress(
            reinterpret_cast<axiom::OperationProgress*>(lparam));
        if (!progress) {
            return;
        }

        // Keep only the newest worker update already waiting in the UI queue.
        // Every payload is heap-owned, so replacing the unique_ptr also releases
        // each stale update without changing the worker's progress cadence.
        MSG queued{};
        while (PeekMessageW(&queued, hwnd_, kOperationProgressMessage,
                            kOperationProgressMessage, PM_REMOVE)) {
            progress.reset(reinterpret_cast<axiom::OperationProgress*>(queued.lParam));
        }
        if (!busy_) {
            return;
        }
        operation_window_.set_progress(*progress);
    }

    void on_operation_done(LPARAM lparam) {
        std::unique_ptr<axiom::gui::OperationResult> result(
            reinterpret_cast<axiom::gui::OperationResult*>(lparam));
        operation_runner_.finish();
        operation_window_.close();
        set_busy(false);
        const bool archive_changed = !operation_archive_output_.empty();
        operation_archive_output_.clear();
        if (archive_changed) archive_catalog_.reset();
        set_status(result->message);
        on_navigate_refresh();
        if (result->cancelled) {
            return;
        }
        show_app_message(
            result->message,
            result->ok ? axiom::gui::MessageDialogIcon::information
                       : axiom::gui::MessageDialogIcon::error,
            result->ok ? L"Axiom" : L"Axiom error");
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE:
                on_create();
                return 0;
            case WM_SIZE:
                layout();
                return 0;
            case WM_DPICHANGED: {
                update_dpi(HIWORD(wparam));
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(hwnd_, nullptr,
                             suggested->left,
                             suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                layout();
                return 0;
            }
            case WM_ERASEBKGND: {
                RECT rect{};
                GetClientRect(hwnd_, &rect);
                FillRect(reinterpret_cast<HDC>(wparam), &rect,
                         window_brush_ != nullptr ? window_brush_ : GetSysColorBrush(COLOR_WINDOW));
                return 1;
            }
            case WM_PAINT:
                paint_shell();
                return 0;
            case WM_SETTINGCHANGE:
            case WM_THEMECHANGED:
                apply_theme();
                return 0;
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORBTN:
            case WM_CTLCOLORLISTBOX:
                return paint_control_background(reinterpret_cast<HWND>(lparam),
                                                 reinterpret_cast<HDC>(wparam),
                                                 message);
            case WM_DRAWITEM:
                if (lparam != 0) {
                    const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
                    draw_owner_button(draw);
                    return TRUE;
                }
                break;
            case WM_CONTEXTMENU:
                if (reinterpret_cast<HWND>(wparam) == list_ ||
                    reinterpret_cast<HWND>(wparam) == hwnd_) {
                    show_browser_context_menu({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                    return 0;
                }
                break;
            case WM_DROPFILES:
                on_drop_files(reinterpret_cast<HDROP>(wparam));
                return 0;
            case WM_TIMER:
                if (wparam == kDirectoryRefreshTimer) {
                    KillTimer(hwnd_, kDirectoryRefreshTimer);
                    if (!busy_) on_navigate_refresh();
                    return 0;
                }
                break;
            case WM_COMMAND:
                switch (LOWORD(wparam)) {
                    case kOpenArchive: on_open_archive(); return 0;
                    case kAddFiles: on_add_to_archive(); return 0;
                    case kExtract: on_extract(); return 0;
                    case kTest: on_test(); return 0;
                    case kNavigateBack: on_navigate_back(); return 0;
                    case kNavigateForward: on_navigate_forward(); return 0;
                    case kNavigateUp: on_navigate_up(); return 0;
                    case kNavigateRefresh: on_navigate_refresh(); return 0;
                    case kAddressGo: on_address_go(); return 0;
                    case kView: on_view(); return 0;
                    case kDelete: on_delete_selected(); return 0;
                    case kInfo: on_info(); return 0;
                    case kArchiveFeatures: on_archive_features(); return 0;
                    case kUpdateArchive:
                        on_update_archive(axiom::gui::ArchiveUpdateMode::update_newer);
                        return 0;
                    case kFreshenArchive:
                        on_update_archive(axiom::gui::ArchiveUpdateMode::fresh_existing);
                        return 0;
                    case kSynchronizeArchive:
                        on_update_archive(axiom::gui::ArchiveUpdateMode::synchronize);
                        return 0;
                    case kDeleteArchiveEntries: on_delete_from_archive(); return 0;
                    case kRepackArchive: on_repack_archive(); return 0;
                    case kEditArchiveComment: on_edit_archive_comment(); return 0;
                    case kLockArchive: on_lock_archive(); return 0;
                    case kRepairArchive: on_pending_archive_command(L"Archive repair"); return 0;
                    case kCreateRecoveryVolumes: on_pending_archive_command(L"Recovery volumes"); return 0;
                    case kVerifyArchiveSignature: on_pending_archive_command(L"Signature verification"); return 0;
                    case kCreateSfx: on_pending_archive_command(L"Self-extracting archives"); return 0;
                    case kSettings: on_settings(); return 0;
                    case kSelectAll: on_select_all(); return 0;
                    case kAbout: on_about(); return 0;
                    case kCheckUpdates:
                        begin_update_check(axiom::gui::UpdateCheckKind::manual);
                        return 0;
                    case kExitApplication: SendMessageW(hwnd_, WM_CLOSE, 0, 0); return 0;
                    case kAddressEdit:
                        if (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == EN_KILLFOCUS) {
                            InvalidateRect(hwnd_, nullptr, FALSE);
                            return 0;
                        }
                        break;
                }
                break;
            case kOperationDoneMessage:
                on_operation_done(lparam);
                return 0;
            case kOperationProgressMessage:
                on_operation_progress(lparam);
                return 0;
            case axiom::gui::kUpdateCheckCompleteMessage:
                on_update_check_complete(lparam);
                return 0;
            case axiom::gui::kUpdateDownloadCompleteMessage:
                on_update_download_complete(lparam);
                return 0;
            case kBrowserLoadedMessage:
                on_browser_loaded(lparam);
                return 0;
            case kTableActivateMessage:
                on_table_activate();
                return 0;
            case kTableSelectionChangedMessage:
                on_table_selection_changed();
                return 0;
            case kTableParentMessage:
                on_navigate_up();
                return 0;
            case kTableSortMessage:
                on_table_sort(static_cast<int>(wparam));
                return 0;
            case kDirectoryChangedMessage:
                // Coalesce bursts from file copies and archive creation into one reload.
                // While an archive operation is active, its growing output would otherwise
                // rebuild the entire browser repeatedly and visibly flash the main window.
                if (busy_) return 0;
                KillTimer(hwnd_, kDirectoryRefreshTimer);
                SetTimer(hwnd_, kDirectoryRefreshTimer, 300, nullptr);
                return 0;
            case WM_GETMINMAXINFO: {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
                limits->ptMinTrackSize.x = scale(760);
                limits->ptMinTrackSize.y = scale(480);
                return 0;
            }
            case WM_CLOSE:
                save_current_settings();
                DestroyWindow(hwnd_);
                return 0;
            case WM_DESTROY:
                KillTimer(hwnd_, kDirectoryRefreshTimer);
                directory_watcher_.stop();
                browser_thread_.request_stop();
                operation_runner_.cancel();
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        MainWindow* window = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            window = static_cast<MainWindow*>(create->lpCreateParams);
            window->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        } else {
            window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (window != nullptr) {
            return window->handle_message(message, wparam, lparam);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    axiom::gui::CustomMenuBar menu_bar_;
    HWND add_files_ = nullptr;
    HWND open_archive_ = nullptr;
    HWND extract_ = nullptr;
    HWND test_ = nullptr;
    HWND navigate_back_ = nullptr;
    HWND navigate_forward_ = nullptr;
    HWND navigate_up_ = nullptr;
    HWND navigate_refresh_ = nullptr;
    HWND address_edit_ = nullptr;
    HWND address_go_ = nullptr;
    HWND view_ = nullptr;
    HWND delete_ = nullptr;
    HWND info_ = nullptr;
    HWND settings_ = nullptr;
    HWND tooltip_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    DarkTableView table_;
    axiom::gui::OperationProgressWindow operation_window_;
    ThemePalette theme_;
    HBRUSH window_brush_ = nullptr;
    HBRUSH panel_brush_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    HFONT ui_font_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    int selected_level_ = 5;
    bool busy_ = false;
    bool operation_paused_ = false;
    bool update_check_in_progress_ = false;
    bool update_download_in_progress_ = false;
    axiom::gui::OperationRunner operation_runner_;
    fs::path operation_archive_output_;
    RECT address_edit_frame_{};
    std::vector<HWND> transient_labels_;
    std::vector<fs::path> inputs_;
    axiom::gui::NavigationHistory history_;
    std::vector<axiom::gui::BrowserItem> browser_items_;
    std::shared_ptr<const axiom::gui::ArchiveCatalog> archive_catalog_;
    axiom::gui::DirectoryWatcher directory_watcher_;
    std::jthread browser_thread_;
    std::uint64_t browser_generation_ = 0;
    int sort_column_ = 0;
    bool sort_ascending_ = true;
    std::wstring initial_path_;
    axiom::gui::ApplicationDialogOptions application_options_;
    axiom::gui::PersistedGuiSettings persisted_settings_;
    std::size_t selected_thread_count_ = 0;
    fs::path pending_archive_path_;
    axiom::gui::ArchiveFeatureOptions pending_archive_features_;
};

}  // namespace

int run_axiom_gui(HINSTANCE instance, int show_command, std::wstring initial_path) {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) {
        axiom::gui::show_message_dialog(
            nullptr, instance, GetDpiForSystem(), system_prefers_dark_mode(),
            L"Axiom", L"Failed to initialize COM.",
            axiom::gui::MessageDialogIcon::error);
        return 1;
    }

    MainWindow window;
    if (!window.create(instance, show_command, std::move(initial_path))) {
        CoUninitialize();
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (window.translate_menu_message(message)) continue;
        if (message.message == WM_KEYDOWN) {
            HWND root = GetAncestor(message.hwnd, GA_ROOT);
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (message.wParam == VK_RETURN && GetDlgCtrlID(message.hwnd) == kAddressEdit) {
                SendMessageW(root, WM_COMMAND, MAKEWPARAM(kAddressGo, BN_CLICKED), 0);
                continue;
            }
            if (message.wParam == VK_F5) {
                SendMessageW(root, WM_COMMAND, MAKEWPARAM(kNavigateRefresh, BN_CLICKED), 0);
                continue;
            }
            if ((GetKeyState(VK_MENU) & 0x8000) != 0 && message.wParam == VK_LEFT) {
                SendMessageW(root, WM_COMMAND, MAKEWPARAM(kNavigateBack, BN_CLICKED), 0);
                continue;
            }
            if ((GetKeyState(VK_MENU) & 0x8000) != 0 && message.wParam == VK_RIGHT) {
                SendMessageW(root, WM_COMMAND, MAKEWPARAM(kNavigateForward, BN_CLICKED), 0);
                continue;
            }
            if (control && message.wParam == 'L') {
                HWND address = GetDlgItem(root, kAddressEdit);
                SetFocus(address);
                SendMessageW(address, EM_SETSEL, 0, -1);
                continue;
            }
            UINT command = 0;
            if (control) {
                switch (message.wParam) {
                    case 'A': command = kSelectAll; break;
                    case 'E': command = kExtract; break;
                    case 'I': command = kInfo; break;
                    case 'N': command = kAddFiles; break;
                    case 'O': command = kOpenArchive; break;
                    case 'T': command = kTest; break;
                }
            } else if (message.wParam == VK_DELETE) {
                command = kDelete;
            } else if (message.wParam == VK_F1) {
                command = kAbout;
            }
            if (command != 0) {
                SendMessageW(root, WM_COMMAND, MAKEWPARAM(command, 0), 0);
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    return static_cast<int>(message.wParam);
}
