#define NOMINMAX
#include "axiom/archive.hpp"
#include "axiom/axiom.hpp"
#include "gui/app.hpp"
#include "gui/archive_dialogs.hpp"
#include "gui/browser_model.hpp"
#include "gui/directory_watcher.hpp"
#include "gui/operation_runner.hpp"
#include "gui/settings_store.hpp"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
    kProgress = 1016,
    kStatus = 1017,
    kPauseOperation = 1020,
    kCancelOperation = 1021,
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

std::wstring format_duration(std::uint64_t seconds) {
    const std::uint64_t hours = seconds / 3600;
    const std::uint64_t minutes = (seconds % 3600) / 60;
    const std::uint64_t remaining = seconds % 60;
    wchar_t text[32]{};
    if (hours != 0) {
        swprintf_s(text, L"%llu:%02llu:%02llu", hours, minutes, remaining);
    } else {
        swprintf_s(text, L"%02llu:%02llu", minutes, remaining);
    }
    return text;
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

std::wstring stage_text(axiom::OperationStage stage) {
    switch (stage) {
        case axiom::OperationStage::scanning: return L"Scanning";
        case axiom::OperationStage::reading: return L"Reading";
        case axiom::OperationStage::compressing: return L"Compressing";
        case axiom::OperationStage::writing: return L"Writing";
        case axiom::OperationStage::testing: return L"Testing";
        case axiom::OperationStage::extracting: return L"Extracting";
        case axiom::OperationStage::finalizing: return L"Finalizing";
        default: return L"Working";
    }
}

std::wstring format_progress_text(const axiom::OperationProgress& progress) {
    std::wstringstream stream;
    stream << stage_text(progress.stage);
    if (progress.total_bytes > 0) {
        const double percent =
            (static_cast<double>(progress.completed_bytes) * 100.0) /
            static_cast<double>(progress.total_bytes);
        stream.setf(std::ios::fixed);
        stream.precision(1);
        stream << L"... " << percent << L"% ("
               << format_size(progress.completed_bytes) << L" / "
               << format_size(progress.total_bytes) << L')';
    } else {
        stream << L"...";
    }
    if (progress.total_items > 0) {
        stream << L"  " << progress.completed_items << L"/" << progress.total_items;
    }
    if (!progress.current_path.empty()) {
        stream << L"  " << widen(progress.current_path);
    }
    return stream.str();
}

std::uint64_t file_size_or_zero(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    return error ? 0 : static_cast<std::uint64_t>(size);
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
    }
    return theme;
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
        case kPauseOperation:
        case kCancelOperation:
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

    int content_height() const {
        return row_height() * static_cast<int>(rows_.size());
    }

    int viewport_height() const {
        RECT client{};
        GetClientRect(hwnd_, &client);
        return std::max(0, static_cast<int>(client.bottom - client.top) - header_height());
    }

    int max_scroll() const {
        return std::max(0, content_height() - viewport_height());
    }

    void clamp_scroll() {
        scroll_y_ = std::clamp(scroll_y_, 0, max_scroll());
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

    RECT scrollbar_track_rect(const RECT& client) const {
        return RECT{client.right - scrollbar_width() - scale(1),
                    client.top + header_height() + scale(1),
                    client.right - scale(1),
                    client.bottom - scale(1)};
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

    void scroll_by(int pixels) {
        scroll_y_ += pixels;
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

        RECT header{client.left + scale(1), client.top + scale(1),
                    client.right - scale(1), client.top + header_height()};
        fill_solid_rect(dc, header, theme_.panel);

        const int table_right = client.right - scale(1) -
                                (content_height() > viewport_height() ? scrollbar_width() + scale(2) : 0);
        const auto widths = column_widths(table_right - client.left - scale(1));
        int x = client.left + scale(1);
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            const int next_x = std::min(table_right, x + widths[i]);
            RECT cell{x + scale(7), header.top, next_x - scale(7), header.bottom};
            SetTextColor(dc, theme_.text);
            std::wstring title = columns_[i].title;
            if (static_cast<int>(i) == sort_column_) title += sort_ascending_ ? L"  ^" : L"  v";
            DrawTextW(dc, title.c_str(), -1, &cell,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            draw_solid_line(dc, next_x, header.top, next_x, client.bottom - scale(1), theme_.border);
            x = next_x;
            if (x >= table_right) {
                break;
            }
        }
        draw_solid_line(dc, client.left + scale(1), header.bottom - scale(1),
                        client.right - scale(1), header.bottom - scale(1), theme_.border);

        RECT rows_clip{client.left + scale(1), header.bottom, table_right, client.bottom - scale(1)};
        HRGN clip = CreateRectRgn(rows_clip.left, rows_clip.top, rows_clip.right, rows_clip.bottom);
        SelectClipRgn(dc, clip);
        DeleteObject(clip);

        const int row_h = row_height();
        const int first_row = row_h > 0 ? scroll_y_ / row_h : 0;
        int y = rows_clip.top - (row_h > 0 ? scroll_y_ % row_h : 0);
        for (int row = first_row; row < static_cast<int>(rows_.size()) && y < rows_clip.bottom; ++row) {
            RECT row_rect{rows_clip.left, y, rows_clip.right, y + row_h};
            const bool selected = row < static_cast<int>(selected_.size()) && selected_[row];
            fill_solid_rect(dc, row_rect, selected ? theme_.selection : theme_.edit);

            x = rows_clip.left;
            for (std::size_t col = 0; col < columns_.size(); ++col) {
                const int next_x = std::min(static_cast<int>(rows_clip.right), x + widths[col]);
                RECT cell{x + scale(7), row_rect.top, next_x - scale(7), row_rect.bottom};
                if (col == 0 && image_list_ != nullptr &&
                    row < static_cast<int>(icon_indices_.size()) && icon_indices_[row] >= 0) {
                    int icon_width = 0;
                    int icon_height = 0;
                    ImageList_GetIconSize(image_list_, &icon_width, &icon_height);
                    const int icon_y = row_rect.top + std::max(0, (row_h - icon_height) / 2);
                    ImageList_Draw(image_list_, icon_indices_[row], dc, cell.left, icon_y, ILD_TRANSPARENT);
                    cell.left += icon_width + scale(5);
                }
                const std::wstring empty;
                const std::wstring& text =
                    col < rows_[row].size() ? rows_[row][col] : empty;
                SetTextColor(dc, selected ? theme_.selection_text : theme_.text);
                DrawTextW(dc, text.c_str(), -1, &cell,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                x = next_x;
                if (x >= rows_clip.right) {
                    break;
                }
            }
            draw_solid_line(dc, rows_clip.left, row_rect.bottom - scale(1),
                            rows_clip.right, row_rect.bottom - scale(1), theme_.panel);
            y += row_h;
        }

        SelectClipRgn(dc, nullptr);

        if (content_height() > viewport_height()) {
            RECT track = scrollbar_track_rect(client);
            RECT thumb = scrollbar_thumb_rect(client);
            fill_solid_rect(dc, track, theme_.panel);
            fill_solid_rect(dc, thumb, dragging_scrollbar_ ? theme_.focus : theme_.selection);
        }

        if (GetFocus() == hwnd_) {
            RECT focus = client;
            InflateRect(&focus, -scale(2), -scale(2));
            frame_solid_rect(dc, focus, theme_.focus);
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
                scroll_by(-GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * row_height() * 3);
                return 0;
            case WM_LBUTTONDOWN: {
                SetFocus(hwnd_);
                RECT client{};
                GetClientRect(hwnd_, &client);
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                if (point.y < header_height()) {
                    const int table_width = std::max(0, static_cast<int>(client.right - client.left));
                    const auto widths = column_widths(table_width);
                    int boundary = 0;
                    for (int column = 0; column < static_cast<int>(widths.size()); ++column) {
                        boundary += widths[column];
                        if (column + 1 < static_cast<int>(widths.size()) &&
                            std::abs(point.x - boundary) <= scale(4)) {
                            resizing_column_ = column;
                            resize_start_x_ = point.x;
                            resize_start_width_ = columns_[column].logical_width;
                            SetCapture(hwnd_);
                            return 0;
                        }
                        if (point.x < boundary) {
                            SendMessageW(GetParent(hwnd_), kTableSortMessage,
                                         static_cast<WPARAM>(column), 0);
                            break;
                        }
                    }
                    return 0;
                }
                if (content_height() > viewport_height()) {
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
                const int y = point.y - header_height() + scroll_y_;
                const int row = row_height() > 0 ? y / row_height() : -1;
                select_row(row >= 0 && row < static_cast<int>(rows_.size()) ? row : -1,
                           (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                           (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                return 0;
            }
            case WM_LBUTTONDBLCLK:
                notify_parent(kTableActivateMessage);
                return 0;
            case WM_MOUSEMOVE:
                if (resizing_column_ >= 0) {
                    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                    const int logical_delta = MulDiv(point.x - resize_start_x_,
                                                     USER_DEFAULT_SCREEN_DPI,
                                                     static_cast<int>(dpi_));
                    columns_[resizing_column_].logical_width =
                        std::max(48, resize_start_width_ + logical_delta);
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
    int selected_row_ = -1;
    int selection_anchor_ = -1;
    bool dragging_scrollbar_ = false;
    int drag_offset_y_ = 0;
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

class DarkProgressView {
public:
    ~DarkProgressView() {
        set_busy(false);
    }

    bool create(HWND parent, HINSTANCE instance, int id) {
        if (!register_class(instance)) {
            return false;
        }

        hwnd_ = CreateWindowExW(0, class_name(), L"",
                                WS_CHILD | WS_VISIBLE,
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

    void set_dpi(UINT dpi) {
        dpi_ = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
        invalidate();
    }

    void set_theme(const ThemePalette& theme) {
        theme_ = theme;
        invalidate();
    }

    void set_busy(bool busy) {
        busy_ = busy;
        if (hwnd_ == nullptr) {
            return;
        }
        if (busy_) {
            SetTimer(hwnd_, kTimerId, 33, nullptr);
        } else {
            KillTimer(hwnd_, kTimerId);
            pulse_ = 0;
            fraction_ = 0.0;
            determinate_ = false;
            text_.clear();
        }
        invalidate();
    }

    void set_progress(std::uint64_t completed, std::uint64_t total) {
        if (total == 0) {
            determinate_ = false;
            text_.clear();
        } else {
            determinate_ = true;
            fraction_ = std::clamp(static_cast<double>(completed) / static_cast<double>(total), 0.0, 1.0);
            const auto percent = static_cast<unsigned>(fraction_ * 100.0 + 0.5);
            text_ = std::to_wstring(percent) + L"%  " + format_size(completed);
        }
        invalidate();
    }

private:
    static constexpr UINT_PTR kTimerId = 1;

    static const wchar_t* class_name() {
        return L"AxiomDarkProgressView";
    }

    static bool register_class(HINSTANCE instance) {
        static ATOM atom = 0;
        if (atom != 0) {
            return true;
        }

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance;
        wc.lpfnWndProc = &DarkProgressView::window_proc;
        wc.lpszClassName = class_name();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        atom = RegisterClassExW(&wc);
        return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    int scale(int value) const {
        return scale_for_dpi(dpi_, value);
    }

    void invalidate() const {
        if (hwnd_ != nullptr) {
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
    }

    void on_paint() {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd_, &paint);
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        fill_solid_rect(dc, rect, theme_.edit);
        frame_solid_rect(dc, rect, theme_.border);
        if (busy_) {
            RECT inner = rect;
            InflateRect(&inner, -scale(2), -scale(2));
            const int width = inner.right - inner.left;
            if (determinate_) {
                RECT filled = inner;
                filled.right = filled.left + static_cast<int>(static_cast<double>(width) * fraction_);
                fill_solid_rect(dc, filled, theme_.selection);
            } else {
                const int block_width = std::max(scale(28), width / 4);
                const int travel = width + block_width;
                const int offset = travel == 0 ? 0 : (pulse_ % travel) - block_width;
                RECT block{inner.left + offset, inner.top, inner.left + offset + block_width, inner.bottom};
                RECT clip{};
                if (IntersectRect(&clip, &inner, &block)) {
                    fill_solid_rect(dc, clip, theme_.selection);
                }
            }
            if (!text_.empty()) {
                HFONT font = font_ != nullptr
                    ? font_
                    : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
                HGDIOBJ old_font = SelectObject(dc, font);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, theme_.selection_text);
                DrawTextW(dc, text_.c_str(), -1, &rect,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(dc, old_font);
            }
        }
        EndPaint(hwnd_, &paint);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM) {
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
            case WM_TIMER:
                pulse_ += scale(5);
                invalidate();
                return 0;
        }
        return DefWindowProcW(hwnd_, message, 0, 0);
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        DarkProgressView* view = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            view = static_cast<DarkProgressView*>(create->lpCreateParams);
            view->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
        } else {
            view = reinterpret_cast<DarkProgressView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (view != nullptr) {
            return view->handle_message(message, wparam, lparam);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    ThemePalette theme_ = make_theme();
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    bool busy_ = false;
    bool determinate_ = false;
    double fraction_ = 0.0;
    int pulse_ = 0;
    HFONT font_ = nullptr;
    std::wstring text_;
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
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
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
            pause_operation_, cancel_operation_, list_, progress_, status_,
            navigate_back_, navigate_forward_, navigate_up_, navigate_refresh_,
            address_edit_, address_go_, view_, delete_, info_, settings_,
        };
        for (HWND control : controls) {
            set_control_font(control);
        }
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
        table_.set_dpi(dpi_);
        progress_view_.set_dpi(dpi_);
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
            pause_operation_, cancel_operation_, list_, progress_, status_,
            navigate_back_, navigate_forward_, navigate_up_, navigate_refresh_,
            address_edit_, address_go_, view_, delete_, info_, settings_,
        };
        for (HWND control : controls) {
            apply_theme_to_control(control);
        }
        table_.set_theme(theme_);
        progress_view_.set_theme(theme_);

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
        DrawTextW(draw.hDC, text.c_str(), -1, &rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

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

    void paint_shell() {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd_, &paint);
        draw_edit_frame(dc, address_edit_, address_edit_frame_);
        EndPaint(hwnd_, &paint);
    }

    void on_create() {
        update_dpi(GetDpiForWindow(hwnd_));

        add_files_ = make_control(L"BUTTON", L"Add", WS_TABSTOP | BS_OWNERDRAW, kAddFiles);
        open_archive_ = make_control(L"BUTTON", L"Open archive", WS_TABSTOP | BS_OWNERDRAW, kOpenArchive);
        extract_ = make_control(L"BUTTON", L"Extract", WS_TABSTOP | BS_OWNERDRAW, kExtract);
        test_ = make_control(L"BUTTON", L"Test", WS_TABSTOP | BS_OWNERDRAW, kTest);
        pause_operation_ = make_control(L"BUTTON", L"Pause", WS_TABSTOP | BS_OWNERDRAW, kPauseOperation);
        cancel_operation_ = make_control(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, kCancelOperation);

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

        progress_view_.create(hwnd_, instance_, kProgress);
        progress_ = progress_view_.hwnd();
        status_ = make_control(L"STATIC", L"Ready", SS_LEFT, kStatus);
        // Custom controls do not expose painted content to accessibility tools, so give
        // each HWND a stable semantic name for UI Automation and screen readers.
        SetWindowTextW(list_, L"Files and archive contents");
        SetWindowTextW(progress_, L"Operation progress");

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
    }

    void layout() {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int margin = scale(8);
        const int button_height = scale(30);
        const int edit_height = scale(30);
        const int gap = scale(6);
        int y = margin;
        const int width = client.right - client.left;
        const int right = width - margin;

        for (HWND child : transient_labels_) {
            DestroyWindow(child);
        }
        transient_labels_.clear();

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
        place(navigate_up_, 48);
        place(navigate_refresh_, 72);
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
        const int progress_width = scale(250);
        const int operation_button_width = scale(70);
        const int list_height = std::max(scale(80), static_cast<int>(client.bottom) - y - bottom_height - margin);
        MoveWindow(list_, margin, y, width - 2 * margin,
                   list_height, TRUE);
        const int bottom_y = client.bottom - bottom_height;
        const int cancel_x = right - operation_button_width;
        const int pause_x = cancel_x - gap - operation_button_width;
        MoveWindow(progress_, margin, bottom_y, progress_width, scale(18), TRUE);
        MoveWindow(pause_operation_, pause_x, bottom_y - scale(2),
                   operation_button_width, button_height, TRUE);
        MoveWindow(cancel_operation_, cancel_x, bottom_y - scale(2),
                   operation_button_width, button_height, TRUE);
        const int status_width = std::max(scale(80), pause_x - margin - progress_width - (2 * gap));
        MoveWindow(status_, margin + progress_width + gap, bottom_y + scale(2),
                   status_width, scale(20), TRUE);
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
        EnableWindow(pause_operation_, busy);
        EnableWindow(cancel_operation_, busy);
        EnableWindow(delete_, !busy);
        EnableWindow(settings_, !busy);
        set_text(pause_operation_, L"Pause");
        operation_paused_ = false;
        progress_view_.set_busy(busy);
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
            MessageBoxW(hwnd_, L"The location does not exist or cannot be opened.",
                        L"Axiom", MB_ICONWARNING);
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
            MessageBoxW(hwnd_, L"Viewing a file directly from an archive requires selective extraction.",
                        L"Axiom", MB_ICONINFORMATION);
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

    void create_archive_from_paths(std::vector<fs::path> paths) {
        if (paths.empty()) return;

        axiom::gui::CreateArchiveDialogOptions dialog_options;
        dialog_options.level = application_options_.default_level;
        dialog_options.thread_count = application_options_.default_thread_count;
        const fs::path base = history_.current().kind == axiom::gui::BrowserLocationKind::filesystem
            ? history_.current().filesystem_path
            : paths.front().parent_path();
        dialog_options.archive_path = base / L"Archive.axar";
        if (!axiom::gui::show_create_archive_dialog(hwnd_, paths.size(), dialog_options)) return;

        inputs_ = std::move(paths);
        selected_level_ = dialog_options.level;
        selected_thread_count_ = dialog_options.thread_count;
        pending_archive_path_ = std::move(dialog_options.archive_path);
        on_compress();
    }

    void on_add_to_archive() {
        auto paths = selected_filesystem_paths();
        if (paths.empty()) paths = pick_files(hwnd_);
        create_archive_from_paths(std::move(paths));
    }

    void on_view() {
        on_table_activate();
    }

    void on_delete_selected() {
        const auto paths = selected_filesystem_paths();
        if (paths.empty()) {
            MessageBoxW(hwnd_, L"Select one or more filesystem items to delete.",
                        L"Axiom", MB_ICONINFORMATION);
            return;
        }
        const std::wstring prompt = L"Move " +
            quote_count(paths.size(), L"selected item", L"selected items") +
            L" to the Recycle Bin?";
        if (application_options_.confirm_delete &&
            MessageBoxW(hwnd_, prompt.c_str(), L"Axiom", MB_YESNO | MB_ICONWARNING) != IDYES) return;

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
            MessageBoxW(hwnd_, L"Windows could not move the selected items to the Recycle Bin.",
                        L"Delete failed", MB_ICONERROR);
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
        if (const auto archive = active_archive_path()) message += L"\nArchive: " + archive->wstring();
        MessageBoxW(hwnd_, message.c_str(), L"Information", MB_ICONINFORMATION);
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

        set_busy(true);
        operation_started_ = std::chrono::steady_clock::now();
        set_status(running);
        if (!operation_runner_.start(hwnd_, kOperationDoneMessage, kOperationProgressMessage,
                                     std::move(running), std::move(success), std::move(work))) {
            set_busy(false);
            set_status(L"Another operation is already running.");
        }
    }

    void on_compress() {
        if (inputs_.empty()) {
            MessageBoxW(hwnd_, L"Add at least one file or folder first.", L"Axiom", MB_ICONINFORMATION);
            return;
        }

        const auto archive = pending_archive_path_;
        if (archive.empty()) {
            MessageBoxW(hwnd_, L"Choose an output .axar archive path.", L"Axiom", MB_ICONINFORMATION);
            return;
        }

        const auto inputs = inputs_;
        const auto options = compression_options();
        operation_archive_output_ = archive;
        start_operation(L"Compressing...",
                        L"Archive created: " + archive.wstring(),
                        [inputs, archive, options](std::shared_ptr<axiom::OperationControl> operation) mutable {
                            auto run_options = options;
                            run_options.operation = std::move(operation);
                            axiom::create_archive(inputs, archive, run_options);
                        });
    }

    void on_extract() {
        const auto archive = active_archive_path();
        if (!archive) {
            MessageBoxW(hwnd_, L"Open or select an Axiom archive first.",
                        L"Axiom", MB_ICONINFORMATION);
            return;
        }
        axiom::gui::ExtractArchiveDialogOptions dialog_options;
        dialog_options.thread_count = application_options_.default_thread_count;
        dialog_options.destination = archive->parent_path() / archive->stem();
        if (!axiom::gui::show_extract_archive_dialog(hwnd_, *archive, dialog_options)) return;

        axiom::ExtractOptions options;
        options.thread_count = dialog_options.thread_count;
        options.restore_mtime = dialog_options.restore_mtime;
        options.overwrite = dialog_options.overwrite
            ? axiom::ExtractOptions::Overwrite::overwrite
            : axiom::ExtractOptions::Overwrite::fail;

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
            MessageBoxW(hwnd_, L"Open or select an Axiom archive first.", L"Axiom", MB_ICONINFORMATION);
            return;
        }

        axiom::DecompressionOptions options;
        options.thread_count = application_options_.default_thread_count;
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
        if (!busy_) {
            return;
        }
        progress_view_.set_progress(progress->completed_bytes, progress->total_bytes);
        std::wstring status = format_progress_text(*progress);
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - operation_started_).count();
        if (elapsed >= 0.25 && progress->completed_bytes != 0) {
            const auto bytes_per_second = static_cast<std::uint64_t>(
                static_cast<double>(progress->completed_bytes) / elapsed);
            status += L" | " + format_size(bytes_per_second) + L"/s";
            if (progress->total_bytes > progress->completed_bytes && bytes_per_second != 0) {
                const auto remaining = (progress->total_bytes - progress->completed_bytes) /
                                       bytes_per_second;
                status += L" | ETA " + format_duration(remaining);
            }
        }
        if (!operation_archive_output_.empty()) {
            fs::path temporary = operation_archive_output_;
            temporary += L".tmp";
            const std::uint64_t output_bytes = file_size_or_zero(temporary);
            if (output_bytes != 0) {
                status += L" | Output " + format_size(output_bytes);
                if (progress->completed_bytes != 0) {
                    std::wstringstream ratio;
                    ratio.setf(std::ios::fixed);
                    ratio.precision(2);
                    ratio << static_cast<double>(progress->completed_bytes) /
                                 static_cast<double>(output_bytes);
                    status += L" | " + ratio.str() + L"x";
                }
            }
        }
        if (operation_paused_) status = L"Paused. " + status;
        set_status(status);
    }

    void on_pause_operation() {
        if (!busy_ || !operation_runner_.running()) {
            return;
        }
        operation_paused_ = !operation_paused_;
        operation_runner_.set_paused(operation_paused_);
        set_text(pause_operation_, operation_paused_ ? L"Resume" : L"Pause");
        InvalidateRect(pause_operation_, nullptr, TRUE);
        set_status(operation_paused_ ? L"Paused." : L"Resuming...");
    }

    void on_cancel_operation() {
        if (!busy_ || !operation_runner_.running()) {
            return;
        }
        operation_runner_.cancel();
        EnableWindow(cancel_operation_, FALSE);
        EnableWindow(pause_operation_, FALSE);
        set_status(L"Cancelling...");
    }

    void on_operation_done(LPARAM lparam) {
        std::unique_ptr<axiom::gui::OperationResult> result(
            reinterpret_cast<axiom::gui::OperationResult*>(lparam));
        operation_runner_.finish();
        set_busy(false);
        operation_archive_output_.clear();
        set_status(result->message);
        on_navigate_refresh();
        if (result->cancelled) {
            return;
        }
        MessageBoxW(hwnd_, result->message.c_str(), result->ok ? L"Axiom" : L"Axiom error",
                    result->ok ? MB_ICONINFORMATION : MB_ICONERROR);
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
                    draw_owner_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
                    return TRUE;
                }
                break;
            case WM_DROPFILES:
                on_drop_files(reinterpret_cast<HDROP>(wparam));
                return 0;
            case WM_TIMER:
                if (wparam == kDirectoryRefreshTimer) {
                    KillTimer(hwnd_, kDirectoryRefreshTimer);
                    on_navigate_refresh();
                    return 0;
                }
                break;
            case WM_COMMAND:
                switch (LOWORD(wparam)) {
                    case kOpenArchive: on_open_archive(); return 0;
                    case kAddFiles: on_add_to_archive(); return 0;
                    case kExtract: on_extract(); return 0;
                    case kTest: on_test(); return 0;
                    case kPauseOperation: on_pause_operation(); return 0;
                    case kCancelOperation: on_cancel_operation(); return 0;
                    case kNavigateBack: on_navigate_back(); return 0;
                    case kNavigateForward: on_navigate_forward(); return 0;
                    case kNavigateUp: on_navigate_up(); return 0;
                    case kNavigateRefresh: on_navigate_refresh(); return 0;
                    case kAddressGo: on_address_go(); return 0;
                    case kView: on_view(); return 0;
                    case kDelete: on_delete_selected(); return 0;
                    case kInfo: on_info(); return 0;
                    case kSettings: on_settings(); return 0;
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
    HWND add_files_ = nullptr;
    HWND open_archive_ = nullptr;
    HWND extract_ = nullptr;
    HWND test_ = nullptr;
    HWND pause_operation_ = nullptr;
    HWND cancel_operation_ = nullptr;
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
    HWND list_ = nullptr;
    HWND progress_ = nullptr;
    HWND status_ = nullptr;
    DarkTableView table_;
    DarkProgressView progress_view_;
    ThemePalette theme_;
    HBRUSH window_brush_ = nullptr;
    HBRUSH panel_brush_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    HFONT ui_font_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    int selected_level_ = 5;
    bool busy_ = false;
    bool operation_paused_ = false;
    axiom::gui::OperationRunner operation_runner_;
    std::chrono::steady_clock::time_point operation_started_{};
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
        MessageBoxW(nullptr, L"Failed to initialize COM.", L"Axiom", MB_ICONERROR);
        return 1;
    }

    MainWindow window;
    if (!window.create(instance, show_command, std::move(initial_path))) {
        CoUninitialize();
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN) {
            HWND root = GetAncestor(message.hwnd, GA_ROOT);
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
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && message.wParam == 'L') {
                HWND address = GetDlgItem(root, kAddressEdit);
                SetFocus(address);
                SendMessageW(address, EM_SETSEL, 0, -1);
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    return static_cast<int>(message.wParam);
}
