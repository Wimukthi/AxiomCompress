#define NOMINMAX
#include "axiom/archive.hpp"
#include "axiom/axiom.hpp"
#include "gui/operation_runner.hpp"

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
#include <ctime>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
#endif

enum ControlId : int {
    kArchiveEdit = 1001,
    kArchiveBrowse = 1002,
    kOutputEdit = 1003,
    kOutputBrowse = 1004,
    kAddFiles = 1005,
    kAddFolder = 1006,
    kClear = 1007,
    kOpenArchive = 1008,
    kCompress = 1009,
    kExtract = 1010,
    kTest = 1011,
    kLevelDown = 1012,
    kThreadsEdit = 1013,
    kOverwriteCheck = 1014,
    kList = 1015,
    kProgress = 1016,
    kStatus = 1017,
    kLevelValue = 1018,
    kLevelUp = 1019,
    kPauseOperation = 1020,
    kCancelOperation = 1021,
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

std::wstring format_time(std::int64_t seconds) {
    if (seconds == 0) {
        return L"-";
    }

    const auto value = static_cast<std::time_t>(seconds);
    std::tm parts{};
    if (localtime_s(&parts, &value) != 0) {
        return L"-";
    }

    wchar_t buffer[32]{};
    if (std::wcsftime(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%Y-%m-%d %H:%M", &parts) == 0) {
        return L"-";
    }
    return buffer;
}

std::wstring format_file_time(const fs::path& path) {
    std::error_code error;
    const auto write_time = fs::last_write_time(path, error);
    if (error) {
        return L"-";
    }

    const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        write_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const auto seconds = std::chrono::system_clock::to_time_t(system_time);
    return format_time(static_cast<std::int64_t>(seconds));
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

std::optional<fs::path> pick_folder(HWND owner, const wchar_t* title) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        return std::nullopt;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);

    if (FAILED(dialog->Show(owner))) {
        return std::nullopt;
    }

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) {
        return std::nullopt;
    }
    return shell_item_path(item.get());
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

std::optional<fs::path> pick_save_archive(HWND owner) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        return std::nullopt;
    }

    set_axar_filter(dialog.get());
    dialog->SetFileName(L"archive.axar");

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
        case kArchiveBrowse:
        case kOutputBrowse:
        case kAddFiles:
        case kAddFolder:
        case kClear:
        case kOpenArchive:
        case kCompress:
        case kExtract:
        case kTest:
        case kLevelDown:
        case kLevelValue:
        case kLevelUp:
        case kPauseOperation:
        case kCancelOperation:
        case kOverwriteCheck:
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

    void clear() {
        rows_.clear();
        selected_row_ = -1;
        scroll_y_ = 0;
        invalidate();
    }

    void append_row(std::vector<std::wstring> row) {
        rows_.push_back(std::move(row));
        clamp_scroll();
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
            DrawTextW(dc, columns_[i].title.c_str(), -1, &cell,
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
            const bool selected = row == selected_row_;
            fill_solid_rect(dc, row_rect, selected ? theme_.selection : theme_.edit);

            x = rows_clip.left;
            for (std::size_t col = 0; col < columns_.size(); ++col) {
                const int next_x = std::min(static_cast<int>(rows_clip.right), x + widths[col]);
                RECT cell{x + scale(7), row_rect.top, next_x - scale(7), row_rect.bottom};
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
                selected_row_ = row >= 0 && row < static_cast<int>(rows_.size()) ? row : -1;
                invalidate();
                return 0;
            }
            case WM_MOUSEMOVE:
                if (dragging_scrollbar_) {
                    RECT client{};
                    GetClientRect(hwnd_, &client);
                    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                    set_scroll_from_thumb(point, client);
                    return 0;
                }
                break;
            case WM_LBUTTONUP:
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
                        selected_row_ = std::min(static_cast<int>(rows_.size()) - 1, selected_row_ + 1);
                        ensure_selected_visible();
                        invalidate();
                        return 0;
                    }
                    if (wparam == VK_UP) {
                        selected_row_ = std::max(0, selected_row_ < 0 ? 0 : selected_row_ - 1);
                        ensure_selected_visible();
                        invalidate();
                        return 0;
                    }
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
    bool dragging_scrollbar_ = false;
    int drag_offset_y_ = 0;
    std::vector<TableColumn> columns_;
    std::vector<std::vector<std::wstring>> rows_;
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
        }
        invalidate();
    }

    void set_progress(std::uint64_t completed, std::uint64_t total) {
        if (total == 0) {
            determinate_ = false;
        } else {
            determinate_ = true;
            fraction_ = std::clamp(static_cast<double>(completed) / static_cast<double>(total), 0.0, 1.0);
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
        }
        EndPaint(hwnd_, &paint);
    }

    LRESULT handle_message(UINT message, WPARAM, LPARAM) {
        switch (message) {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
                on_paint();
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
};

class MainWindow {
public:
    ~MainWindow() {
        reset_theme_brushes();
        reset_font();
    }

    bool create(HINSTANCE instance, int show_command) {
        instance_ = instance;
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

        ShowWindow(hwnd_, show_command);
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
            archive_edit_, archive_browse_, output_edit_, output_browse_,
            add_files_, add_folder_, clear_, open_archive_, compress_, extract_, test_,
            level_down_, level_value_, level_up_, threads_edit_, overwrite_,
            pause_operation_, cancel_operation_, list_, progress_, status_,
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
            archive_edit_, archive_browse_, output_edit_, output_browse_,
            add_files_, add_folder_, clear_, open_archive_, compress_, extract_, test_,
            level_down_, level_value_, level_up_, threads_edit_, overwrite_,
            pause_operation_, cancel_operation_, list_, progress_, status_,
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

    void draw_check_mark(HDC dc, RECT rect, COLORREF color) const {
        HPEN pen = CreatePen(PS_SOLID, std::max(1, scale(2)), color);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        const int left = rect.left + scale(4);
        const int mid_y = rect.top + ((rect.bottom - rect.top) / 2);
        MoveToEx(dc, left, mid_y, nullptr);
        LineTo(dc, left + scale(4), mid_y + scale(4));
        LineTo(dc, left + scale(11), mid_y - scale(5));
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }

    void draw_owner_button(const DRAWITEMSTRUCT& draw) const {
        if (!is_button_id(draw.CtlID)) {
            return;
        }

        const bool checkbox = draw.CtlID == kOverwriteCheck;
        const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
        const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
        const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;
        const bool focused = (draw.itemState & ODS_FOCUS) != 0;
        const bool checked = checkbox && SendMessageW(draw.hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;

        RECT rect = draw.rcItem;
        const COLORREF button_color = pressed ? theme_.button_pressed :
            (hot ? theme_.button_hot : theme_.button);
        fill_rect(draw.hDC, rect, checkbox ? theme_.window : button_color);

        HFONT font = ui_font_ != nullptr ? ui_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(draw.hDC, font);
        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, disabled ? theme_.muted_text : theme_.text);

        if (checkbox) {
            const int box_size = scale(16);
            RECT box{
                rect.left,
                rect.top + ((rect.bottom - rect.top - box_size) / 2),
                rect.left + box_size,
                rect.top + ((rect.bottom - rect.top - box_size) / 2) + box_size,
            };
            fill_rect(draw.hDC, box, theme_.edit);
            frame_rect(draw.hDC, box, focused ? theme_.button_hot : theme_.border);
            if (checked) {
                draw_check_mark(draw.hDC, box, disabled ? theme_.muted_text : theme_.text);
            }

            std::wstring text = get_text(draw.hwndItem);
            RECT text_rect = rect;
            text_rect.left += box_size + scale(7);
            DrawTextW(draw.hDC, text.c_str(), -1, &text_rect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
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
        HWND edits[] = {archive_edit_, output_edit_, threads_edit_};
        for (HWND edit : edits) {
            if (edit != nullptr) {
                SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, margins);
            }
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
        draw_edit_frame(dc, archive_edit_, archive_edit_frame_);
        draw_edit_frame(dc, output_edit_, output_edit_frame_);
        draw_edit_frame(dc, threads_edit_, threads_edit_frame_);
        EndPaint(hwnd_, &paint);
    }

    void update_level_label() {
        if (level_value_ != nullptr) {
            set_text(level_value_, L"Level " + std::to_wstring(selected_level_));
            InvalidateRect(level_value_, nullptr, TRUE);
        }
    }

    void adjust_level(int delta) {
        selected_level_ = std::clamp(selected_level_ + delta, 1, 9);
        update_level_label();
    }

    void on_create() {
        update_dpi(GetDpiForWindow(hwnd_));

        archive_edit_ = make_control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                                     kArchiveEdit);
        archive_browse_ = make_control(L"BUTTON", L"...", WS_TABSTOP | BS_OWNERDRAW, kArchiveBrowse);
        output_edit_ = make_control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                                    kOutputEdit);
        output_browse_ = make_control(L"BUTTON", L"...", WS_TABSTOP | BS_OWNERDRAW, kOutputBrowse);

        add_files_ = make_control(L"BUTTON", L"Add files", WS_TABSTOP | BS_OWNERDRAW, kAddFiles);
        add_folder_ = make_control(L"BUTTON", L"Add folder", WS_TABSTOP | BS_OWNERDRAW, kAddFolder);
        clear_ = make_control(L"BUTTON", L"Clear", WS_TABSTOP | BS_OWNERDRAW, kClear);
        open_archive_ = make_control(L"BUTTON", L"Open archive", WS_TABSTOP | BS_OWNERDRAW, kOpenArchive);
        compress_ = make_control(L"BUTTON", L"Compress", WS_TABSTOP | BS_OWNERDRAW, kCompress);
        extract_ = make_control(L"BUTTON", L"Extract", WS_TABSTOP | BS_OWNERDRAW, kExtract);
        test_ = make_control(L"BUTTON", L"Test", WS_TABSTOP | BS_OWNERDRAW, kTest);
        overwrite_ = make_control(L"BUTTON", L"Overwrite on extract",
                                  WS_TABSTOP | BS_AUTOCHECKBOX | BS_OWNERDRAW, kOverwriteCheck);
        pause_operation_ = make_control(L"BUTTON", L"Pause", WS_TABSTOP | BS_OWNERDRAW, kPauseOperation);
        cancel_operation_ = make_control(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, kCancelOperation);

        level_down_ = make_control(L"BUTTON", L"-", WS_TABSTOP | BS_OWNERDRAW, kLevelDown);
        level_value_ = make_control(L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, kLevelValue);
        level_up_ = make_control(L"BUTTON", L"+", WS_TABSTOP | BS_OWNERDRAW, kLevelUp);
        update_level_label();

        threads_edit_ = make_control(L"EDIT", L"0", WS_TABSTOP | ES_NUMBER,
                                     kThreadsEdit);

        table_.create(hwnd_, instance_, kList);
        list_ = table_.hwnd();
        table_.set_columns({
            {L"Name", 260},
            {L"Type", 90},
            {L"Size", 120},
            {L"Modified", 150},
            {L"Path", 420},
        });

        progress_view_.create(hwnd_, instance_, kProgress);
        progress_ = progress_view_.hwnd();
        status_ = make_control(L"STATIC", L"Ready", SS_LEFT, kStatus);

        apply_edit_margins();
        apply_fonts();
        DragAcceptFiles(hwnd_, TRUE);
        apply_theme();
        set_busy(false);
        set_status(L"Ready. Add files/folders, choose an archive, then compress.");
    }

    void layout() {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int margin = scale(8);
        const int label_width = scale(58);
        const int button_height = scale(26);
        const int edit_height = scale(26);
        const int gap = scale(6);
        int y = margin;
        const int width = client.right - client.left;
        const int right = width - margin;

        auto label = [&](const wchar_t* text, int x, int top) {
            HWND child = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                        x, top + scale(4), label_width, edit_height,
                                        hwnd_, nullptr, instance_, nullptr);
            set_control_font(child);
            apply_theme_to_control(child);
            transient_labels_.push_back(child);
        };

        for (HWND child : transient_labels_) {
            DestroyWindow(child);
        }
        transient_labels_.clear();

        label(L"Archive", margin, y);
        move_framed_edit(archive_edit_, archive_edit_frame_,
                         margin + label_width, y,
                         right - margin - label_width - scale(38), edit_height);
        MoveWindow(archive_browse_, right - scale(32), y, scale(32), edit_height, TRUE);
        y += edit_height + gap;

        label(L"Output", margin, y);
        move_framed_edit(output_edit_, output_edit_frame_,
                         margin + label_width, y,
                         right - margin - label_width - scale(38), edit_height);
        MoveWindow(output_browse_, right - scale(32), y, scale(32), edit_height, TRUE);
        y += edit_height + gap + scale(2);

        int x = margin;
        auto place = [&](HWND child, int logical_width) {
            const int width_px = scale(logical_width);
            MoveWindow(child, x, y, width_px, button_height, TRUE);
            x += width_px + gap;
        };
        place(add_files_, 82);
        place(add_folder_, 86);
        place(clear_, 58);
        place(open_archive_, 104);
        place(compress_, 84);
        place(extract_, 74);
        place(test_, 58);

        HWND level_label = CreateWindowExW(0, L"STATIC", L"Level", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                          x + scale(8), y + scale(5), scale(38), button_height,
                                          hwnd_, nullptr, instance_, nullptr);
        set_control_font(level_label);
        apply_theme_to_control(level_label);
        transient_labels_.push_back(level_label);
        x += scale(48);
        MoveWindow(level_down_, x, y, scale(28), button_height, TRUE);
        x += scale(28) + scale(2);
        MoveWindow(level_value_, x, y, scale(78), button_height, TRUE);
        x += scale(78) + scale(2);
        MoveWindow(level_up_, x, y, scale(28), button_height, TRUE);
        x += scale(28) + gap;

        HWND threads_label = CreateWindowExW(0, L"STATIC", L"Threads", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                            x, y + scale(5), scale(54), button_height,
                                            hwnd_, nullptr, instance_, nullptr);
        set_control_font(threads_label);
        apply_theme_to_control(threads_label);
        transient_labels_.push_back(threads_label);
        x += scale(58);
        move_framed_edit(threads_edit_, threads_edit_frame_, x, y, scale(52), button_height);
        x += scale(54);
        MoveWindow(overwrite_, x, y + scale(3), scale(150), button_height, TRUE);
        y += button_height + gap;

        const int bottom_height = scale(28);
        const int progress_width = scale(170);
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
        EnableWindow(add_folder_, !busy);
        EnableWindow(clear_, !busy);
        EnableWindow(open_archive_, !busy);
        EnableWindow(compress_, !busy);
        EnableWindow(extract_, !busy);
        EnableWindow(test_, !busy);
        EnableWindow(archive_browse_, !busy);
        EnableWindow(output_browse_, !busy);
        EnableWindow(level_down_, !busy);
        EnableWindow(level_value_, !busy);
        EnableWindow(level_up_, !busy);
        EnableWindow(threads_edit_, !busy);
        EnableWindow(overwrite_, !busy);
        EnableWindow(pause_operation_, busy);
        EnableWindow(cancel_operation_, busy);
        set_text(pause_operation_, L"Pause");
        operation_paused_ = false;
        progress_view_.set_busy(busy);
    }

    int selected_level() const {
        return selected_level_;
    }

    std::size_t selected_threads() const {
        const auto text = get_text(threads_edit_);
        if (text.empty()) {
            return 0;
        }
        try {
            return static_cast<std::size_t>(std::stoull(text));
        } catch (...) {
            return 0;
        }
    }

    axiom::CompressionOptions compression_options() const {
        axiom::CompressionOptions options;
        apply_level(options, selected_level());
        options.thread_count = selected_threads();
        return options;
    }

    void refresh_input_list() {
        table_.clear();
        for (const auto& path : inputs_) {
            std::error_code error;
            const bool is_dir = fs::is_directory(path, error);
            const std::wstring type = is_dir ? L"Folder" : L"File";
            const std::wstring size = is_dir ? L"" : format_size(file_size_or_zero(path));
            table_.append_row({
                path.filename().wstring(),
                type,
                size,
                format_file_time(path),
                path.wstring(),
            });
        }
        set_status(quote_count(inputs_.size(), L"input selected", L"inputs selected"));
    }

    void add_inputs(std::vector<fs::path> paths) {
        for (auto& path : paths) {
            if (std::find(inputs_.begin(), inputs_.end(), path) == inputs_.end()) {
                inputs_.push_back(std::move(path));
            }
        }
        refresh_input_list();
    }

    void show_archive_entries(const fs::path& archive_path,
                              const std::vector<axiom::ArchiveEntry>& entries) {
        table_.clear();
        for (const auto& entry : entries) {
            const std::wstring path = widen(entry.path);
            const fs::path display_path(path);
            table_.append_row({
                display_path.filename().wstring().empty() ? path : display_path.filename().wstring(),
                entry.is_directory ? L"Folder" : L"File",
                entry.is_directory ? L"" : format_size(entry.size),
                format_time(entry.mtime),
                path,
            });
        }
        set_text(archive_edit_, archive_path.wstring());
        set_status(L"Opened " + quote_count(entries.size(), L"entry", L"entries"));
    }

    fs::path archive_path_from_edit() const {
        return fs::path(get_text(archive_edit_));
    }

    fs::path output_path_from_edit() const {
        return fs::path(get_text(output_edit_));
    }

    void on_archive_browse() {
        if (auto path = pick_save_archive(hwnd_)) {
            set_text(archive_edit_, path->wstring());
        }
    }

    void on_output_browse() {
        if (auto path = pick_folder(hwnd_, L"Choose extraction folder")) {
            set_text(output_edit_, path->wstring());
        }
    }

    void on_open_archive() {
        auto path = pick_open_archive(hwnd_);
        if (!path) {
            return;
        }

        try {
            const auto entries = axiom::list_archive(*path);
            inputs_.clear();
            show_archive_entries(*path, entries);
        } catch (const std::exception& error) {
            MessageBoxW(hwnd_, widen(error.what()).c_str(), L"Open archive failed", MB_ICONERROR);
        }
    }

    void on_add_files() {
        add_inputs(pick_files(hwnd_));
    }

    void on_add_folder() {
        if (auto path = pick_folder(hwnd_, L"Choose folder to add")) {
            add_inputs({*path});
        }
    }

    void on_clear() {
        inputs_.clear();
        table_.clear();
        set_status(L"Ready");
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
        add_inputs(std::move(paths));
    }

    void start_operation(std::wstring running,
                         std::wstring success,
                         std::function<void(std::shared_ptr<axiom::OperationControl>)> work) {
        if (busy_) {
            return;
        }

        set_busy(true);
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

        const auto archive = archive_path_from_edit();
        if (archive.empty()) {
            MessageBoxW(hwnd_, L"Choose an output .axar archive path.", L"Axiom", MB_ICONINFORMATION);
            return;
        }

        const auto inputs = inputs_;
        const auto options = compression_options();
        start_operation(L"Compressing...",
                        L"Archive created: " + archive.wstring(),
                        [inputs, archive, options](std::shared_ptr<axiom::OperationControl> operation) mutable {
                            auto run_options = options;
                            run_options.operation = std::move(operation);
                            axiom::create_archive(inputs, archive, run_options);
                        });
    }

    void on_extract() {
        const auto archive = archive_path_from_edit();
        const auto output = output_path_from_edit();
        if (archive.empty() || output.empty()) {
            MessageBoxW(hwnd_, L"Choose both an archive and an output folder.", L"Axiom", MB_ICONINFORMATION);
            return;
        }

        axiom::ExtractOptions options;
        options.thread_count = selected_threads();
        options.overwrite = SendMessageW(overwrite_, BM_GETCHECK, 0, 0) == BST_CHECKED
            ? axiom::ExtractOptions::Overwrite::overwrite
            : axiom::ExtractOptions::Overwrite::fail;

        start_operation(L"Extracting...",
                        L"Extracted to: " + output.wstring(),
                        [archive, output, options](std::shared_ptr<axiom::OperationControl> operation) mutable {
                            auto run_options = options;
                            run_options.operation = std::move(operation);
                            axiom::extract_archive(archive, output, run_options);
                        });
    }

    void on_test() {
        const auto archive = archive_path_from_edit();
        if (archive.empty()) {
            MessageBoxW(hwnd_, L"Choose an archive first.", L"Axiom", MB_ICONINFORMATION);
            return;
        }

        axiom::DecompressionOptions options;
        options.thread_count = selected_threads();
        start_operation(L"Testing archive...",
                        L"Archive integrity test passed.",
                        [archive, options](std::shared_ptr<axiom::OperationControl> operation) mutable {
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
        if (operation_paused_) {
            set_status(L"Paused. " + format_progress_text(*progress));
        } else {
            set_status(format_progress_text(*progress));
        }
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
        set_status(result->message);
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
            case WM_COMMAND:
                switch (LOWORD(wparam)) {
                    case kArchiveBrowse: on_archive_browse(); return 0;
                    case kOutputBrowse: on_output_browse(); return 0;
                    case kOpenArchive: on_open_archive(); return 0;
                    case kAddFiles: on_add_files(); return 0;
                    case kAddFolder: on_add_folder(); return 0;
                    case kClear: on_clear(); return 0;
                    case kCompress: on_compress(); return 0;
                    case kExtract: on_extract(); return 0;
                    case kTest: on_test(); return 0;
                    case kPauseOperation: on_pause_operation(); return 0;
                    case kCancelOperation: on_cancel_operation(); return 0;
                    case kLevelDown: adjust_level(-1); return 0;
                    case kLevelValue: adjust_level(selected_level_ == 9 ? -8 : 1); return 0;
                    case kLevelUp: adjust_level(1); return 0;
                    case kArchiveEdit:
                    case kOutputEdit:
                    case kThreadsEdit:
                        if (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == EN_KILLFOCUS) {
                            InvalidateRect(hwnd_, nullptr, FALSE);
                            return 0;
                        }
                        break;
                    case kOverwriteCheck:
                        if (HIWORD(wparam) == BN_CLICKED) {
                            const auto checked = SendMessageW(overwrite_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                            SendMessageW(overwrite_, BM_SETCHECK, checked ? BST_UNCHECKED : BST_CHECKED, 0);
                            InvalidateRect(overwrite_, nullptr, TRUE);
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
            case WM_DESTROY:
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
    HWND archive_edit_ = nullptr;
    HWND archive_browse_ = nullptr;
    HWND output_edit_ = nullptr;
    HWND output_browse_ = nullptr;
    HWND add_files_ = nullptr;
    HWND add_folder_ = nullptr;
    HWND clear_ = nullptr;
    HWND open_archive_ = nullptr;
    HWND compress_ = nullptr;
    HWND extract_ = nullptr;
    HWND test_ = nullptr;
    HWND level_down_ = nullptr;
    HWND level_value_ = nullptr;
    HWND level_up_ = nullptr;
    HWND threads_edit_ = nullptr;
    HWND overwrite_ = nullptr;
    HWND pause_operation_ = nullptr;
    HWND cancel_operation_ = nullptr;
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
    RECT archive_edit_frame_{};
    RECT output_edit_frame_{};
    RECT threads_edit_frame_{};
    std::vector<HWND> transient_labels_;
    std::vector<fs::path> inputs_;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
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
    if (!window.create(instance, show_command)) {
        CoUninitialize();
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    return static_cast<int>(message.wParam);
}
