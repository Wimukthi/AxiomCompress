#define NOMINMAX
#include "gui/archive_dialogs.hpp"
#include "gui/dialog_support.hpp"
#include "gui/archive_feature_dialogs.hpp"
#include "gui/message_dialog.hpp"

#include <dwmapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace axiom::gui {
namespace fs = std::filesystem;
namespace {

constexpr int kPathEdit = 2001;
constexpr int kBrowse = 2002;
constexpr int kLevel = 2003;
constexpr int kThreads = 2006;
constexpr int kOverwrite = 2007;
constexpr int kRestoreTime = 2008;
constexpr int kConfirmDelete = 2009;
constexpr int kShowHidden = 2010;
constexpr int kAdvancedFeatures = 2011;
constexpr int kDictionarySize = 2012;
constexpr int kWordSize = 2013;
constexpr int kSolidBlockSize = 2014;
constexpr int kArchiveFormat = 2015;
constexpr int kCreateTabs = 2099;
constexpr int kCreateTabBase = 2100;
constexpr int kUpdateMode = 2110;
constexpr int kArchiveComment = 2111;
constexpr int kLockArchive = 2112;
constexpr int kRepackAfterUpdate = 2113;
constexpr int kEncryptData = 2120;
constexpr int kEncryptNames = 2121;
constexpr int kPassword = 2122;
constexpr int kConfirmPassword = 2123;
constexpr int kShowPassword = 2124;
constexpr int kVolumeSize = 2130;
constexpr int kVolumeUnit = 2131;
constexpr int kRecoveryPercent = 2132;
constexpr int kRecoveryVolumes = 2133;
constexpr int kSignArchive = 2140;
constexpr int kSigningKey = 2141;
constexpr int kBrowseSigningKey = 2142;
constexpr int kCreateSfx = 2143;
constexpr int kSettingsTabs = 2200;
constexpr int kSettingsTabBase = 2210;
constexpr int kApply = 2220;
constexpr int kDefaults = 2221;
constexpr int kThemeMode = 2230;
constexpr int kStartupMode = 2231;
constexpr int kStartupCustomPath = 2232;
constexpr int kRestoreWindowPlacement = 2233;
constexpr int kConfirmOverwrite = 2234;
constexpr int kRecentLocationCount = 2235;
constexpr int kBrowseStartupCustomPath = 2236;
constexpr int kToolbarIconStyle = 2237;
constexpr int kAccentColorMode = 2238;
constexpr int kCustomAccentColor = 2239;
constexpr int kDefaultUpdateMode = 2250;
constexpr int kDefaultVolumeSize = 2251;
constexpr int kDefaultVolumeUnit = 2252;
constexpr int kDefaultRecoveryPercent = 2253;
constexpr int kDefaultRecoveryVolumes = 2254;
constexpr int kDefaultCreateSfx = 2255;
constexpr int kDefaultSignArchive = 2256;
constexpr int kDefaultSigningKey = 2257;
constexpr int kBrowseDefaultSigningKey = 2258;
constexpr int kArchiveOutputMode = 2300;
constexpr int kArchiveOutputFolder = 2301;
constexpr int kExtractDestinationMode = 2302;
constexpr int kExtractDestinationFolder = 2303;
constexpr int kTempFolderMode = 2304;
constexpr int kTempFolder = 2305;
constexpr int kTempCleanupDays = 2306;
constexpr int kBrowseArchiveOutputFolder = 2307;
constexpr int kBrowseExtractDestinationFolder = 2308;
constexpr int kBrowseTempFolder = 2309;
constexpr int kShowParentEntry = 2350;
constexpr int kShowGridLines = 2351;
constexpr int kShowHorizontalScrollbar = 2352;
constexpr int kFullRowSelect = 2353;
constexpr int kShowAddressShellLocations = 2354;
constexpr int kShowAddressRecentLocations = 2355;
constexpr int kShowAddressArchiveChildren = 2356;
constexpr int kFileOpenMode = 2400;
constexpr int kExternalViewer = 2401;
constexpr int kExternalEditor = 2402;
constexpr int kWarnExecutableOpen = 2403;
constexpr int kKeepViewedFilesUntilExit = 2404;
constexpr int kBrowseExternalViewer = 2405;
constexpr int kBrowseExternalEditor = 2406;
constexpr int kPasswordPromptMode = 2450;
constexpr int kCachePasswords = 2451;
constexpr int kVerifySignatures = 2452;
constexpr int kWipeEncryptedTempFiles = 2453;
constexpr int kTrustedKeysFolder = 2454;
constexpr int kBrowseTrustedKeysFolder = 2455;
constexpr int kAssociateAxar = 2500;
constexpr int kAssociateZip = 2501;
constexpr int kAssociate7z = 2502;
constexpr int kAssociateRar = 2503;
constexpr int kAssociateTar = 2504;
constexpr int kAssociateIso = 2505;
constexpr int kAssociateCab = 2506;
constexpr int kContextOpen = 2510;
constexpr int kContextAdd = 2511;
constexpr int kContextExtract = 2512;
constexpr int kContextTest = 2513;
constexpr int kAutomaticUpdateChecks = 2550;
constexpr int kUpdateChannel = 2551;
constexpr int kUpdateUrl = 2552;
constexpr int kWorkerPriority = 2600;
constexpr int kVerboseLogging = 2601;
constexpr int kLogFolder = 2602;
constexpr int kIoBufferMode = 2603;
constexpr int kIoBufferSize = 2604;
constexpr int kMemoryLimitMode = 2605;
constexpr int kMemoryLimit = 2606;
constexpr int kBrowseLogFolder = 2607;
constexpr int kShortcutCommand = 2650;
constexpr int kShortcutValue = 2651;
constexpr int kShortcutAssign = 2652;
constexpr int kShortcutClear = 2653;
constexpr int kShortcutResetAll = 2654;
constexpr int kAccept = IDOK;
constexpr int kCancel = IDCANCEL;
constexpr std::uint64_t kMinIoBufferSize = 64ull << 10;
constexpr std::uint64_t kMaxIoBufferSize = 64ull << 20;

constexpr std::array<const wchar_t*, 9> kLevelNames{
    L"1 - Fastest", L"2 - Very fast", L"3 - Fast", L"4 - Normal",
    L"5 - Balanced", L"6 - Strong", L"7 - High", L"8 - Very high",
    L"9 - Maximum"};
constexpr std::array<const wchar_t*, 11> kDictionaryNames{
    L"Default for level", L"64 KiB", L"256 KiB", L"1 MiB", L"4 MiB",
    L"8 MiB", L"16 MiB", L"32 MiB", L"64 MiB", L"256 MiB", L"512 MiB"};
constexpr std::array<std::size_t, 11> kDictionaryValues{
    0, 64u << 10, 256u << 10, 1u << 20, 4u << 20, 8u << 20,
    16u << 20, 32u << 20, 64u << 20, 256u << 20, 512u << 20};
constexpr std::array<const wchar_t*, 6> kWordSizeNames{
    L"Default for level", L"32", L"64", L"128", L"192", L"273"};
constexpr std::array<std::size_t, 6> kWordSizeValues{0, 32, 64, 128, 192, 273};
constexpr std::array<const wchar_t*, 10> kSolidBlockNames{
    L"Default for level", L"1 MiB", L"4 MiB", L"8 MiB", L"16 MiB",
    L"32 MiB", L"64 MiB", L"128 MiB", L"256 MiB", L"512 MiB"};
constexpr std::array<std::size_t, 10> kSolidBlockValues{
    0, 1u << 20, 4u << 20, 8u << 20, 16u << 20, 32u << 20,
    64u << 20, 128u << 20, 256u << 20, 512u << 20};
constexpr std::array<const wchar_t*, 5> kCreateTabNames{
    L"Compression", L"General", L"Security", L"Recovery & volumes", L"SFX & signing"};
constexpr std::array<const wchar_t*, 10> kSettingsTabNames{
    L"General", L"Compression", L"Paths", L"File list", L"Viewer",
    L"Security", L"Integration", L"Updates", L"Shortcuts", L"Advanced"};
constexpr std::array<const wchar_t*, 5> kUpdateModeNames{
    L"Create a new archive", L"Add or replace entries",
    L"Update entries that are newer", L"Freshen existing entries",
    L"Synchronize with source"};
constexpr std::array<const wchar_t*, 4> kVolumeUnitNames{
    L"KiB", L"MiB", L"GiB", L"TiB"};
constexpr std::array<const wchar_t*, 3> kThemeModeNames{
    L"Use Windows app theme", L"Dark", L"Light"};
constexpr std::array<const wchar_t*, 7> kAccentColorNames{
    L"Use Windows accent", L"Axiom amber", L"Blue", L"Green", L"Purple", L"Red",
    L"Custom #RRGGBB"};
constexpr std::array<const wchar_t*, 3> kToolbarIconStyleNames{
    L"Theme-tinted monochrome", L"Colorful by command", L"Accent-colored"};
constexpr std::array<const wchar_t*, 4> kStartupLocationNames{
    L"Last location", L"This PC", L"Desktop", L"Custom path"};
constexpr std::array<const wchar_t*, 3> kFolderPolicyNames{
    L"Same as source/archive", L"Last used", L"Custom folder"};
constexpr std::array<const wchar_t*, 3> kTempFolderModeNames{
    L"System temporary folder", L"Axiom temporary folder", L"Custom folder"};
constexpr std::array<const wchar_t*, 2> kFileOpenModeNames{
    L"Extract to temp and open", L"Prompt before opening"};
constexpr std::array<const wchar_t*, 2> kPasswordPromptModeNames{
    L"Once per archive session", L"Every operation"};
constexpr std::array<const wchar_t*, 2> kUpdateChannelNames{
    L"Stable/custom feed", L"Preview/custom feed"};
constexpr std::array<const wchar_t*, 3> kWorkerPriorityNames{
    L"Normal", L"Below normal", L"Background"};
constexpr std::array<const wchar_t*, 2> kAutomaticCustomNames{
    L"Automatic", L"Custom"};

template <std::size_t Size>
int value_index(const std::array<std::size_t, Size>& values, std::size_t value) {
    const auto found = std::find(values.begin(), values.end(), value);
    return found == values.end() ? 0 : static_cast<int>(found - values.begin());
}

std::wstring widen_ascii(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

int archive_format_index(axiom::ArchiveFormat format) {
    const auto formats = axiom::supported_archive_formats();
    for (std::size_t index = 0; index < formats.size(); ++index) {
        if (formats[index].format == format) {
            return static_cast<int>(index);
        }
    }
    return 0;
}

std::vector<const axiom::ArchiveFormatInfo*> creatable_archive_formats() {
    std::vector<const axiom::ArchiveFormatInfo*> result;
    for (const auto& format : axiom::supported_archive_formats()) {
        if (format.format == axiom::ArchiveFormat::axar ||
            format.format == axiom::ArchiveFormat::zip) {
            result.push_back(&format);
        }
    }
    return result;
}

int creatable_archive_format_index(axiom::ArchiveFormat format) {
    const auto formats = creatable_archive_formats();
    for (std::size_t index = 0; index < formats.size(); ++index) {
        if (formats[index]->format == format) {
            return static_cast<int>(index);
        }
    }
    return 0;
}

const axiom::ArchiveFormatInfo& archive_format_info(axiom::ArchiveFormat format) {
    const auto formats = axiom::supported_archive_formats();
    const int index = archive_format_index(format);
    return formats[static_cast<std::size_t>(index)];
}

axiom::ArchiveFormat archive_format_from_path(const fs::path& path,
                                              axiom::ArchiveFormat fallback) {
    if (const auto* provider = axiom::archive_provider_for_path(path)) {
        if (provider->info().format == axiom::ArchiveFormat::axar ||
            provider->info().format == axiom::ArchiveFormat::zip) {
            return provider->info().format;
        }
    }
    return fallback;
}

bool is_known_archive_extension(const fs::path& path) {
    const auto filename = path.filename().wstring();
    if (filename.empty()) {
        return false;
    }
    auto folded = filename;
    std::transform(folded.begin(), folded.end(), folded.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    for (const auto& format : axiom::supported_archive_formats()) {
        std::wstring pattern(format.open_filter_pattern);
        std::size_t start = 0;
        while (start <= pattern.size()) {
            const std::size_t end = pattern.find(L';', start);
            std::wstring part = pattern.substr(start, end == std::wstring::npos
                                                          ? std::wstring::npos
                                                          : end - start);
            if (part.size() > 1 && part[0] == L'*') {
                part.erase(part.begin());
            }
            std::transform(part.begin(), part.end(), part.begin(), [](wchar_t value) {
                return static_cast<wchar_t>(std::towlower(value));
            });
            if (!part.empty() && folded.size() >= part.size() &&
                folded.substr(folded.size() - part.size()) == part) {
                return true;
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        if (_wcsicmp(path.extension().c_str(),
                    widen_ascii(format.default_extension).c_str()) == 0) {
            return true;
        }
    }
    return _wcsicmp(path.extension().c_str(), L".exe") == 0;
}

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
    return dialog_should_use_dark();
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
    COLORREF accent = GetSysColor(COLOR_HIGHLIGHT);
};

struct PageControl {
    HWND window = nullptr;
    int page = 0;
};

struct SettingControl {
    HWND window = nullptr;
    int page = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool wrapped = false;
};

Palette make_palette() {
    Palette result;
    result.dark = use_dark_theme();
    result.accent = dialog_accent_color();
    if (result.dark) {
        result.window = RGB(30, 30, 30);
        result.edit = RGB(36, 36, 36);
        result.button = RGB(45, 45, 48);
        result.hot = RGB(58, 58, 61);
        result.pressed = RGB(72, 72, 76);
        result.border = RGB(63, 63, 67);
        result.text = RGB(242, 242, 242);
        result.muted = RGB(176, 176, 176);
        result.focus = result.accent;
    } else {
        result.focus = result.accent;
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

std::wstring color_to_hex(COLORREF color) {
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"#%02X%02X%02X",
               GetRValue(color), GetGValue(color), GetBValue(color));
    return buffer;
}

std::wstring trim_color_text(std::wstring text) {
    while (!text.empty() && std::iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && std::iswspace(text.back())) text.pop_back();
    return text;
}

std::optional<COLORREF> color_from_hex(std::wstring text) {
    text = trim_color_text(std::move(text));
    if (!text.empty() && text.front() == L'#') {
        text.erase(text.begin());
    }
    if (text.size() != 6) return std::nullopt;
    for (wchar_t ch : text) {
        if (!std::iswxdigit(ch)) return std::nullopt;
    }
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text.c_str(), &end, 16);
    if (end == text.c_str() || *end != L'\0') return std::nullopt;
    return RGB((value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff);
}

std::optional<std::uint64_t> parse_size_text(std::wstring text) {
    text = trim_color_text(std::move(text));
    if (text.empty()) return std::nullopt;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long value = _wcstoui64(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || value == 0) return std::nullopt;
    while (*end != L'\0' && std::iswspace(*end)) ++end;
    std::wstring unit = end;
    std::transform(unit.begin(), unit.end(), unit.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
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

std::optional<fs::path> browse_save_archive(HWND owner,
                                            axiom::ArchiveFormat format,
                                            bool executable = false) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) return std::nullopt;
    const COMDLG_FILTERSPEC archive_filters[] = {
        {L"Axiom archives", L"*.axar"},
        {L"ZIP archives", L"*.zip"},
        {L"All files", L"*.*"}};
    const COMDLG_FILTERSPEC executable_filters[] = {
        {L"Axiom self-extractors", L"*.exe"}, {L"All files", L"*.*"}};
    const auto* filters = executable ? executable_filters : archive_filters;
    dialog->SetFileTypes(executable ? 2 : 3, filters);
    if (!executable) {
        dialog->SetFileTypeIndex(static_cast<UINT>(archive_format_index(format) + 1));
    }
    const auto& info = archive_format_info(format);
    const std::wstring default_extension = executable
        ? L"exe"
        : widen_ascii(info.default_extension).substr(1);
    dialog->SetDefaultExtension(default_extension.c_str());
    if (dialog->Show(owner) != S_OK) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::optional<fs::path> browse_signing_key(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) return std::nullopt;
    const COMDLG_FILTERSPEC filters[] = {
        {L"Axiom signing keys", L"*.key"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(2, filters);
    FILEOPENDIALOGOPTIONS flags{};
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (dialog->Show(owner) != S_OK) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
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

std::optional<fs::path> browse_file(HWND owner,
                                    const wchar_t* title,
                                    const COMDLG_FILTERSPEC* filters,
                                    UINT filter_count,
                                    const fs::path& initial = {}) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) return std::nullopt;
    FILEOPENDIALOGOPTIONS flags{};
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (title != nullptr) dialog->SetTitle(title);
    if (filters != nullptr && filter_count != 0) {
        dialog->SetFileTypes(filter_count, filters);
    }
    set_initial_folder(dialog.get(), initial);
    if (dialog->Show(owner) != S_OK) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    fs::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::optional<fs::path> browse_executable(HWND owner, const fs::path& initial = {}) {
    const COMDLG_FILTERSPEC filters[] = {
        {L"Applications and scripts", L"*.exe;*.com;*.bat;*.cmd"},
        {L"All files", L"*.*"}};
    return browse_file(owner, L"Choose application", filters,
                       static_cast<UINT>(sizeof(filters) / sizeof(filters[0])), initial);
}

constexpr wchar_t kDarkTabClass[] = L"AxiomDarkTabControl";
constexpr UINT kDarkTabSetSelection = WM_APP + 41;

struct DarkTabState {
    int selected = 0;
    int hot = -1;
    HFONT font = nullptr;
    bool tracking_mouse = false;
};

int dark_tab_count(HWND window) {
    return GetDlgCtrlID(window) == kSettingsTabs
        ? static_cast<int>(kSettingsTabNames.size())
        : static_cast<int>(kCreateTabNames.size());
}

const wchar_t* dark_tab_text(HWND window, int index) {
    if (GetDlgCtrlID(window) == kSettingsTabs) {
        return kSettingsTabNames[static_cast<std::size_t>(index)];
    }
    return kCreateTabNames[static_cast<std::size_t>(index)];
}

int dark_tab_command_base(HWND window) {
    return GetDlgCtrlID(window) == kSettingsTabs ? kSettingsTabBase : kCreateTabBase;
}

int tab_at_position(HWND window, int x) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = std::max(1, static_cast<int>(client.right));
    const int count = dark_tab_count(window);
    return std::clamp(x * count / width, 0, count - 1);
}

void select_dark_tab(HWND window, DarkTabState& state, int selection,
                     bool notify_parent) {
    selection = std::clamp(selection, 0, dark_tab_count(window) - 1);
    if (selection == state.selected && !notify_parent) return;
    state.selected = selection;
    InvalidateRect(window, nullptr, FALSE);
    if (notify_parent) {
        SendMessageW(GetParent(window), WM_COMMAND,
                     MAKEWPARAM(dark_tab_command_base(window) + selection, BN_CLICKED),
                     reinterpret_cast<LPARAM>(window));
    }
}

LRESULT CALLBACK dark_tab_window_proc(HWND window, UINT message,
                                      WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<DarkTabState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = new DarkTabState{};
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);

    switch (message) {
        case WM_SETFONT:
            state->font = reinterpret_cast<HFONT>(wparam);
            if (LOWORD(lparam) != 0) InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_GETFONT:
            return reinterpret_cast<LRESULT>(state->font);
        case kDarkTabSetSelection:
            select_dark_tab(window, *state, static_cast<int>(wparam), false);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN: {
            int selection = state->selected;
            if (wparam == VK_LEFT) --selection;
            else if (wparam == VK_RIGHT) ++selection;
            else if (wparam == VK_HOME) selection = 0;
            else if (wparam == VK_END) {
                selection = dark_tab_count(window) - 1;
            } else {
                break;
            }
            select_dark_tab(window, *state, selection, true);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(window);
            select_dark_tab(window, *state,
                            tab_at_position(window, GET_X_LPARAM(lparam)), true);
            return 0;
        case WM_MOUSEMOVE: {
            if (!state->tracking_mouse) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
                TrackMouseEvent(&tracking);
                state->tracking_mouse = true;
            }
            const int hot = tab_at_position(window, GET_X_LPARAM(lparam));
            if (hot != state->hot) {
                state->hot = hot;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            state->tracking_mouse = false;
            state->hot = -1;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC target = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            HDC memory = CreateCompatibleDC(target);
            HBITMAP bitmap = CreateCompatibleBitmap(
                target, std::max(1, static_cast<int>(client.right)),
                std::max(1, static_cast<int>(client.bottom)));
            HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
            const Palette colors = make_palette();
            HBRUSH background = CreateSolidBrush(colors.window);
            FillRect(memory, &client, background);
            DeleteObject(background);
            HFONT font = state->font != nullptr
                ? state->font
                : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            HGDIOBJ old_font = SelectObject(memory, font);
            SetBkMode(memory, TRANSPARENT);
            SetTextColor(memory, colors.text);
            const int count = dark_tab_count(window);
            for (int index = 0; index < count; ++index) {
                RECT tab{MulDiv(client.right, index, count), 0,
                         MulDiv(client.right, index + 1, count), client.bottom};
                const COLORREF fill = index == state->selected
                    ? colors.focus
                    : index == state->hot ? colors.hot : colors.button;
                HBRUSH brush = CreateSolidBrush(fill);
                FillRect(memory, &tab, brush);
                DeleteObject(brush);
                HBRUSH border = CreateSolidBrush(colors.border);
                FrameRect(memory, &tab, border);
                DeleteObject(border);
                RECT text_rect = tab;
                text_rect.left += 8;
                text_rect.right -= 8;
                DrawTextW(memory, dark_tab_text(window, index), -1, &text_rect,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                              DT_NOPREFIX | DT_END_ELLIPSIS);
                if (index == state->selected && GetFocus() == window) {
                    InflateRect(&tab, -3, -3);
                    DrawFocusRect(memory, &tab);
                }
            }
            SelectObject(memory, old_font);
            BitBlt(target, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, old_bitmap);
            DeleteObject(bitmap);
            DeleteDC(memory);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            delete state;
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool register_dark_tab_class(HINSTANCE instance) {
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = dark_tab_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_HAND);
    window_class.lpszClassName = kDarkTabClass;
    return RegisterClassExW(&window_class) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

std::optional<fs::path> browse_folder(HWND owner,
                                      const wchar_t* title = L"Choose folder",
                                      const fs::path& initial = {}) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) return std::nullopt;
    FILEOPENDIALOGOPTIONS flags{};
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (title != nullptr) dialog->SetTitle(title);
    set_initial_folder(dialog.get(), initial);
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
        int width = scale(mode_ == DialogMode::create_archive
            ? 840
            : mode_ == DialogMode::settings ? 900 : 540);
        int height = scale(mode_ == DialogMode::create_archive
            ? 650
            : mode_ == DialogMode::settings ? 740 : 290);
        RECT owner_rect{};
        GetWindowRect(owner, &owner_rect);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) {
            width = std::min(width, static_cast<int>(
                monitor.rcWork.right - monitor.rcWork.left) - scale(24));
            height = std::min(height, static_cast<int>(
                monitor.rcWork.bottom - monitor.rcWork.top) - scale(24));
        }
        const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
        const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
        const wchar_t* title = mode_ == DialogMode::create_archive ? L"Add to archive"
            : (mode_ == DialogMode::extract_archive ? L"Extract archive" : L"Axiom settings");
        const DWORD window_style = WS_POPUP | WS_CAPTION | WS_SYSMENU |
            WS_CLIPCHILDREN |
            (mode_ == DialogMode::create_archive || mode_ == DialogMode::settings
                 ? WS_THICKFRAME | WS_MAXIMIZEBOX : 0);
        const DWORD extended_style = WS_EX_DLGMODALFRAME;
        window_ = CreateWindowExW(extended_style, class_name(), title,
                                  window_style,
                                  x, y, width, height, owner, nullptr, instance_, this);
        if (!window_) return false;
        restore_named_window_placement(window_, owner, layout_name());
        owner_was_enabled_ = disable_dialog_owner(owner);
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        MSG message{};
        while (IsWindow(window_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        restore_dialog_owner(owner_, owner_was_enabled_);
        owner_was_enabled_ = false;
        return accepted_;
    }

    CreateArchiveDialogOptions create_options;
    ExtractArchiveDialogOptions extract_options;
    ApplicationDialogOptions application_options;
    std::function<void(const ApplicationDialogOptions&)> settings_apply_callback;
    std::size_t input_count = 0;
    fs::path archive_path;

private:
    static const wchar_t* class_name() { return L"AxiomDarkOptionsDialog"; }

    const wchar_t* layout_name() const {
        switch (mode_) {
            case DialogMode::create_archive: return L"AddToArchiveDialog";
            case DialogMode::extract_archive: return L"ExtractArchiveDialog";
            case DialogMode::settings: return L"SettingsDialog";
        }
        return L"OptionsDialog";
    }

    bool register_class() {
        static ATOM atom = 0;
        bool dialog_registered = atom != 0;
        if (!dialog_registered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = &OptionsDialog::window_proc;
            wc.hInstance = instance_;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = class_name();
            atom = RegisterClassExW(&wc);
            dialog_registered = atom != 0 ||
                                GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        return dialog_registered && register_dark_tab_class(instance_);
    }

    int scale(int value) const {
        return MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
    }

    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id) {
        HWND result = CreateWindowExW(0, type, text,
                                      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
                                      0, 0, 0, 0, window_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                      instance_, nullptr);
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        apply_dialog_control_theme(result, palette_.dark);
        return result;
    }

    HWND label(const wchar_t* text) {
        return control(L"STATIC", text, SS_LEFT, 0);
    }

    template <std::size_t Size>
    HWND selection_combo(int id, const std::array<const wchar_t*, Size>& items) {
        HWND combo = control(
            L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, id);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, scale(24));
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scale(24));
        for (const wchar_t* item : items) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        }
        return combo;
    }

    HWND thread_combo() {
        HWND combo = control(
            L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN |
                CBS_AUTOHSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, kThreads);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, scale(24));
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scale(24));
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"0 (all processors)"));
        const unsigned int hardware_threads = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned int i = 1; i <= hardware_threads; ++i) {
            const std::wstring value = std::to_wstring(i);
            SendMessageW(combo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(value.c_str()));
        }
        return combo;
    }

    HWND archive_format_combo() {
        HWND combo = control(
            L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, kArchiveFormat);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, scale(24));
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scale(24));
        for (const auto* format : creatable_archive_formats()) {
            const std::wstring label = widen_ascii(format->display_name);
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        return combo;
    }

    HWND page_control(int page, const wchar_t* type, const wchar_t* text,
                      DWORD style, int id) {
        HWND result = control(type, text, style, id);
        page_controls_.push_back({result, page});
        return result;
    }

    HWND page_label(int page, const wchar_t* text, bool wrap = false) {
        return page_control(page, L"STATIC", text,
                            SS_LEFT | SS_NOPREFIX | (wrap ? SS_EDITCONTROL : 0), 0);
    }

    HWND page_edit(int page, int id, DWORD extra_style = ES_AUTOHSCROLL) {
        HWND edit = page_control(page, L"EDIT", L"",
                                 WS_TABSTOP | WS_BORDER | extra_style, id);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(scale(6), scale(6)));
        return edit;
    }

    HWND page_checkbox(int page, int id, const wchar_t* text) {
        return page_control(page, L"BUTTON", text,
                            WS_TABSTOP | BS_AUTOCHECKBOX | BS_OWNERDRAW, id);
    }

    template <std::size_t Size>
    HWND page_combo(int page, int id, const std::array<const wchar_t*, Size>& items,
                    bool editable = false) {
        HWND combo = page_control(
            page, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL |
                (editable ? CBS_DROPDOWN | CBS_AUTOHSCROLL : CBS_DROPDOWNLIST) |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, id);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, scale(24));
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scale(24));
        for (const wchar_t* item : items) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        }
        return combo;
    }

    HWND page_thread_combo(int page) {
        HWND combo = page_control(
            page, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN |
                CBS_AUTOHSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, kThreads);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, scale(24));
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scale(24));
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"0 (all processors)"));
        const unsigned hardware_threads = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned index = 1; index <= hardware_threads; ++index) {
            const std::wstring value = std::to_wstring(index);
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
        }
        return combo;
    }

    HWND setting_control(int page, const wchar_t* type, const wchar_t* text,
                         DWORD style, int id, int x, int y, int width, int height,
                         bool wrapped = false) {
        HWND result = control(type, text, style, id);
        settings_controls_.push_back({result, page, x, y, width, height, wrapped});
        return result;
    }

    HWND setting_label(int page, const wchar_t* text, int x, int y, int width,
                       int height = 24, bool wrapped = false) {
        return setting_control(page, L"STATIC", text,
                               SS_LEFT | SS_NOPREFIX | (wrapped ? SS_EDITCONTROL : 0),
                               0, x, y, width, height, wrapped);
    }

    HWND setting_edit(int page, int id, int x, int y, int width,
                      DWORD extra_style = ES_AUTOHSCROLL) {
        HWND edit = setting_control(page, L"EDIT", L"",
                                    WS_TABSTOP | WS_BORDER | extra_style,
                                    id, x, y, width, 30);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(scale(6), scale(6)));
        return edit;
    }

    HWND setting_browse(int page, int id, int x, int y) {
        return setting_control(page, L"BUTTON", L"Browse...",
                               WS_TABSTOP | BS_OWNERDRAW, id, x, y, 96, 30);
    }

    HWND setting_checkbox(int page, int id, const wchar_t* text,
                          int x, int y, int width = 520) {
        return setting_control(page, L"BUTTON", text,
                               WS_TABSTOP | BS_AUTOCHECKBOX | BS_OWNERDRAW,
                               id, x, y, width, 28);
    }

    template <std::size_t Size>
    HWND setting_combo(int page, int id, const std::array<const wchar_t*, Size>& items,
                       int x, int y, int width) {
        HWND combo = setting_control(
            page, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
            id, x, y, width, 220);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, scale(24));
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scale(24));
        for (const wchar_t* item : items) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        }
        return combo;
    }

    HWND setting_shortcut_command_combo(int page, int x, int y, int width) {
        HWND combo = setting_control(
            page, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
            kShortcutCommand, x, y, width, 320);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, scale(24));
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scale(24));
        for (const auto& command : kShortcutCommandCatalog) {
            const LRESULT item_index = SendMessageW(
                combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(command.label));
            if (item_index != CB_ERR && item_index != CB_ERRSPACE) {
                SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(item_index),
                             reinterpret_cast<LPARAM>(command.id));
            }
        }
        return combo;
    }

    HWND setting_thread_combo(int page, int x, int y, int width) {
        HWND combo = setting_control(
            page, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN |
                CBS_AUTOHSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
            kThreads, x, y, width, 240);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, scale(24));
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scale(24));
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"0 (all processors)"));
        const unsigned hardware_threads = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned index = 1; index <= hardware_threads; ++index) {
            const std::wstring value = std::to_wstring(index);
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
        }
        return combo;
    }

    void create_settings_controls() {
        settings_tabs_ = control(kDarkTabClass, L"", WS_TABSTOP | WS_GROUP,
                                 kSettingsTabs);

        setting_label(0, L"Application behavior", 0, 0, 660);
        setting_label(0, L"Theme", 0, 42, 170);
        setting_combo(0, kThemeMode, kThemeModeNames, 180, 36, 260);
        setting_label(0, L"Accent color", 0, 84, 170);
        setting_combo(0, kAccentColorMode, kAccentColorNames, 180, 78, 260);
        setting_label(0, L"Custom accent", 0, 126, 170);
        setting_edit(0, kCustomAccentColor, 180, 120, 150);
        setting_label(0, L"Use #RRGGBB when Accent color is set to Custom.",
                      344, 126, 380, 32, true);
        setting_label(0, L"Button icons", 0, 168, 170);
        setting_combo(0, kToolbarIconStyle, kToolbarIconStyleNames, 180, 162, 260);
        setting_label(0, L"Startup location", 0, 210, 170);
        setting_combo(0, kStartupMode, kStartupLocationNames, 180, 204, 260);
        setting_label(0, L"Custom startup path", 0, 252, 170);
        setting_edit(0, kStartupCustomPath, 180, 246, 470);
        setting_browse(0, kBrowseStartupCustomPath, 660, 246);
        setting_checkbox(0, kRestoreWindowPlacement,
                         L"Restore previous window size and position", 180, 290);
        setting_checkbox(0, kConfirmDelete, L"Confirm before deleting files", 180, 324);
        setting_checkbox(0, kConfirmOverwrite,
                         L"Confirm before overwriting existing files", 180, 358);
        setting_label(0, L"Recent locations", 0, 402, 170);
        setting_edit(0, kRecentLocationCount, 180, 396, 95, ES_NUMBER | ES_AUTOHSCROLL);
        setting_label(0, L"0 disables recent path entries; 12 matches the current default.",
                      288, 402, 430, 38, true);

        setting_label(1, L"Default Add-to-archive options", 0, 0, 660);
        setting_label(1, L"Compression level", 0, 42, 170);
        setting_combo(1, kLevel, kLevelNames, 180, 36, 260);
        setting_label(1, L"Dictionary size", 0, 84, 170);
        setting_combo(1, kDictionarySize, kDictionaryNames, 180, 78, 260);
        setting_label(1, L"Word size", 0, 126, 170);
        setting_combo(1, kWordSize, kWordSizeNames, 180, 120, 260);
        setting_label(1, L"Solid block size", 0, 168, 170);
        setting_combo(1, kSolidBlockSize, kSolidBlockNames, 180, 162, 260);
        setting_label(1, L"CPU threads", 0, 210, 170);
        setting_thread_combo(1, 180, 204, 190);
        setting_label(1, L"Update mode", 0, 252, 170);
        setting_combo(1, kDefaultUpdateMode, kUpdateModeNames, 180, 246, 330);
        setting_label(1, L"Split volume size", 0, 294, 170);
        setting_edit(1, kDefaultVolumeSize, 180, 288, 160);
        setting_combo(1, kDefaultVolumeUnit, kVolumeUnitNames, 350, 288, 105);
        setting_label(1, L"Recovery record", 0, 336, 170);
        setting_edit(1, kDefaultRecoveryPercent, 180, 330, 80, ES_NUMBER | ES_AUTOHSCROLL);
        setting_label(1, L"%", 268, 336, 24);
        setting_checkbox(1, kDefaultRecoveryVolumes,
                         L"Create recovery volumes when split volumes are enabled", 180, 372);
        setting_checkbox(1, kDefaultCreateSfx,
                         L"Create self-extracting .exe by default", 180, 406);
        setting_checkbox(1, kDefaultSignArchive,
                         L"Sign archives by default", 180, 440);
        setting_label(1, L"Signing key", 0, 482, 170);
        setting_edit(1, kDefaultSigningKey, 180, 476, 470);
        setting_browse(1, kBrowseDefaultSigningKey, 660, 476);

        setting_label(2, L"Default folders", 0, 0, 660);
        setting_label(2, L"Archive output", 0, 42, 170);
        setting_combo(2, kArchiveOutputMode, kFolderPolicyNames, 180, 36, 260);
        setting_label(2, L"Archive folder", 0, 84, 170);
        setting_edit(2, kArchiveOutputFolder, 180, 78, 470);
        setting_browse(2, kBrowseArchiveOutputFolder, 660, 78);
        setting_label(2, L"Extraction output", 0, 126, 170);
        setting_combo(2, kExtractDestinationMode, kFolderPolicyNames, 180, 120, 260);
        setting_label(2, L"Extraction folder", 0, 168, 170);
        setting_edit(2, kExtractDestinationFolder, 180, 162, 470);
        setting_browse(2, kBrowseExtractDestinationFolder, 660, 162);
        setting_label(2, L"Temporary files", 0, 210, 170);
        setting_combo(2, kTempFolderMode, kTempFolderModeNames, 180, 204, 300);
        setting_label(2, L"Temporary folder", 0, 252, 170);
        setting_edit(2, kTempFolder, 180, 246, 470);
        setting_browse(2, kBrowseTempFolder, 660, 246);
        setting_label(2, L"Cleanup after days", 0, 294, 170);
        setting_edit(2, kTempCleanupDays, 180, 288, 80, ES_NUMBER | ES_AUTOHSCROLL);
        setting_label(2,
                      L"Last-used folders are remembered after successful dialog confirmation. "
                      L"Temporary cleanup removes old Axiom staging folders on startup.",
                      0, 340, 660, 54, true);

        setting_label(3, L"File browser", 0, 0, 660);
        setting_checkbox(3, kShowHidden, L"Show hidden and system items", 0, 42);
        setting_checkbox(3, kShowParentEntry, L"Show parent folder entry", 0, 76);
        setting_checkbox(3, kShowGridLines, L"Show row and column grid lines", 0, 110);
        setting_checkbox(3, kShowHorizontalScrollbar, L"Show horizontal scrollbar", 0, 144);
        setting_checkbox(3, kFullRowSelect, L"Use full-row selection", 0, 178);
        setting_checkbox(3, kShowAddressShellLocations,
                         L"Address dropdown: show shell locations and drives", 0, 232);
        setting_checkbox(3, kShowAddressRecentLocations,
                         L"Address dropdown: show recent locations", 0, 266);
        setting_checkbox(3, kShowAddressArchiveChildren,
                         L"Address dropdown: show folders in the current location", 0, 300);

        setting_label(4, L"Viewing files from archives", 0, 0, 660);
        setting_label(4, L"Double-click action", 0, 42, 170);
        setting_combo(4, kFileOpenMode, kFileOpenModeNames, 180, 36, 280);
        setting_label(4, L"External viewer", 0, 84, 170);
        setting_edit(4, kExternalViewer, 180, 78, 470);
        setting_browse(4, kBrowseExternalViewer, 660, 78);
        setting_label(4, L"External editor", 0, 126, 170);
        setting_edit(4, kExternalEditor, 180, 120, 470);
        setting_browse(4, kBrowseExternalEditor, 660, 120);
        setting_checkbox(4, kWarnExecutableOpen,
                         L"Warn before opening executable/script files from archives", 180, 164);
        setting_checkbox(4, kKeepViewedFilesUntilExit,
                         L"Keep viewed temporary files until Axiom exits", 180, 198);
        setting_label(4,
                      L"Opening still uses selective extraction. Editor update-back can be wired "
                      L"when the backend exposes safe replace-in-archive semantics.",
                      0, 252, 660, 54, true);

        setting_label(5, L"Security", 0, 0, 660);
        setting_label(5, L"Password prompts", 0, 42, 170);
        setting_combo(5, kPasswordPromptMode, kPasswordPromptModeNames, 180, 36, 280);
        setting_checkbox(5, kCachePasswords,
                         L"Cache archive passwords in memory while the archive is open", 180, 80);
        setting_checkbox(5, kVerifySignatures,
                         L"Verify signed archives before extraction by default", 180, 114);
        setting_checkbox(5, kWipeEncryptedTempFiles,
                         L"Wipe temporary files extracted from encrypted archives", 180, 148);
        setting_label(5, L"Trusted keys folder", 0, 198, 170);
        setting_edit(5, kTrustedKeysFolder, 180, 192, 470);
        setting_browse(5, kBrowseTrustedKeysFolder, 660, 192);
        setting_label(5,
                      L"Passwords are never written to settings. These options only control "
                      L"prompting, in-memory reuse, verification, and temporary-file handling.",
                      0, 252, 660, 54, true);

        setting_label(6, L"Windows integration", 0, 0, 660);
        setting_checkbox(6, kAssociateAxar, L"Associate .axar files with Axiom", 0, 42);
        setting_checkbox(6, kAssociateZip, L"Associate .zip, .jar, .war, .apk files with Axiom", 0, 76);
        setting_checkbox(6, kAssociate7z, L"Associate .7z files with Axiom", 0, 110);
        setting_checkbox(6, kAssociateRar, L"Associate .rar and RAR volume files with Axiom", 0, 144);
        setting_checkbox(6, kAssociateTar,
                         L"Associate TAR family files (.tar, .tgz, .txz, .tbz2, .tzst)", 0, 178);
        setting_checkbox(6, kAssociateIso, L"Associate .iso images with Axiom", 0, 212);
        setting_checkbox(6, kAssociateCab, L"Associate .cab archives with Axiom", 0, 246);
        setting_checkbox(6, kContextOpen, L"Explorer context menu: Open with Axiom", 0, 300);
        setting_checkbox(6, kContextAdd, L"Explorer context menu: Add to archive", 0, 334);
        setting_checkbox(6, kContextExtract, L"Explorer context menu: Extract here/to folder", 0, 368);
        setting_checkbox(6, kContextTest, L"Explorer context menu: Test archive", 0, 402);
        setting_label(6,
                      L"Integration is written per-user under HKCU and applies/removes Axiom's "
                      L"own file association and Explorer context-menu entries. Read-only "
                      L"formats can be opened, tested, and extracted but not edited.",
                      0, 456, 660, 64, true);

        setting_label(7, L"Updates", 0, 0, 660);
        setting_checkbox(7, kAutomaticUpdateChecks,
                         L"Check for updates automatically on startup", 0, 42);
        setting_label(7, L"Release channel", 0, 92, 170);
        setting_combo(7, kUpdateChannel, kUpdateChannelNames, 180, 86, 260);
        setting_label(7, L"Update feed URL", 0, 134, 170);
        setting_edit(7, kUpdateUrl, 180, 128, 470);
        setting_label(7,
                      L"The existing update checker reads this URL directly. Leave it empty to "
                      L"disable automatic checks until a release feed is configured.",
                      0, 184, 660, 54, true);

        setting_label(8, L"Keyboard shortcuts", 0, 0, 660);
        setting_label(8, L"Command", 0, 42, 170);
        setting_shortcut_command_combo(8, 180, 36, 470);
        setting_label(8, L"Shortcut", 0, 92, 170);
        setting_edit(8, kShortcutValue, 180, 86, 190);
        setting_control(8, L"BUTTON", L"Assign", WS_TABSTOP | BS_OWNERDRAW,
                        kShortcutAssign, 386, 86, 86, 30);
        setting_control(8, L"BUTTON", L"Clear", WS_TABSTOP | BS_OWNERDRAW,
                        kShortcutClear, 482, 86, 86, 30);
        setting_control(8, L"BUTTON", L"Restore defaults", WS_TABSTOP | BS_OWNERDRAW,
                        kShortcutResetAll, 180, 136, 150, 30);
        settings_shortcut_default_label_ = setting_label(8, L"", 350, 142, 360, 24);
        setting_label(8,
                      L"Type shortcuts as text, for example Ctrl+O, Alt+Left, "
                      L"Ctrl+Shift+R, F5, Delete, or None. Duplicate shortcuts are rejected.",
                      0, 206, 660, 58, true);
        setting_label(8,
                      L"Text boxes keep standard editing shortcuts such as Ctrl+A, Ctrl+C, "
                      L"Ctrl+V, Delete, Backspace, and Enter.",
                      0, 276, 660, 58, true);

        setting_label(9, L"Advanced", 0, 0, 660);
        setting_label(9, L"Worker priority", 0, 42, 170);
        setting_combo(9, kWorkerPriority, kWorkerPriorityNames, 180, 36, 240);
        setting_checkbox(9, kVerboseLogging, L"Enable verbose operation logging", 180, 80);
        setting_label(9, L"Log folder", 0, 126, 170);
        setting_edit(9, kLogFolder, 180, 120, 470);
        setting_browse(9, kBrowseLogFolder, 660, 120);
        setting_label(9, L"I/O buffer", 0, 168, 170);
        setting_combo(9, kIoBufferMode, kAutomaticCustomNames, 180, 162, 180);
        setting_edit(9, kIoBufferSize, 370, 162, 160);
        setting_label(9, L"Memory limit", 0, 210, 170);
        setting_combo(9, kMemoryLimitMode, kAutomaticCustomNames, 180, 204, 180);
        setting_edit(9, kMemoryLimit, 370, 204, 160);
        setting_label(9,
                      L"Worker priority, I/O buffer size, and memory limit are applied to GUI "
                      L"operations. Automatic I/O uses 1 MiB; custom values must be 64 KiB "
                      L"through 64 MiB.",
                      0, 264, 660, 72, true);

        accept_ = control(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW, kAccept);
        cancel_ = control(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, kCancel);
        apply_ = control(L"BUTTON", L"Apply", WS_TABSTOP | BS_OWNERDRAW, kApply);
        defaults_ = control(L"BUTTON", L"Defaults...", WS_TABSTOP | BS_OWNERDRAW, kDefaults);
        load_settings_values();
        select_settings_page(0);
    }

    void create_create_controls() {
        summary_ = label(L"");
        path_label_ = label(L"Output");
        path_edit_ = control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kPathEdit);
        browse_ = control(L"BUTTON", L"Browse...", WS_TABSTOP | BS_OWNERDRAW, kBrowse);
        SendMessageW(path_edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(scale(6), scale(6)));
        format_label_ = label(L"Format");
        format_combo_ = archive_format_combo();

        create_tabs_ = control(kDarkTabClass, L"", WS_TABSTOP | WS_GROUP,
                               kCreateTabs);

        update_mode_label_ = page_label(1, L"Update mode");
        update_mode_combo_ = page_combo(1, kUpdateMode, kUpdateModeNames);
        comment_label_ = page_label(1, L"Archive comment");
        comment_edit_ = page_edit(1, kArchiveComment,
                                  ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN);
        lock_archive_ = page_checkbox(1, kLockArchive,
                                      L"Lock archive against further changes");
        repack_after_update_ = page_checkbox(
            1, kRepackAfterUpdate, L"Repack affected solid runs after updating");
        metadata_heading_ = page_label(1, L"Metadata and links");
        metadata_info_ = page_label(
            1,
            L"Windows attributes and timestamps, NTFS alternate data streams, supported links, "
            L"and POSIX mode and ownership are captured automatically.", true);

        level_label_ = page_label(0, L"Compression level");
        level_combo_ = page_combo(0, kLevel, kLevelNames);
        dictionary_label_ = page_label(0, L"Dictionary size");
        dictionary_combo_ = page_combo(0, kDictionarySize, kDictionaryNames);
        word_size_label_ = page_label(0, L"Word size");
        word_size_combo_ = page_combo(0, kWordSize, kWordSizeNames);
        solid_block_label_ = page_label(0, L"Solid block size");
        solid_block_combo_ = page_combo(0, kSolidBlockSize, kSolidBlockNames);
        threads_label_ = page_label(0, L"Threads (0 = all)");
        threads_combo_ = page_thread_combo(0);
        compression_info_ = page_label(
            0,
            L"Default values follow the selected compression level. Larger dictionaries and "
            L"solid blocks can improve ratio but increase memory use.", true);

        encrypt_data_ = page_checkbox(2, kEncryptData, L"Encrypt file data");
        encrypt_names_ = page_checkbox(2, kEncryptNames,
                                       L"Encrypt file names and archive directory");
        password_label_ = page_label(2, L"Password");
        password_edit_ = page_edit(2, kPassword, ES_PASSWORD | ES_AUTOHSCROLL);
        confirm_password_label_ = page_label(2, L"Confirm password");
        confirm_password_edit_ = page_edit(
            2, kConfirmPassword, ES_PASSWORD | ES_AUTOHSCROLL);
        show_password_ = page_checkbox(2, kShowPassword, L"Show password");
        security_info_ = page_label(
            2,
            L"Axiom uses Argon2id key derivation and XChaCha20-Poly1305. Passwords are never "
            L"saved in GUI settings and are cleared when this dialog closes.", true);

        volume_size_label_ = page_label(3, L"Split volume size");
        volume_size_edit_ = page_edit(3, kVolumeSize, ES_NUMBER | ES_AUTOHSCROLL);
        volume_unit_combo_ = page_combo(3, kVolumeUnit, kVolumeUnitNames);
        recovery_percent_label_ = page_label(3, L"Recovery record");
        recovery_percent_edit_ = page_edit(3, kRecoveryPercent,
                                           ES_NUMBER | ES_AUTOHSCROLL);
        recovery_percent_suffix_ = page_label(3, L"% of archive data");
        recovery_volumes_ = page_checkbox(
            3, kRecoveryVolumes,
            L"Create .rev recovery volumes for missing-volume reconstruction");
        recovery_info_ = page_label(
            3,
            L"Leave the split size empty for one .axar file. A recovery record repairs bounded "
            L"damage inside an archive; .rev volumes reconstruct missing or corrupt parts.", true);

        sign_archive_ = page_checkbox(4, kSignArchive,
                                      L"Sign the completed archive");
        signing_key_label_ = page_label(4, L"Signing key");
        signing_key_edit_ = page_edit(4, kSigningKey);
        browse_signing_key_ = page_control(4, L"BUTTON", L"Browse...",
                                           WS_TABSTOP | BS_OWNERDRAW, kBrowseSigningKey);
        create_sfx_ = page_checkbox(4, kCreateSfx,
                                    L"Create one self-extracting Windows executable");
        sfx_info_ = page_label(
            4,
            L"When SFX is enabled, the Output field becomes an .exe path. The completed archive "
            L"is merged into that executable and no separate .axar is retained.", true);

        output_preview_ = control(L"STATIC", L"",
                                  SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, 0);
        accept_ = control(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW, kAccept);
        cancel_ = control(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, kCancel);
        load_create_values();
        select_create_page(0);
        update_create_dependencies();
        update_output_preview();
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

        if (mode_ == DialogMode::create_archive) {
            create_create_controls();
            layout();
            return;
        }
        if (mode_ == DialogMode::settings) {
            create_settings_controls();
            layout();
            return;
        }

        if (mode_ != DialogMode::settings) {
            summary_ = label(mode_ == DialogMode::create_archive ? L"Selected items" : L"Archive");
            path_label_ = label(mode_ == DialogMode::create_archive ? L"Archive path" : L"Destination");
            path_edit_ = control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kPathEdit);
            browse_ = control(L"BUTTON", L"Browse...", WS_TABSTOP | BS_OWNERDRAW, kBrowse);
            advanced_features_ = control(L"BUTTON", L"Advanced features...",
                                         WS_TABSTOP | BS_OWNERDRAW, kAdvancedFeatures);
        }
        level_label_ = label(L"Compression level");
        level_combo_ = selection_combo(kLevel, kLevelNames);
        if (mode_ == DialogMode::create_archive) {
            dictionary_label_ = label(L"Dictionary size");
            dictionary_combo_ = selection_combo(kDictionarySize, kDictionaryNames);
            word_size_label_ = label(L"Word size");
            word_size_combo_ = selection_combo(kWordSize, kWordSizeNames);
            solid_block_label_ = label(L"Solid block size");
            solid_block_combo_ = selection_combo(kSolidBlockSize, kSolidBlockNames);
        }
        threads_label_ = label(L"Threads (0 = all)");
        threads_combo_ = thread_combo();
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
        SendMessageW(level_combo_, CB_SETCURSEL, static_cast<WPARAM>(level_ - 1), 0);
        set_window_text(threads_combo_, std::to_wstring(threads));
        if (mode_ == DialogMode::create_archive) {
            SendMessageW(dictionary_combo_, CB_SETCURSEL,
                         value_index(kDictionaryValues, create_options.dictionary_size), 0);
            SendMessageW(word_size_combo_, CB_SETCURSEL,
                         value_index(kWordSizeValues, create_options.word_size), 0);
            SendMessageW(solid_block_combo_, CB_SETCURSEL,
                         value_index(kSolidBlockValues, create_options.solid_block_size), 0);
        }
    }

    HWND item(int id) const {
        return GetDlgItem(window_, id);
    }

    int selected_index(int id, int fallback = 0) const {
        const LRESULT selection = SendMessageW(item(id), CB_GETCURSEL, 0, 0);
        return selection == CB_ERR ? fallback : static_cast<int>(selection);
    }

    void set_selected_index(int id, int value) const {
        SendMessageW(item(id), CB_SETCURSEL, static_cast<WPARAM>(std::max(0, value)), 0);
    }

    const ShortcutCommandInfo* selected_shortcut_command() const {
        HWND combo = item(kShortcutCommand);
        const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        if (selection == CB_ERR) return nullptr;
        const auto id = reinterpret_cast<const wchar_t*>(
            SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(selection), 0));
        if (id == nullptr || reinterpret_cast<INT_PTR>(id) == CB_ERR) return nullptr;
        return shortcut_command_info(id);
    }

    static bool shortcut_duplicate_is_contextual_pair(std::wstring_view left,
                                                      std::wstring_view right) {
        return (left == L"commands.view" && right == L"navigation.go_address") ||
               (left == L"navigation.go_address" && right == L"commands.view");
    }

    bool shortcut_conflict(std::wstring_view command_id,
                           const KeyboardShortcut& shortcut,
                           std::wstring& conflicting_command) const {
        if (shortcut.key == 0) return false;
        for (const auto& command : kShortcutCommandCatalog) {
            if (command_id == std::wstring_view(command.id) ||
                shortcut_duplicate_is_contextual_pair(command_id, command.id)) {
                continue;
            }
            const auto other = parse_keyboard_shortcut(
                effective_shortcut_for_command(application_options.shortcut_overrides,
                                               command.id));
            if (other && other->key != 0 && *other == shortcut) {
                conflicting_command = command.label;
                return true;
            }
        }
        return false;
    }

    bool commit_shortcut_edit(bool show_errors) {
        const ShortcutCommandInfo* command = selected_shortcut_command();
        if (command == nullptr) return true;

        const std::wstring raw = window_text(item(kShortcutValue));
        const auto canonical = canonical_keyboard_shortcut(raw);
        if (!canonical) {
            if (show_errors) {
                show_message_dialog(window_, instance_, dpi_, palette_.dark,
                                    L"Keyboard shortcut",
                                    L"Shortcut text is not valid. Examples: Ctrl+O, "
                                    L"Alt+Left, Ctrl+Shift+R, F5, Delete, None.",
                                    MessageDialogIcon::warning);
            }
            return false;
        }

        const auto parsed = parse_keyboard_shortcut(*canonical);
        std::wstring conflict;
        if (parsed && shortcut_conflict(command->id, *parsed, conflict)) {
            if (show_errors) {
                show_message_dialog(
                    window_, instance_, dpi_, palette_.dark,
                    L"Keyboard shortcut",
                    L"That shortcut is already assigned to:\n\n" + conflict,
                    MessageDialogIcon::warning);
            }
            return false;
        }

        set_shortcut_override(application_options.shortcut_overrides,
                              command->id, *canonical);
        set_window_text(item(kShortcutValue), canonical->empty() ? L"None" : *canonical);
        update_shortcut_controls();
        return true;
    }

    void update_shortcut_controls() {
        const ShortcutCommandInfo* command = selected_shortcut_command();
        if (command == nullptr) {
            set_window_text(item(kShortcutValue), L"");
            if (settings_shortcut_default_label_ != nullptr) {
                set_window_text(settings_shortcut_default_label_, L"");
            }
            return;
        }
        const std::wstring effective = effective_shortcut_for_command(
            application_options.shortcut_overrides, command->id);
        set_window_text(item(kShortcutValue), effective.empty() ? L"None" : effective);
        if (settings_shortcut_default_label_ != nullptr) {
            const std::wstring default_text = default_shortcut_for_command(command->id);
            set_window_text(settings_shortcut_default_label_,
                            L"Default: " +
                                (default_text.empty() ? std::wstring(L"None") : default_text));
        }
    }

    void load_shortcut_controls() {
        HWND combo = item(kShortcutCommand);
        if (combo == nullptr) return;
        if (SendMessageW(combo, CB_GETCURSEL, 0, 0) == CB_ERR) {
            SendMessageW(combo, CB_SETCURSEL, 0, 0);
        }
        update_shortcut_controls();
    }

    int edit_int(int id, int fallback, int minimum, int maximum) const {
        try {
            const std::wstring text = window_text(item(id));
            if (text.empty()) return fallback;
            return std::clamp(std::stoi(text), minimum, maximum);
        } catch (...) {
            return fallback;
        }
    }

    std::size_t thread_count_from(HWND combo) const {
        try {
            return static_cast<std::size_t>(std::stoull(window_text(combo)));
        } catch (...) {
            return 0;
        }
    }

    void load_settings_values() {
        set_selected_index(kThemeMode, std::clamp(application_options.theme_mode, 0, 2));
        set_selected_index(kAccentColorMode,
                           std::clamp(application_options.accent_color_mode, 0, 6));
        set_window_text(item(kCustomAccentColor),
                        color_to_hex(application_options.custom_accent_color));
        set_selected_index(kToolbarIconStyle,
                           std::clamp(application_options.toolbar_icon_style, 0, 2));
        set_selected_index(kStartupMode,
                           std::clamp(application_options.startup_location_mode, 0, 3));
        set_window_text(item(kStartupCustomPath), application_options.startup_custom_path);
        set_window_text(item(kRecentLocationCount),
                        std::to_wstring(application_options.recent_location_count));

        set_selected_index(kLevel, std::clamp(application_options.default_level, 1, 9) - 1);
        SendMessageW(item(kDictionarySize), CB_SETCURSEL,
                     value_index(kDictionaryValues,
                                 application_options.default_dictionary_size), 0);
        SendMessageW(item(kWordSize), CB_SETCURSEL,
                     value_index(kWordSizeValues,
                                 application_options.default_word_size), 0);
        SendMessageW(item(kSolidBlockSize), CB_SETCURSEL,
                     value_index(kSolidBlockValues,
                                 application_options.default_solid_block_size), 0);
        set_window_text(item(kThreads),
                        std::to_wstring(application_options.default_thread_count));
        set_selected_index(kDefaultUpdateMode,
                           std::clamp(application_options.default_update_mode, 0, 4));
        set_window_text(item(kDefaultVolumeSize),
                        application_options.default_volume_size);
        set_selected_index(kDefaultVolumeUnit,
                           std::clamp(application_options.default_volume_unit, 0, 3));
        set_window_text(item(kDefaultRecoveryPercent),
                        std::to_wstring(application_options.default_recovery_percent));
        set_window_text(item(kDefaultSigningKey),
                        application_options.default_signing_key);

        set_selected_index(kArchiveOutputMode,
                           std::clamp(application_options.archive_output_mode, 0, 2));
        set_window_text(item(kArchiveOutputFolder),
                        application_options.archive_output_folder);
        set_selected_index(kExtractDestinationMode,
                           std::clamp(application_options.extract_destination_mode, 0, 2));
        set_window_text(item(kExtractDestinationFolder),
                        application_options.extract_destination_folder);
        set_selected_index(kTempFolderMode,
                           std::clamp(application_options.temp_folder_mode, 0, 2));
        set_window_text(item(kTempFolder), application_options.temp_folder);
        set_window_text(item(kTempCleanupDays),
                        std::to_wstring(application_options.temp_cleanup_days));

        set_selected_index(kFileOpenMode,
                           std::clamp(application_options.file_open_mode, 0, 1));
        set_window_text(item(kExternalViewer), application_options.external_viewer);
        set_window_text(item(kExternalEditor), application_options.external_editor);

        set_selected_index(kPasswordPromptMode,
                           std::clamp(application_options.password_prompt_mode, 0, 1));
        set_window_text(item(kTrustedKeysFolder), application_options.trusted_keys_folder);

        set_selected_index(kUpdateChannel,
                           std::clamp(application_options.update_channel, 0, 1));
        set_window_text(item(kUpdateUrl), application_options.update_url);

        set_selected_index(kWorkerPriority,
                           std::clamp(application_options.worker_priority, 0, 2));
        set_window_text(item(kLogFolder), application_options.log_folder);
        set_selected_index(kIoBufferMode,
                           std::clamp(application_options.io_buffer_mode, 0, 1));
        set_window_text(item(kIoBufferSize), application_options.io_buffer_size);
        set_selected_index(kMemoryLimitMode,
                           std::clamp(application_options.memory_limit_mode, 0, 1));
        set_window_text(item(kMemoryLimit), application_options.memory_limit);
        load_shortcut_controls();
        update_settings_dependencies();
        for (const SettingControl& control : settings_controls_) {
            InvalidateRect(control.window, nullptr, TRUE);
        }
    }

    void update_settings_dependencies() {
        if (mode_ != DialogMode::settings) return;
        EnableWindow(item(kIoBufferSize), selected_index(kIoBufferMode, 0) == 1);
        EnableWindow(item(kMemoryLimit), selected_index(kMemoryLimitMode, 0) == 1);
    }

    bool apply_settings_values() {
        if (!commit_shortcut_edit(true)) return false;
        application_options.theme_mode = selected_index(kThemeMode, 0);
        application_options.accent_color_mode = selected_index(kAccentColorMode, 0);
        if (const auto color = color_from_hex(window_text(item(kCustomAccentColor)))) {
            application_options.custom_accent_color = *color;
            set_window_text(item(kCustomAccentColor), color_to_hex(*color));
        } else {
            show_message_dialog(window_, instance_, dpi_, palette_.dark,
                                L"Axiom settings",
                                L"Custom accent color must use #RRGGBB, for example #FFB93C.",
                                MessageDialogIcon::warning);
            return false;
        }
        application_options.toolbar_icon_style = selected_index(kToolbarIconStyle, 0);
        application_options.startup_location_mode = selected_index(kStartupMode, 0);
        application_options.startup_custom_path = window_text(item(kStartupCustomPath));
        application_options.recent_location_count =
            edit_int(kRecentLocationCount, 12, 0, 50);

        application_options.default_level = selected_index(kLevel, 4) + 1;
        application_options.default_dictionary_size =
            selected_combo_value(item(kDictionarySize), kDictionaryValues);
        application_options.default_word_size =
            selected_combo_value(item(kWordSize), kWordSizeValues);
        application_options.default_solid_block_size =
            selected_combo_value(item(kSolidBlockSize), kSolidBlockValues);
        application_options.default_thread_count = thread_count_from(item(kThreads));
        application_options.default_update_mode = selected_index(kDefaultUpdateMode, 0);
        application_options.default_volume_size = window_text(item(kDefaultVolumeSize));
        application_options.default_volume_unit = selected_index(kDefaultVolumeUnit, 2);
        application_options.default_recovery_percent =
            edit_int(kDefaultRecoveryPercent, 0, 0, 100);
        application_options.default_signing_key = window_text(item(kDefaultSigningKey));

        application_options.archive_output_mode = selected_index(kArchiveOutputMode, 0);
        application_options.archive_output_folder = window_text(item(kArchiveOutputFolder));
        application_options.extract_destination_mode =
            selected_index(kExtractDestinationMode, 0);
        application_options.extract_destination_folder =
            window_text(item(kExtractDestinationFolder));
        application_options.temp_folder_mode = selected_index(kTempFolderMode, 0);
        application_options.temp_folder = window_text(item(kTempFolder));
        application_options.temp_cleanup_days = edit_int(kTempCleanupDays, 7, 0, 365);

        application_options.file_open_mode = selected_index(kFileOpenMode, 0);
        application_options.external_viewer = window_text(item(kExternalViewer));
        application_options.external_editor = window_text(item(kExternalEditor));

        application_options.password_prompt_mode = selected_index(kPasswordPromptMode, 0);
        application_options.trusted_keys_folder = window_text(item(kTrustedKeysFolder));

        application_options.update_channel = selected_index(kUpdateChannel, 0);
        application_options.update_url = window_text(item(kUpdateUrl));

        application_options.worker_priority = selected_index(kWorkerPriority, 0);
        application_options.log_folder = window_text(item(kLogFolder));
        application_options.io_buffer_mode = selected_index(kIoBufferMode, 0);
        application_options.io_buffer_size = window_text(item(kIoBufferSize));
        if (application_options.io_buffer_mode == 1) {
            const auto size = parse_size_text(application_options.io_buffer_size);
            if (!size || *size < kMinIoBufferSize || *size > kMaxIoBufferSize) {
                show_message_dialog(
                    window_, instance_, dpi_, palette_.dark,
                    L"Axiom settings",
                    L"Custom I/O buffer size must be between 64 KiB and 64 MiB. "
                    L"Examples: 1 MiB, 4 MiB, 8388608.",
                    MessageDialogIcon::warning);
                return false;
            }
        }
        application_options.memory_limit_mode = selected_index(kMemoryLimitMode, 0);
        application_options.memory_limit = window_text(item(kMemoryLimit));
        return true;
    }

    void refresh_settings_appearance() {
        if (mode_ != DialogMode::settings || window_ == nullptr) return;
        set_dialog_appearance({
            application_options.theme_mode,
            application_options.accent_color_mode,
            application_options.custom_accent_color,
            application_options.toolbar_icon_style,
        });
        palette_ = make_palette();
        if (window_brush_ != nullptr) {
            DeleteObject(window_brush_);
            window_brush_ = nullptr;
        }
        if (edit_brush_ != nullptr) {
            DeleteObject(edit_brush_);
            edit_brush_ = nullptr;
        }
        window_brush_ = CreateSolidBrush(palette_.window);
        edit_brush_ = CreateSolidBrush(palette_.edit);
        set_dark_title(window_, palette_.dark);
        EnumChildWindows(window_, [](HWND child, LPARAM self_param) -> BOOL {
            auto* self = reinterpret_cast<OptionsDialog*>(self_param);
            apply_dialog_control_theme(child, self->palette_.dark);
            InvalidateRect(child, nullptr, FALSE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(this));
        InvalidateRect(window_, nullptr, TRUE);
    }

    bool apply_settings_live(bool close_after) {
        if (!apply_settings_values()) return false;
        if (settings_apply_callback) {
            settings_apply_callback(application_options);
        }
        refresh_settings_appearance();
        accepted_ = true;
        if (close_after) {
            close_dialog();
        }
        return true;
    }

    void layout() {
        if (mode_ == DialogMode::create_archive) {
            layout_create();
            return;
        }
        if (mode_ == DialogMode::settings) {
            layout_settings();
            return;
        }
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
            MoveWindow(level_combo_, margin + label_width, y, scale(230), scale(250), TRUE);
            y += row_height + gap;
        } else {
            ShowWindow(level_label_, SW_HIDE);
            ShowWindow(level_combo_, SW_HIDE);
        }
        if (mode_ == DialogMode::create_archive) {
            MoveWindow(dictionary_label_, margin, y + scale(6), label_width, row_height, TRUE);
            MoveWindow(dictionary_combo_, margin + label_width, y,
                       scale(230), scale(280), TRUE);
            y += row_height + gap;
            MoveWindow(word_size_label_, margin, y + scale(6), label_width, row_height, TRUE);
            MoveWindow(word_size_combo_, margin + label_width, y,
                       scale(230), scale(180), TRUE);
            y += row_height + gap;
            MoveWindow(solid_block_label_, margin, y + scale(6), label_width, row_height, TRUE);
            MoveWindow(solid_block_combo_, margin + label_width, y,
                       scale(230), scale(260), TRUE);
            y += row_height + gap;
        }
        MoveWindow(threads_label_, margin, y + scale(6), label_width, row_height, TRUE);
        MoveWindow(threads_combo_, margin + label_width, y, scale(180), scale(260), TRUE);
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
        if (advanced_features_ != nullptr) {
            MoveWindow(advanced_features_, margin, button_y, scale(154), row_height, TRUE);
        }
        MoveWindow(cancel_, client.right - margin - button_width, button_y, button_width, row_height, TRUE);
        MoveWindow(accept_, client.right - margin - button_width * 2 - scale(8),
                   button_y, button_width, row_height, TRUE);
    }

    void layout_settings() {
        RECT client{};
        GetClientRect(window_, &client);
        const int margin = scale(18);
        const int row = scale(30);
        const int button_width = scale(88);
        const int button_y = client.bottom - margin - row;
        MoveWindow(defaults_, margin, button_y, scale(112), row, TRUE);
        MoveWindow(cancel_, client.right - margin - button_width, button_y,
                   button_width, row, TRUE);
        MoveWindow(apply_, client.right - margin - button_width * 2 - scale(8),
                   button_y, button_width, row, TRUE);
        MoveWindow(accept_, client.right - margin - button_width * 3 - scale(16),
                   button_y, button_width, row, TRUE);

        MoveWindow(settings_tabs_, margin, margin,
                   client.right - margin * 2, scale(38), TRUE);
        const int content_left = margin;
        const int content_top = margin + scale(56);
        const int content_width = client.right - margin * 2;
        const int bottom_limit = button_y - scale(12);
        for (const SettingControl& control : settings_controls_) {
            const int available_width = std::max(
                scale(80), content_width - scale(control.x));
            const int width = std::min(scale(control.width), available_width);
            int height = scale(control.height);
            if (control.wrapped) {
                HDC dc = GetDC(control.window);
                HGDIOBJ old_font = SelectObject(dc, font_);
                RECT measured{0, 0, width, 0};
                const std::wstring text = window_text(control.window);
                DrawTextW(dc, text.c_str(), -1, &measured,
                          DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
                SelectObject(dc, old_font);
                ReleaseDC(control.window, dc);
                height = std::max(height, static_cast<int>(measured.bottom) + scale(3));
            }
            const int y = content_top + scale(control.y);
            MoveWindow(control.window,
                       content_left + scale(control.x), y,
                       width, std::min(height, std::max(scale(20), bottom_limit - y)),
                       TRUE);
        }
    }

    int wrapped_height(HWND control_window, int width, int minimum) const {
        HDC dc = GetDC(control_window);
        HGDIOBJ old_font = SelectObject(dc, font_);
        RECT measured{0, 0, std::max(width, scale(80)), 0};
        const std::wstring text = window_text(control_window);
        DrawTextW(dc, text.c_str(), -1, &measured,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        SelectObject(dc, old_font);
        ReleaseDC(control_window, dc);
        return std::max(minimum, static_cast<int>(
            measured.bottom - measured.top) + scale(4));
    }

    void move_wrapped(HWND control_window, int x, int& y, int width,
                      int minimum = 0, int gap = 10) {
        const int height = wrapped_height(control_window, width, scale(minimum));
        MoveWindow(control_window, x, y, width, height, TRUE);
        y += height + scale(gap);
    }

    void layout_create() {
        if (window_ == nullptr || accept_ == nullptr) return;
        SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
        RECT client{};
        GetClientRect(window_, &client);
        const int margin = scale(18);
        const int row = scale(30);
        const int gap = scale(10);
        const int label_width = scale(150);
        const int content_left = margin + scale(12);
        const int content_right = client.right - margin - scale(12);
        const int content_width = std::max(scale(260),
                                           content_right - content_left);

        int y = margin;
        MoveWindow(summary_, margin, y, client.right - margin * 2, scale(24), TRUE);
        y += scale(31);
        MoveWindow(path_label_, margin, y + scale(6), scale(72), row, TRUE);
        const int browse_width = scale(86);
        const int edit_x = margin + scale(78);
        MoveWindow(path_edit_, edit_x, y,
                   std::max(scale(120), static_cast<int>(client.right) -
                            margin - browse_width - gap - edit_x),
                   row, TRUE);
        MoveWindow(browse_, client.right - margin - browse_width, y,
                   browse_width, row, TRUE);
        y += row + scale(10);
        MoveWindow(format_label_, margin, y + scale(6), scale(72), row, TRUE);
        MoveWindow(format_combo_, edit_x, y, scale(260), scale(240), TRUE);
        y += row + scale(15);

        MoveWindow(create_tabs_, margin, y,
                   std::max(scale(300), static_cast<int>(client.right) - margin * 2),
                   scale(36), TRUE);
        const int page_top = y + scale(50);
        const int button_y = client.bottom - margin - row;
        const int preview_y = button_y - scale(39);
        MoveWindow(output_preview_, margin, preview_y,
                   std::max(scale(200), static_cast<int>(client.right) - margin * 2),
                   scale(28), TRUE);
        const int button_width = scale(86);
        MoveWindow(cancel_, client.right - margin - button_width, button_y,
                   button_width, row, TRUE);
        MoveWindow(accept_, client.right - margin - button_width * 2 - scale(8),
                   button_y, button_width, row, TRUE);

        y = page_top;
        auto row_pair = [&](HWND label_window, HWND value, int value_width = 310,
                            bool combo = true) {
            MoveWindow(label_window, content_left, y + scale(6), label_width, row, TRUE);
            MoveWindow(value, content_left + label_width, y,
                       std::min(scale(value_width), content_width - label_width),
                       combo ? scale(240) : row, TRUE);
            y += row + scale(12);
        };

        switch (create_page_) {
            case 1: {
                row_pair(update_mode_label_, update_mode_combo_, 340);
                MoveWindow(comment_label_, content_left, y + scale(5), label_width, row, TRUE);
                const int comment_height = std::clamp(
                    preview_y - y - scale(160), scale(58), scale(105));
                MoveWindow(comment_edit_, content_left + label_width, y,
                           content_width - label_width, comment_height, TRUE);
                y += comment_height + scale(12);
                MoveWindow(lock_archive_, content_left + label_width, y,
                           content_width - label_width, row, TRUE);
                y += row + scale(4);
                MoveWindow(repack_after_update_, content_left + label_width, y,
                           content_width - label_width, row, TRUE);
                y += row + scale(16);
                MoveWindow(metadata_heading_, content_left, y,
                           content_width, scale(24), TRUE);
                y += scale(30);
                move_wrapped(metadata_info_, content_left, y, content_width, 38);
                break;
            }
            case 0:
                row_pair(level_label_, level_combo_);
                row_pair(dictionary_label_, dictionary_combo_);
                row_pair(word_size_label_, word_size_combo_);
                row_pair(solid_block_label_, solid_block_combo_);
                row_pair(threads_label_, threads_combo_, 230);
                y += scale(8);
                move_wrapped(compression_info_, content_left, y, content_width, 38);
                break;
            case 2:
                MoveWindow(encrypt_data_, content_left, y, content_width, row, TRUE);
                y += row + scale(4);
                MoveWindow(encrypt_names_, content_left, y, content_width, row, TRUE);
                y += row + scale(14);
                row_pair(password_label_, password_edit_, 340, false);
                row_pair(confirm_password_label_, confirm_password_edit_, 340, false);
                MoveWindow(show_password_, content_left + label_width, y,
                           content_width - label_width, row, TRUE);
                y += row + scale(20);
                move_wrapped(security_info_, content_left, y, content_width, 46);
                break;
            case 3:
                MoveWindow(volume_size_label_, content_left, y + scale(6), label_width, row, TRUE);
                MoveWindow(volume_size_edit_, content_left + label_width, y, scale(180), row, TRUE);
                MoveWindow(volume_unit_combo_, content_left + label_width + scale(190), y,
                           scale(105), scale(180), TRUE);
                y += row + scale(15);
                MoveWindow(recovery_percent_label_, content_left, y + scale(6), label_width, row, TRUE);
                MoveWindow(recovery_percent_edit_, content_left + label_width, y,
                           scale(90), row, TRUE);
                MoveWindow(recovery_percent_suffix_, content_left + label_width + scale(100),
                           y + scale(6), scale(170), row, TRUE);
                y += row + scale(18);
                MoveWindow(recovery_volumes_, content_left + label_width, y,
                           content_width - label_width, row, TRUE);
                y += row + scale(28);
                move_wrapped(recovery_info_, content_left, y, content_width, 52);
                break;
            case 4:
                MoveWindow(sign_archive_, content_left, y, content_width, row, TRUE);
                y += row + scale(15);
                MoveWindow(signing_key_label_, content_left, y + scale(6), label_width, row, TRUE);
                MoveWindow(signing_key_edit_, content_left + label_width, y,
                           std::max(scale(120), content_width - label_width - browse_width - gap),
                           row, TRUE);
                MoveWindow(browse_signing_key_, content_right - browse_width, y,
                           browse_width, row, TRUE);
                y += row + scale(28);
                MoveWindow(create_sfx_, content_left, y, content_width, row, TRUE);
                y += row + scale(26);
                move_wrapped(sfx_info_, content_left, y, content_width, 52);
                break;
        }
        SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void browse() {
        const auto path = mode_ == DialogMode::create_archive
            ? browse_save_archive(window_, selected_archive_format(),
                                  create_options.features.create_sfx)
            : browse_folder(window_);
        if (path) {
            set_window_text(path_edit_, path->wstring());
            if (mode_ == DialogMode::create_archive &&
                !create_options.features.create_sfx) {
                create_options.archive_format =
                    archive_format_from_path(*path, selected_archive_format());
                SendMessageW(format_combo_, CB_SETCURSEL,
                             static_cast<WPARAM>(creatable_archive_format_index(
                                 create_options.archive_format)), 0);
                clear_options_unsupported_by_selected_format();
                update_create_dependencies();
            }
            update_output_preview();
        }
    }

    bool browse_settings_path(int id) {
        int target = 0;
        std::optional<fs::path> selected;
        switch (id) {
            case kBrowseStartupCustomPath:
                target = kStartupCustomPath;
                selected = browse_folder(window_, L"Choose startup folder",
                                         window_text(item(kStartupCustomPath)));
                if (selected) set_selected_index(kStartupMode, 3);
                break;
            case kBrowseDefaultSigningKey:
                target = kDefaultSigningKey;
                selected = browse_signing_key(window_);
                break;
            case kBrowseArchiveOutputFolder:
                target = kArchiveOutputFolder;
                selected = browse_folder(window_, L"Choose archive output folder",
                                         window_text(item(kArchiveOutputFolder)));
                if (selected) set_selected_index(kArchiveOutputMode, 2);
                break;
            case kBrowseExtractDestinationFolder:
                target = kExtractDestinationFolder;
                selected = browse_folder(window_, L"Choose extraction folder",
                                         window_text(item(kExtractDestinationFolder)));
                if (selected) set_selected_index(kExtractDestinationMode, 2);
                break;
            case kBrowseTempFolder:
                target = kTempFolder;
                selected = browse_folder(window_, L"Choose temporary folder",
                                         window_text(item(kTempFolder)));
                if (selected) set_selected_index(kTempFolderMode, 2);
                break;
            case kBrowseExternalViewer:
                target = kExternalViewer;
                selected = browse_executable(window_, window_text(item(kExternalViewer)));
                break;
            case kBrowseExternalEditor:
                target = kExternalEditor;
                selected = browse_executable(window_, window_text(item(kExternalEditor)));
                break;
            case kBrowseTrustedKeysFolder:
                target = kTrustedKeysFolder;
                selected = browse_folder(window_, L"Choose trusted keys folder",
                                         window_text(item(kTrustedKeysFolder)));
                break;
            case kBrowseLogFolder:
                target = kLogFolder;
                selected = browse_folder(window_, L"Choose log folder",
                                         window_text(item(kLogFolder)));
                break;
            default:
                return false;
        }
        if (selected) {
            set_window_text(item(target), selected->wstring());
        }
        return true;
    }

    std::size_t thread_count() const {
        try {
            return static_cast<std::size_t>(std::stoull(window_text(threads_combo_)));
        } catch (...) {
            return 0;
        }
    }

    void load_create_values() {
        create_options.archive_format =
            archive_format_from_path(create_options.archive_path, create_options.archive_format);
        SendMessageW(format_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(creatable_archive_format_index(
                         create_options.archive_format)), 0);
        level_ = std::clamp(create_options.level, 1, 9);
        SendMessageW(level_combo_, CB_SETCURSEL, static_cast<WPARAM>(level_ - 1), 0);
        SendMessageW(dictionary_combo_, CB_SETCURSEL,
                     value_index(kDictionaryValues, create_options.dictionary_size), 0);
        SendMessageW(word_size_combo_, CB_SETCURSEL,
                     value_index(kWordSizeValues, create_options.word_size), 0);
        SendMessageW(solid_block_combo_, CB_SETCURSEL,
                     value_index(kSolidBlockValues, create_options.solid_block_size), 0);
        set_window_text(threads_combo_, std::to_wstring(create_options.thread_count));
        SendMessageW(update_mode_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(create_options.features.update_mode), 0);
        SendMessageW(volume_unit_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(std::clamp(create_options.features.volume_unit, 0, 3)), 0);
        set_window_text(summary_, std::to_wstring(input_count) +
                        (input_count == 1 ? L" item selected" : L" items selected"));

        fs::path output = create_options.archive_path;
        if (create_options.features.create_sfx) {
            if (!create_options.features.sfx_destination.empty()) {
                output = create_options.features.sfx_destination;
            } else {
                output.replace_extension(L".exe");
            }
        }
        set_window_text(path_edit_, output.wstring());
        set_window_text(comment_edit_, create_options.features.comment);
        set_window_text(password_edit_, create_options.features.password);
        set_window_text(confirm_password_edit_, create_options.features.password);
        set_window_text(volume_size_edit_, create_options.features.volume_size);
        set_window_text(recovery_percent_edit_,
                        std::to_wstring(create_options.features.recovery_percent));
        set_window_text(signing_key_edit_, create_options.features.signing_key.wstring());
        clear_options_unsupported_by_selected_format();
    }

    void select_create_page(int page) {
        create_page_ = std::clamp(page, 0, static_cast<int>(kCreateTabNames.size()) - 1);
        for (const auto& item : page_controls_) {
            ShowWindow(item.window, SW_HIDE);
        }
        layout();
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW | RDW_UPDATENOW);
        for (const auto& item : page_controls_) {
            if (item.page == create_page_) ShowWindow(item.window, SW_SHOWNA);
        }
        SendMessageW(create_tabs_, kDarkTabSetSelection,
                     static_cast<WPARAM>(create_page_), 0);
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                         RDW_ERASENOW | RDW_UPDATENOW);
    }

    void select_settings_page(int page) {
        settings_page_ = std::clamp(page, 0, static_cast<int>(kSettingsTabNames.size()) - 1);
        for (const SettingControl& control : settings_controls_) {
            ShowWindow(control.window,
                       control.page == settings_page_ ? SW_SHOWNA : SW_HIDE);
        }
        SendMessageW(settings_tabs_, kDarkTabSetSelection,
                     static_cast<WPARAM>(settings_page_), 0);
        InvalidateRect(window_, nullptr, TRUE);
    }

    void set_password_visibility() {
        const WPARAM character = create_show_password_ ? 0 : static_cast<WPARAM>(L'\x25cf');
        SendMessageW(password_edit_, EM_SETPASSWORDCHAR, character, 0);
        SendMessageW(confirm_password_edit_, EM_SETPASSWORDCHAR, character, 0);
        InvalidateRect(password_edit_, nullptr, TRUE);
        InvalidateRect(confirm_password_edit_, nullptr, TRUE);
    }

    axiom::ArchiveFormat selected_archive_format() const {
        if (format_combo_ == nullptr) {
            return create_options.archive_format;
        }
        const LRESULT selection = SendMessageW(format_combo_, CB_GETCURSEL, 0, 0);
        const auto formats = creatable_archive_formats();
        if (selection == CB_ERR || selection < 0 ||
            selection >= static_cast<LRESULT>(formats.size())) {
            return create_options.archive_format;
        }
        return formats[static_cast<std::size_t>(selection)]->format;
    }

    bool selected_format_is_native() const {
        return archive_format_info(selected_archive_format()).native;
    }

    ArchiveFeatureAvailability selected_format_availability() const {
        ArchiveFeatureAvailability available = create_options.feature_availability;
        if (!selected_format_is_native()) {
            const bool zip = selected_archive_format() == ArchiveFormat::zip;
            available.metadata = false;
            available.comments = false;
            available.lock = false;
            available.encryption = zip;
            available.header_encryption = false;
            available.kdf_presets = false;
            available.volumes = false;
            available.recovery = false;
            available.authenticity = false;
            available.sfx = false;
            available.posix_metadata = false;
            available.update = true;
        }
        return available;
    }

    void clear_options_unsupported_by_selected_format() {
        if (selected_format_is_native()) {
            return;
        }
        const bool zip = selected_archive_format() == ArchiveFormat::zip;
        create_options.features.comment.clear();
        create_options.features.lock_archive = false;
        create_options.features.repack_after_update = false;
        if (!zip) {
            create_options.features.encrypt_data = false;
            create_options.features.password.clear();
            set_window_text(password_edit_, L"");
            set_window_text(confirm_password_edit_, L"");
        }
        create_options.features.encrypt_names = false;
        create_options.features.volume_size.clear();
        create_options.features.recovery_percent = 0;
        create_options.features.create_recovery_volumes = false;
        create_options.features.sign_archive = false;
        create_options.features.signing_key.clear();
        create_options.features.create_sfx = false;
        create_options.features.sfx_destination.clear();
        set_window_text(comment_edit_, L"");
        set_window_text(volume_size_edit_, L"");
        set_window_text(recovery_percent_edit_, L"0");
        set_window_text(signing_key_edit_, L"");
    }

    void apply_selected_format_extension(bool force = false) {
        if (path_edit_ == nullptr || create_options.features.create_sfx) {
            return;
        }
        fs::path output = window_text(path_edit_);
        if (output.empty()) {
            return;
        }
        const auto& format = archive_format_info(selected_archive_format());
        if (force || output.extension().empty() || is_known_archive_extension(output)) {
            output.replace_extension(widen_ascii(format.default_extension));
            set_window_text(path_edit_, output.wstring());
        }
    }

    void on_archive_format_changed() {
        create_options.archive_format = selected_archive_format();
        clear_options_unsupported_by_selected_format();
        apply_selected_format_extension(true);
        update_create_dependencies();
        update_output_preview();
    }

    void sync_archive_format_from_path() {
        if (create_options.fixed_archive_format || create_options.features.create_sfx) {
            return;
        }
        const fs::path output = window_text(path_edit_);
        if (const auto* provider = axiom::archive_provider_for_path(output)) {
            if (provider->info().format != selected_archive_format()) {
                create_options.archive_format = provider->info().format;
                SendMessageW(format_combo_, CB_SETCURSEL,
                             static_cast<WPARAM>(creatable_archive_format_index(
                                 create_options.archive_format)), 0);
                clear_options_unsupported_by_selected_format();
                update_create_dependencies();
            }
        }
    }

    void update_create_dependencies() {
        const auto available = selected_format_availability();
        const bool updating = create_options.features.update_mode != ArchiveUpdateMode::create_new;
        const bool native = selected_format_is_native();
        EnableWindow(format_combo_, !create_options.fixed_archive_format &&
                                    !create_options.features.create_sfx);
        EnableWindow(level_combo_, TRUE);
        EnableWindow(dictionary_combo_, native);
        EnableWindow(word_size_combo_, native);
        EnableWindow(solid_block_combo_, native);
        set_window_text(
            compression_info_,
            native
                ? L"Default values follow the selected compression level. Larger dictionaries "
                  L"and solid blocks can improve ratio but increase memory use."
                : L"ZIP uses Deflate compression. The compression level and thread count apply; "
                  L"Axiom dictionary, word, and solid-block controls are disabled for this format.");
        EnableWindow(update_mode_combo_, available.update);
        EnableWindow(comment_edit_, available.comments);
        EnableWindow(lock_archive_, available.lock);
        EnableWindow(repack_after_update_, available.update && updating);

        EnableWindow(encrypt_data_, available.encryption);
        EnableWindow(encrypt_names_, available.encryption && available.header_encryption &&
                                      create_options.features.encrypt_data);
        const bool password_enabled = available.encryption &&
                                      create_options.features.encrypt_data;
        EnableWindow(password_edit_, password_enabled);
        EnableWindow(confirm_password_edit_, password_enabled);
        EnableWindow(show_password_, password_enabled);
        set_window_text(
            security_info_,
            native
                ? L"Axiom uses Argon2id key derivation and XChaCha20-Poly1305. Passwords "
                  L"are never saved in GUI settings and are cleared when this dialog closes."
                : L"ZIP encryption uses WinZip AES-256 for file data. File names remain "
                  L"visible; use AXAR if archive directory encryption is required.");

        const bool split_enabled = available.volumes && !create_options.features.create_sfx;
        EnableWindow(volume_size_edit_, split_enabled);
        EnableWindow(volume_unit_combo_, split_enabled);
        EnableWindow(recovery_percent_edit_, available.recovery);
        EnableWindow(recovery_volumes_, available.recovery && split_enabled);

        EnableWindow(sign_archive_, available.authenticity);
        const bool key_enabled = available.authenticity && create_options.features.sign_archive;
        EnableWindow(signing_key_edit_, key_enabled);
        EnableWindow(browse_signing_key_, key_enabled);
        EnableWindow(create_sfx_, available.sfx);
        InvalidateRect(window_, nullptr, TRUE);
    }

    void update_output_preview() {
        if (path_edit_ == nullptr || output_preview_ == nullptr) return;
        fs::path output = window_text(path_edit_);
        std::wstring prefix = L"Output: ";
        if (create_options.features.create_sfx) {
            if (!output.empty() && lstrcmpiW(output.extension().c_str(), L".exe") != 0) {
                output.replace_extension(L".exe");
            }
            prefix = L"Self-extractor: ";
        } else if (volume_size_edit_ != nullptr && !window_text(volume_size_edit_).empty()) {
            fs::path root = output;
            if (lstrcmpiW(root.extension().c_str(), L".axar") == 0) root.replace_extension();
            output = fs::path(root.wstring() + L".part001.axar");
            prefix = L"First volume: ";
        } else if (!output.empty() &&
                   (output.extension().empty() || is_known_archive_extension(output))) {
            output.replace_extension(
                widen_ascii(archive_format_info(selected_archive_format()).default_extension));
        }
        set_window_text(output_preview_, prefix + output.wstring());
    }

    void convert_output_mode(bool sfx) {
        fs::path output = window_text(path_edit_);
        if (output.empty()) return;
        if (sfx) {
            output.replace_extension(L".exe");
        } else if (lstrcmpiW(output.extension().c_str(), L".exe") == 0) {
            output.replace_extension(L".axar");
        }
        set_window_text(path_edit_, output.wstring());
    }

    template <std::size_t Size>
    std::size_t selected_combo_value(HWND combo,
                                     const std::array<std::size_t, Size>& values) const {
        const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        if (selection == CB_ERR || selection < 0 ||
            selection >= static_cast<LRESULT>(values.size())) {
            return 0;
        }
        return values[static_cast<std::size_t>(selection)];
    }

    void accept() {
        if (mode_ == DialogMode::create_archive) {
            accept_create();
            return;
        }
        if (mode_ == DialogMode::settings) {
            apply_settings_live(true);
            return;
        }
        if (mode_ != DialogMode::settings && window_text(path_edit_).empty()) {
            show_message_dialog(window_, instance_, dpi_, palette_.dark,
                                L"Axiom", L"Choose a path first.",
                                MessageDialogIcon::information);
            return;
        }
        const LRESULT level_selection = SendMessageW(level_combo_, CB_GETCURSEL, 0, 0);
        if (level_selection != CB_ERR) level_ = static_cast<int>(level_selection) + 1;
        if (mode_ == DialogMode::create_archive) {
            create_options.archive_path = window_text(path_edit_);
            create_options.level = level_;
            create_options.thread_count = thread_count();
            create_options.dictionary_size = selected_combo_value(
                dictionary_combo_, kDictionaryValues);
            create_options.word_size = selected_combo_value(
                word_size_combo_, kWordSizeValues);
            create_options.solid_block_size = selected_combo_value(
                solid_block_combo_, kSolidBlockValues);
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
        close_dialog();
    }

    void accept_create() {
        fs::path displayed_output = window_text(path_edit_);
        if (displayed_output.empty()) {
            show_message_dialog(window_, instance_, dpi_, palette_.dark,
                                L"Axiom", L"Choose an output path first.",
                                MessageDialogIcon::information);
            return;
        }

        const LRESULT level_selection = SendMessageW(level_combo_, CB_GETCURSEL, 0, 0);
        if (level_selection != CB_ERR) level_ = static_cast<int>(level_selection) + 1;
        create_options.level = level_;
        create_options.thread_count = thread_count();
        create_options.dictionary_size = selected_combo_value(
            dictionary_combo_, kDictionaryValues);
        create_options.word_size = selected_combo_value(
            word_size_combo_, kWordSizeValues);
        create_options.solid_block_size = selected_combo_value(
            solid_block_combo_, kSolidBlockValues);
        create_options.archive_format = selected_archive_format();
        clear_options_unsupported_by_selected_format();

        if (const LRESULT selection = SendMessageW(update_mode_combo_, CB_GETCURSEL, 0, 0);
            selection != CB_ERR) {
            create_options.features.update_mode = static_cast<ArchiveUpdateMode>(selection);
        }
        create_options.features.comment = window_text(comment_edit_);
        create_options.features.volume_size = window_text(volume_size_edit_);
        if (const LRESULT selection = SendMessageW(volume_unit_combo_, CB_GETCURSEL, 0, 0);
            selection != CB_ERR) {
            create_options.features.volume_unit = static_cast<int>(selection);
        }
        try {
            const std::wstring recovery = window_text(recovery_percent_edit_);
            create_options.features.recovery_percent = recovery.empty() ? 0 : std::stoi(recovery);
        } catch (...) {
            create_options.features.recovery_percent = -1;
        }
        if (create_options.features.recovery_percent < 0 ||
            create_options.features.recovery_percent > 100) {
            select_create_page(3);
            show_message_dialog(window_, instance_, dpi_, palette_.dark,
                                L"Recovery and volumes",
                                L"Recovery percentage must be between 0 and 100.",
                                MessageDialogIcon::warning);
            return;
        }

        if (create_options.features.encrypt_data) {
            const std::wstring password = window_text(password_edit_);
            if (password.empty()) {
                select_create_page(2);
                show_message_dialog(window_, instance_, dpi_, palette_.dark,
                                    L"Archive security", L"Enter an archive password.",
                                    MessageDialogIcon::warning);
                return;
            }
            if (password != window_text(confirm_password_edit_)) {
                select_create_page(2);
                show_message_dialog(window_, instance_, dpi_, palette_.dark,
                                    L"Archive security", L"The passwords do not match.",
                                    MessageDialogIcon::warning);
                return;
            }
            if (create_options.features.encrypt_names &&
                (!create_options.features.comment.empty() ||
                 create_options.features.lock_archive)) {
                select_create_page(1);
                show_message_dialog(
                    window_, instance_, dpi_, palette_.dark, L"Archive security",
                    L"Comments and locking cannot currently be changed after file-name "
                    L"encryption is applied. Clear those options or encrypt file data only.",
                    MessageDialogIcon::warning);
                return;
            }
            create_options.features.password = password;
        } else {
            create_options.features.encrypt_names = false;
            create_options.features.password.clear();
        }

        create_options.features.signing_key = window_text(signing_key_edit_);
        if (create_options.features.sign_archive &&
            create_options.features.signing_key.empty()) {
            select_create_page(4);
            show_message_dialog(window_, instance_, dpi_, palette_.dark,
                                L"Archive signing", L"Choose an Axiom signing key.",
                                MessageDialogIcon::warning);
            return;
        }
        if (create_options.features.create_sfx) {
            displayed_output.replace_extension(L".exe");
            create_options.features.sfx_destination = displayed_output.wstring();
            create_options.archive_path = displayed_output;
            create_options.archive_path.replace_extension(L".axar");
            create_options.archive_format = axiom::ArchiveFormat::axar;
            create_options.features.volume_size.clear();
            create_options.features.create_recovery_volumes = false;
        } else {
            if (displayed_output.extension().empty() ||
                is_known_archive_extension(displayed_output)) {
                displayed_output.replace_extension(
                    widen_ascii(archive_format_info(create_options.archive_format)
                                    .default_extension));
            }
            create_options.archive_path = displayed_output;
            create_options.features.sfx_destination.clear();
        }

        SetWindowTextW(password_edit_, L"");
        SetWindowTextW(confirm_password_edit_, L"");
        accepted_ = true;
        close_dialog();
    }

    void close_dialog() {
        save_named_window_placement(layout_name(), window_);
        restore_dialog_owner(owner_, owner_was_enabled_);
        owner_was_enabled_ = false;
        if (window_ != nullptr && IsWindow(window_)) {
            DestroyWindow(window_);
        }
    }

    void show_advanced_features() {
        ArchiveFeatureOptions archive_features;
        ExtractFeatureOptions extract_features = extract_options.features;
        if (show_archive_feature_options_dialog(
                window_, ArchiveFeatureDialogContext::extract,
                archive_features, extract_features,
                extract_options.feature_availability)) {
            extract_options.features = std::move(extract_features);
        }
    }

    bool checkbox_checked(int id) const {
        switch (id) {
            case kOverwrite: return overwrite_checked_;
            case kRestoreTime: return restore_time_checked_;
            case kConfirmDelete:
                return mode_ == DialogMode::settings
                    ? application_options.confirm_delete
                    : confirm_delete_checked_;
            case kShowHidden:
                return mode_ == DialogMode::settings
                    ? application_options.show_hidden
                    : show_hidden_checked_;
            case kRestoreWindowPlacement: return application_options.restore_window_placement;
            case kConfirmOverwrite: return application_options.confirm_overwrite;
            case kDefaultRecoveryVolumes: return application_options.default_recovery_volumes;
            case kDefaultCreateSfx: return application_options.default_create_sfx;
            case kDefaultSignArchive: return application_options.default_sign_archive;
            case kShowParentEntry: return application_options.show_parent_entry;
            case kShowGridLines: return application_options.show_grid_lines;
            case kShowHorizontalScrollbar: return application_options.show_horizontal_scrollbar;
            case kFullRowSelect: return application_options.full_row_select;
            case kShowAddressShellLocations:
                return application_options.show_address_shell_locations;
            case kShowAddressRecentLocations:
                return application_options.show_address_recent_locations;
            case kShowAddressArchiveChildren:
                return application_options.show_address_archive_children;
            case kWarnExecutableOpen: return application_options.warn_executable_open;
            case kKeepViewedFilesUntilExit:
                return application_options.keep_viewed_files_until_exit;
            case kCachePasswords: return application_options.cache_passwords;
            case kVerifySignatures: return application_options.verify_signatures;
            case kWipeEncryptedTempFiles: return application_options.wipe_encrypted_temp_files;
            case kAssociateAxar: return application_options.associate_axar;
            case kAssociateZip: return application_options.associate_zip;
            case kAssociate7z: return application_options.associate_7z;
            case kAssociateRar: return application_options.associate_rar;
            case kAssociateTar: return application_options.associate_tar;
            case kAssociateIso: return application_options.associate_iso;
            case kAssociateCab: return application_options.associate_cab;
            case kContextOpen: return application_options.context_open;
            case kContextAdd: return application_options.context_add;
            case kContextExtract: return application_options.context_extract;
            case kContextTest: return application_options.context_test;
            case kAutomaticUpdateChecks: return application_options.automatic_update_checks;
            case kVerboseLogging: return application_options.verbose_logging;
            case kLockArchive: return create_options.features.lock_archive;
            case kRepackAfterUpdate: return create_options.features.repack_after_update;
            case kEncryptData: return create_options.features.encrypt_data;
            case kEncryptNames: return create_options.features.encrypt_names;
            case kShowPassword: return create_show_password_;
            case kRecoveryVolumes: return create_options.features.create_recovery_volumes;
            case kSignArchive: return create_options.features.sign_archive;
            case kCreateSfx: return create_options.features.create_sfx;
            default: return false;
        }
    }

    void toggle(int id, HWND checkbox) {
        switch (id) {
            case kOverwrite: overwrite_checked_ = !overwrite_checked_; break;
            case kRestoreTime: restore_time_checked_ = !restore_time_checked_; break;
            case kConfirmDelete:
                if (mode_ == DialogMode::settings) {
                    application_options.confirm_delete = !application_options.confirm_delete;
                } else {
                    confirm_delete_checked_ = !confirm_delete_checked_;
                }
                break;
            case kShowHidden:
                if (mode_ == DialogMode::settings) {
                    application_options.show_hidden = !application_options.show_hidden;
                } else {
                    show_hidden_checked_ = !show_hidden_checked_;
                }
                break;
            case kRestoreWindowPlacement:
                application_options.restore_window_placement =
                    !application_options.restore_window_placement;
                break;
            case kConfirmOverwrite:
                application_options.confirm_overwrite = !application_options.confirm_overwrite;
                break;
            case kDefaultRecoveryVolumes:
                application_options.default_recovery_volumes =
                    !application_options.default_recovery_volumes;
                break;
            case kDefaultCreateSfx:
                application_options.default_create_sfx = !application_options.default_create_sfx;
                break;
            case kDefaultSignArchive:
                application_options.default_sign_archive =
                    !application_options.default_sign_archive;
                break;
            case kShowParentEntry:
                application_options.show_parent_entry = !application_options.show_parent_entry;
                break;
            case kShowGridLines:
                application_options.show_grid_lines = !application_options.show_grid_lines;
                break;
            case kShowHorizontalScrollbar:
                application_options.show_horizontal_scrollbar =
                    !application_options.show_horizontal_scrollbar;
                break;
            case kFullRowSelect:
                application_options.full_row_select = !application_options.full_row_select;
                break;
            case kShowAddressShellLocations:
                application_options.show_address_shell_locations =
                    !application_options.show_address_shell_locations;
                break;
            case kShowAddressRecentLocations:
                application_options.show_address_recent_locations =
                    !application_options.show_address_recent_locations;
                break;
            case kShowAddressArchiveChildren:
                application_options.show_address_archive_children =
                    !application_options.show_address_archive_children;
                break;
            case kWarnExecutableOpen:
                application_options.warn_executable_open =
                    !application_options.warn_executable_open;
                break;
            case kKeepViewedFilesUntilExit:
                application_options.keep_viewed_files_until_exit =
                    !application_options.keep_viewed_files_until_exit;
                break;
            case kCachePasswords:
                application_options.cache_passwords = !application_options.cache_passwords;
                break;
            case kVerifySignatures:
                application_options.verify_signatures = !application_options.verify_signatures;
                break;
            case kWipeEncryptedTempFiles:
                application_options.wipe_encrypted_temp_files =
                    !application_options.wipe_encrypted_temp_files;
                break;
            case kAssociateAxar:
                application_options.associate_axar = !application_options.associate_axar;
                break;
            case kAssociateZip:
                application_options.associate_zip = !application_options.associate_zip;
                break;
            case kAssociate7z:
                application_options.associate_7z = !application_options.associate_7z;
                break;
            case kAssociateRar:
                application_options.associate_rar = !application_options.associate_rar;
                break;
            case kAssociateTar:
                application_options.associate_tar = !application_options.associate_tar;
                break;
            case kAssociateIso:
                application_options.associate_iso = !application_options.associate_iso;
                break;
            case kAssociateCab:
                application_options.associate_cab = !application_options.associate_cab;
                break;
            case kContextOpen:
                application_options.context_open = !application_options.context_open;
                break;
            case kContextAdd:
                application_options.context_add = !application_options.context_add;
                break;
            case kContextExtract:
                application_options.context_extract = !application_options.context_extract;
                break;
            case kContextTest:
                application_options.context_test = !application_options.context_test;
                break;
            case kAutomaticUpdateChecks:
                application_options.automatic_update_checks =
                    !application_options.automatic_update_checks;
                break;
            case kVerboseLogging:
                application_options.verbose_logging = !application_options.verbose_logging;
                break;
            case kLockArchive:
                create_options.features.lock_archive = !create_options.features.lock_archive;
                break;
            case kRepackAfterUpdate:
                create_options.features.repack_after_update =
                    !create_options.features.repack_after_update;
                break;
            case kEncryptData:
                create_options.features.encrypt_data = !create_options.features.encrypt_data;
                if (!create_options.features.encrypt_data) {
                    create_options.features.encrypt_names = false;
                }
                break;
            case kEncryptNames:
                create_options.features.encrypt_names = !create_options.features.encrypt_names;
                if (create_options.features.encrypt_names) {
                    create_options.features.encrypt_data = true;
                }
                break;
            case kShowPassword:
                create_show_password_ = !create_show_password_;
                set_password_visibility();
                break;
            case kRecoveryVolumes:
                create_options.features.create_recovery_volumes =
                    !create_options.features.create_recovery_volumes;
                break;
            case kSignArchive:
                create_options.features.sign_archive = !create_options.features.sign_archive;
                break;
            case kCreateSfx:
                create_options.features.create_sfx = !create_options.features.create_sfx;
                if (create_options.features.create_sfx) {
                    create_options.archive_format = axiom::ArchiveFormat::axar;
                    SendMessageW(format_combo_, CB_SETCURSEL,
                                 static_cast<WPARAM>(creatable_archive_format_index(
                                     create_options.archive_format)), 0);
                    create_options.features.create_recovery_volumes = false;
                    create_options.features.volume_size.clear();
                    SetWindowTextW(volume_size_edit_, L"");
                }
                convert_output_mode(create_options.features.create_sfx);
                break;
        }
        InvalidateRect(checkbox, nullptr, TRUE);
        if (mode_ == DialogMode::create_archive) {
            update_create_dependencies();
            update_output_preview();
        }
    }

    bool is_checkbox_id(int id) const {
        switch (id) {
            case kOverwrite:
            case kRestoreTime:
            case kConfirmDelete:
            case kShowHidden:
            case kRestoreWindowPlacement:
            case kConfirmOverwrite:
            case kDefaultRecoveryVolumes:
            case kDefaultCreateSfx:
            case kDefaultSignArchive:
            case kShowParentEntry:
            case kShowGridLines:
            case kShowHorizontalScrollbar:
            case kFullRowSelect:
            case kShowAddressShellLocations:
            case kShowAddressRecentLocations:
            case kShowAddressArchiveChildren:
            case kWarnExecutableOpen:
            case kKeepViewedFilesUntilExit:
            case kCachePasswords:
            case kVerifySignatures:
            case kWipeEncryptedTempFiles:
            case kAssociateAxar:
            case kAssociateZip:
            case kAssociate7z:
            case kAssociateRar:
            case kAssociateTar:
            case kAssociateIso:
            case kAssociateCab:
            case kContextOpen:
            case kContextAdd:
            case kContextExtract:
            case kContextTest:
            case kAutomaticUpdateChecks:
            case kVerboseLogging:
            case kLockArchive:
            case kRepackAfterUpdate:
            case kEncryptData:
            case kEncryptNames:
            case kShowPassword:
            case kRecoveryVolumes:
            case kSignArchive:
            case kCreateSfx:
                return true;
            default:
                return false;
        }
    }

    void draw_button(const DRAWITEMSTRUCT& draw) const {
        RECT rect = draw.rcItem;
        const int id = GetDlgCtrlID(draw.hwndItem);
        const bool checkbox = is_checkbox_id(id);
        if (!checkbox) {
            draw_dialog_button(draw, palette_.dark);
            return;
        }
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
                HPEN pen = CreatePen(PS_SOLID, scale(2),
                                     disabled ? palette_.muted : palette_.accent);
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

    void rebuild_font_for_dpi() {
        if (font_) DeleteObject(font_);
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if (!SystemParametersInfoForDpi(
                SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        }
        font_ = CreateFontIndirectW(&metrics.lfMessageFont);
        EnumChildWindows(window_, [](HWND child, LPARAM font) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(font_));
        EnumChildWindows(window_, [](HWND child, LPARAM self_param) -> BOOL {
            auto* self = reinterpret_cast<OptionsDialog*>(self_param);
            wchar_t class_name[32]{};
            GetClassNameW(child, class_name,
                          static_cast<int>(sizeof(class_name) / sizeof(class_name[0])));
            if (lstrcmpiW(class_name, L"ComboBox") == 0) {
                SendMessageW(child, CB_SETITEMHEIGHT, 0, self->scale(24));
                SendMessageW(child, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1),
                             self->scale(24));
                apply_dialog_control_theme(child, self->palette_.dark);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(this));
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE: create_controls(); return 0;
            case WM_SIZE: layout(); return 0;
            case WM_GETMINMAXINFO:
                if (mode_ == DialogMode::create_archive || mode_ == DialogMode::settings) {
                    auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
                    int minimum_width = scale(mode_ == DialogMode::settings ? 840 : 650);
                    int minimum_height = scale(mode_ == DialogMode::settings ? 700 : 520);
                    MONITORINFO monitor{sizeof(monitor)};
                    if (GetMonitorInfoW(
                            MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                            &monitor)) {
                        minimum_width = std::min(
                            minimum_width, static_cast<int>(monitor.rcWork.right -
                                                             monitor.rcWork.left));
                        minimum_height = std::min(
                            minimum_height, static_cast<int>(monitor.rcWork.bottom -
                                                              monitor.rcWork.top));
                    }
                    limits->ptMinTrackSize = {minimum_width, minimum_height};
                    return 0;
                }
                break;
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wparam);
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                rebuild_font_for_dpi();
                layout();
                return 0;
            }
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
            case WM_CTLCOLORLISTBOX:
                SetBkColor(reinterpret_cast<HDC>(wparam), palette_.edit);
                SetTextColor(reinterpret_cast<HDC>(wparam), palette_.text);
                return reinterpret_cast<LRESULT>(edit_brush_);
            case WM_DRAWITEM: {
                const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
                if (draw.CtlType == ODT_COMBOBOX) {
                    draw_dialog_combo_item(draw, palette_.dark);
                } else {
                    draw_button(draw);
                }
                return TRUE;
            }
            case WM_COMMAND: {
                const int id = LOWORD(wparam);
                if (mode_ == DialogMode::settings &&
                    id >= kSettingsTabBase &&
                    id < kSettingsTabBase + static_cast<int>(kSettingsTabNames.size())) {
                    select_settings_page(id - kSettingsTabBase);
                    return 0;
                }
                if (mode_ == DialogMode::settings && is_checkbox_id(id)) {
                    toggle(id, item(id));
                    return 0;
                }
                if (mode_ == DialogMode::settings &&
                    (id == kIoBufferMode || id == kMemoryLimitMode) &&
                    HIWORD(wparam) == CBN_SELCHANGE) {
                    update_settings_dependencies();
                    return 0;
                }
                if (mode_ == DialogMode::settings &&
                    id == kShortcutCommand && HIWORD(wparam) == CBN_SELCHANGE) {
                    update_shortcut_controls();
                    return 0;
                }
                if (id >= kCreateTabBase &&
                    id < kCreateTabBase + static_cast<int>(kCreateTabNames.size())) {
                    select_create_page(id - kCreateTabBase);
                    return 0;
                }
                if (mode_ == DialogMode::create_archive &&
                    id == kUpdateMode && HIWORD(wparam) == CBN_SELCHANGE) {
                    const LRESULT selection = SendMessageW(update_mode_combo_, CB_GETCURSEL, 0, 0);
                    if (selection != CB_ERR) {
                        create_options.features.update_mode =
                            static_cast<ArchiveUpdateMode>(selection);
                    }
                    update_create_dependencies();
                    return 0;
                }
                if (mode_ == DialogMode::create_archive &&
                    id == kArchiveFormat && HIWORD(wparam) == CBN_SELCHANGE) {
                    on_archive_format_changed();
                    return 0;
                }
                if (mode_ == DialogMode::create_archive &&
                    (id == kPathEdit || id == kVolumeSize) &&
                    HIWORD(wparam) == EN_CHANGE) {
                    if (id == kPathEdit) {
                        sync_archive_format_from_path();
                    }
                    update_output_preview();
                    return 0;
                }
                switch (id) {
                    case kBrowse: browse(); return 0;
                    case kBrowseStartupCustomPath:
                    case kBrowseDefaultSigningKey:
                    case kBrowseArchiveOutputFolder:
                    case kBrowseExtractDestinationFolder:
                    case kBrowseTempFolder:
                    case kBrowseExternalViewer:
                    case kBrowseExternalEditor:
                    case kBrowseTrustedKeysFolder:
                    case kBrowseLogFolder:
                        if (browse_settings_path(id)) return 0;
                        break;
                    case kAdvancedFeatures: show_advanced_features(); return 0;
                    case kBrowseSigningKey:
                        if (const auto key = browse_signing_key(window_)) {
                            set_window_text(signing_key_edit_, key->wstring());
                        }
                        return 0;
                    case kApply:
                        if (mode_ == DialogMode::settings) apply_settings_live(false);
                        return 0;
                    case kShortcutAssign:
                        if (mode_ == DialogMode::settings) commit_shortcut_edit(true);
                        return 0;
                    case kShortcutClear:
                        if (mode_ == DialogMode::settings) {
                            set_window_text(item(kShortcutValue), L"None");
                            commit_shortcut_edit(true);
                        }
                        return 0;
                    case kShortcutResetAll:
                        if (mode_ == DialogMode::settings) {
                            application_options.shortcut_overrides.clear();
                            update_shortcut_controls();
                        }
                        return 0;
                    case kDefaults:
                        if (mode_ == DialogMode::settings) {
                            application_options = ApplicationDialogOptions{};
                            load_settings_values();
                            InvalidateRect(window_, nullptr, TRUE);
                        }
                        return 0;
                    case kAccept: accept(); return 0;
                    case kCancel:
                        if (password_edit_) SetWindowTextW(password_edit_, L"");
                        if (confirm_password_edit_) SetWindowTextW(confirm_password_edit_, L"");
                        close_dialog();
                        return 0;
                    case kOverwrite: toggle(kOverwrite, overwrite_); return 0;
                    case kRestoreTime: toggle(kRestoreTime, restore_time_); return 0;
                    case kConfirmDelete: toggle(kConfirmDelete, confirm_delete_); return 0;
                    case kShowHidden: toggle(kShowHidden, show_hidden_); return 0;
                    case kLockArchive: toggle(kLockArchive, lock_archive_); return 0;
                    case kRepackAfterUpdate:
                        toggle(kRepackAfterUpdate, repack_after_update_); return 0;
                    case kEncryptData: toggle(kEncryptData, encrypt_data_); return 0;
                    case kEncryptNames: toggle(kEncryptNames, encrypt_names_); return 0;
                    case kShowPassword: toggle(kShowPassword, show_password_); return 0;
                    case kRecoveryVolumes:
                        toggle(kRecoveryVolumes, recovery_volumes_); return 0;
                    case kSignArchive: toggle(kSignArchive, sign_archive_); return 0;
                    case kCreateSfx: toggle(kCreateSfx, create_sfx_); return 0;
                }
                break;
            }
            case WM_CLOSE:
                if (password_edit_) SetWindowTextW(password_edit_, L"");
                if (confirm_password_edit_) SetWindowTextW(confirm_password_edit_, L"");
                close_dialog();
                return 0;
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
    bool owner_was_enabled_ = false;
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
    bool create_show_password_ = false;
    int create_page_ = 0;
    int settings_page_ = 0;
    HWND create_tabs_ = nullptr;
    HWND settings_tabs_ = nullptr;
    std::vector<PageControl> page_controls_;
    std::vector<SettingControl> settings_controls_;
    HWND summary_ = nullptr;
    HWND path_label_ = nullptr;
    HWND path_edit_ = nullptr;
    HWND browse_ = nullptr;
    HWND format_label_ = nullptr;
    HWND format_combo_ = nullptr;
    HWND advanced_features_ = nullptr;
    HWND update_mode_label_ = nullptr;
    HWND update_mode_combo_ = nullptr;
    HWND comment_label_ = nullptr;
    HWND comment_edit_ = nullptr;
    HWND lock_archive_ = nullptr;
    HWND repack_after_update_ = nullptr;
    HWND metadata_heading_ = nullptr;
    HWND metadata_info_ = nullptr;
    HWND level_label_ = nullptr;
    HWND level_combo_ = nullptr;
    HWND dictionary_label_ = nullptr;
    HWND dictionary_combo_ = nullptr;
    HWND word_size_label_ = nullptr;
    HWND word_size_combo_ = nullptr;
    HWND solid_block_label_ = nullptr;
    HWND solid_block_combo_ = nullptr;
    HWND threads_label_ = nullptr;
    HWND threads_combo_ = nullptr;
    HWND compression_info_ = nullptr;
    HWND encrypt_data_ = nullptr;
    HWND encrypt_names_ = nullptr;
    HWND password_label_ = nullptr;
    HWND password_edit_ = nullptr;
    HWND confirm_password_label_ = nullptr;
    HWND confirm_password_edit_ = nullptr;
    HWND show_password_ = nullptr;
    HWND security_info_ = nullptr;
    HWND volume_size_label_ = nullptr;
    HWND volume_size_edit_ = nullptr;
    HWND volume_unit_combo_ = nullptr;
    HWND recovery_percent_label_ = nullptr;
    HWND recovery_percent_edit_ = nullptr;
    HWND recovery_percent_suffix_ = nullptr;
    HWND recovery_volumes_ = nullptr;
    HWND recovery_info_ = nullptr;
    HWND sign_archive_ = nullptr;
    HWND signing_key_label_ = nullptr;
    HWND signing_key_edit_ = nullptr;
    HWND browse_signing_key_ = nullptr;
    HWND create_sfx_ = nullptr;
    HWND sfx_info_ = nullptr;
    HWND output_preview_ = nullptr;
    HWND overwrite_ = nullptr;
    HWND restore_time_ = nullptr;
    HWND confirm_delete_ = nullptr;
    HWND show_hidden_ = nullptr;
    HWND accept_ = nullptr;
    HWND cancel_ = nullptr;
    HWND apply_ = nullptr;
    HWND defaults_ = nullptr;
    HWND settings_shortcut_default_label_ = nullptr;
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
    return show_application_settings_dialog(owner, options, {});
}

bool show_application_settings_dialog(
    HWND owner,
    ApplicationDialogOptions& options,
    const std::function<void(const ApplicationDialogOptions&)>& apply_callback) {
    OptionsDialog dialog(DialogMode::settings);
    dialog.application_options = options;
    dialog.settings_apply_callback = apply_callback;
    if (!dialog.show(owner)) return false;
    options = std::move(dialog.application_options);
    return true;
}

}  // namespace axiom::gui
