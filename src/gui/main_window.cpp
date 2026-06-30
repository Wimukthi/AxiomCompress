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
constexpr UINT kTableBeginDragMessage = WM_APP + 9;
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
    kBenchmark = 1132,
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

void secure_clear(std::string& text) {
    if (!text.empty()) {
        SecureZeroMemory(text.data(), text.size());
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

std::optional<fs::path> known_folder_path(REFKNOWNFOLDERID folder_id) {
    PWSTR raw_path = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT, nullptr, &raw_path)) ||
        raw_path == nullptr) {
        return std::nullopt;
    }
    fs::path path(raw_path);
    CoTaskMemFree(raw_path);
    return path;
}

std::wstring quote_argument(const fs::path& path) {
    std::wstring text = path.wstring();
    std::wstring quoted = L"\"";
    for (wchar_t ch : text) {
        if (ch == L'"') quoted += L'\\';
        quoted += ch;
    }
    quoted += L"\"";
    return quoted;
}

std::optional<std::uint64_t> parse_size_setting(std::wstring text) {
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](wchar_t ch) {
        return !std::iswspace(ch);
    }));
    while (!text.empty() && std::iswspace(text.back())) text.pop_back();
    if (text.empty()) return std::nullopt;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long value = _wcstoui64(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || value == 0) return std::nullopt;
    while (*end != L'\0' && std::iswspace(*end)) ++end;
    std::wstring unit = end;
    std::transform(unit.begin(), unit.end(), unit.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    std::uint64_t multiplier = 1;
    if (unit.empty() || unit == L"b" || unit == L"bytes") {
        multiplier = 1;
    } else if (unit == L"k" || unit == L"kb" || unit == L"kib") {
        multiplier = 1024ull;
    } else if (unit == L"m" || unit == L"mb" || unit == L"mib") {
        multiplier = 1024ull * 1024ull;
    } else if (unit == L"g" || unit == L"gb" || unit == L"gib") {
        multiplier = 1024ull * 1024ull * 1024ull;
    } else {
        return std::nullopt;
    }
    if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        return std::nullopt;
    }
    return value * multiplier;
}

bool has_executable_extension(const fs::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    static constexpr std::array<std::wstring_view, 13> executable_extensions{
        L".exe", L".com", L".bat", L".cmd", L".ps1", L".vbs", L".vbe",
        L".js", L".jse", L".wsf", L".msi", L".msp", L".scr",
    };
    return std::find(executable_extensions.begin(), executable_extensions.end(),
                     std::wstring_view(extension)) != executable_extensions.end();
}

void wipe_file_best_effort(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error) return;
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) return;
    std::array<char, 64 * 1024> zeros{};
    std::uint64_t remaining = size;
    while (remaining > 0 && file) {
        const auto count = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, zeros.size()));
        file.write(zeros.data(), count);
        remaining -= static_cast<std::uint64_t>(count);
    }
    file.flush();
}

void remove_temp_directory(const fs::path& path, bool sensitive) {
    if (sensitive) {
        std::error_code iterate_error;
        for (fs::recursive_directory_iterator it(
                 path, fs::directory_options::skip_permission_denied, iterate_error), end;
             !iterate_error && it != end; it.increment(iterate_error)) {
            std::error_code status_error;
            if (it->is_regular_file(status_error)) {
                wipe_file_best_effort(it->path());
            }
        }
    }
    std::error_code remove_error;
    fs::remove_all(path, remove_error);
}

bool key_file_contains_public_key(const fs::path& path,
                                  const std::array<std::uint8_t, 32>& public_key) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
    if (bytes.size() == public_key.size()) {
        return std::equal(public_key.begin(), public_key.end(), bytes.begin());
    }
    if (bytes.size() == 64) {
        return std::equal(public_key.begin(), public_key.end(), bytes.begin() + 32);
    }
    return false;
}

bool set_registry_string(HKEY root, const std::wstring& subkey,
                         const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status = RegSetValueExW(
        key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

std::wstring registry_string(HKEY root, const std::wstring& subkey, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return {};
    }
    DWORD size = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) !=
            ERROR_SUCCESS ||
        size < sizeof(wchar_t)) {
        RegCloseKey(key);
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    const LSTATUS status = RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ,
                                        nullptr, value.data(), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) return {};
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

void delete_registry_tree(HKEY root, const std::wstring& subkey) {
    RegDeleteTreeW(root, subkey.c_str());
}

std::wstring quoted_executable_command(const fs::path& executable,
                                       std::wstring_view arguments) {
    std::wstring command = quote_argument(executable);
    if (!arguments.empty()) {
        command += L" ";
        command += arguments;
    }
    return command;
}

ShellIconRef shell_icon_for_path(const fs::path& path, bool drive = false) {
    axiom::gui::BrowserItem item;
    item.kind = drive ? axiom::gui::BrowserItemKind::drive
                      : axiom::gui::BrowserItemKind::directory;
    item.filesystem_path = path;
    item.name = path.filename().wstring();
    return shell_icon_for_item(item);
}

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
        {L"Axiom archives and volumes (*.axar;*.rev*)", L"*.axar;*.rev*"},
        {L"All files (*.*)", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(sizeof(filters) / sizeof(filters[0])), filters);
    dialog->SetDefaultExtension(L"axar");
}

std::optional<fs::path> joined_archive_path_for_volume(const fs::path& volume) {
    std::wstring name = volume.filename().wstring();
    std::wstring folded = name;
    std::transform(folded.begin(), folded.end(), folded.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    const auto part = folded.rfind(L".part");
    const auto recovery = folded.rfind(L".rev");
    std::size_t cut = std::wstring::npos;
    if (part != std::wstring::npos) cut = part;
    if (recovery != std::wstring::npos &&
        (cut == std::wstring::npos || recovery > cut)) {
        cut = recovery;
    }
    if (cut == std::wstring::npos) return std::nullopt;
    return volume.parent_path() / fs::path(name.substr(0, cut) + L".axar");
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

ThemePalette make_theme(int preference = 0) {
    ThemePalette theme;
    theme.dark = preference == 1
        ? true
        : preference == 2 ? false : system_prefers_dark_mode();
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

struct TableViewOptions {
    bool show_grid_lines = true;
    bool show_horizontal_scrollbar = true;
    bool full_row_select = true;
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

    void set_options(TableViewOptions options) {
        options_ = options;
        if (!options_.show_horizontal_scrollbar) {
            scroll_x_ = 0;
        }
        clamp_scroll();
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

    int row_at_screen_point(POINT point) const {
        if (hwnd_ == nullptr) return -1;
        ScreenToClient(hwnd_, &point);
        RECT client{};
        GetClientRect(hwnd_, &client);
        if (point.x < table_left(client) || point.x >= table_right(client) ||
            point.y < header_height() || point.y >= rows_bottom(client)) {
            return -1;
        }
        const int y = point.y - header_height() + scroll_y_;
        const int row = row_height() > 0 ? y / row_height() : -1;
        return row >= 0 && row < static_cast<int>(rows_.size()) ? row : -1;
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
            if (options_.show_horizontal_scrollbar && requested_content_width() > width) {
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
        if (!options_.show_horizontal_scrollbar) {
            return 0;
        }
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
            if (!toggle) std::fill(selected_.begin(), selected_.end(), false);
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

    bool point_can_select_row(POINT point, int row, const RECT& client) const {
        if (options_.full_row_select) return true;
        if (row < 0 || row >= static_cast<int>(rows_.size())) return false;
        const auto widths = column_widths(client);
        if (widths.empty()) return false;
        const int first_left = table_left(client) - scroll_x_;
        const int first_right = first_left + widths.front();
        return point.x >= first_left && point.x < first_right;
    }

    void begin_marquee_selection(POINT point, bool preserve_selection) {
        marquee_selecting_ = true;
        marquee_start_ = point;
        marquee_current_ = point;
        marquee_base_selection_ = selected_;
        if (!preserve_selection) {
            std::fill(marquee_base_selection_.begin(), marquee_base_selection_.end(), false);
            std::fill(selected_.begin(), selected_.end(), false);
        }
        selected_row_ = -1;
        selection_anchor_ = -1;
        SetCapture(hwnd_);
        notify_parent(kTableSelectionChangedMessage);
        invalidate();
    }

    void update_marquee_selection(POINT point) {
        marquee_current_ = point;
        RECT client{};
        GetClientRect(hwnd_, &client);
        const RECT rows_area{table_left(client), header_height(),
                             table_right(client), rows_bottom(client)};
        RECT marquee{
            std::min(marquee_start_.x, marquee_current_.x),
            std::min(marquee_start_.y, marquee_current_.y),
            std::max(marquee_start_.x, marquee_current_.x) + 1,
            std::max(marquee_start_.y, marquee_current_.y) + 1,
        };
        RECT clipped{};
        const bool visible = IntersectRect(&clipped, &marquee, &rows_area) != FALSE;
        std::vector<bool> next = marquee_base_selection_;
        int first_selected = -1;
        int last_selected = -1;
        if (visible) {
            for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
                const int top = header_height() + row * row_height() - scroll_y_;
                const RECT row_rect{rows_area.left, top, rows_area.right, top + row_height()};
                RECT overlap{};
                if (IntersectRect(&overlap, &row_rect, &clipped)) {
                    next[static_cast<std::size_t>(row)] =
                        !marquee_base_selection_[static_cast<std::size_t>(row)];
                    if (next[static_cast<std::size_t>(row)]) {
                        if (first_selected < 0) first_selected = row;
                        last_selected = row;
                    }
                }
            }
        }
        if (next != selected_) {
            selected_ = std::move(next);
            selected_row_ = last_selected;
            selection_anchor_ = first_selected;
            notify_parent(kTableSelectionChangedMessage);
        }
        invalidate();
    }

    void end_marquee_selection() {
        if (!marquee_selecting_) return;
        marquee_selecting_ = false;
        marquee_base_selection_.clear();
        if (GetCapture() == hwnd_) ReleaseCapture();
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
                if (options_.show_grid_lines) {
                    draw_solid_line(dc, next_x, header.top, next_x, header.bottom, theme_.border);
                }
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
            if (options_.show_grid_lines) {
                draw_solid_line(dc, rows_clip.left, row_rect.bottom - scale(1),
                                rows_clip.right, row_rect.bottom - scale(1), theme_.panel);
            }
            y += row_h;
        }

        if (options_.show_grid_lines) {
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
        }
        RestoreDC(dc, saved_rows_dc);

        if (marquee_selecting_) {
            RECT marquee{
                std::min(marquee_start_.x, marquee_current_.x),
                std::min(marquee_start_.y, marquee_current_.y),
                std::max(marquee_start_.x, marquee_current_.x) + 1,
                std::max(marquee_start_.y, marquee_current_.y) + 1,
            };
            RECT rows_area{content_left, header.bottom, content_right, rows_bottom(client)};
            RECT clipped{};
            if (IntersectRect(&clipped, &marquee, &rows_area)) {
                frame_solid_rect(dc, clipped, theme_.focus);
            }
        }

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
                int valid_row = row >= 0 && row < static_cast<int>(rows_.size()) ? row : -1;
                if (valid_row >= 0 && !point_can_select_row(point, valid_row, client)) {
                    valid_row = -1;
                }
                const bool extend = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool toggle = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool preserve_selection = valid_row >= 0 && !extend && !toggle &&
                    selected_[static_cast<std::size_t>(valid_row)];
                if (valid_row < 0) {
                    begin_marquee_selection(point, toggle);
                    return 0;
                }
                if (!preserve_selection) select_row(valid_row, extend, toggle);
                drag_candidate_ = valid_row >= 0 &&
                    selected_[static_cast<std::size_t>(valid_row)];
                drag_started_ = false;
                drag_start_ = point;
                collapse_selection_on_click_ = preserve_selection ? valid_row : -1;
                if (drag_candidate_) SetCapture(hwnd_);
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
                const int y = point.y - header_height() + scroll_y_;
                const int row = row_height() > 0 ? y / row_height() : -1;
                if (row >= 0 && row < static_cast<int>(rows_.size()) &&
                    !point_can_select_row(point, row, client)) {
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
                    point_can_select_row(point, row, client) &&
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
                if (marquee_selecting_ && (wparam & MK_LBUTTON) != 0) {
                    update_marquee_selection(
                        POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                    return 0;
                }
                if (drag_candidate_ && !drag_started_ &&
                    (wparam & MK_LBUTTON) != 0) {
                    const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                    const int threshold_x = GetSystemMetrics(SM_CXDRAG);
                    const int threshold_y = GetSystemMetrics(SM_CYDRAG);
                    if (std::abs(point.x - drag_start_.x) >= threshold_x ||
                        std::abs(point.y - drag_start_.y) >= threshold_y) {
                        drag_started_ = true;
                        collapse_selection_on_click_ = -1;
                        if (GetCapture() == hwnd_) ReleaseCapture();
                        SendMessageW(GetParent(hwnd_), kTableBeginDragMessage, 0, 0);
                        drag_candidate_ = false;
                        return 0;
                    }
                }
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
                if (marquee_selecting_) {
                    update_marquee_selection(
                        POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                    end_marquee_selection();
                    return 0;
                }
                if (drag_candidate_) {
                    drag_candidate_ = false;
                    if (GetCapture() == hwnd_) ReleaseCapture();
                    if (!drag_started_ && collapse_selection_on_click_ >= 0) {
                        select_row(collapse_selection_on_click_, false, false);
                    }
                    collapse_selection_on_click_ = -1;
                    drag_started_ = false;
                    return 0;
                }
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
                                   (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                                   (GetKeyState(VK_SHIFT) & 0x8000) != 0 &&
                                       (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                        return 0;
                    }
                    if (wparam == VK_UP) {
                        select_row(std::max(0, selected_row_ < 0 ? 0 : selected_row_ - 1),
                                   (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                                   (GetKeyState(VK_SHIFT) & 0x8000) != 0 &&
                                       (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                        return 0;
                    }
                    if (wparam == VK_HOME || wparam == VK_END) {
                        select_row(wparam == VK_HOME ? 0 : static_cast<int>(rows_.size()) - 1,
                                   (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                                   (GetKeyState(VK_SHIFT) & 0x8000) != 0 &&
                                       (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                        return 0;
                    }
                    if (wparam == VK_SPACE &&
                        (GetKeyState(VK_CONTROL) & 0x8000) != 0 && selected_row_ >= 0) {
                        select_row(selected_row_, false, true);
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
                drag_candidate_ = false;
                drag_started_ = false;
                collapse_selection_on_click_ = -1;
                marquee_selecting_ = false;
                marquee_base_selection_.clear();
                invalidate();
                return 0;
            case WM_CAPTURECHANGED:
                resizing_column_ = -1;
                dragging_scrollbar_ = false;
                dragging_horizontal_scrollbar_ = false;
                drag_candidate_ = false;
                drag_started_ = false;
                collapse_selection_on_click_ = -1;
                marquee_selecting_ = false;
                marquee_base_selection_.clear();
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
};

class MainWindow {
public:
    ~MainWindow() {
        drop_target_.revoke();
        clear_archive_password();
        secure_clear(pending_archive_password_);
        restore_operation_priority();
        for (const auto& temp : temp_directories_) {
            remove_temp_directory(temp.path,
                                  temp.sensitive &&
                                      application_options_.wipe_encrypted_temp_files);
        }
        reset_theme_brushes();
        reset_font();
    }

    bool create(HINSTANCE instance,
                int show_command,
                std::wstring initial_path,
                AxiomGuiStartupCommand startup_command = {}) {
        instance_ = instance;
        startup_command_ = std::move(startup_command);
        persisted_settings_ = axiom::gui::load_gui_settings();
        application_options_ = persisted_settings_.application;
        selected_level_ = application_options_.default_level;
        selected_thread_count_ = application_options_.default_thread_count;
        selected_dictionary_size_ = application_options_.default_dictionary_size;
        selected_word_size_ = application_options_.default_word_size;
        selected_solid_block_size_ = application_options_.default_solid_block_size;
        theme_ = make_theme(application_options_.theme_mode);
        cleanup_old_temp_directories();
        apply_shell_integration();
        sort_column_ = persisted_settings_.sort_column;
        sort_ascending_ = persisted_settings_.sort_ascending;
        std::wstring configured_startup = persisted_settings_.last_location;
        if (application_options_.startup_location_mode == 1) {
            configured_startup.clear();
        } else if (application_options_.startup_location_mode == 2) {
            if (const auto desktop = known_folder_path(FOLDERID_Desktop)) {
                configured_startup = desktop->wstring();
            }
        } else if (application_options_.startup_location_mode == 3 &&
                   !application_options_.startup_custom_path.empty()) {
            configured_startup = application_options_.startup_custom_path;
        }
        initial_path_ = initial_path.empty() ? std::move(configured_startup)
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

        if (application_options_.restore_window_placement &&
            persisted_settings_.has_placement) {
            if (persisted_settings_.placement.showCmd == SW_SHOWMINIMIZED) {
                persisted_settings_.placement.showCmd = SW_SHOWNORMAL;
            }
            SetWindowPlacement(hwnd_, &persisted_settings_.placement);
        }

        ShowWindow(hwnd_, application_options_.restore_window_placement &&
                              persisted_settings_.has_placement
                              ? persisted_settings_.placement.showCmd
                              : show_command);
        UpdateWindow(hwnd_);
        on_initial_navigate();
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
        theme_ = make_theme(application_options_.theme_mode);
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
        axiom::gui::apply_dialog_control_theme(address_edit_, theme_.dark);
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
            COMBOBOXINFO info{sizeof(info)};
            if (GetComboBoxInfo(address_edit_, &info) && info.hwndItem != nullptr) {
                SendMessageW(info.hwndItem, EM_SETMARGINS,
                             EC_LEFTMARGIN | EC_RIGHTMARGIN, margins);
            }
            SendMessageW(address_edit_, CB_SETITEMHEIGHT, 0, scale(24));
            SendMessageW(address_edit_, CB_SETITEMHEIGHT,
                         static_cast<WPARAM>(-1), scale(24));
        }
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
                                      !capabilities.directory_encrypted;
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
                    {kBenchmark, L"&Benchmark...", L"", !busy_},
                    {0, L"", L"", false, true},
                    {kEditArchiveComment, L"Edit archive &comment...", L"",
                      !busy_ && has_archive && capabilities.comments && archive_editable},
                    {kLockArchive, L"&Lock archive...", L"",
                      !busy_ && has_archive && capabilities.lock && archive_editable},
                    {kRepairArchive, L"&Repair archive...", L"",
                     !busy_ && has_archive},
                    {kCreateRecoveryVolumes, L"Create recovery &volumes...", L"",
                     !busy_ && has_archive},
                    {kVerifyArchiveSignature, L"Verify &signature...", L"",
                     !busy_ && has_archive},
                    {kCreateSfx, L"Create &self-extracting archive...", L"",
                     !busy_ && has_archive},
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
                                      !capabilities.directory_encrypted;
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
        BeginPaint(hwnd_, &paint);
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
        address_edit_ = make_control(
            L"COMBOBOX", L"",
            WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
            kAddressEdit);
        set_text(address_edit_, L"This PC");
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
        apply_table_options();

        status_ = make_control(L"STATIC", L"Ready", SS_LEFT, kStatus);
        // Custom controls do not expose painted content to accessibility tools, so give
        // each HWND a stable semantic name for UI Automation and screen readers.
        SetWindowTextW(list_, L"Files and archive contents");

        apply_edit_margins();
        apply_fonts();
        DragAcceptFiles(hwnd_, TRUE);
        drop_target_.register_window(
            list_,
            [this](IDataObject* object, POINT point, DWORD keys, DWORD allowed) {
                return query_file_drop(object, point, keys, allowed);
            },
            [this](IDataObject* object, POINT point, DWORD keys, DWORD allowed) {
                return perform_file_drop(object, point, keys, allowed);
            });
        apply_theme();
        set_busy(false);
        history_.reset(axiom::gui::BrowserLocation::computer());
        maybe_start_automatic_update_check();
    }

    void on_initial_navigate() {
        if (startup_command_.kind == AxiomGuiStartupCommand::Kind::add_to_archive) {
            std::vector<fs::path> paths;
            paths.reserve(startup_command_.paths.size());
            for (const auto& path : startup_command_.paths) {
                if (!path.empty()) paths.emplace_back(path);
            }
            navigate_to(history_.current(), false);
            create_archive_from_paths(std::move(paths));
            return;
        }
        if (initial_path_.empty()) {
            navigate_to(history_.current(), false);
        } else {
            set_text(address_edit_, initial_path_);
            on_address_go();
        }
        if (startup_command_.kind == AxiomGuiStartupCommand::Kind::extract_archive) {
            on_extract();
        } else if (startup_command_.kind == AxiomGuiStartupCommand::Kind::test_archive) {
            on_test();
        }
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
        MoveWindow(address_edit_, address_x, y, address_width, scale(360), TRUE);
        SendMessageW(address_edit_, CB_SETDROPPEDWIDTH,
                     static_cast<WPARAM>(address_width), 0);
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
        axiom::apply_compression_level(options, selected_level());
        options.thread_count = selected_thread_count_;
        if (selected_dictionary_size_ != 0) {
            options.window_size = selected_dictionary_size_;
        }
        if (selected_word_size_ != 0) {
            options.nice_length = selected_word_size_;
        }
        if (selected_solid_block_size_ != 0) {
            options.block_size = selected_solid_block_size_;
            options.auto_block_size_for_threads = false;
        }
        if (application_options_.memory_limit_mode == 1) {
            if (const auto limit = parse_size_setting(application_options_.memory_limit)) {
                const std::size_t capped = static_cast<std::size_t>(std::min<std::uint64_t>(
                    *limit, std::numeric_limits<std::size_t>::max()));
                const std::size_t practical_cap = std::max<std::size_t>(capped, 64u << 10);
                options.window_size = std::min(options.window_size, practical_cap);
                options.block_size = std::min(options.block_size, practical_cap);
                options.auto_block_size_for_threads = false;
            }
        }
        return options;
    }

    void apply_table_options() {
        table_.set_options({
            application_options_.show_grid_lines,
            application_options_.show_horizontal_scrollbar,
            application_options_.full_row_select,
        });
    }

    void add_address_entry(std::wstring label, std::wstring value,
                           ShellIconRef icon = {}) {
        if (value.empty()) return;
        const auto duplicate = std::find_if(
            address_entries_.begin(), address_entries_.end(),
            [&](const AddressEntry& entry) {
                return CompareStringOrdinal(entry.value.c_str(), -1,
                                            value.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
        if (duplicate != address_entries_.end()) return;
        const LRESULT item = SendMessageW(
            address_edit_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (item == CB_ERR || item == CB_ERRSPACE) return;
        const std::size_t entry_index = address_entries_.size();
        address_entries_.push_back({std::move(label), std::move(value), icon});
        SendMessageW(address_edit_, CB_SETITEMDATA, static_cast<WPARAM>(item),
                     static_cast<LPARAM>(entry_index));
    }

    void add_known_address(const wchar_t* label, REFKNOWNFOLDERID folder_id) {
        if (const auto path = known_folder_path(folder_id)) {
            add_address_entry(label, path->wstring(), shell_icon_for_path(*path));
        }
    }

    void populate_address_dropdown() {
        if (address_edit_ == nullptr) return;
        const std::wstring current_text = get_text(address_edit_);
        SendMessageW(address_edit_, CB_RESETCONTENT, 0, 0);
        address_entries_.clear();

        if (application_options_.show_address_shell_locations) {
            add_address_entry(L"This PC", L"This PC", shell_icon_for_path(L"folder"));
            add_known_address(L"Home", FOLDERID_Profile);
            add_known_address(L"Desktop", FOLDERID_Desktop);
            add_known_address(L"Documents", FOLDERID_Documents);
            add_known_address(L"Downloads", FOLDERID_Downloads);
            add_known_address(L"Pictures", FOLDERID_Pictures);
            add_known_address(L"OneDrive", FOLDERID_SkyDrive);
        }

        const DWORD required = GetLogicalDriveStringsW(0, nullptr);
        if (application_options_.show_address_shell_locations && required > 0) {
            std::wstring drives(static_cast<std::size_t>(required), L'\0');
            if (GetLogicalDriveStringsW(required, drives.data()) != 0) {
                for (const wchar_t* drive = drives.c_str(); *drive != L'\0';
                     drive += wcslen(drive) + 1) {
                    const fs::path path(drive);
                    wchar_t volume[MAX_PATH]{};
                    std::wstring label;
                    if (GetVolumeInformationW(drive, volume, MAX_PATH, nullptr, nullptr,
                                              nullptr, nullptr, 0) && volume[0] != L'\0') {
                        label = std::wstring(volume) + L" (" +
                                path.root_name().wstring() + L")";
                    } else {
                        label = path.root_name().wstring();
                    }
                    add_address_entry(std::move(label), path.wstring(),
                                      shell_icon_for_path(path, true));
                }
            }
        }

        if (application_options_.show_address_recent_locations) {
            for (const std::wstring& recent : recent_addresses_) {
                add_address_entry(recent, recent, shell_icon_for_path(recent));
            }
        }

        if (application_options_.show_address_archive_children) {
            const auto& location = history_.current();
            for (const auto& item : browser_items_) {
                if (item.is_parent() || !item.is_container()) continue;
                if (location.kind == axiom::gui::BrowserLocationKind::archive) {
                    add_address_entry(
                        L"    " + item.name,
                        axiom::gui::BrowserLocation::archive(
                            location.archive_path, item.archive_path).display_name(),
                        shell_icon_for_item(item));
                } else if (!item.filesystem_path.empty()) {
                    add_address_entry(L"    " + item.name, item.filesystem_path.wstring(),
                                      shell_icon_for_item(item));
                }
            }
        }

        SendMessageW(address_edit_, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
        COMBOBOXINFO combo_info{sizeof(combo_info)};
        if (GetComboBoxInfo(address_edit_, &combo_info) && combo_info.hwndItem != nullptr) {
            SetWindowTextW(combo_info.hwndItem, current_text.c_str());
        } else {
            set_text(address_edit_, current_text);
        }
        SendMessageW(address_edit_, CB_SETMINVISIBLE,
                     static_cast<WPARAM>(std::clamp(
                         application_options_.recent_location_count, 4, 20)), 0);
    }

    void remember_address(std::wstring value) {
        if (value.empty()) return;
        const int limit = std::clamp(application_options_.recent_location_count, 0, 50);
        if (limit == 0) {
            recent_addresses_.clear();
            return;
        }
        std::erase_if(recent_addresses_, [&](const std::wstring& existing) {
            return CompareStringOrdinal(existing.c_str(), -1,
                                        value.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        recent_addresses_.insert(recent_addresses_.begin(), std::move(value));
        if (recent_addresses_.size() > static_cast<std::size_t>(limit)) {
            recent_addresses_.resize(static_cast<std::size_t>(limit));
        }
    }

    void select_address_entry() {
        const LRESULT selection = SendMessageW(address_edit_, CB_GETCURSEL, 0, 0);
        if (selection == CB_ERR) return;
        const LRESULT entry_index = SendMessageW(
            address_edit_, CB_GETITEMDATA, static_cast<WPARAM>(selection), 0);
        if (entry_index < 0 ||
            static_cast<std::size_t>(entry_index) >= address_entries_.size()) return;
        set_text(address_edit_, address_entries_[static_cast<std::size_t>(entry_index)].value);
        on_address_go();
    }

    void draw_address_entry(const DRAWITEMSTRUCT& draw) const {
        const bool selected = (draw.itemState & ODS_SELECTED) != 0;
        fill_rect(draw.hDC, draw.rcItem, selected ? theme_.selection : theme_.edit);
        if (draw.itemID == static_cast<UINT>(-1)) return;
        const LRESULT entry_index = SendMessageW(
            address_edit_, CB_GETITEMDATA, draw.itemID, 0);
        if (entry_index < 0 ||
            static_cast<std::size_t>(entry_index) >= address_entries_.size()) return;
        const AddressEntry& entry = address_entries_[static_cast<std::size_t>(entry_index)];
        RECT text_rect = draw.rcItem;
        text_rect.left += scale(6);
        if (entry.icon.image_list != nullptr && entry.icon.index >= 0) {
            int icon_width = 0;
            int icon_height = 0;
            ImageList_GetIconSize(entry.icon.image_list, &icon_width, &icon_height);
            const int icon_y = static_cast<int>(text_rect.top) + std::max(
                0, (static_cast<int>(text_rect.bottom - text_rect.top) - icon_height) / 2);
            ImageList_Draw(entry.icon.image_list, entry.icon.index, draw.hDC,
                           static_cast<int>(text_rect.left), icon_y,
                           ILD_TRANSPARENT);
            text_rect.left += icon_width + scale(6);
        }
        text_rect.right -= scale(5);
        HFONT font = ui_font_ != nullptr
            ? ui_font_
            : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(draw.hDC, font);
        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, selected ? theme_.selection_text : theme_.text);
        DrawTextW(draw.hDC, entry.label.c_str(), -1, &text_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(draw.hDC, old_font);
        if ((draw.itemState & ODS_FOCUS) != 0) {
            RECT focus = draw.rcItem;
            InflateRect(&focus, -2, -2);
            DrawFocusRect(draw.hDC, &focus);
        }
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

    std::vector<std::string> selected_archive_paths() const {
        std::vector<std::string> result;
        for (int index : selected_browser_indices()) {
            const auto& item = browser_items_[index];
            if (!item.is_parent() && !item.archive_path.empty()) {
                result.push_back(item.archive_path);
            }
        }
        return result;
    }

    static std::string archive_name(std::string_view path) {
        const std::size_t separator = path.find_last_of('/');
        return std::string(separator == std::string_view::npos
                               ? path
                               : path.substr(separator + 1));
    }

    static std::string join_archive_directory(std::string_view directory,
                                              std::string_view name) {
        if (directory.empty()) return std::string(name);
        return std::string(directory) + "/" + std::string(name);
    }

    static bool same_filesystem_path(const fs::path& left, const fs::path& right) {
        std::error_code error;
        if (fs::equivalent(left, right, error)) return true;
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    std::string archive_drop_directory(POINT screen_point) const {
        if (history_.current().kind != axiom::gui::BrowserLocationKind::archive) return {};
        const int row = table_.row_at_screen_point(screen_point);
        if (row >= 0 && row < static_cast<int>(browser_items_.size())) {
            const auto& item = browser_items_[static_cast<std::size_t>(row)];
            if (item.kind == axiom::gui::BrowserItemKind::directory &&
                !item.archive_path.empty()) {
                return item.archive_path;
            }
        }
        return history_.current().archive_directory;
    }

    std::optional<std::string> password_for_archive_edit(const fs::path& archive) {
        try {
            if (!axiom::archive_is_encrypted(archive)) return std::string{};
        } catch (const std::exception& error) {
            show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                             L"Open archive");
            return std::nullopt;
        }
        const bool cache_password =
            application_options_.cache_passwords &&
            application_options_.password_prompt_mode == 0;
        if (cache_password && !archive_password_path_.empty() &&
            same_filesystem_path(archive_password_path_, archive)) {
            return archive_password_;
        }
        std::wstring password;
        if (!axiom::gui::show_archive_password_dialog(hwnd_, password)) return std::nullopt;
        std::string encoded = utf8(password);
        secure_clear(password);
        if (cache_password) {
            clear_archive_password();
            archive_password_path_ = archive;
            archive_password_ = encoded;
            return archive_password_;
        }
        return encoded;
    }

    void clear_archive_password() {
        secure_clear(archive_password_);
        secure_clear(pending_archive_password_);
        archive_password_path_.clear();
    }

    bool prepare_archive_password(const axiom::gui::BrowserLocation& location) {
        secure_clear(pending_archive_password_);
        if (location.kind != axiom::gui::BrowserLocationKind::archive) {
            clear_archive_password();
            return true;
        }
        const bool cache_password =
            application_options_.cache_passwords &&
            application_options_.password_prompt_mode == 0;
        if (cache_password && !archive_password_path_.empty() &&
            same_filesystem_path(archive_password_path_, location.archive_path)) {
            return true;
        }

        bool encrypted = false;
        try {
            encrypted = axiom::archive_is_encrypted(location.archive_path);
        } catch (const std::exception& error) {
            show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                             L"Open archive");
            return false;
        }
        if (!encrypted) {
            clear_archive_password();
            return true;
        }

        std::wstring entered;
        if (!axiom::gui::show_archive_password_dialog(hwnd_, entered)) return false;
        std::string encoded = utf8(entered);
        secure_clear(entered);
        if (cache_password) {
            clear_archive_password();
            archive_password_path_ = location.archive_path;
            archive_password_ = encoded;
        } else {
            clear_archive_password();
            pending_archive_password_ = std::move(encoded);
        }
        return true;
    }

    fs::path configured_temp_base() const {
        wchar_t temporary[MAX_PATH + 1]{};
        const DWORD length = GetTempPathW(MAX_PATH, temporary);
        if (length == 0 || length > MAX_PATH) {
            throw std::runtime_error("could not locate the temporary directory");
        }
        fs::path base(temporary);
        if (application_options_.temp_folder_mode == 1) {
            if (const auto local = known_folder_path(FOLDERID_LocalAppData)) {
                base = *local / L"AxiomCompress" / L"Temp";
            }
        } else if (application_options_.temp_folder_mode == 2 &&
                   !application_options_.temp_folder.empty()) {
            base = application_options_.temp_folder;
        }
        return base;
    }

    void cleanup_old_temp_directories() {
        fs::path base;
        try {
            base = configured_temp_base();
        } catch (...) {
            return;
        }
        const int days = std::clamp(application_options_.temp_cleanup_days, 0, 365);
        const auto now = fs::file_time_type::clock::now();
        const auto max_age = std::chrono::hours(24 * days);
        std::error_code iterate_error;
        for (fs::directory_iterator it(base, fs::directory_options::skip_permission_denied,
                                       iterate_error), end;
             !iterate_error && it != end; it.increment(iterate_error)) {
            const auto name = it->path().filename().wstring();
            if (name.rfind(L"AxiomDrag-", 0) != 0 && name.rfind(L"AxiomSfx-", 0) != 0) {
                continue;
            }
            std::error_code time_error;
            const auto modified = fs::last_write_time(it->path(), time_error);
            if (time_error) continue;
            if (days == 0 || now - modified >= max_age) {
                remove_temp_directory(it->path(), application_options_.wipe_encrypted_temp_files);
            }
        }
    }

    fs::path create_drag_staging_directory(bool sensitive = false) {
        fs::path base = configured_temp_base();
        std::error_code create_base_error;
        fs::create_directories(base, create_base_error);
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const fs::path candidate = base /
                (L"AxiomDrag-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                temp_directories_.push_back({candidate, sensitive});
                return candidate;
            }
        }
        throw std::runtime_error("could not create a temporary drag directory");
    }

    DWORD query_file_drop(IDataObject* object, POINT, DWORD, DWORD allowed) const {
        if (busy_ || history_.current().kind != axiom::gui::BrowserLocationKind::archive) {
            return DROPEFFECT_NONE;
        }
        const auto capabilities = active_archive_capabilities();
        if (capabilities.locked || capabilities.directory_encrypted) {
            return DROPEFFECT_NONE;
        }

        if (axiom::gui::data_object_has_archive_entries(object)) {
            axiom::gui::ArchiveDragPayload payload;
            if (!axiom::gui::read_archive_entries(object, payload)) return DROPEFFECT_NONE;
            if (same_filesystem_path(payload.archive_path,
                                     history_.current().archive_path)) {
                return (allowed & DROPEFFECT_MOVE) != 0
                    ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            }
        }
        return axiom::gui::data_object_has_file_drop(object) ? DROPEFFECT_COPY
                                                              : DROPEFFECT_NONE;
    }

    DWORD perform_file_drop(IDataObject* object, POINT point, DWORD, DWORD allowed) {
        if (busy_ || history_.current().kind != axiom::gui::BrowserLocationKind::archive) {
            return DROPEFFECT_NONE;
        }
        const fs::path archive = history_.current().archive_path;
        const auto capabilities = active_archive_capabilities();
        if (capabilities.locked || capabilities.directory_encrypted) {
            show_app_message(
                capabilities.locked
                    ? L"This archive is locked and cannot be changed."
                    : L"Editing archives with encrypted file names is not supported yet.",
                             axiom::gui::MessageDialogIcon::warning,
                             L"Drop into archive");
            return DROPEFFECT_NONE;
        }
        const std::string destination_directory = archive_drop_directory(point);

        axiom::gui::ArchiveDragPayload payload;
        const bool archive_drag = axiom::gui::data_object_has_archive_entries(object);
        if (archive_drag && !axiom::gui::read_archive_entries(object, payload)) {
            return DROPEFFECT_NONE;
        }
        if (archive_drag && same_filesystem_path(payload.archive_path, archive)) {
            std::vector<axiom::ArchiveMove> moves;
            moves.reserve(payload.entry_paths.size());
            for (const auto& source : payload.entry_paths) {
                const std::string destination =
                    join_archive_directory(destination_directory, archive_name(source));
                if (destination != source) moves.push_back({source, destination});
            }
            if (moves.empty()) return DROPEFFECT_NONE;
            auto password = password_for_archive_edit(archive);
            if (!password) return DROPEFFECT_NONE;
            auto options = compression_options();
            options.password = std::move(*password);
            operation_archive_output_ = archive;
            start_operation(
                L"Moving archive entries...", L"Archive entries moved.",
                [archive, moves = std::move(moves), options](
                    std::shared_ptr<axiom::OperationControl> operation) mutable {
                    auto run_options = options;
                    run_options.operation = std::move(operation);
                    axiom::move_archive_entries(archive, moves, run_options);
                });
            return (allowed & DROPEFFECT_MOVE) != 0 ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        }

        auto paths = axiom::gui::read_file_drop(object);
        if (paths.empty()) return DROPEFFECT_NONE;
        const std::wstring prompt =
            L"Add " + quote_count(paths.size(), L"item", L"items") +
            L" to this archive folder?\n\nFiles with matching names will be replaced.";
        if (show_app_message(prompt, axiom::gui::MessageDialogIcon::question,
                             L"Add to archive", axiom::gui::MessageDialogButtons::yes_no,
                             IDYES) != IDYES) {
            return DROPEFFECT_NONE;
        }
        auto password = password_for_archive_edit(archive);
        if (!password) return DROPEFFECT_NONE;
        std::vector<axiom::ArchiveInput> inputs;
        inputs.reserve(paths.size());
        for (const auto& path : paths) {
            inputs.push_back({path,
                join_archive_directory(destination_directory, utf8(path.filename().wstring()))});
        }
        auto options = compression_options();
        options.password = std::move(*password);
        operation_archive_output_ = archive;
        start_operation(
            L"Adding dropped items...", L"Dropped items were added to the archive.",
            [archive, inputs = std::move(inputs), options](
                std::shared_ptr<axiom::OperationControl> operation) mutable {
                auto run_options = options;
                run_options.operation = std::move(operation);
                axiom::add_to_archive(inputs, archive, run_options);
            });
        return DROPEFFECT_COPY;
    }

    StagedArchiveEntries extract_archive_entries_to_staging(
        const fs::path& archive, const std::vector<std::string>& entries,
        const std::string& password, bool for_drag, bool sensitive) {
        const fs::path staging = create_drag_staging_directory(sensitive);
        const HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (completed == nullptr) throw std::runtime_error("could not start temporary extraction");
        auto control = std::make_shared<axiom::OperationControl>();
        std::exception_ptr failure;

        set_busy(true);
        set_status(for_drag ? L"Preparing archive entries for drag and drop..."
                            : L"Preparing archive file for viewing...");
        if (!operation_window_.create(
                hwnd_, instance_,
                for_drag ? L"Preparing dragged archive entries..."
                         : L"Opening archive file...",
                staging,
                make_operation_window_theme(theme_),
                [control](bool paused) { control->set_paused(paused); },
                [control] { control->request_cancel(); })) {
            CloseHandle(completed);
            set_busy(false);
            throw std::runtime_error("could not create temporary extraction progress window");
        }
        control->set_progress_callback([target = hwnd_](const axiom::OperationProgress& progress) {
            auto* copy = new axiom::OperationProgress(progress);
            if (!PostMessageW(target, kOperationProgressMessage, 0,
                              reinterpret_cast<LPARAM>(copy))) {
                delete copy;
            }
        });

        std::jthread worker([&, control] {
            try {
                axiom::ExtractOptions options;
                options.overwrite = axiom::ExtractOptions::Overwrite::overwrite;
                options.password = password;
                options.operation = control;
                axiom::extract_entries(archive, entries, staging, options);
            } catch (...) {
                failure = std::current_exception();
            }
            SetEvent(completed);
        });

        bool quit_seen = false;
        while (WaitForSingleObject(completed, 0) != WAIT_OBJECT_0) {
            const DWORD wait = MsgWaitForMultipleObjects(1, &completed, FALSE, INFINITE,
                                                         QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0) break;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    quit_seen = true;
                    control->request_cancel();
                    continue;
                }
                if (message.message == WM_CLOSE && message.hwnd == hwnd_) {
                    quit_seen = true;
                    control->request_cancel();
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        worker.join();
        CloseHandle(completed);
        operation_window_.close();
        set_busy(false);
        operation_paused_ = false;
        if (quit_seen) PostQuitMessage(0);
        if (failure) std::rethrow_exception(failure);

        StagedArchiveEntries staged;
        staged.directory = staging;
        staged.paths.reserve(entries.size());
        for (const auto& entry : entries) {
            staged.paths.push_back(staging / fs::path(widen(entry)));
        }
        set_status(for_drag ? L"Drag and drop ready." : L"Archive file is ready to open.");
        return staged;
    }

    void on_table_begin_drag() {
        if (busy_) return;
        auto filesystem_paths = selected_filesystem_paths();
        if (!filesystem_paths.empty()) {
            axiom::gui::FileDragSource source;
            source.files = [paths = std::move(filesystem_paths)] { return paths; };
            source.preferred_effect = DROPEFFECT_COPY;
            DWORD effect = DROPEFFECT_NONE;
            axiom::gui::do_file_drag(std::move(source),
                                     DROPEFFECT_COPY | DROPEFFECT_MOVE, effect);
            return;
        }

        if (history_.current().kind != axiom::gui::BrowserLocationKind::archive) return;
        auto entries = selected_archive_paths();
        if (entries.empty()) return;
        const fs::path archive = history_.current().archive_path;

        auto password = password_for_archive_edit(archive);
        if (!password) return;

        std::wstring drag_error;
        axiom::gui::FileDragSource source;
        source.archive_payload = {archive, entries};
        source.preferred_effect = DROPEFFECT_COPY;
        source.error_message = &drag_error;
        source.files = [this, archive, entries, password = std::move(*password)]() mutable {
            try {
                auto staged = extract_archive_entries_to_staging(
                    archive, entries, password, true, !password.empty());
                secure_clear(password);
                return staged.paths;
            } catch (...) {
                secure_clear(password);
                throw;
            }
        };
        DWORD effect = DROPEFFECT_NONE;
        axiom::gui::do_file_drag(std::move(source),
                                 DROPEFFECT_COPY | DROPEFFECT_MOVE, effect);
        if (!drag_error.empty()) {
            show_app_message(drag_error, axiom::gui::MessageDialogIcon::error,
                             L"Drag from archive");
        }
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
        // Metadata, ADS, and links are automatic; the dialog explains that rather
        // than exposing toggles which the archive API cannot honor independently.
        availability.metadata = false;
        availability.update = true;
        availability.comments = true;
        availability.lock = true;
        availability.encryption = true;
        availability.recovery = true;
        availability.volumes = true;
        availability.authenticity = true;
        availability.sfx = true;
        availability.posix_metadata = true;
        availability.header_encryption = true;
        // Custom KDF presets are not exposed by the current API.
        availability.kdf_presets = false;
        return availability;
    }

    void navigate_to(axiom::gui::BrowserLocation location, bool record_history = true) {
        if (!prepare_archive_password(location)) return;
        directory_watcher_.stop();
        KillTimer(hwnd_, kDirectoryRefreshTimer);
        if (record_history) history_.navigate(location);
        update_navigation_buttons();
        set_text(address_edit_, location.display_name());
        remember_address(location.display_name());
        table_.clear();
        browser_items_.clear();
        set_status(L"Loading " + location.display_name() + L"...");

        const std::uint64_t generation = ++browser_generation_;
        auto catalog = archive_catalog_;
        std::string archive_password;
        if (location.kind == axiom::gui::BrowserLocationKind::archive &&
            !archive_password_path_.empty() &&
            same_filesystem_path(archive_password_path_, location.archive_path)) {
            archive_password = archive_password_;
        } else if (location.kind == axiom::gui::BrowserLocationKind::archive &&
                   !pending_archive_password_.empty()) {
            archive_password = pending_archive_password_;
            secure_clear(pending_archive_password_);
        }
        HWND target = hwnd_;
        browser_thread_ = std::jthread(
            [target, generation, location = std::move(location), catalog = std::move(catalog),
             archive_password = std::move(archive_password)](
                std::stop_token stop) mutable {
                auto result = std::make_unique<axiom::gui::BrowserLoadResult>(
                    axiom::gui::load_browser_location(location, generation, std::move(catalog), stop,
                                                      archive_password));
                secure_clear(archive_password);
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
        if (!application_options_.show_parent_entry) {
            std::erase_if(browser_items_, [](const auto& item) { return item.is_parent(); });
        }
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
        if (fs::is_regular_file(path, error) && open_archive_path(path)) {
            return;
        } else if (fs::is_directory(path, error)) {
            navigate_to(axiom::gui::BrowserLocation::filesystem(path));
        } else {
            show_app_message(L"The location does not exist or cannot be opened.",
                             axiom::gui::MessageDialogIcon::warning);
            set_text(address_edit_, history_.current().display_name());
        }
    }

    bool launch_viewed_archive_file(const fs::path& file, const fs::path& staging_root,
                                    bool sensitive_temp) {
        if (application_options_.warn_executable_open && has_executable_extension(file)) {
            if (show_app_message(
                    L"This archive entry is executable or script-like:\n\n" +
                        file.filename().wstring() +
                        L"\n\nOnly open it if you trust the archive.",
                    axiom::gui::MessageDialogIcon::warning,
                    L"Open archive file",
                    axiom::gui::MessageDialogButtons::yes_no,
                    IDNO) != IDYES) {
                return false;
            }
        }

        std::wstring parameters;
        const std::wstring directory = file.parent_path().wstring();
        SHELLEXECUTEINFOW execute{};
        execute.cbSize = sizeof(execute);
        execute.fMask = SEE_MASK_NOCLOSEPROCESS;
        execute.hwnd = hwnd_;
        execute.nShow = SW_SHOWNORMAL;
        const std::wstring& external_tool = !application_options_.external_viewer.empty()
            ? application_options_.external_viewer
            : application_options_.external_editor;
        if (!external_tool.empty()) {
            parameters = quote_argument(file);
            execute.lpFile = external_tool.c_str();
            execute.lpParameters = parameters.c_str();
            execute.lpDirectory = directory.c_str();
        } else {
            execute.lpVerb = L"open";
            execute.lpFile = file.c_str();
            execute.lpDirectory = directory.c_str();
        }
        if (!ShellExecuteExW(&execute)) {
            throw std::runtime_error("Windows could not open this file type");
        }

        if (!application_options_.keep_viewed_files_until_exit && !staging_root.empty()) {
            const bool wipe = sensitive_temp && application_options_.wipe_encrypted_temp_files;
            const fs::path cleanup_root = staging_root;
            HANDLE process = execute.hProcess;
            std::thread([cleanup_root, wipe, process] {
                if (process != nullptr) {
                    WaitForSingleObject(process, INFINITE);
                    CloseHandle(process);
                } else {
                    Sleep(10000);
                }
                remove_temp_directory(cleanup_root, wipe);
            }).detach();
        } else if (execute.hProcess != nullptr) {
            CloseHandle(execute.hProcess);
        }
        return true;
    }

    bool signature_key_is_trusted(const axiom::ArchiveSignatureInfo& info) const {
        if (!info.present || !info.valid) return false;
        if (application_options_.trusted_keys_folder.empty()) return true;
        std::error_code iterate_error;
        for (fs::directory_iterator it(application_options_.trusted_keys_folder,
                                       fs::directory_options::skip_permission_denied,
                                       iterate_error), end;
             !iterate_error && it != end; it.increment(iterate_error)) {
            std::error_code status_error;
            if (!it->is_regular_file(status_error)) continue;
            if (key_file_contains_public_key(it->path(), info.public_key)) {
                return true;
            }
        }
        return false;
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
            open_archive_path(item.filesystem_path);
        } else if (!item.filesystem_path.empty()) {
            if (joined_archive_path_for_volume(item.filesystem_path) &&
                open_archive_path(item.filesystem_path)) {
                return;
            }
            ShellExecuteW(hwnd_, L"open", item.filesystem_path.c_str(), nullptr,
                          item.filesystem_path.parent_path().c_str(), SW_SHOWNORMAL);
        } else if (history_.current().kind == axiom::gui::BrowserLocationKind::archive &&
                   !item.archive_path.empty()) {
            const fs::path archive = history_.current().archive_path;
            if (application_options_.file_open_mode == 1 &&
                show_app_message(L"Extract this archive entry to a temporary folder and open it?\n\n" +
                                     item.name,
                                 axiom::gui::MessageDialogIcon::question,
                                 L"Open archive file",
                                 axiom::gui::MessageDialogButtons::yes_no,
                                 IDYES) != IDYES) {
                return;
            }
            auto password = password_for_archive_edit(archive);
            if (!password) return;
            try {
                const bool sensitive_temp = !password->empty();
                const auto staged = extract_archive_entries_to_staging(
                    archive, {item.archive_path}, *password, false, sensitive_temp);
                secure_clear(*password);
                if (staged.paths.empty()) {
                    throw std::runtime_error("the archive file was not extracted");
                }
                if (launch_viewed_archive_file(staged.paths.front(), staged.directory,
                                               sensitive_temp)) {
                    set_status(L"Opened " + item.name + L" from the archive.");
                }
            } catch (const axiom::OperationCancelled&) {
                secure_clear(*password);
                set_status(L"Opening the archive file was cancelled.");
            } catch (const std::exception& error) {
                secure_clear(*password);
                show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                                 L"Open archive file");
            }
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
        dialog_options.dictionary_size = selected_dictionary_size_ != 0
            ? selected_dictionary_size_
            : application_options_.default_dictionary_size;
        dialog_options.word_size = selected_word_size_ != 0
            ? selected_word_size_
            : application_options_.default_word_size;
        dialog_options.solid_block_size = selected_solid_block_size_ != 0
            ? selected_solid_block_size_
            : application_options_.default_solid_block_size;
        dialog_options.feature_availability = implemented_feature_availability();
        dialog_options.features.update_mode =
            update_mode == axiom::gui::ArchiveUpdateMode::create_new
                ? static_cast<axiom::gui::ArchiveUpdateMode>(
                      std::clamp(application_options_.default_update_mode, 0, 4))
                : update_mode;
        dialog_options.features.volume_size = application_options_.default_volume_size;
        dialog_options.features.volume_unit =
            std::clamp(application_options_.default_volume_unit, 0, 3);
        dialog_options.features.recovery_percent =
            std::clamp(application_options_.default_recovery_percent, 0, 100);
        dialog_options.features.create_recovery_volumes =
            application_options_.default_recovery_volumes;
        dialog_options.features.create_sfx = application_options_.default_create_sfx;
        dialog_options.features.sign_archive = application_options_.default_sign_archive;
        dialog_options.features.signing_key = application_options_.default_signing_key;
        const fs::path base = history_.current().kind == axiom::gui::BrowserLocationKind::filesystem
            ? history_.current().filesystem_path
            : paths.front().parent_path();
        fs::path default_archive = base / L"Archive.axar";
        if (application_options_.archive_output_mode == 1 &&
            !persisted_settings_.last_archive_output_folder.empty()) {
            default_archive =
                fs::path(persisted_settings_.last_archive_output_folder) / L"Archive.axar";
        } else if (application_options_.archive_output_mode == 2 &&
            !application_options_.archive_output_folder.empty()) {
            default_archive = fs::path(application_options_.archive_output_folder) / L"Archive.axar";
        }
        dialog_options.archive_path = target_archive.value_or(default_archive);
        if (target_archive) {
            try {
                const auto mode = axiom::archive_encryption_mode(*target_archive);
                if (mode == axiom::ArchiveEncryptionMode::data_and_directory) {
                    show_app_message(
                        L"Editing archives with encrypted file names is not supported yet.",
                        axiom::gui::MessageDialogIcon::information);
                    return;
                }
                // Existing plaintext archives cannot be converted to encrypted form
                // by append/update, and existing data-only archives cannot switch to
                // encrypted names without a complete format rewrite.
                dialog_options.feature_availability.header_encryption = false;
                if (mode == axiom::ArchiveEncryptionMode::none) {
                    dialog_options.feature_availability.encryption = false;
                }
                if (mode == axiom::ArchiveEncryptionMode::data_only) {
                    auto password = password_for_archive_edit(*target_archive);
                    if (!password) return;
                    dialog_options.features.encrypt_data = true;
                    dialog_options.features.password = widen(*password);
                    secure_clear(*password);
                }
                dialog_options.features.comment = widen(axiom::archive_comment(
                    *target_archive, dialog_options.features.encrypt_data
                        ? utf8(dialog_options.features.password) : std::string{}));
            } catch (...) {
                // The operation itself will report a precise archive error if necessary.
            }
        }
        if (!axiom::gui::show_create_archive_dialog(hwnd_, paths.size(), dialog_options)) return;

        inputs_ = std::move(paths);
        selected_level_ = dialog_options.level;
        selected_thread_count_ = dialog_options.thread_count;
        selected_dictionary_size_ = dialog_options.dictionary_size;
        selected_word_size_ = dialog_options.word_size;
        selected_solid_block_size_ = dialog_options.solid_block_size;
        pending_archive_path_ = std::move(dialog_options.archive_path);
        if (pending_archive_path_.has_parent_path()) {
            persisted_settings_.last_archive_output_folder =
                pending_archive_path_.parent_path().wstring();
            save_current_settings();
        }
        pending_archive_features_ = std::move(dialog_options.features);
        on_compress();
    }

    void on_add_to_archive() {
        if (history_.current().kind == axiom::gui::BrowserLocationKind::archive) {
            const auto archive = active_archive_path();
            if (!archive || !active_archive_is_editable()) return;
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
        if (capabilities.locked || capabilities.directory_encrypted) {
            show_app_message(
                capabilities.locked
                    ? L"This archive is locked and cannot be changed."
                    : L"Editing archives with encrypted file names is not supported yet.",
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
        if (!capabilities.locked && !capabilities.directory_encrypted) return true;
        show_app_message(
            capabilities.locked
                ? L"This archive is locked and cannot be changed."
                : L"Editing archives with encrypted file names is not supported yet.",
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
            std::string metadata_password;
            try {
                const auto encryption = axiom::archive_encryption_mode(*archive);
                if (encryption == axiom::ArchiveEncryptionMode::data_and_directory) {
                    auto password = password_for_archive_edit(*archive);
                    if (!password) return;
                    metadata_password = std::move(*password);
                }
                message += encryption != axiom::ArchiveEncryptionMode::none
                    ? L"\nEncryption: File data encrypted"
                    : L"\nEncryption: None";
                message += axiom::archive_is_locked(*archive, metadata_password)
                    ? L"\nState: Locked (read-only)"
                    : L"\nState: Editable";
                const std::wstring comment = widen(
                    axiom::archive_comment(*archive, metadata_password));
                secure_clear(metadata_password);
                if (!comment.empty()) message += L"\nComment: " + comment;
            } catch (...) {
                secure_clear(metadata_password);
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

    void on_repair_archive() {
        const auto archive = active_archive_path();
        if (!archive || busy_) return;
        axiom::ArchiveRecoveryInfo info;
        try {
            info = axiom::archive_recovery_info(*archive);
        } catch (const std::exception& error) {
            show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                             L"Repair archive");
            return;
        }
        if (!info.present) {
            show_app_message(L"This archive does not contain a recovery record.",
                             axiom::gui::MessageDialogIcon::information,
                             L"Repair archive");
            return;
        }
        if (show_app_message(
                L"Check every recovery shard and reconstruct damaged archive data?\n\n"
                L"The archive is replaced atomically only after reconstruction succeeds.",
                axiom::gui::MessageDialogIcon::question, L"Repair archive",
                axiom::gui::MessageDialogButtons::yes_no, IDYES) != IDYES) {
            return;
        }
        operation_archive_output_ = *archive;
        start_operation(
            L"Repairing archive...", L"Archive recovery data was verified and rebuilt.",
            [archive = *archive](std::shared_ptr<axiom::OperationControl> operation) {
                if (!axiom::repair_archive(archive, operation)) {
                    throw std::runtime_error("archive has no recovery record");
                }
            });
    }

    void on_create_recovery_volumes() {
        const auto archive = active_archive_path();
        if (!archive || busy_) return;
        axiom::gui::ArchiveFeatureOptions archive_options;
        axiom::gui::ExtractFeatureOptions extract_options;
        axiom::gui::ArchiveFeatureAvailability availability;
        availability.recovery = true;
        availability.volumes = true;
        try {
            const auto info = axiom::archive_recovery_info(*archive);
            archive_options.recovery_percent = info.present
                ? static_cast<int>(info.percent) : 10;
        } catch (...) {
            archive_options.recovery_percent = 10;
        }
        if (!axiom::gui::show_archive_feature_options_dialog(
                hwnd_, axiom::gui::ArchiveFeatureDialogContext::create_or_update,
                archive_options, extract_options, availability)) {
            return;
        }
        const auto size = parse_volume_size(archive_options.volume_size,
                                            archive_options.volume_unit);
        if (!size) {
            show_app_message(L"Enter a valid positive split-volume size, or leave it blank.",
                             axiom::gui::MessageDialogIcon::warning,
                             L"Recovery and volumes");
            return;
        }
        const unsigned percent = static_cast<unsigned>(
            std::clamp(archive_options.recovery_percent, 0, 100));
        const bool split = *size != 0;
        if (archive_options.create_recovery_volumes && !split) {
            show_app_message(L"Set a split-volume size before enabling recovery volumes.",
                             axiom::gui::MessageDialogIcon::warning,
                             L"Recovery and volumes");
            return;
        }
        operation_archive_output_ = *archive;
        start_operation(
            split ? L"Creating recovery and split volumes..."
                  : L"Updating archive recovery record...",
            split ? L"Archive and recovery volumes created beside the source archive."
                  : (percent == 0 ? L"Archive recovery record removed."
                                  : L"Archive recovery record updated."),
            [archive = *archive, percent, split, volume_size = *size,
             make_recovery_volumes = archive_options.create_recovery_volumes](
                std::shared_ptr<axiom::OperationControl> operation) {
                axiom::set_archive_recovery(archive, percent, operation);
                if (!split) return;
                const std::uint64_t archive_bytes = fs::file_size(archive);
                const std::uint64_t data_count = std::max<std::uint64_t>(
                    1, (archive_bytes + volume_size - 1) / volume_size);
                const unsigned recovery_count = make_recovery_volumes
                    ? static_cast<unsigned>(std::max<std::uint64_t>(
                        1, (data_count * std::max(percent, 10u) + 99) / 100))
                    : 0;
                axiom::create_archive_volumes(
                    archive, volume_size, recovery_count, operation);
            });
    }

    void on_about() {
        axiom::gui::show_about_dialog(hwnd_, instance_, dpi_, theme_.dark, kCheckUpdates);
    }

    void on_benchmark() {
        axiom::gui::show_benchmark_dialog(hwnd_, instance_, dpi_, theme_.dark);
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
            selected_dictionary_size_ = application_options_.default_dictionary_size;
            selected_word_size_ = application_options_.default_word_size;
            selected_solid_block_size_ = application_options_.default_solid_block_size;
            if (!application_options_.cache_passwords ||
                application_options_.password_prompt_mode != 0) {
                clear_archive_password();
            }
            apply_theme();
            apply_table_options();
            cleanup_old_temp_directories();
            apply_shell_integration();
            save_current_settings();
            on_navigate_refresh();
        }
    }

    void apply_shell_command(bool enabled, const std::wstring& key,
                             const std::wstring& label,
                             const std::wstring& command) const {
        if (!enabled) {
            delete_registry_tree(HKEY_CURRENT_USER, key);
            return;
        }
        set_registry_string(HKEY_CURRENT_USER, key, nullptr, label);
        set_registry_string(HKEY_CURRENT_USER, key + L"\\command", nullptr, command);
    }

    void apply_shell_integration() const {
        const fs::path executable = current_executable_path();
        if (executable.empty()) return;
        constexpr wchar_t prog_id[] = L"AxiomCompress.Archive";
        const std::wstring classes = L"Software\\Classes\\";
        const std::wstring axar_key = classes + L".axar";
        const std::wstring prog_key = classes + prog_id;
        const std::wstring axar_context = classes + L"SystemFileAssociations\\.axar\\shell\\";

        if (application_options_.associate_axar) {
            set_registry_string(HKEY_CURRENT_USER, axar_key, nullptr, prog_id);
            set_registry_string(HKEY_CURRENT_USER, prog_key, nullptr, L"Axiom archive");
            set_registry_string(HKEY_CURRENT_USER, prog_key + L"\\DefaultIcon", nullptr,
                                quote_argument(executable) + L",0");
            set_registry_string(HKEY_CURRENT_USER, prog_key + L"\\shell\\open\\command", nullptr,
                                quoted_executable_command(executable, L"\"%1\""));
        } else {
            if (registry_string(HKEY_CURRENT_USER, axar_key, nullptr) == prog_id) {
                delete_registry_tree(HKEY_CURRENT_USER, axar_key);
            }
            delete_registry_tree(HKEY_CURRENT_USER, prog_key);
        }

        apply_shell_command(application_options_.context_open,
                            axar_context + L"AxiomOpen",
                            L"Open with Axiom",
                            quoted_executable_command(executable, L"\"%1\""));
        apply_shell_command(application_options_.context_extract,
                            axar_context + L"AxiomExtract",
                            L"Extract with Axiom...",
                            quoted_executable_command(executable, L"--extract \"%1\""));
        apply_shell_command(application_options_.context_test,
                            axar_context + L"AxiomTest",
                            L"Test with Axiom",
                            quoted_executable_command(executable, L"--test \"%1\""));
        apply_shell_command(application_options_.context_add,
                            classes + L"*\\shell\\AxiomAdd",
                            L"Add to Axiom archive...",
                            quoted_executable_command(executable, L"--add \"%1\""));
        apply_shell_command(application_options_.context_add,
                            classes + L"Directory\\shell\\AxiomAdd",
                            L"Add to Axiom archive...",
                            quoted_executable_command(executable, L"--add \"%1\""));
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }

    bool open_archive_path(const fs::path& path) {
        if (const auto joined = joined_archive_path_for_volume(path)) {
            try {
                const auto info = axiom::archive_volume_set_info(path);
                std::wstring prompt =
                    L"This is one volume of a split Axiom archive. Reconstruct and open the "
                    L"complete archive?\n\n" + joined->wstring() + L"\n\n" +
                    std::to_wstring(info.data_volumes) + L" data volume(s), " +
                    std::to_wstring(info.recovery_volumes) + L" recovery volume(s)";
                std::error_code exists_error;
                if (fs::exists(*joined, exists_error)) {
                    prompt += L"\n\nThe existing complete archive will be replaced.";
                }
                if (show_app_message(prompt, axiom::gui::MessageDialogIcon::question,
                                     L"Open split archive",
                                     axiom::gui::MessageDialogButtons::yes_no,
                                     IDYES) != IDYES) {
                    return true;
                }
                operation_archive_output_ = *joined;
                operation_open_after_ = *joined;
                const fs::path volume = path;
                const fs::path output = *joined;
                start_operation(
                    L"Joining archive volumes...", L"Archive volumes joined successfully.",
                    [volume, output](const std::shared_ptr<axiom::OperationControl>& operation) {
                        axiom::join_archive_volumes(volume, output, operation);
                    });
                return true;
            } catch (const axiom::FormatError&) {
                // A normal file can contain `.part` or `.rev` in its name. Only a
                // validated Axiom volume is intercepted here.
            } catch (const std::exception& error) {
                show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                                 L"Open split archive");
                return true;
            }
        }
        if (!axiom::gui::is_axiom_archive(path)) return false;
        navigate_to(axiom::gui::BrowserLocation::archive(path));
        return true;
    }

    void on_open_archive() {
        auto path = pick_open_archive(hwnd_);
        if (path && !open_archive_path(*path)) {
            show_app_message(L"The selected file is not an Axiom archive or volume.",
                             axiom::gui::MessageDialogIcon::warning, L"Open archive");
        }
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
        if (paths.size() == 1 && open_archive_path(paths.front())) {
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
        persisted_settings_.has_placement = application_options_.restore_window_placement &&
            GetWindowPlacement(hwnd_, &persisted_settings_.placement) != FALSE;
        axiom::gui::save_gui_settings(persisted_settings_);
    }

    fs::path log_file_path() const {
        fs::path folder;
        if (!application_options_.log_folder.empty()) {
            folder = application_options_.log_folder;
        } else if (const auto local = known_folder_path(FOLDERID_LocalAppData)) {
            folder = *local / L"AxiomCompress" / L"Logs";
        } else {
            folder = fs::temp_directory_path() / L"AxiomCompress" / L"Logs";
        }
        std::error_code ignored;
        fs::create_directories(folder, ignored);
        return folder / L"Axiom.log";
    }

    void append_log(const std::wstring& message) const {
        if (!application_options_.verbose_logging) return;
        try {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            wchar_t stamp[64]{};
            swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u ",
                       now.wYear, now.wMonth, now.wDay, now.wHour,
                       now.wMinute, now.wSecond);
            std::ofstream log(log_file_path(), std::ios::binary | std::ios::app);
            if (!log) return;
            const std::string line = utf8(std::wstring(stamp) + message + L"\r\n");
            log.write(line.data(), static_cast<std::streamsize>(line.size()));
        } catch (...) {
        }
    }

    void apply_operation_priority() {
        operation_priority_changed_ = false;
        previous_priority_class_ = 0;
        DWORD desired = 0;
        if (application_options_.worker_priority == 1) {
            desired = BELOW_NORMAL_PRIORITY_CLASS;
        } else if (application_options_.worker_priority == 2) {
            desired = IDLE_PRIORITY_CLASS;
        }
        if (desired == 0) return;
        HANDLE process = GetCurrentProcess();
        previous_priority_class_ = GetPriorityClass(process);
        if (previous_priority_class_ != 0 && previous_priority_class_ != desired &&
            SetPriorityClass(process, desired)) {
            operation_priority_changed_ = true;
        }
    }

    void restore_operation_priority() {
        if (operation_priority_changed_ && previous_priority_class_ != 0) {
            SetPriorityClass(GetCurrentProcess(), previous_priority_class_);
        }
        operation_priority_changed_ = false;
        previous_priority_class_ = 0;
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
        append_log(L"Starting operation: " + running);
        apply_operation_priority();
        if (!operation_runner_.start(hwnd_, kOperationDoneMessage, kOperationProgressMessage,
                                     std::move(running), std::move(success), std::move(work))) {
            restore_operation_priority();
            append_log(L"Could not start operation: another operation is already running.");
            operation_window_.close();
            set_busy(false);
            set_status(L"Another operation is already running.");
            on_navigate_refresh();
        }
    }

    static std::optional<std::uint64_t> parse_volume_size(
        const std::wstring& text, int unit) {
        if (text.empty()) return std::uint64_t{0};
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long long value = _wcstoui64(text.c_str(), &end, 10);
        if (errno == ERANGE || end == text.c_str() || *end != L'\0' || value == 0 ||
            unit < 0 || unit > 3) {
            return std::nullopt;
        }
        std::uint64_t multiplier = 1024;
        for (int index = 0; index < unit; ++index) {
            if (multiplier > std::numeric_limits<std::uint64_t>::max() / 1024) {
                return std::nullopt;
            }
            multiplier *= 1024;
        }
        if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(value) * multiplier;
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
        options.encrypt_header = pending_archive_features_.encrypt_names;
        options.recovery_percent = static_cast<unsigned>(
            std::clamp(pending_archive_features_.recovery_percent, 0, 100));
        options.password = pending_archive_features_.encrypt_data
            ? utf8(pending_archive_features_.password)
            : std::string{};
        const auto volume_size = parse_volume_size(
            pending_archive_features_.volume_size, pending_archive_features_.volume_unit);
        if (!volume_size) {
            show_app_message(L"Enter a valid positive split-volume size, or leave it blank.",
                             axiom::gui::MessageDialogIcon::warning,
                             L"Recovery and volumes");
            return;
        }
        const bool split_after = *volume_size != 0;
        const bool recovery_volumes = pending_archive_features_.create_recovery_volumes;
        if (recovery_volumes && !split_after) {
            show_app_message(L"Set a split-volume size before enabling recovery volumes.",
                             axiom::gui::MessageDialogIcon::warning,
                             L"Recovery and volumes");
            return;
        }
        const std::string comment = utf8(pending_archive_features_.comment);
        const bool set_comment = mode != axiom::gui::ArchiveUpdateMode::create_new ||
                                 !comment.empty();
        const bool repack_after = pending_archive_features_.repack_after_update &&
                                  mode != axiom::gui::ArchiveUpdateMode::create_new;
        const bool lock_after = pending_archive_features_.lock_archive;
        const bool sign_after = pending_archive_features_.sign_archive;
        const bool create_sfx_after = pending_archive_features_.create_sfx;
        if (create_sfx_after && split_after) {
            show_app_message(L"Self-extracting archives and split volumes are separate output "
                             L"modes. Disable one of them.",
                             axiom::gui::MessageDialogIcon::warning,
                             L"Archive output");
            return;
        }
        fs::path sfx_output = pending_archive_features_.sfx_destination;
        if (create_sfx_after && sfx_output.empty()) {
            sfx_output = archive;
            sfx_output.replace_extension(L".exe");
        }
        if (application_options_.confirm_overwrite &&
            mode == axiom::gui::ArchiveUpdateMode::create_new) {
            std::error_code exists_error;
            const fs::path output_path = create_sfx_after ? sfx_output : archive;
            if (!output_path.empty() && fs::exists(output_path, exists_error)) {
                if (show_app_message(
                        L"Replace the existing output file?\n\n" + output_path.wstring(),
                        axiom::gui::MessageDialogIcon::warning,
                        L"Overwrite output",
                        axiom::gui::MessageDialogButtons::yes_no,
                        IDNO) != IDYES) {
                    return;
                }
            }
        }
        const fs::path sfx_stub = current_executable_path();
        axiom::ArchiveSigningKey signing_key;
        if (sign_after) {
            std::ifstream key_file(pending_archive_features_.signing_key, std::ios::binary);
            if (!key_file ||
                !key_file.read(reinterpret_cast<char*>(signing_key.secret_key.data()),
                               static_cast<std::streamsize>(signing_key.secret_key.size())) ||
                key_file.peek() != std::char_traits<char>::eof()) {
                show_app_message(L"Choose a valid 64-byte Axiom signing key.",
                                 axiom::gui::MessageDialogIcon::error,
                                 L"Sign archive");
                return;
            }
            std::copy_n(signing_key.secret_key.begin() + 32,
                        signing_key.public_key.size(), signing_key.public_key.begin());
        }
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
        if (create_sfx_after) {
            if (mode == axiom::gui::ArchiveUpdateMode::create_new) {
                running = L"Creating self-extracting archive...";
            }
            success = L"Self-extracting archive created: " + sfx_output.wstring();
        } else if (split_after) {
            running = L"Creating split archive volumes...";
            success = L"Split archive volumes created beside: " + archive.wstring();
        }
        operation_archive_output_ = archive;
        start_operation(std::move(running), std::move(success),
                        [inputs, archive, options, mode, comment, set_comment,
                         repack_after, lock_after, sign_after, signing_key,
                         create_sfx_after, sfx_output, sfx_stub, split_after,
                         volume_size = *volume_size, recovery_volumes](
                            std::shared_ptr<axiom::OperationControl> operation) mutable {
                            auto run_options = options;
                            run_options.operation = operation;
                            try {
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
                                if (sign_after) {
                                    axiom::sign_archive(archive, signing_key, run_options);
                                    SecureZeroMemory(signing_key.secret_key.data(),
                                                     signing_key.secret_key.size());
                                }
                                if (lock_after) {
                                    axiom::lock_archive(archive, run_options);
                                }
                                if (split_after) {
                                    const std::uint64_t archive_bytes = fs::file_size(archive);
                                    const std::uint64_t data_volumes = std::max<std::uint64_t>(
                                        1, (archive_bytes + volume_size - 1) / volume_size);
                                    const unsigned recovery_count = recovery_volumes
                                        ? static_cast<unsigned>(std::max<std::uint64_t>(
                                            1, (data_volumes *
                                                std::max<unsigned>(options.recovery_percent, 10) +
                                                99) / 100))
                                        : 0;
                                    axiom::create_archive_volumes(
                                        archive, volume_size, recovery_count, operation);
                                    std::error_code remove_error;
                                    if (!fs::remove(archive, remove_error) && remove_error) {
                                        throw fs::filesystem_error(
                                            "could not remove the unsplit archive", archive,
                                            remove_error);
                                    }
                                }
                                if (create_sfx_after) {
                                    axiom::create_sfx_archive(archive, sfx_stub, sfx_output,
                                                              operation);
                                    std::error_code remove_error;
                                    if (!fs::remove(archive, remove_error) && remove_error) {
                                        std::error_code cleanup_error;
                                        fs::remove(sfx_output, cleanup_error);
                                        throw fs::filesystem_error(
                                            "could not remove the intermediate SFX archive",
                                            archive, remove_error);
                                    }
                                }
                            } catch (...) {
                                if (create_sfx_after &&
                                    mode == axiom::gui::ArchiveUpdateMode::create_new) {
                                    std::error_code cleanup_error;
                                    fs::remove(archive, cleanup_error);
                                }
                                throw;
                            }
                        });
    }

    fs::path current_executable_path() const {
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size()) return {};
        buffer.resize(length);
        return fs::path(std::move(buffer));
    }

    void on_create_sfx() {
        const auto archive = active_archive_path();
        if (!archive || busy_) return;
        fs::path output = *archive;
        output.replace_extension(L".exe");
        if (show_app_message(L"Create this self-extracting archive?\n\n" + output.wstring(),
                             axiom::gui::MessageDialogIcon::question, L"Create SFX",
                             axiom::gui::MessageDialogButtons::yes_no, IDYES) != IDYES) {
            return;
        }
        const fs::path stub = current_executable_path();
        operation_archive_output_.clear();
        start_operation(L"Creating self-extracting archive...",
                        L"Self-extracting archive created: " + output.wstring(),
                        [archive = *archive, stub, output](
                            std::shared_ptr<axiom::OperationControl> operation) {
                            axiom::create_sfx_archive(archive, stub, output, operation);
                        });
    }

    void on_verify_archive_signature() {
        const auto archive = active_archive_path();
        if (!archive) return;
        std::string password;
        try {
            if (axiom::archive_is_encrypted(*archive)) {
                std::wstring entered;
                if (!axiom::gui::show_archive_password_dialog(hwnd_, entered)) return;
                password = utf8(entered);
                secure_clear(entered);
            }
            const auto info = axiom::verify_archive_signature(*archive, password);
            if (!info.present) {
                show_app_message(L"This archive is not signed.",
                                 axiom::gui::MessageDialogIcon::information,
                                 L"Verify signature");
                return;
            }
            const bool trusted = signature_key_is_trusted(info);
            std::wstringstream fingerprint;
            fingerprint << std::hex << std::setfill(L'0');
            for (std::size_t index = 0; index < 8; ++index) {
                fingerprint << std::setw(2) << static_cast<unsigned>(info.public_key[index]);
                if (index == 3) fingerprint << L'-';
            }
            show_app_message(
                std::wstring(info.valid ? L"The archive signature is valid."
                                        : L"The archive signature is invalid.") +
                    L"\n\nSigner fingerprint: " + fingerprint.str() +
                    (info.valid && !application_options_.trusted_keys_folder.empty()
                         ? (trusted ? L"\nTrusted key: yes" : L"\nTrusted key: no")
                         : L""),
                info.valid && trusted ? axiom::gui::MessageDialogIcon::information
                                      : axiom::gui::MessageDialogIcon::error,
                L"Verify signature");
        } catch (const std::exception& error) {
            show_app_message(widen(error.what()), axiom::gui::MessageDialogIcon::error,
                             L"Verify signature");
        }
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
        if (application_options_.extract_destination_mode == 1 &&
            !persisted_settings_.last_extract_destination_folder.empty()) {
            dialog_options.destination =
                fs::path(persisted_settings_.last_extract_destination_folder) / archive->stem();
        } else if (application_options_.extract_destination_mode == 2 &&
            !application_options_.extract_destination_folder.empty()) {
            dialog_options.destination =
                fs::path(application_options_.extract_destination_folder) / archive->stem();
        } else {
            dialog_options.destination = archive->parent_path() / archive->stem();
        }
        dialog_options.feature_availability = implemented_feature_availability();
        dialog_options.features.verify_signature = application_options_.verify_signatures;
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
        if (dialog_options.destination.has_parent_path()) {
            persisted_settings_.last_extract_destination_folder =
                dialog_options.destination.parent_path().wstring();
            save_current_settings();
        }

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
        const bool attempt_recovery = dialog_options.features.attempt_recovery;
        const bool verify_signature = dialog_options.features.verify_signature;
        const std::wstring trusted_keys_folder = application_options_.trusted_keys_folder;

        operation_archive_output_ = attempt_recovery ? *archive : fs::path{};
        start_operation(L"Extracting...",
                        L"Extracted to: " + dialog_options.destination.wstring(),
                        [archive = *archive, output = dialog_options.destination, options,
                         attempt_recovery, verify_signature, trusted_keys_folder](
                            std::shared_ptr<axiom::OperationControl> operation) mutable {
                            auto run_options = options;
                            run_options.operation = operation;
                            if (verify_signature) {
                                const auto signature = axiom::verify_archive_signature(
                                    archive, run_options.password);
                                if (signature.present && !signature.valid) {
                                    throw axiom::FormatError("archive signature is invalid");
                                }
                                if (signature.present && signature.valid &&
                                    !trusted_keys_folder.empty()) {
                                    bool trusted = false;
                                    std::error_code iterate_error;
                                    for (fs::directory_iterator it(
                                             trusted_keys_folder,
                                             fs::directory_options::skip_permission_denied,
                                             iterate_error), end;
                                         !iterate_error && it != end;
                                         it.increment(iterate_error)) {
                                        std::error_code status_error;
                                        if (!it->is_regular_file(status_error)) continue;
                                        if (key_file_contains_public_key(it->path(),
                                                                        signature.public_key)) {
                                            trusted = true;
                                            break;
                                        }
                                    }
                                    if (!trusted) {
                                        throw axiom::FormatError(
                                            "archive signature is valid but not trusted");
                                    }
                                }
                            }
                            try {
                                axiom::extract_archive(archive, output, run_options);
                            } catch (const axiom::FormatError&) {
                                if (!attempt_recovery ||
                                    !axiom::repair_archive(archive, operation)) {
                                    throw;
                                }
                                auto retry_options = run_options;
                                retry_options.overwrite =
                                    axiom::ExtractOptions::Overwrite::overwrite;
                                axiom::extract_archive(archive, output, retry_options);
                            }
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
        operation_archive_output_ = *archive;
        start_operation(L"Testing archive...",
                        L"Archive integrity test passed.",
                        [archive = *archive, options](std::shared_ptr<axiom::OperationControl> operation) mutable {
                            auto run_options = options;
                            run_options.operation = operation;
                            try {
                                axiom::test_archive(archive, run_options);
                            } catch (const axiom::FormatError&) {
                                if (!axiom::repair_archive(archive, operation)) throw;
                                axiom::test_archive(archive, run_options);
                            }
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
        restore_operation_priority();
        operation_window_.close();
        set_busy(false);
        const bool archive_changed = !operation_archive_output_.empty();
        fs::path open_after = std::move(operation_open_after_);
        operation_archive_output_.clear();
        if (archive_changed) archive_catalog_.reset();
        append_log(std::wstring(result->ok ? L"Operation succeeded: "
                                           : result->cancelled ? L"Operation cancelled: "
                                                               : L"Operation failed: ") +
                   result->message);
        set_status(result->message);
        if (result->ok && !open_after.empty()) {
            navigate_to(axiom::gui::BrowserLocation::archive(std::move(open_after)));
        } else {
            on_navigate_refresh();
        }
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
                    if (draw.CtlType == ODT_COMBOBOX && draw.CtlID == kAddressEdit) {
                        draw_address_entry(draw);
                        return TRUE;
                    }
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
                    case kRepairArchive: on_repair_archive(); return 0;
                    case kCreateRecoveryVolumes: on_create_recovery_volumes(); return 0;
                    case kVerifyArchiveSignature: on_verify_archive_signature(); return 0;
                    case kCreateSfx: on_create_sfx(); return 0;
                    case kBenchmark: on_benchmark(); return 0;
                    case kSettings: on_settings(); return 0;
                    case kSelectAll: on_select_all(); return 0;
                    case kAbout: on_about(); return 0;
                    case kCheckUpdates:
                        begin_update_check(axiom::gui::UpdateCheckKind::manual);
                        return 0;
                    case kExitApplication: SendMessageW(hwnd_, WM_CLOSE, 0, 0); return 0;
                    case kAddressEdit:
                        if (HIWORD(wparam) == CBN_DROPDOWN) {
                            populate_address_dropdown();
                            return 0;
                        }
                        if (HIWORD(wparam) == CBN_SELENDOK) {
                            select_address_entry();
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
            case kTableBeginDragMessage:
                on_table_begin_drag();
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
                drop_target_.revoke();
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
    bool operation_priority_changed_ = false;
    DWORD previous_priority_class_ = 0;
    bool update_check_in_progress_ = false;
    bool update_download_in_progress_ = false;
    axiom::gui::OperationRunner operation_runner_;
    fs::path operation_archive_output_;
    fs::path operation_open_after_;
    std::vector<HWND> transient_labels_;
    std::vector<AddressEntry> address_entries_;
    std::vector<std::wstring> recent_addresses_;
    std::vector<fs::path> inputs_;
    axiom::gui::NavigationHistory history_;
    std::vector<axiom::gui::BrowserItem> browser_items_;
    std::shared_ptr<const axiom::gui::ArchiveCatalog> archive_catalog_;
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

std::optional<int> run_embedded_sfx(HINSTANCE instance, const std::wstring& requested_destination) {
    constexpr std::array<char, 8> magic = {'A', 'X', 'I', 'O', 'M', 'S', 'F', 'X'};
    std::wstring module(32768, L'\0');
    const DWORD module_length = GetModuleFileNameW(nullptr, module.data(),
                                                   static_cast<DWORD>(module.size()));
    if (module_length == 0 || module_length >= module.size()) return std::nullopt;
    module.resize(module_length);
    const fs::path executable(module);

    std::ifstream input(executable, std::ios::binary);
    if (!input) return std::nullopt;
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < static_cast<std::streamoff>(16)) return std::nullopt;
    input.seekg(end - static_cast<std::streamoff>(16));
    std::array<char, 8> found{};
    input.read(found.data(), static_cast<std::streamsize>(found.size()));
    std::array<std::uint8_t, 8> encoded_size{};
    input.read(reinterpret_cast<char*>(encoded_size.data()),
               static_cast<std::streamsize>(encoded_size.size()));
    if (!input || found != magic) return std::nullopt;
    std::uint64_t archive_size = 0;
    for (unsigned index = 0; index < 8; ++index) {
        archive_size |= static_cast<std::uint64_t>(encoded_size[index]) << (index * 8);
    }
    const std::uint64_t file_size = static_cast<std::uint64_t>(end);
    if (archive_size == 0 || archive_size > file_size - 16) return std::nullopt;
    const std::uint64_t archive_offset = file_size - 16 - archive_size;

    fs::path destination = requested_destination.empty()
        ? executable.parent_path() / executable.stem()
        : fs::path(requested_destination);
    const bool dark = system_prefers_dark_mode();

    wchar_t temp_path[MAX_PATH + 1]{};
    if (GetTempPathW(MAX_PATH, temp_path) == 0) return 1;
    const fs::path temporary = fs::path(temp_path) /
        (L"AxiomSfx-" + std::to_wstring(GetCurrentProcessId()) + L".axar");
    try {
        input.clear();
        input.seekg(static_cast<std::streamoff>(archive_offset));
        std::ofstream archive(temporary, std::ios::binary | std::ios::trunc);
        if (!archive) throw std::runtime_error("cannot create the temporary archive");
        std::array<char, 1u << 16> buffer{};
        std::uint64_t remaining = archive_size;
        while (remaining > 0) {
            const auto count = static_cast<std::streamsize>(
                std::min<std::uint64_t>(remaining, buffer.size()));
            input.read(buffer.data(), count);
            if (input.gcount() != count) throw std::runtime_error("SFX payload is truncated");
            archive.write(buffer.data(), count);
            remaining -= static_cast<std::uint64_t>(count);
        }
        archive.close();
        axiom::ExtractOptions options;
        options.overwrite = axiom::ExtractOptions::Overwrite::overwrite;
        if (axiom::archive_is_encrypted(temporary)) {
            std::wstring password;
            if (!axiom::gui::show_archive_password_dialog(nullptr, password)) {
                std::error_code ignored;
                fs::remove(temporary, ignored);
                return 0;
            }
            options.password = utf8(password);
            secure_clear(password);
        }
        const auto signature = axiom::verify_archive_signature(temporary, options.password);
        if (signature.present && !signature.valid) {
            throw std::runtime_error("archive authenticity signature is invalid");
        }

        const auto entries = axiom::list_archive(temporary, options.password);
        axiom::gui::SfxArchiveSummary summary;
        summary.archive_name = executable.filename().wstring();
        summary.encrypted = axiom::archive_is_encrypted(temporary);
        summary.signature_present = signature.present;
        summary.signature_valid = signature.valid;
        summary.comment = widen(axiom::archive_comment(temporary, options.password));
        for (const auto& entry : entries) {
            if (entry.is_directory) {
                ++summary.directory_count;
            } else {
                ++summary.file_count;
                summary.unpacked_size += entry.size;
            }
        }

        axiom::gui::SfxExtractDialogOptions dialog_options;
        dialog_options.destination = destination;
        dialog_options.overwrite = axiom::ExtractOptions::Overwrite::overwrite;
        if (!axiom::gui::show_sfx_extract_dialog(nullptr, instance, summary, dialog_options)) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return 0;
        }
        destination = dialog_options.destination;
        options.overwrite = dialog_options.overwrite;
        options.restore_mtime = dialog_options.restore_mtime;
        options.thread_count = dialog_options.thread_count;

        auto operation = std::make_shared<axiom::OperationControl>();
        options.operation = operation;
        std::mutex progress_mutex;
        std::optional<axiom::OperationProgress> latest_progress;
        operation->set_progress_callback([&](const axiom::OperationProgress& progress) {
            std::lock_guard lock(progress_mutex);
            latest_progress = progress;
        });

        std::atomic_bool completed = false;
        std::atomic_bool cancelled = false;
        std::exception_ptr failure;
        axiom::gui::OperationProgressWindow progress_window;
        const auto operation_theme = make_operation_window_theme(make_theme());
        progress_window.create(
            nullptr, instance, L"Extracting archive", {}, operation_theme,
            [operation](bool paused) { operation->set_paused(paused); },
            [operation] { operation->request_cancel(); });

        std::jthread worker([&] {
            try {
                axiom::extract_archive(temporary, destination, options);
            } catch (const axiom::OperationCancelled&) {
                cancelled.store(true, std::memory_order_release);
            } catch (...) {
                failure = std::current_exception();
            }
            completed.store(true, std::memory_order_release);
        });

        while (!completed.load(std::memory_order_acquire)) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    operation->request_cancel();
                    continue;
                }
                if (progress_window.hwnd() != nullptr &&
                    IsDialogMessageW(progress_window.hwnd(), &message)) {
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            std::optional<axiom::OperationProgress> progress;
            {
                std::lock_guard lock(progress_mutex);
                progress.swap(latest_progress);
            }
            if (progress && progress_window.hwnd() != nullptr) {
                progress_window.set_progress(*progress);
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 33, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        worker.join();
        progress_window.close();
        if (failure) std::rethrow_exception(failure);

        std::error_code ignored;
        fs::remove(temporary, ignored);
        if (cancelled.load(std::memory_order_acquire)) return 0;
        if (dialog_options.open_destination) {
            ShellExecuteW(nullptr, L"open", destination.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        axiom::gui::show_message_dialog(
            nullptr, instance, GetDpiForSystem(), dark, L"Axiom Self-Extractor",
            L"Files were extracted to:\n\n" + destination.wstring(),
            axiom::gui::MessageDialogIcon::information);
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        axiom::gui::show_message_dialog(
            nullptr, instance, GetDpiForSystem(), dark, L"Axiom Self-Extractor",
            L"Extraction failed:\n\n" + widen(error.what()),
            axiom::gui::MessageDialogIcon::error);
        return 1;
    }
}

}  // namespace

int run_axiom_gui(HINSTANCE instance,
                  int show_command,
                  std::wstring initial_path,
                  AxiomGuiStartupCommand startup_command) {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    const HRESULT com = OleInitialize(nullptr);
    if (FAILED(com)) {
        axiom::gui::show_message_dialog(
            nullptr, instance, GetDpiForSystem(), system_prefers_dark_mode(),
            L"Axiom", L"Failed to initialize COM.",
            axiom::gui::MessageDialogIcon::error);
        return 1;
    }

    if (const auto sfx_result = run_embedded_sfx(instance, initial_path)) {
        OleUninitialize();
        return *sfx_result;
    }

    MainWindow window;
    if (!window.create(instance, show_command, std::move(initial_path),
                       std::move(startup_command))) {
        OleUninitialize();
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (window.translate_menu_message(message)) continue;
        if (message.message == WM_KEYDOWN) {
            HWND root = GetAncestor(message.hwnd, GA_ROOT);
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            HWND address = GetDlgItem(root, kAddressEdit);
            const bool address_target = address != nullptr &&
                (message.hwnd == address || IsChild(address, message.hwnd));
            if (message.wParam == VK_RETURN && address_target) {
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
                SetFocus(address);
                COMBOBOXINFO info{sizeof(info)};
                if (GetComboBoxInfo(address, &info) && info.hwndItem != nullptr) {
                    SetFocus(info.hwndItem);
                    SendMessageW(info.hwndItem, EM_SETSEL, 0, -1);
                }
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

    OleUninitialize();
    return static_cast<int>(message.wParam);
}
