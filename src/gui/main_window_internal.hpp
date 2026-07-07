#pragma once

// Internals shared between the main-window translation units
// (main_window*.cpp). This header is private to src/gui.

#define NOMINMAX
#include "axiom/archive.hpp"
#include "axiom/axiom.hpp"
#include "gui/about_dialog.hpp"
#include "gui/app.hpp"
#include "gui/archive_dialogs.hpp"
#include "gui/archive_feature_dialogs.hpp"
#include "gui/benchmark_dialog.hpp"
#include "gui/browser_model.hpp"
#include "gui/custom_menu.hpp"
#include "gui/dialog_support.hpp"
#include "gui/directory_watcher.hpp"
#include "gui/drag_drop.hpp"
#include "gui/message_dialog.hpp"
#include "gui/operation_runner.hpp"
#include "gui/operation_progress_window.hpp"
#include "gui/resource.hpp"
#include "gui/settings_store.hpp"
#include "gui/sfx_dialog.hpp"
#include "gui/toolbar_icons.hpp"
#include "gui/update_checker.hpp"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Uxtheme.lib")

namespace axiom::gui {

namespace fs = std::filesystem;
constexpr UINT kOperationDoneMessage = WM_APP + 1;
constexpr UINT kOperationProgressMessage = WM_APP + 2;
constexpr UINT kTableActivateMessage = WM_APP + 3;
constexpr UINT kTableSelectionChangedMessage = WM_APP + 4;
constexpr UINT kBrowserLoadedMessage = WM_APP + 5;
constexpr UINT kTableParentMessage = WM_APP + 6;
constexpr UINT kTableSortMessage = WM_APP + 7;
constexpr UINT kDirectoryChangedMessage = WM_APP + 8;
constexpr UINT kTableBeginDragMessage = WM_APP + 9;
constexpr UINT_PTR kBrowserPopulateTimer = 42;
constexpr UINT_PTR kDirectoryRefreshTimer = 41;
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
#endif
enum ControlId : int {
    kAddFiles = 1005,
    kOpenArchive = 1008,
    kExtract = 1010,
    kTest = 1011,
    kTree = 1014,
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
    kBenchmark = 1132,
    kFind = 1133,
    kCopyPath = 1134,
    kCopyCrc32 = 1135,
    kAddFavorite = 1136,
    kRemoveFavorite = 1137,
    kToggleTreePane = 1138,
    kFocusAddress = 1139,
    kTreeOpen = 1140,
    kTreeRefresh = 1141,
    kTreeExpand = 1142,
    kTreeCollapse = 1143,
    kTreeOpenInExplorer = 1144,
    kTreeAddToArchive = 1145,
    kTreeExtractArchive = 1146,
    kTreeTestArchive = 1147,
    kTreeArchiveInfo = 1148,
    kTreeAddFavorite = 1149,
    kTreeRemoveFavorite = 1150,
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
std::wstring widen(std::string_view text, UINT code_page = CP_UTF8);
std::string utf8(std::wstring_view text);
void secure_clear(std::wstring& text);
void secure_clear(std::string& text);
std::wstring get_text(HWND hwnd);
void set_text(HWND hwnd, const std::wstring& text);
std::wstring quote_count(std::size_t count, const wchar_t* singular, const wchar_t* plural);
std::wstring format_size(std::uint64_t size);
std::wstring format_packed_size(const std::optional<std::uint64_t>& size,
                                bool estimated);
std::wstring format_ratio(std::uint64_t packed, std::uint64_t unpacked);
std::uint64_t saturated_add(std::uint64_t left, std::uint64_t right);
struct ArchiveEntryTotals {
    std::uint64_t unpacked = 0;
    std::uint64_t packed = 0;
    std::size_t files = 0;
    std::size_t directories = 0;
    bool has_packed = false;
    bool packed_estimated = false;
};
ArchiveEntryTotals summarize_archive_entries(
    const std::vector<axiom::ArchiveEntry>& entries);
std::wstring format_crc32(const std::optional<std::uint32_t>& crc);
struct ShellIconRef {
    HIMAGELIST image_list = nullptr;
    int index = -1;
};
struct AddressEntry {
    std::wstring label;
    std::wstring value;
    ShellIconRef icon;
};
struct TempDirectoryRecord {
    fs::path path;
    bool sensitive = false;
};
struct StagedArchiveEntries {
    fs::path directory;
    std::vector<fs::path> paths;
};
ShellIconRef shell_icon_for_item(const axiom::gui::BrowserItem& item);
std::optional<fs::path> known_folder_path(REFKNOWNFOLDERID folder_id);
std::wstring quote_argument(const fs::path& path);
std::optional<std::uint64_t> parse_size_setting(std::wstring text);
std::size_t configured_io_buffer_size(
    const axiom::gui::ApplicationDialogOptions& options);
bool has_executable_extension(const fs::path& path);
void wipe_file_best_effort(const fs::path& path);
void remove_temp_directory(const fs::path& path, bool sensitive);
bool key_file_contains_public_key(const fs::path& path,
                                  const std::array<std::uint8_t, 32>& public_key);
bool set_registry_string(HKEY root, const std::wstring& subkey,
                         const wchar_t* name, const std::wstring& value);
std::wstring registry_string(HKEY root, const std::wstring& subkey, const wchar_t* name);
void delete_registry_tree(HKEY root, const std::wstring& subkey);
std::wstring quoted_executable_command(const fs::path& executable,
                                       std::wstring_view arguments);
ShellIconRef shell_icon_for_path(const fs::path& path, bool drive = false);
std::wstring lowercase_extension_for_icon(const fs::path& path);
bool extension_can_have_unique_shell_icon(std::wstring_view extension);
bool item_needs_real_shell_icon(const axiom::gui::BrowserItem& item);
ShellIconRef shell_icon_for_item(const axiom::gui::BrowserItem& item);
std::optional<fs::path> shell_item_path(IShellItem* item);
void set_axar_filter(IFileDialog* dialog);
std::optional<fs::path> joined_archive_path_for_volume(const fs::path& volume);
std::vector<fs::path> pick_files(HWND owner);
std::optional<fs::path> pick_open_archive(HWND owner);
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
    COLORREF accent = GetSysColor(COLOR_HIGHLIGHT);
    COLORREF scrollbar_track = GetSysColor(COLOR_BTNFACE);
    COLORREF scrollbar_thumb = GetSysColor(COLOR_BTNSHADOW);
    COLORREF scrollbar_thumb_pressed = GetSysColor(COLOR_3DDKSHADOW);
};
bool high_contrast_enabled();
bool system_prefers_dark_mode();
COLORREF blend_color(COLORREF base, COLORREF overlay, int overlay_percent);
COLORREF readable_selection_text(COLORREF background);
ThemePalette make_theme(int preference = 0,
                        int accent_mode = 0,
                        COLORREF custom_accent = RGB(255, 185, 60));
void configure_dialog_appearance(const axiom::gui::ApplicationDialogOptions& options);
axiom::gui::OperationWindowTheme make_operation_window_theme(const ThemePalette& theme);
void set_dark_title_bar(HWND hwnd, bool dark);
bool is_button_id(UINT id);
axiom::gui::ToolbarIcon toolbar_icon_for_button(UINT id);
bool is_icon_only_button(UINT id);
int scale_for_dpi(UINT dpi, int value);
void fill_solid_rect(HDC dc, const RECT& rect, COLORREF color);
void frame_solid_rect(HDC dc, const RECT& rect, COLORREF color);
void draw_solid_line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color);
std::wstring folded_text(std::wstring_view text);
bool starts_with_folded(std::wstring_view text, std::wstring_view prefix);
bool contains_folded(std::wstring_view text, std::wstring_view needle);
class PaintBuffer {
public:
    ~PaintBuffer() {
        release();
    }

    PaintBuffer(const PaintBuffer&) = delete;
    PaintBuffer& operator=(const PaintBuffer&) = delete;

    PaintBuffer() = default;

    bool ensure(HDC reference_dc, int width, int height) {
        if (reference_dc == nullptr || width <= 0 || height <= 0) return false;
        if (dc_ == nullptr) {
            dc_ = CreateCompatibleDC(reference_dc);
            if (dc_ == nullptr) return false;
        }
        if (bitmap_ == nullptr || width > width_ || height > height_) {
            const int next_width = std::max(width, width_);
            const int next_height = std::max(height, height_);
            HBITMAP next_bitmap =
                CreateCompatibleBitmap(reference_dc, next_width, next_height);
            if (next_bitmap == nullptr) return false;
            if (bitmap_ != nullptr) {
                SelectObject(dc_, old_bitmap_);
                DeleteObject(bitmap_);
            }
            bitmap_ = next_bitmap;
            old_bitmap_ = SelectObject(dc_, bitmap_);
            width_ = next_width;
            height_ = next_height;
        }
        return dc_ != nullptr && bitmap_ != nullptr;
    }

    HDC dc() const {
        return dc_;
    }

    void release() {
        if (dc_ != nullptr) {
            if (bitmap_ != nullptr) {
                SelectObject(dc_, old_bitmap_);
                DeleteObject(bitmap_);
            }
            DeleteDC(dc_);
        }
        dc_ = nullptr;
        bitmap_ = nullptr;
        old_bitmap_ = nullptr;
        width_ = 0;
        height_ = 0;
    }

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};
struct TableColumn {
    std::wstring title;
    int logical_width = 0;
};
struct TableViewOptions {
    bool show_grid_lines = true;
    bool show_horizontal_scrollbar = true;
    bool full_row_select = true;
};
// Native report/list controls still leak light header and scrollbar pixels on some
// Windows builds, so Axiom owns every table pixel through this lightweight view.
class DarkTableView {

public:
    bool create(HWND parent, HINSTANCE instance, int id);
    HWND hwnd() const;
    void set_font(HFONT font);
    void set_dpi(UINT dpi);
    void set_theme(const ThemePalette& theme);
    void set_columns(std::vector<TableColumn> columns);
    std::vector<int> logical_column_widths() const;
    void set_image_list(HIMAGELIST image_list);
    void set_sort_indicator(int column, bool ascending);
    void set_options(TableViewOptions options);
    void clear();
    void append_row(std::vector<std::wstring> row, int icon_index = -1);
    void reserve_rows(std::size_t count);
    void append_rows(std::vector<std::vector<std::wstring>> rows,
                     std::vector<int> icon_indices,
                     HIMAGELIST image_list);
    void set_rows(std::vector<std::vector<std::wstring>> rows,
                  std::vector<int> icon_indices,
                  HIMAGELIST image_list);
    std::vector<int> selected_indices() const;
    int focused_index() const;
    int vertical_scroll_position() const;
    int horizontal_scroll_position() const;
    std::optional<RECT> cell_rect(int row, int column) const;
    void set_selection_and_scroll(std::vector<int> selected_indices,
                                  int focused_index,
                                  int horizontal_scroll,
                                  int vertical_scroll);
    void select_index(int row);
    int row_at_screen_point(POINT point) const;
    void select_all();


private:
    static const wchar_t* class_name();
    static bool register_class(HINSTANCE instance);
    int scale(int value) const;
    int header_height() const;
    int row_height() const;
    int scrollbar_width() const;
    int scrollbar_gap() const;
    int content_height() const;
    int requested_content_width() const;


    struct ScrollbarVisibility {
        bool vertical = false;
        bool horizontal = false;
    };
    ScrollbarVisibility scrollbar_visibility(const RECT& client) const;
    int rows_bottom(const RECT& client) const;
    int viewport_height(const RECT& client) const;
    int viewport_height() const;
    int max_scroll() const;
    void clamp_scroll();
    void invalidate() const;
    std::vector<int> column_widths(int available_width) const;
    int table_left(const RECT& client) const;
    int table_right(const RECT& client) const;
    std::vector<int> column_widths(const RECT& client) const;
    int max_scroll_x(const RECT& client) const;
    int max_scroll_x() const;
    int column_separator_at(POINT point, const RECT& client) const;
    RECT scrollbar_track_rect(const RECT& client) const;
    RECT scrollbar_thumb_rect(const RECT& client) const;
    RECT horizontal_scrollbar_track_rect(const RECT& client) const;
    RECT horizontal_scrollbar_thumb_rect(const RECT& client) const;
    bool point_in_rect(POINT point, const RECT& rect) const;
    void set_scroll_from_thumb(POINT point, const RECT& client);
    void set_horizontal_scroll_from_thumb(POINT point, const RECT& client);
    void scroll_by(int pixels);
    void scroll_horizontally_by(int pixels);
    void notify_parent(UINT message) const;
    void select_row(int row, bool extend, bool toggle);
    std::wstring row_match_text(int row) const;
    int find_typeahead_match(std::wstring_view needle, bool cycle) const;
    bool handle_typeahead_char(wchar_t character);
    bool point_can_select_row(POINT point, int row, const RECT& client) const;
    void begin_marquee_selection(POINT point, bool preserve_selection);
    void update_marquee_selection(POINT point);
    void end_marquee_selection();
    void ensure_selected_visible();
    void paint_content(HDC dc, const RECT& client);
    void on_paint();
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);


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

    TableViewOptions options_;

    HIMAGELIST image_list_ = nullptr;

    int sort_column_ = 0;

    bool sort_ascending_ = true;

    int resizing_column_ = -1;

    int resize_start_x_ = 0;

    int resize_start_width_ = 0;

    bool drag_candidate_ = false;

    bool drag_started_ = false;

    POINT drag_start_{};

    int collapse_selection_on_click_ = -1;

    bool marquee_selecting_ = false;

    POINT marquee_start_{};

    POINT marquee_current_{};

    std::vector<bool> marquee_base_selection_;

    std::wstring typeahead_;

    ULONGLONG typeahead_last_tick_ = 0;

    PaintBuffer paint_buffer_;

};
enum class DirectoryTreeNodeKind {
    dummy,
    computer,
    filesystem,
    file,
    archive,
    archive_directory,
};
struct DirectoryTreeNode {
    DirectoryTreeNodeKind kind = DirectoryTreeNodeKind::dummy;
    fs::path filesystem_path;
    fs::path archive_path;
    std::string archive_directory;
    bool populated = false;
};
struct DirectoryTreeItem {
    DirectoryTreeNode node;
    std::wstring text;
    ShellIconRef icon;
    DirectoryTreeItem* parent = nullptr;
    std::vector<std::unique_ptr<DirectoryTreeItem>> children;
    bool may_have_children = false;
    bool expanded = false;
    bool populated = false;
};
struct DirectoryTreeViewState {
    std::vector<std::wstring> expanded_keys;
    std::wstring selected_key;
    int vertical_scroll = 0;
};
class DarkDirectoryTreeView {

public:

    using PopulateCallback = std::function<void(DirectoryTreeItem&)>;

    using SelectCallback = std::function<void(const DirectoryTreeItem&)>;
    bool create(HWND parent, HINSTANCE instance, int id);
    HWND hwnd() const;
    void set_theme(const ThemePalette& theme);
    void set_font(HFONT font);
    void set_dpi(UINT dpi);
    void set_populate_callback(PopulateCallback callback);
    void set_select_callback(SelectCallback callback);
    void move(int x, int y, int width, int height, bool repaint = true);
    void clear();
    void begin_update();
    void end_update();
    DirectoryTreeItem* insert_item(DirectoryTreeItem* parent,
                                   std::wstring text,
                                   DirectoryTreeNode node,
                                   ShellIconRef icon,
                                   bool may_have_children);
    void clear_children(DirectoryTreeItem& item);
    void refresh();
    void select_item(DirectoryTreeItem* item, bool notify);
    DirectoryTreeItem* selected_item() const;
    void set_expanded(DirectoryTreeItem* item, bool expanded);
    void refresh_item(DirectoryTreeItem* item);
    void ensure_visible(DirectoryTreeItem* item);
    DirectoryTreeViewState capture_state() const;
    void restore_state(const DirectoryTreeViewState& state);


private:

    struct VisibleItem {
        DirectoryTreeItem* item = nullptr;
        int depth = 0;
    };
    static const wchar_t* class_name();
    static bool register_class(HINSTANCE instance);
    int scale(int value) const;
    int row_height() const;
    int indent_width() const;
    int scrollbar_width() const;
    int content_height() const;
    int viewport_height(const RECT& client) const;
    int max_scroll(const RECT& client) const;
    bool needs_scrollbar(const RECT& client) const;
    RECT content_rect(const RECT& client) const;
    RECT scrollbar_track_rect(const RECT& client) const;
    RECT scrollbar_thumb_rect(const RECT& client) const;
    void clamp_scroll();
    void scroll_by(int delta);
    void set_scroll_from_thumb(POINT point, const RECT& client);
    static bool is_ancestor_of(const DirectoryTreeItem& ancestor,
                               const DirectoryTreeItem* item);
    void flatten(DirectoryTreeItem& item, int depth);
    void rebuild_visible_items();
    int visible_index(const DirectoryTreeItem* item) const;
    void expand_ancestors(DirectoryTreeItem* item);
    void ensure_populated(DirectoryTreeItem& item);
    void toggle_item(DirectoryTreeItem& item);
    DirectoryTreeItem* item_at_point(POINT point, int* depth = nullptr, RECT* row_rect = nullptr);
    RECT glyph_rect_for_row(const RECT& row, int depth) const;
    void draw_chevron(HDC dc, const RECT& rect, bool expanded, COLORREF color) const;
    void paint_content(HDC dc, const RECT& client);
    void on_paint();
    void invalidate() const;
    void select_visible_index(int index);
    int find_typeahead_match(std::wstring_view needle, bool cycle);
    bool handle_typeahead_char(wchar_t character);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    int viewport_height_for_current() const;
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);


    HWND hwnd_ = nullptr;

    ThemePalette theme_ = make_theme();

    HFONT font_ = nullptr;

    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;

    std::vector<std::unique_ptr<DirectoryTreeItem>> roots_;

    std::vector<VisibleItem> visible_;

    DirectoryTreeItem* selected_ = nullptr;

    int scroll_y_ = 0;

    bool dragging_scrollbar_ = false;

    int drag_offset_y_ = 0;

    int update_depth_ = 0;

    bool refresh_pending_ = false;

    PopulateCallback populate_callback_;

    SelectCallback select_callback_;

    std::wstring typeahead_;

    ULONGLONG typeahead_last_tick_ = 0;

    PaintBuffer paint_buffer_;

};
struct FileSearchSourceItem {
    int browser_index = -1;
    axiom::gui::BrowserItemKind kind = axiom::gui::BrowserItemKind::file;
    std::wstring name;
    std::wstring location;
    std::wstring type;
    std::wstring size;
    std::wstring modified;
};
struct FileSearchDialogResult {
    bool accepted = false;
    int browser_index = -1;
};
class FileSearchDialog {

public:
    FileSearchDialog(HINSTANCE instance,
                     ThemePalette theme,
                     UINT dpi,
                     std::wstring scope,
                     std::vector<FileSearchSourceItem> source);
    FileSearchDialogResult show(HWND owner);


private:

    enum : int {
        kSearchText = 30100,
        kMatchCase = 30101,
        kWholeName = 30102,
        kSearchPath = 30103,
        kIncludeFiles = 30104,
        kIncludeFolders = 30105,
        kIncludeArchives = 30106,
        kSearchButton = 30107,
        kGoToButton = 30108,
        kResultsTable = 30109,
    };
    static const wchar_t* class_name();
    static bool register_class(HINSTANCE instance);
    int scale(int value) const;
    HWND make_control(const wchar_t* class_name,
                      const wchar_t* text,
                      DWORD style,
                      int id = 0,
                      DWORD ex_style = 0);
    std::array<HWND, 14> controls() const;
    bool checkbox_checked(int id) const;
    void set_checkbox(int id, bool checked);
    void toggle_checkbox(int id);
    static bool is_folder_kind(axiom::gui::BrowserItemKind kind);
    static bool is_archive_kind(axiom::gui::BrowserItemKind kind);
    static bool is_file_kind(axiom::gui::BrowserItemKind kind);
    bool type_allowed(const FileSearchSourceItem& item) const;
    bool text_matches(const FileSearchSourceItem& item, std::wstring_view query) const;
    void run_search();
    void accept_selected();
    void layout();
    void create_controls();
    LRESULT control_color(WPARAM wparam, bool edit);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);


    HWND hwnd_ = nullptr;

    HWND owner_ = nullptr;

    HINSTANCE instance_ = nullptr;

    ThemePalette theme_;

    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;

    std::wstring scope_;

    std::vector<FileSearchSourceItem> source_;

    std::vector<int> result_indices_;

    FileSearchDialogResult result_;

    HFONT font_ = nullptr;

    HBRUSH background_brush_ = nullptr;

    HBRUSH edit_brush_ = nullptr;

    bool match_case_checked_ = false;

    bool whole_name_checked_ = false;

    bool search_path_checked_ = false;

    bool include_files_checked_ = true;

    bool include_folders_checked_ = true;

    bool include_archives_checked_ = true;

    HWND title_ = nullptr;

    HWND scope_label_ = nullptr;

    HWND query_label_ = nullptr;

    HWND search_edit_ = nullptr;

    HWND match_case_ = nullptr;

    HWND whole_name_ = nullptr;

    HWND search_path_ = nullptr;

    HWND include_files_ = nullptr;

    HWND include_folders_ = nullptr;

    HWND include_archives_ = nullptr;

    HWND result_count_ = nullptr;

    HWND search_button_ = nullptr;

    HWND go_to_button_ = nullptr;

    HWND close_button_ = nullptr;

    DarkTableView results_;

};
class MainWindow {

public:
    ~MainWindow();
    bool create(HINSTANCE instance,
                int show_command,
                std::wstring initial_path,
                AxiomGuiStartupCommand startup_command = {});
    bool translate_menu_message(const MSG& message);
    bool translate_keyboard_shortcut(const MSG& message);


private:
    int scale(int value) const;
    void reset_font();
    void rebuild_font();
    void set_control_font(HWND control) const;
    void apply_fonts();
    void update_dpi(UINT dpi);
    void reset_theme_brushes();
    void rebuild_theme_brushes();
    void apply_theme_to_control(HWND control) const;
    void apply_theme();
    ShellIconRef tree_icon_for_filesystem(const fs::path& path, bool drive = false) const;
    ShellIconRef tree_icon_for_archive(const fs::path& path) const;
    DirectoryTreeItem* insert_tree_item(DirectoryTreeItem* parent,
                                        std::wstring text,
                                        DirectoryTreeNode node,
                                        ShellIconRef icon,
                                        bool may_have_children);
    bool tree_should_show_filesystem_item(const WIN32_FIND_DATAW& data) const;
    bool filesystem_has_tree_children(const fs::path& path) const;


    struct TreeChildCandidate {
        std::wstring name;
        fs::path path;
        bool directory = false;
        bool archive = false;
        bool drive = false;
    };
    void populate_tree_filesystem_children(DirectoryTreeItem& item, const fs::path& path);
    void add_known_tree_folder(DirectoryTreeItem& parent, const wchar_t* label,
                               REFKNOWNFOLDERID folder_id);
    void populate_tree_computer_children(DirectoryTreeItem& item);
    std::shared_ptr<const axiom::gui::ArchiveCatalog> tree_archive_catalog(
        const fs::path& archive);
    void populate_tree_archive_children(DirectoryTreeItem& item, const fs::path& archive,
                                        const std::string& directory);
    void populate_tree_item(DirectoryTreeItem& item);
    DirectoryTreeItem* find_tree_child_by_filesystem_path(DirectoryTreeItem& parent,
                                                          const fs::path& path) const;
    DirectoryTreeItem* find_tree_child_by_archive_directory(DirectoryTreeItem& parent,
                                                            const fs::path& archive,
                                                            const std::string& directory) const;
    DirectoryTreeItem* ensure_filesystem_tree_path(const fs::path& path);
    DirectoryTreeItem* ensure_archive_tree_path(const fs::path& archive,
                                                const std::string& directory);
    void sync_tree_to_location(const axiom::gui::BrowserLocation& location);
    void rebuild_directory_tree();
    void on_tree_selection_changed(const DirectoryTreeItem& item);
    LRESULT paint_control_background(HWND control, HDC dc, UINT message);
    void fill_rect(HDC dc, const RECT& rect, COLORREF color) const;
    void frame_rect(HDC dc, const RECT& rect, COLORREF color) const;
    void draw_owner_button(const DRAWITEMSTRUCT& draw) const;
    HWND make_control(const wchar_t* class_name,
                      const wchar_t* text,
                      DWORD style,
                      int id,
                      DWORD ex_style = 0);
    void apply_edit_margins() const;
    void add_tooltip(HWND control, const wchar_t* text) const;
    static UINT toolbar_command_for_action(std::wstring_view action);
    static int toolbar_button_width(UINT command);
    HWND toolbar_button(UINT command) const;
    void assign_toolbar_button(UINT command, HWND button);
    void create_toolbar_buttons();
    std::vector<UINT> visible_toolbar_commands() const;
    int command_toolbar_height_for_width(int width) const;
    void update_toolbar_button_states();
    int show_app_message(
        std::wstring_view message,
        axiom::gui::MessageDialogIcon icon,
        std::wstring_view title = L"Axiom",
        axiom::gui::MessageDialogButtons buttons = axiom::gui::MessageDialogButtons::ok,
        int default_result = IDOK) const;
    std::vector<axiom::gui::CustomMenuItem> menu_items(UINT menu_id) const;
    std::wstring shortcut_for_command(UINT command) const;
    bool can_execute_shortcut_command(UINT command) const;
    bool shortcut_reserved_for_focused_control(UINT command,
                                               const axiom::gui::KeyboardShortcut& shortcut,
                                               HWND target) const;
    void focus_address_bar();
    void show_browser_context_menu(POINT point);
    void show_tree_context_menu(POINT point);
    void paint_shell();
    void on_create();
    void on_initial_navigate();
    int browser_pane_top() const;
    void layout_browser_panes(const RECT& client, int y, bool invalidate_splitter_only);
    void layout_browser_panes_for_current_size();
    void repaint_browser_panes_now() const;
    void layout();
    void set_status(const std::wstring& text);
    void set_busy(bool busy);
    void toggle_tree_pane();
    int selected_level() const;
    axiom::CompressionOptions compression_options() const;
    void apply_table_options();
    void add_address_entry(std::wstring label, std::wstring value,
                           ShellIconRef icon = {});
    void add_known_address(const wchar_t* label, REFKNOWNFOLDERID folder_id);
    static bool same_location_text(const std::wstring& left, const std::wstring& right);
    static std::wstring compact_location_label(const std::wstring& value);
    ShellIconRef address_icon_for_value(const std::wstring& value) const;
    bool favorite_contains(const std::wstring& value) const;
    static void remember_limited_unique(std::vector<std::wstring>& values,
                                        std::wstring value,
                                        int limit);
    void add_favorite_location(std::wstring value);
    void remove_favorite_location(const std::wstring& value);
    std::wstring current_location_value() const;
    std::optional<std::wstring> tree_location_value(const DirectoryTreeNode& node) const;
    void populate_address_dropdown();
    void remember_address(std::wstring value);
    void remember_archive_path(const fs::path& archive);
    void select_address_entry();
    void draw_address_entry(const DRAWITEMSTRUCT& draw) const;
    void update_navigation_buttons();


    struct BrowserStatusTotals {
        std::size_t items = 0;
        std::size_t files = 0;
        std::size_t folders = 0;
        std::size_t archives = 0;
        std::size_t drives = 0;
        std::size_t links = 0;
        std::uint64_t size = 0;
        std::uint64_t packed = 0;
        bool has_size = false;
        bool has_packed = false;
        bool packed_estimated = false;
    };


    struct BrowserViewState {
        std::vector<std::uint64_t> selected_ids;
        std::optional<std::uint64_t> focused_id;
        int horizontal_scroll = 0;
        int vertical_scroll = 0;
        DirectoryTreeViewState tree;
    };
    static void add_status_size(BrowserStatusTotals& totals, std::uint64_t size);
    static void add_status_item(BrowserStatusTotals& totals,
                                const axiom::gui::BrowserItem& item);
    BrowserStatusTotals summarize_browser_status(const std::vector<int>* indices) const;
    static void append_status_part(std::vector<std::wstring>& parts, std::wstring text);
    static std::wstring join_status_parts(const std::vector<std::wstring>& parts,
                                          std::wstring_view separator = L", ");
    static std::wstring status_breakdown(const BrowserStatusTotals& totals);
    std::wstring status_location_suffix() const;
    void update_browser_status(const std::vector<int>* selected_indices = nullptr);
    void update_browser_status_for_current_selection();
    static std::wstring folded_extension_key(fs::path path);
    static std::wstring folded_path_key(fs::path path);
    static std::wstring shell_icon_cache_key(const axiom::gui::BrowserItem& item,
                                             bool prefer_generic_unique_icon = false);
    ShellIconRef cached_shell_icon_for_item(const axiom::gui::BrowserItem& item,
                                            bool prefer_generic_unique_icon = false);
    std::vector<int> selected_browser_indices() const;
    bool selected_has_crc32() const;
    BrowserViewState capture_browser_view_state() const;
    void restore_browser_view_state(const BrowserViewState& state);
    std::optional<BrowserViewState> current_or_pending_browser_view_state() const;
    void remember_current_history_view_state();
    std::optional<BrowserViewState> saved_history_view_state() const;
    std::vector<fs::path> selected_filesystem_paths() const;
    std::vector<std::string> selected_archive_paths() const;
    bool copy_text_to_clipboard(const std::wstring& text) const;
    static std::wstring newline_join(const std::vector<std::wstring>& lines);
    std::wstring display_path_for_item(const axiom::gui::BrowserItem& item) const;
    void on_copy_paths();
    void on_copy_crc32();
    static std::string archive_name(std::string_view path);
    static std::string join_archive_directory(std::string_view directory,
                                              std::string_view name);
    static bool same_filesystem_path(const fs::path& left, const fs::path& right);
    static std::wstring sanitize_archive_stem(std::wstring stem);
    static bool path_is_directory(const fs::path& path);
    static std::wstring archive_stem_for_single_input(const fs::path& path);
    static std::wstring archive_stem_for_multiple_inputs(
        const std::vector<fs::path>& paths,
        const fs::path& current_folder);
    static std::wstring suggested_archive_stem_for_inputs(
        const std::vector<fs::path>& paths,
        const fs::path& current_folder);
    static bool output_collides_with_input(const fs::path& output,
                                           const std::vector<fs::path>& paths);
    static fs::path avoid_archive_input_collision(fs::path output,
                                                  const std::vector<fs::path>& paths);
    std::string archive_drop_directory(POINT screen_point) const;
    std::optional<std::string> password_for_archive_edit(const fs::path& archive);
    void clear_archive_password();
    bool prepare_archive_password(const axiom::gui::BrowserLocation& location);
    fs::path configured_temp_base() const;
    void cleanup_old_temp_directories();
    fs::path create_drag_staging_directory(bool sensitive = false);
    DWORD query_file_drop(IDataObject* object, POINT, DWORD, DWORD allowed) const;
    DWORD perform_file_drop(IDataObject* object, POINT point, DWORD, DWORD allowed);
    StagedArchiveEntries extract_archive_entries_to_staging(
        const fs::path& archive, const std::vector<std::string>& entries,
        const std::string& password, bool for_drag, bool sensitive);
    void on_table_begin_drag();
    std::optional<fs::path> active_archive_path() const;
    axiom::gui::ArchiveCapabilities active_archive_capabilities() const;
    const axiom::ArchiveProvider* active_archive_provider() const;
    static axiom::gui::ArchiveFeatureAvailability implemented_feature_availability();
    static axiom::gui::ArchiveFeatureAvailability feature_availability_from_capabilities(
        const axiom::gui::ArchiveCapabilities& capabilities);
    void navigate_to(axiom::gui::BrowserLocation location, bool record_history = true);
    void sort_browser_items_for_table();
    std::vector<std::wstring> browser_row_for_item(
        const axiom::gui::BrowserItem& item) const;
    bool append_browser_table_batch();
    void cancel_browser_table_population();
    void finish_browser_table_population();
    void begin_browser_table_population(std::optional<BrowserViewState> restore_state);
    void on_browser_populate_timer();
    void on_table_sort(int column);
    void on_browser_loaded(LPARAM lparam);
    void on_navigate_back();
    void on_navigate_forward();
    void on_navigate_up();
    void on_navigate_refresh();
    void on_address_go();
    bool launch_viewed_archive_file(const fs::path& file, const fs::path& staging_root,
                                    bool sensitive_temp);
    bool signature_key_is_trusted(const axiom::ArchiveSignatureInfo& info) const;
    void activate_browser_item(int index);
    void on_table_activate();
    void on_table_selection_changed();
    void create_archive_from_paths(
        std::vector<fs::path> paths,
        std::optional<fs::path> target_archive = std::nullopt,
        axiom::gui::ArchiveUpdateMode update_mode =
            axiom::gui::ArchiveUpdateMode::create_new);
    void on_add_to_archive();
    void on_update_archive(axiom::gui::ArchiveUpdateMode mode);
    bool active_archive_is_editable();
    void on_view();
    void on_delete_from_archive();
    void on_repack_archive();
    void on_edit_archive_comment();
    void on_lock_archive();
    void on_delete_selected();
    void on_info();
    void on_repair_archive();
    void on_create_recovery_volumes();
    void on_about();
    void on_benchmark();
    void maybe_start_automatic_update_check();
    void begin_update_check(axiom::gui::UpdateCheckKind kind);
    void begin_update_download(const axiom::gui::UpdateInfo& update);
    void on_update_check_complete(LPARAM lparam);
    void on_update_download_complete(LPARAM lparam);
    void on_select_all();
    void on_find_files();
    void apply_application_options(
        const axiom::gui::ApplicationDialogOptions& updated_options);
    void on_settings();
    void apply_shell_integration() const;
    bool maybe_execute_sfx_archive(const fs::path& path);
    bool open_archive_path(const fs::path& path);
    void on_open_archive();
    void on_drop_files(HDROP drop);
    void save_current_settings();
    fs::path log_file_path() const;
    void append_log(const std::wstring& message) const;
    void apply_operation_priority();
    void restore_operation_priority();
    void start_operation(std::wstring running,
                         std::wstring success,
                         std::function<void(std::shared_ptr<axiom::OperationControl>)> work);
    static std::optional<std::uint64_t> parse_volume_size(
        const std::wstring& text, int unit);
    void on_compress();
    fs::path current_executable_path() const;
    void on_create_sfx();
    void on_verify_archive_signature();
    void on_extract();
    void on_test();
    void on_operation_progress(LPARAM lparam);
    void on_operation_done(LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);


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

    struct ToolbarButton {
        UINT command = 0;
        HWND window = nullptr;
    };

    std::vector<ToolbarButton> toolbar_buttons_;

    DarkDirectoryTreeView tree_view_;

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

    bool operation_priority_changed_ = false;

    DWORD previous_priority_class_ = 0;

    bool update_check_in_progress_ = false;

    bool update_download_in_progress_ = false;

    axiom::gui::OperationRunner operation_runner_;

    fs::path operation_archive_output_;

    fs::path operation_open_after_;

    std::vector<HWND> transient_labels_;

    int tree_width_ = 0;

    RECT tree_splitter_rect_{};

    bool tree_pane_visible_ = true;

    bool dragging_tree_splitter_ = false;

    int tree_splitter_start_x_ = 0;

    int tree_width_start_ = 0;

    DirectoryTreeItem* tree_computer_item_ = nullptr;

    bool syncing_tree_ = false;

    std::vector<AddressEntry> address_entries_;

    std::vector<std::wstring> recent_addresses_;

    std::vector<std::wstring> recent_archives_;

    std::vector<std::wstring> favorite_locations_;

    std::vector<fs::path> inputs_;

    axiom::gui::NavigationHistory history_;
    std::vector<std::optional<BrowserViewState>> browser_history_states_;

    std::vector<axiom::gui::BrowserItem> browser_items_;

    std::unordered_map<std::wstring, ShellIconRef> shell_icon_cache_;

    std::shared_ptr<const axiom::gui::ArchiveCatalog> archive_catalog_;

    std::optional<axiom::gui::BrowserLocation> displayed_browser_location_;

    std::optional<BrowserViewState> pending_browser_view_state_;

    std::optional<BrowserViewState> pending_table_restore_state_;

    std::size_t table_population_next_ = 0;

    std::uint64_t table_population_generation_ = 0;

    bool table_population_active_ = false;

    axiom::gui::DirectoryWatcher directory_watcher_;

    axiom::gui::OleDropTarget drop_target_;

    std::vector<TempDirectoryRecord> temp_directories_;

    fs::path archive_password_path_;

    std::string archive_password_;

    std::string pending_archive_password_;

    std::jthread browser_thread_;

    std::uint64_t browser_generation_ = 0;

    int sort_column_ = 0;

    bool sort_ascending_ = true;

    std::wstring initial_path_;

    AxiomGuiStartupCommand startup_command_;

    axiom::gui::ApplicationDialogOptions application_options_;

    axiom::gui::PersistedGuiSettings persisted_settings_;

    std::size_t selected_thread_count_ = 0;

    std::size_t selected_dictionary_size_ = 0;

    std::size_t selected_word_size_ = 0;

    std::size_t selected_solid_block_size_ = 0;

    fs::path pending_archive_path_;

    axiom::gui::ArchiveFeatureOptions pending_archive_features_;

};
int run_quick_add_to_archive(HINSTANCE instance, const std::vector<std::wstring>& paths);
std::optional<int> run_embedded_sfx(HINSTANCE instance, const std::wstring& requested_destination);


int run_quick_add_to_archive(HINSTANCE instance, const std::vector<std::wstring>& paths);
std::optional<int> run_embedded_sfx(HINSTANCE instance, const std::wstring& requested_destination);

}  // namespace axiom::gui
