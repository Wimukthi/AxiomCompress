#define NOMINMAX
#include "gui/archive_dialogs.hpp"
#include "core/cpu.hpp"
#include "core/path_text.hpp"
#include "gui/dialog_support.hpp"
#include "gui/main_window_internal.hpp"
#include "gui/message_dialog.hpp"

#include <dwmapi.h>
#include <gdiplus.h>
#include <shobjidl.h>
#include <winhttp.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace axiom::gui {
namespace {

constexpr int kPathEdit = 2001;
constexpr int kBrowse = 2002;
constexpr int kLevel = 2003;
constexpr int kCompressionMethod = 2004;
constexpr int kThreads = 2006;
constexpr int kOverwrite = 2007;
constexpr int kRestoreTime = 2008;
constexpr int kConfirmDelete = 2009;
constexpr int kShowHidden = 2010;
constexpr int kDictionarySize = 2012;
constexpr int kWordSize = 2013;
constexpr int kSolidBlockSize = 2014;
constexpr int kArchiveFormat = 2015;
constexpr int kThreadModel = 2016;
constexpr int kCompressionProfile = 2017;
constexpr int kSaveCompressionProfile = 2018;
constexpr int kDeleteCompressionProfile = 2019;
constexpr int kCompressionPreview = 2020;
constexpr int kCreateNavigation = 2099;
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
// SFX options page. 2144-2199 is free before the settings-dialog block.
constexpr int kSfxStubTier = 2144;
constexpr int kSfxTitle = 2145;
constexpr int kSfxDefaultPath = 2146;
constexpr int kSfxOverwrite = 2147;
constexpr int kSfxMode = 2148;
constexpr int kSfxElevation = 2149;
constexpr int kSfxRunProgram = 2150;
constexpr int kSfxRunArguments = 2151;
constexpr int kSfxLicenseText = 2152;
constexpr int kSfxAllowPathChange = 2153;
constexpr int kSfxRequireAccept = 2154;
constexpr int kSfxOpenDestination = 2155;
constexpr int kSfxDescription = 2156;
constexpr int kSfxTheme = 2157;
constexpr int kContentDedup = 2160;
constexpr int kDedupMinChunk = 2161;
constexpr int kDedupAverageChunk = 2162;
constexpr int kDedupMaxChunk = 2163;
constexpr int kSettingsTabs = 2200;
constexpr int kSettingsTabBase = 2210;
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
constexpr int kCenterChildWindows = 2240;
constexpr int kPickAccentColor = 2241;
constexpr int kToolbarDisplayMode = 2242;
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
constexpr int kCustomizeFileColumns = 2365;
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
constexpr int kToolbarList = 2700;
constexpr int kToolbarResetDefaults = 2790;
constexpr int kToolbarStatusCombo = 2791;
constexpr int kApply = 2800;
constexpr int kDefaults = 2801;
constexpr int kAccept = IDOK;
constexpr int kCancel = IDCANCEL;
constexpr std::uint64_t kMinIoBufferSize = 64ull << 10;
constexpr std::uint64_t kMaxIoBufferSize = 64ull << 20;
constexpr std::uint64_t kMinMemoryLimitSize = 64ull << 10;
constexpr std::uint64_t kSigningSecretKeySize = 64;
constexpr UINT kCompressionCurveUpdated = WM_APP + 91;
constexpr UINT kCompressionCurveFinished = WM_APP + 92;
constexpr UINT_PTR kCompressionCurveDebounceTimer = 0xA711;

constexpr std::array<const wchar_t*, 9> kLevelNames{
    L"1 - Fastest", L"2 - Very fast", L"3 - Fast", L"4 - Normal",
    L"5 - Balanced", L"6 - Strong", L"7 - High", L"8 - Very high",
    L"9 - Maximum"};
constexpr std::array<const wchar_t*, 16> kDictionaryNames{
    L"Default for level", L"64 KiB", L"256 KiB", L"1 MiB", L"2 MiB",
    L"4 MiB", L"8 MiB", L"16 MiB", L"32 MiB", L"64 MiB",
    L"128 MiB", L"256 MiB", L"512 MiB", L"1 GiB", L"2 GiB", L"4 GiB"};
constexpr std::array<std::size_t, 16> kDictionaryValues{
    0, 64u << 10, 256u << 10, 1u << 20, 2u << 20, 4u << 20,
    8u << 20, 16u << 20, 32u << 20, 64u << 20, 128u << 20,
    256u << 20, 512u << 20, 1u << 30, 2u << 30,
    axiom::kMaxAxiomWindowSize};
constexpr std::array<const wchar_t*, 6> kWordSizeNames{
    L"Default for level", L"32", L"64", L"128", L"192", L"273"};
constexpr std::array<std::size_t, 6> kWordSizeValues{0, 32, 64, 128, 192, 273};
constexpr std::array<const wchar_t*, 17> kSolidBlockNames{
    L"Default for level", L"1 MiB", L"4 MiB", L"8 MiB", L"16 MiB",
    L"32 MiB", L"64 MiB", L"128 MiB", L"256 MiB", L"512 MiB",
    L"1 GiB", L"2 GiB", L"4 GiB", L"8 GiB", L"16 GiB", L"32 GiB", L"64 GiB"};
constexpr std::array<std::size_t, 17> kSolidBlockValues{
    0, 1u << 20, 4u << 20, 8u << 20, 16u << 20, 32u << 20,
    64u << 20, 128u << 20, 256u << 20, 512u << 20,
    std::size_t{1} << 30, std::size_t{2} << 30, std::size_t{4} << 30,
    std::size_t{8} << 30, std::size_t{16} << 30, std::size_t{32} << 30,
    std::size_t{64} << 30};
constexpr std::array<const wchar_t*, 2> kThreadModelNames{
    L"Split blocks (default)", L"Swarm (cores share each block)"};
constexpr std::array<const wchar_t*, 5> kCompressionMethodNames{
    L"Axiom adaptive", L"Zstandard", L"LZMA2", L"Deflate", L"Store"};
constexpr std::array<const wchar_t*, 2> kLzmaMatchFinderNames{
    L"HC4 (faster)", L"BT4 (better ratio)"};

const std::array<CompressionProfile, 5>& built_in_compression_profiles() {
    static const std::array<CompressionProfile, 5> profiles{{
        // Type presets are practical defaults, not maximum-ratio modes. Text
        // still benefits from the tree matcher and a moderate window without
        // paying for the level-9 optimal parse.
        {L"Text, logs and source code (built-in)",
         7, 0, 16u << 20, 192, 16u << 20, 0},
        // Executable filtering is selected per file by the archive writer;
        // a larger tree window preserves repeated code and library records,
        // while level 7 avoids the much slower optimal-parser pass.
        {L"Executables and libraries (built-in)",
         7, 0, 32u << 20, 128, 32u << 20, 0},
        // Tables, database pages, CSV, and numeric arrays retain enough context
        // for repeated records, but stop at the level-7 tree Pareto point.
        {L"Databases and structured data (built-in)",
         7, 0, 16u << 20, 128, 16u << 20, 0},
        // Most media payloads are already entropy-coded. Keep probes and blocks
        // small so incompressible data is recognized and stored quickly.
        {L"Photos, audio and video (built-in)",
         1, 0, 64u << 10, 32, 1u << 20, 0},
        // Heterogeneous folders favor the balanced hash parser: it is much
        // faster than level 6 here for only a marginal size difference.
        {L"Mixed files and folders (built-in)",
         5, 0, 8u << 20, 128, 16u << 20, 0},
    }};
    return profiles;
}

bool profile_names_equal(std::wstring_view left, std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()),
                                TRUE) == CSTR_EQUAL;
}

bool is_built_in_profile_name(std::wstring_view name) {
    return std::any_of(
        built_in_compression_profiles().begin(), built_in_compression_profiles().end(),
        [&](const CompressionProfile& profile) {
            return profile_names_equal(profile.name, name);
        });
}
constexpr std::array<const wchar_t*, 6> kCreateTabNames{
    L"Compression", L"General", L"Security", L"Recovery & volumes", L"SFX",
    L"Deduplication"};
constexpr std::array<const wchar_t*, 2> kSfxStubTierNames{
    L"Full window (dialogs)", L"Console only (unattended)"};
constexpr std::array<const wchar_t*, 9> kSfxDefaultPathNames{
    L"%SFXDIR%",
    L"%SFXDIR%\\%SFXNAME%",
    L"%TEMP%\\%SFXNAME%",
    L"%LOCALAPPDATA%\\%SFXNAME%",
    L"%APPDATA%\\%SFXNAME%",
    L"%PROGRAMFILES%\\%SFXNAME%",
    L"%USERPROFILE%\\%SFXNAME%",
    L"%DESKTOP%\\%SFXNAME%",
    L"%DOCUMENTS%\\%SFXNAME%"};
constexpr std::array<const wchar_t*, 3> kSfxOverwriteNames{
    L"Replace existing files", L"Skip existing files", L"Stop on an existing file"};
constexpr std::array<const wchar_t*, 3> kSfxModeNames{
    L"Interactive", L"Silent (progress and errors only)", L"No window"};
constexpr std::array<const wchar_t*, 3> kSfxThemeNames{
    L"Follow the system", L"Always light", L"Always dark"};
constexpr std::array<const wchar_t*, 3> kSfxElevationNames{
    L"Never elevate", L"Elevate when the destination needs it", L"Always elevate"};
constexpr std::array<const wchar_t*, 11> kSettingsTabNames{
    L"General", L"Compression", L"Paths", L"File list", L"Viewer",
    L"Security", L"Integration", L"Updates", L"Shortcuts", L"Toolbar", L"Advanced"};
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
constexpr std::array<const wchar_t*, 2> kToolbarDisplayModeNames{
    L"Icons and text", L"Icons only"};
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
constexpr std::array<const wchar_t*, 2> kToolbarStatusNames{
    L"Enabled", L"Hidden"};

template <std::size_t Size>
int value_index(const std::array<std::size_t, Size>& values, std::size_t value) {
    const auto found = std::find(values.begin(), values.end(), value);
    return found == values.end() ? 0 : static_cast<int>(found - values.begin());
}

std::wstring widen_ascii(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

// The SFX configuration is UTF-8 on disk while dialog text is UTF-16. Going
// through path's u8string conversion keeps every valid Unicode string intact.
std::string narrow_utf8(const std::wstring& value) {
    return axiom::core::path_to_utf8(std::filesystem::path(value));
}

class CompressionGraphGdiplusSession {
public:
    CompressionGraphGdiplusSession() {
        Gdiplus::GdiplusStartupInput input;
        ready_ =
            Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
    }

    ~CompressionGraphGdiplusSession() {
        if (ready_) Gdiplus::GdiplusShutdown(token_);
    }

    bool ready() const { return ready_; }

private:
    ULONG_PTR token_ = 0;
    bool ready_ = false;
};

bool compression_graph_gdiplus_ready() {
    static CompressionGraphGdiplusSession session;
    return session.ready();
}

Gdiplus::Color graph_color(COLORREF color, BYTE alpha = 255) {
    return Gdiplus::Color(
        alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

COLORREF compression_graph_saved_color(COLORREF accent) {
    constexpr COLORREF green = RGB(38, 166, 91);
    constexpr COLORREF amber = RGB(232, 153, 34);
    const auto distance = [accent](COLORREF candidate) {
        const int red =
            static_cast<int>(GetRValue(accent)) -
            static_cast<int>(GetRValue(candidate));
        const int green_delta =
            static_cast<int>(GetGValue(accent)) -
            static_cast<int>(GetGValue(candidate));
        const int blue =
            static_cast<int>(GetBValue(accent)) -
            static_cast<int>(GetBValue(candidate));
        return red * red + green_delta * green_delta + blue * blue;
    };
    // Green communicates savings well for the normal blue, red, purple, and
    // amber accents. Switch to amber only when the active accent is already
    // close enough to green that the two areas would become indistinguishable.
    return distance(green) < 6000 ? amber : green;
}

std::wstring format_preview_bytes(std::uint64_t bytes) {
    static constexpr std::array<const wchar_t*, 6> units{
        L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB"};
    long double value = static_cast<long double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0L && unit + 1 < units.size()) {
        value /= 1024.0L;
        ++unit;
    }
    std::wostringstream output;
    output << std::fixed << std::setprecision(unit == 0 ? 0 : 1)
           << static_cast<double>(value) << L' ' << units[unit];
    return output.str();
}

std::wstring format_preview_percent(double value) {
    std::wostringstream output;
    output << std::fixed << std::setprecision(1) << value << L'%';
    return output.str();
}

const wchar_t* estimate_confidence_name(EstimateConfidence confidence) {
    switch (confidence) {
        case EstimateConfidence::high: return L"high confidence";
        case EstimateConfidence::medium: return L"medium confidence";
        case EstimateConfidence::low: return L"low confidence";
    }
    return L"low confidence";
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
    COLORREF window = RGB(250, 250, 250);
    COLORREF edit = RGB(255, 255, 255);
    COLORREF button = RGB(255, 255, 255);
    COLORREF hot = RGB(244, 244, 244);
    COLORREF pressed = RGB(235, 235, 235);
    COLORREF border = RGB(204, 204, 204);
    COLORREF text = RGB(32, 32, 32);
    COLORREF muted = RGB(96, 96, 96);
    COLORREF focus = GetSysColor(COLOR_HIGHLIGHT);
    COLORREF accent = GetSysColor(COLOR_HIGHLIGHT);
    COLORREF selection_text = GetSysColor(COLOR_HIGHLIGHTTEXT);
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
    const DialogColors shared = dialog_colors(result.dark);
    result.window = shared.background;
    result.edit = shared.control_background;
    result.button = shared.control_background;
    result.hot = blend_color(result.button, shared.focus_border,
                             result.dark ? 10 : 8);
    result.pressed = blend_color(result.button, shared.focus_border,
                                 result.dark ? 18 : 16);
    result.border = shared.border;
    result.text = shared.text;
    result.muted = shared.disabled_text;
    result.focus = shared.focus_border;
    result.selection_text = shared.selection_text;
    if (high_contrast_enabled()) {
        result.dark = false;
        result.button = GetSysColor(COLOR_BTNFACE);
        result.hot = GetSysColor(COLOR_HIGHLIGHT);
        result.pressed = GetSysColor(COLOR_HIGHLIGHT);
        result.accent = result.focus;
        return result;
    }
    return result;
}

void set_dark_title(HWND window, bool dark) {
    apply_dialog_dark_frame(window, dark);
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
    } else if (unit == L"t" || unit == L"tb" || unit == L"tib") {
        multiplier = 1024ull * 1024ull * 1024ull * 1024ull;
    } else {
        return std::nullopt;
    }
    if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        return std::nullopt;
    }
    return value * multiplier;
}

std::wstring format_size_text(std::size_t bytes) {
    struct Unit {
        std::size_t bytes;
        const wchar_t* suffix;
    };
    constexpr std::array<Unit, 4> units{{
        {std::size_t{1} << 30, L"GiB"},
        {std::size_t{1} << 20, L"MiB"},
        {std::size_t{1} << 10, L"KiB"},
        {1, L"B"},
    }};
    for (const auto& unit : units) {
        if (bytes >= unit.bytes && bytes % unit.bytes == 0) {
            return std::to_wstring(bytes / unit.bytes) + L" " + unit.suffix;
        }
    }
    return std::to_wstring(bytes) + L" B";
}

std::optional<std::uint64_t> parse_integer_size_with_unit(
    std::wstring text, int unit) {
    text = trim_color_text(std::move(text));
    if (text.empty() || unit < 0 ||
        unit >= static_cast<int>(kVolumeUnitNames.size())) {
        return std::nullopt;
    }
    return parse_size_text(
        text + L" " + kVolumeUnitNames[static_cast<std::size_t>(unit)]);
}

bool valid_signing_secret_key(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) return false;
    return fs::file_size(path, error) == kSigningSecretKeySize && !error;
}

bool valid_https_url(std::wstring_view url) {
    if (url.empty() || url.size() > std::numeric_limits<DWORD>::max()) {
        return false;
    }
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    return WinHttpCrackUrl(url.data(), static_cast<DWORD>(url.size()), 0,
                           &components) != FALSE &&
           components.nScheme == INTERNET_SCHEME_HTTPS &&
           components.dwHostNameLength != 0;
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
        {L"Self-extracting archives", L"*.exe"}, {L"All files", L"*.*"}};
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

constexpr wchar_t kSettingsNavClass[] = L"AxiomSettingsNavigation";
constexpr wchar_t kSettingsViewportClass[] = L"AxiomSettingsViewport";
constexpr UINT kPageNavigationSetSelection = WM_APP + 41;
constexpr UINT kSettingsViewportSetMetrics = WM_APP + 81;
constexpr UINT kSettingsViewportGetOffset = WM_APP + 82;
constexpr UINT kSettingsViewportScrollBy = WM_APP + 83;
constexpr UINT kSettingsViewportOffsetChanged = WM_APP + 84;

struct SettingsViewportMetrics {
    int content_height = 0;
    int line_height = 0;
};

int page_navigation_count(HWND window) {
    return GetDlgCtrlID(window) == kSettingsTabs
        ? static_cast<int>(kSettingsTabNames.size())
        : static_cast<int>(kCreateTabNames.size());
}

const wchar_t* page_navigation_text(HWND window, int index) {
    if (GetDlgCtrlID(window) == kSettingsTabs) {
        return kSettingsTabNames[static_cast<std::size_t>(index)];
    }
    return kCreateTabNames[static_cast<std::size_t>(index)];
}

int page_navigation_command_base(HWND window) {
    return GetDlgCtrlID(window) == kSettingsTabs
        ? kSettingsTabBase : kCreateTabBase;
}

struct SettingsNavState {
    int selected = 0;
    int hot = -1;
    HFONT font = nullptr;
    bool tracking_mouse = false;
};

int scale_for_window(HWND window, int value) {
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)),
                  USER_DEFAULT_SCREEN_DPI);
}

int settings_nav_item_height(HWND window) {
    return scale_for_window(window, 34);
}

int settings_nav_index_at_position(HWND window, int y) {
    if (y < 0) return -1;
    const int index = y / std::max(1, settings_nav_item_height(window));
    const int count = page_navigation_count(window);
    return index >= 0 && index < count ? index : -1;
}

void select_settings_nav(HWND window, SettingsNavState& state, int selection,
                         bool notify_parent) {
    selection = std::clamp(selection, 0,
                           page_navigation_count(window) - 1);
    if (selection == state.selected && !notify_parent) return;
    state.selected = selection;
    InvalidateRect(window, nullptr, FALSE);
    if (notify_parent) {
        SendMessageW(GetParent(window), WM_COMMAND,
                     MAKEWPARAM(page_navigation_command_base(window) + selection,
                                BN_CLICKED),
                     reinterpret_cast<LPARAM>(window));
    }
}

LRESULT CALLBACK settings_nav_window_proc(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<SettingsNavState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = new SettingsNavState{};
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
        case kPageNavigationSetSelection:
            select_settings_nav(window, *state, static_cast<int>(wparam), false);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN: {
            int selection = state->selected;
            if (wparam == VK_UP) --selection;
            else if (wparam == VK_DOWN) ++selection;
            else if (wparam == VK_HOME) selection = 0;
            else if (wparam == VK_END) {
                selection = page_navigation_count(window) - 1;
            }
            else if (wparam == VK_PRIOR) selection -= 3;
            else if (wparam == VK_NEXT) selection += 3;
            else if (wparam == VK_SPACE || wparam == VK_RETURN) {
                select_settings_nav(window, *state, state->selected, true);
                return 0;
            } else {
                break;
            }
            select_settings_nav(window, *state, selection, true);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const int index = settings_nav_index_at_position(
                window, GET_Y_LPARAM(lparam));
            SetFocus(window);
            if (index >= 0) select_settings_nav(window, *state, index, true);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!state->tracking_mouse) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
                TrackMouseEvent(&tracking);
                state->tracking_mouse = true;
            }
            const int hot = settings_nav_index_at_position(
                window, GET_Y_LPARAM(lparam));
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

            const int count = page_navigation_count(window);
            const int item_height = settings_nav_item_height(window);
            const int text_pad_x = scale_for_window(window, 14);
            for (int index = 0; index < count; ++index) {
                RECT item{0, item_height * index,
                          client.right, item_height * (index + 1)};
                if (item.top >= client.bottom) break;
                const bool selected = index == state->selected;
                const bool hot = index == state->hot;
                const COLORREF fill = selected ? colors.focus
                    : hot ? colors.hot : colors.button;
                HBRUSH brush = CreateSolidBrush(fill);
                FillRect(memory, &item, brush);
                DeleteObject(brush);
                SetTextColor(memory, selected
                    ? colors.selection_text : colors.text);

                HBRUSH border = CreateSolidBrush(colors.border);
                FrameRect(memory, &item, border);
                DeleteObject(border);

                RECT text_rect = item;
                text_rect.left += text_pad_x;
                text_rect.right -= text_pad_x;
                DrawTextW(memory, page_navigation_text(window, index), -1,
                          &text_rect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                              DT_NOPREFIX | DT_END_ELLIPSIS);
            }

            RECT divider{client.right - 1, 0, client.right, client.bottom};
            HBRUSH divider_brush = CreateSolidBrush(colors.border);
            FillRect(memory, &divider, divider_brush);
            DeleteObject(divider_brush);

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

bool register_settings_nav_class(HINSTANCE instance) {
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = settings_nav_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_HAND);
    window_class.lpszClassName = kSettingsNavClass;
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

struct SettingsViewportState {
    int content_height = 0;
    int offset = 0;
    int line_height = 34;
    int wheel_remainder = 0;
    bool dragging = false;
    int drag_anchor_y = 0;
    int drag_anchor_offset = 0;
};

int settings_viewport_max_offset(HWND window,
                                 const SettingsViewportState& state) {
    RECT client{};
    GetClientRect(window, &client);
    return std::max(0, state.content_height -
                           static_cast<int>(client.bottom - client.top));
}

RECT settings_viewport_track_rect(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = scale_for_window(window, 12);
    return {std::max(0, static_cast<int>(client.right) - width),
            client.top, client.right, client.bottom};
}

RECT settings_viewport_thumb_rect(HWND window,
                                  const SettingsViewportState& state) {
    const RECT track = settings_viewport_track_rect(window);
    const int track_height = std::max(0, static_cast<int>(track.bottom - track.top));
    const int maximum = settings_viewport_max_offset(window, state);
    if (maximum <= 0 || track_height <= 0) return track;
    RECT client{};
    GetClientRect(window, &client);
    const int viewport_height =
        std::max(1, static_cast<int>(client.bottom - client.top));
    const int thumb_height = std::clamp(
        MulDiv(track_height, viewport_height,
               std::max(viewport_height, state.content_height)),
        scale_for_window(window, 30), track_height);
    const int travel = std::max(0, track_height - thumb_height);
    const int top = track.top +
        (maximum == 0 ? 0 : MulDiv(travel, state.offset, maximum));
    return {track.left + scale_for_window(window, 2), top,
            track.right - scale_for_window(window, 2), top + thumb_height};
}

void set_settings_viewport_offset(HWND window, SettingsViewportState& state,
                                  int requested) {
    const int offset = std::clamp(
        requested, 0, settings_viewport_max_offset(window, state));
    if (offset == state.offset) return;
    state.offset = offset;
    InvalidateRect(window, nullptr, FALSE);
    SendMessageW(GetParent(window), kSettingsViewportOffsetChanged,
                 static_cast<WPARAM>(offset), 0);
}

LRESULT CALLBACK settings_viewport_window_proc(HWND window, UINT message,
                                               WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<SettingsViewportState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = new SettingsViewportState{};
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);

    switch (message) {
        case kSettingsViewportSetMetrics: {
            const auto* metrics =
                reinterpret_cast<const SettingsViewportMetrics*>(lparam);
            if (metrics != nullptr) {
                state->content_height = std::max(0, metrics->content_height);
                state->line_height = std::max(1, metrics->line_height);
                state->offset = std::clamp(
                    state->offset, 0,
                    settings_viewport_max_offset(window, *state));
                InvalidateRect(window, nullptr, FALSE);
            }
            return state->offset;
        }
        case kSettingsViewportGetOffset:
            return state->offset;
        case kSettingsViewportScrollBy:
            set_settings_viewport_offset(
                window, *state, state->offset + static_cast<int>(wparam));
            return 0;
        case WM_SIZE:
            state->offset = std::clamp(
                state->offset, 0,
                settings_viewport_max_offset(window, *state));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: {
            state->wheel_remainder += GET_WHEEL_DELTA_WPARAM(wparam);
            const int steps = state->wheel_remainder / WHEEL_DELTA;
            state->wheel_remainder %= WHEEL_DELTA;
            if (steps != 0) {
                set_settings_viewport_offset(
                    window, *state,
                    state->offset - steps * state->line_height * 3);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_PRIOR || wparam == VK_NEXT) {
                RECT client{};
                GetClientRect(window, &client);
                const int direction = wparam == VK_PRIOR ? -1 : 1;
                set_settings_viewport_offset(
                    window, *state,
                    state->offset + direction *
                        std::max(state->line_height,
                                 static_cast<int>(client.bottom) -
                                     state->line_height));
                return 0;
            }
            break;
        case WM_LBUTTONDOWN: {
            if (settings_viewport_max_offset(window, *state) <= 0) break;
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const RECT track = settings_viewport_track_rect(window);
            if (!PtInRect(&track, point)) break;
            const RECT thumb = settings_viewport_thumb_rect(window, *state);
            if (PtInRect(&thumb, point)) {
                state->dragging = true;
                state->drag_anchor_y = point.y;
                state->drag_anchor_offset = state->offset;
                SetCapture(window);
            } else {
                RECT client{};
                GetClientRect(window, &client);
                const int direction = point.y < thumb.top ? -1 : 1;
                set_settings_viewport_offset(
                    window, *state,
                    state->offset + direction *
                        std::max(state->line_height,
                                 static_cast<int>(client.bottom) -
                                     state->line_height));
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (state->dragging && GetCapture() == window) {
                const RECT track = settings_viewport_track_rect(window);
                const RECT thumb = settings_viewport_thumb_rect(window, *state);
                const int travel = std::max(
                    1, static_cast<int>(track.bottom - track.top) -
                           static_cast<int>(thumb.bottom - thumb.top));
                const int maximum = settings_viewport_max_offset(window, *state);
                const int delta = GET_Y_LPARAM(lparam) - state->drag_anchor_y;
                set_settings_viewport_offset(
                    window, *state,
                    state->drag_anchor_offset + MulDiv(delta, maximum, travel));
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (state->dragging) {
                state->dragging = false;
                if (GetCapture() == window) ReleaseCapture();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            state->dragging = false;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            const Palette colors = make_palette();
            HBRUSH background = CreateSolidBrush(colors.window);
            FillRect(dc, &client, background);
            DeleteObject(background);
            if (settings_viewport_max_offset(window, *state) > 0) {
                const RECT track = settings_viewport_track_rect(window);
                const RECT thumb = settings_viewport_thumb_rect(window, *state);
                HBRUSH track_brush = CreateSolidBrush(
                    blend_color(colors.window, colors.border,
                                colors.dark ? 28 : 18));
                FillRect(dc, &track, track_brush);
                DeleteObject(track_brush);
                HBRUSH thumb_brush = CreateSolidBrush(
                    state->dragging ? colors.accent
                                    : blend_color(colors.border, colors.text, 28));
                FillRect(dc, &thumb, thumb_brush);
                DeleteObject(thumb_brush);
            }
            EndPaint(window, &paint);
            return 0;
        }
        case WM_COMMAND:
        case WM_DRAWITEM:
        case WM_MEASUREITEM:
        case WM_NOTIFY:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return SendMessageW(GetParent(window), message, wparam, lparam);
        case kTableActivateMessage:
        case kTableSelectionChangedMessage:
        case kTableParentMessage:
        case kTableSortMessage:
            return SendMessageW(GetParent(window), message, wparam, lparam);
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            delete state;
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool register_settings_viewport_class(HINSTANCE instance) {
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = settings_viewport_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kSettingsViewportClass;
    return RegisterClassExW(&window_class) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

struct HsvColor {
    double hue = 0.0;
    double saturation = 0.0;
    double value = 0.0;
};

HsvColor rgb_to_hsv(COLORREF color) {
    const double red = static_cast<double>(GetRValue(color)) / 255.0;
    const double green = static_cast<double>(GetGValue(color)) / 255.0;
    const double blue = static_cast<double>(GetBValue(color)) / 255.0;
    const double maximum = std::max({red, green, blue});
    const double minimum = std::min({red, green, blue});
    const double range = maximum - minimum;

    HsvColor result;
    result.value = maximum;
    result.saturation = maximum == 0.0 ? 0.0 : range / maximum;
    if (range == 0.0) {
        result.hue = 0.0;
    } else if (maximum == red) {
        result.hue = 60.0 * std::fmod((green - blue) / range, 6.0);
    } else if (maximum == green) {
        result.hue = 60.0 * (((blue - red) / range) + 2.0);
    } else {
        result.hue = 60.0 * (((red - green) / range) + 4.0);
    }
    if (result.hue < 0.0) result.hue += 360.0;
    return result;
}

COLORREF hsv_to_rgb(HsvColor color) {
    color.hue = std::fmod(color.hue, 360.0);
    if (color.hue < 0.0) color.hue += 360.0;
    color.saturation = std::clamp(color.saturation, 0.0, 1.0);
    color.value = std::clamp(color.value, 0.0, 1.0);

    const double chroma = color.value * color.saturation;
    const double hue_section = color.hue / 60.0;
    const double secondary =
        chroma * (1.0 - std::abs(std::fmod(hue_section, 2.0) - 1.0));
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    if (hue_section < 1.0) {
        red = chroma;
        green = secondary;
    } else if (hue_section < 2.0) {
        red = secondary;
        green = chroma;
    } else if (hue_section < 3.0) {
        green = chroma;
        blue = secondary;
    } else if (hue_section < 4.0) {
        green = secondary;
        blue = chroma;
    } else if (hue_section < 5.0) {
        red = secondary;
        blue = chroma;
    } else {
        red = chroma;
        blue = secondary;
    }
    const double offset = color.value - chroma;
    const auto channel = [offset](double value) {
        return static_cast<BYTE>(std::lround(
            std::clamp(value + offset, 0.0, 1.0) * 255.0));
    };
    return RGB(channel(red), channel(green), channel(blue));
}

class AccentColorPicker {
public:
    std::optional<COLORREF> show(HWND owner, COLORREF initial) {
        owner_ = owner;
        instance_ = reinterpret_cast<HINSTANCE>(
            GetWindowLongPtrW(owner, GWLP_HINSTANCE));
        dpi_ = GetDpiForWindow(owner);
        palette_ = make_palette();
        color_ = initial;
        hsv_ = rgb_to_hsv(initial);
        if (!register_class()) return std::nullopt;

        constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU |
                                WS_CLIPCHILDREN;
        constexpr DWORD extended_style = WS_EX_DLGMODALFRAME;
        const SIZE desired_size = dialog_window_size_for_client(
            580, 350, style, extended_style, dpi_);
        RECT owner_rect{};
        GetWindowRect(owner, &owner_rect);
        int width = desired_size.cx;
        int height = desired_size.cy;
        MONITORINFO monitor{sizeof(monitor)};
        int x = owner_rect.left +
            ((owner_rect.right - owner_rect.left) - width) / 2;
        int y = owner_rect.top +
            ((owner_rect.bottom - owner_rect.top) - height) / 2;
        if (GetMonitorInfoW(
                MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) {
            width = std::min(
                width,
                static_cast<int>(monitor.rcWork.right - monitor.rcWork.left) -
                    scale(24));
            height = std::min(
                height,
                static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) -
                    scale(24));
            x = owner_rect.left +
                ((owner_rect.right - owner_rect.left) - width) / 2;
            y = owner_rect.top +
                ((owner_rect.bottom - owner_rect.top) - height) / 2;
            x = std::clamp(
                x, static_cast<int>(monitor.rcWork.left),
                std::max(static_cast<int>(monitor.rcWork.left),
                         static_cast<int>(monitor.rcWork.right) - width));
            y = std::clamp(
                y, static_cast<int>(monitor.rcWork.top),
                std::max(static_cast<int>(monitor.rcWork.top),
                         static_cast<int>(monitor.rcWork.bottom) - height));
        }
        window_ = CreateWindowExW(
            extended_style, class_name(), L"Choose accent color", style,
            x, y, width, height, owner, nullptr, instance_, this);
        if (window_ == nullptr) return std::nullopt;

        owner_was_enabled_ = disable_dialog_owner(owner, window_);
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
        return accepted_ ? std::optional<COLORREF>(color_) : std::nullopt;
    }

private:
    static constexpr int kHexEdit = 2920;
    static constexpr int kAcceptColor = IDOK;
    static constexpr int kCancelColor = IDCANCEL;

    static const wchar_t* class_name() {
        return L"AxiomAccentColorPicker";
    }

    bool register_class() const {
        static ATOM atom = 0;
        if (atom != 0) return true;
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = &AccentColorPicker::window_proc;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = class_name();
        assign_axiom_window_class_icons(window_class, instance_);
        atom = RegisterClassExW(&window_class);
        return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    int scale(int value) const {
        return MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
    }

    RECT saturation_value_rect() const {
        return {scale(18), scale(64), scale(338), scale(254)};
    }

    RECT hue_rect() const {
        return {scale(354), scale(64), scale(382), scale(254)};
    }

    RECT preview_rect() const {
        return {scale(410), scale(64), scale(552), scale(132)};
    }

    void create_font() {
        if (font_ != nullptr) {
            DeleteObject(font_);
            font_ = nullptr;
        }
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if (!SystemParametersInfoForDpi(
                SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
            SystemParametersInfoW(
                SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        }
        font_ = CreateFontIndirectW(&metrics.lfMessageFont);
    }

    HWND create_control(const wchar_t* type, const wchar_t* text,
                        DWORD style, int id) {
        HWND result = CreateWindowExW(
            0, type, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
            0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_, nullptr);
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        apply_dialog_control_theme(result, palette_.dark);
        return result;
    }

    void create_controls() {
        create_font();
        window_brush_ = CreateSolidBrush(palette_.window);
        edit_brush_ = CreateSolidBrush(palette_.edit);
        tooltip_.create(window_, dpi_, palette_.dark);
        hex_edit_ = create_control(
            L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_CENTER |
                           ES_UPPERCASE | ES_AUTOHSCROLL,
            kHexEdit);
        SendMessageW(hex_edit_, EM_SETLIMITTEXT, 7, 0);
        SendMessageW(hex_edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(scale(6), scale(6)));
        apply_dialog_input_filter(
            hex_edit_, DialogInputFilter::hexadecimal_color, 7);
        accept_ = create_control(
            L"BUTTON", L"OK",
            WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW, kAcceptColor);
        cancel_ = create_control(
            L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, kCancelColor);
        add_dialog_tooltip(
            tooltip_, hex_edit_,
            L"Enter a hexadecimal RGB color as #RRGGBB, for example #FFB93C. The preview updates after a valid value is entered.");
        sync_hex_edit();
        layout();
    }

    void layout() const {
        if (window_ == nullptr || hex_edit_ == nullptr) return;
        RECT client{};
        GetClientRect(window_, &client);
        const int margin = scale(18);
        const int row = scale(30);
        const int button_width = scale(88);
        const int button_y = client.bottom - margin - row;
        MoveWindow(hex_edit_, scale(410), scale(168), scale(142), row, TRUE);
        MoveWindow(cancel_, client.right - margin - button_width, button_y,
                   button_width, row, TRUE);
        MoveWindow(accept_, client.right - margin - button_width * 2 - scale(8),
                   button_y, button_width, row, TRUE);
    }

    void sync_hex_edit() {
        if (hex_edit_ == nullptr) return;
        updating_edit_ = true;
        set_window_text(hex_edit_, color_to_hex(color_));
        updating_edit_ = false;
    }

    void set_color(COLORREF color, bool update_hsv) {
        color_ = color;
        if (update_hsv) hsv_ = rgb_to_hsv(color);
        sync_hex_edit();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void update_saturation_value(POINT point) {
        const RECT rect = saturation_value_rect();
        const int width = std::max(
            1, static_cast<int>(rect.right - rect.left - 1));
        const int height = std::max(
            1, static_cast<int>(rect.bottom - rect.top - 1));
        hsv_.saturation = std::clamp(
            static_cast<double>(point.x - rect.left) / width, 0.0, 1.0);
        hsv_.value = 1.0 - std::clamp(
            static_cast<double>(point.y - rect.top) / height, 0.0, 1.0);
        set_color(hsv_to_rgb(hsv_), false);
    }

    void update_hue(POINT point) {
        const RECT rect = hue_rect();
        const int height = std::max(
            1, static_cast<int>(rect.bottom - rect.top - 1));
        hsv_.hue = std::clamp(
            static_cast<double>(point.y - rect.top) / height, 0.0, 1.0) * 359.999;
        set_color(hsv_to_rgb(hsv_), false);
    }

    static std::uint32_t dib_pixel(COLORREF color) {
        return (static_cast<std::uint32_t>(GetRValue(color)) << 16) |
               (static_cast<std::uint32_t>(GetGValue(color)) << 8) |
               static_cast<std::uint32_t>(GetBValue(color));
    }

    template <typename PixelFunction>
    void draw_gradient(HDC target, const RECT& rect, PixelFunction pixel) const {
        const int width = std::max(
            1, static_cast<int>(rect.right - rect.left));
        const int height = std::max(
            1, static_cast<int>(rect.bottom - rect.top));
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(
            target, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr || bits == nullptr) {
            if (bitmap != nullptr) DeleteObject(bitmap);
            return;
        }
        auto* pixels = static_cast<std::uint32_t*>(bits);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                pixels[static_cast<std::size_t>(y) * width + x] =
                    dib_pixel(pixel(x, y, width, height));
            }
        }
        HDC memory = CreateCompatibleDC(target);
        HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
        BitBlt(target, rect.left, rect.top, width, height,
               memory, 0, 0, SRCCOPY);
        SelectObject(memory, old_bitmap);
        DeleteDC(memory);
        DeleteObject(bitmap);
    }

    void draw_picker(HDC target, const RECT& client) const {
        HDC memory = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(
            target, std::max(1, static_cast<int>(client.right)),
            std::max(1, static_cast<int>(client.bottom)));
        HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
        HBRUSH background = CreateSolidBrush(palette_.window);
        FillRect(memory, &client, background);
        DeleteObject(background);

        HGDIOBJ old_font = SelectObject(memory, font_);
        SetBkMode(memory, TRANSPARENT);
        SetTextColor(memory, palette_.text);
        RECT heading{scale(18), scale(14), client.right - scale(18), scale(40)};
        DrawTextW(memory, L"Choose the color used for selections, progress, and accents.",
                  -1, &heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT sv_label{scale(18), scale(41), scale(338), scale(64)};
        RECT hue_label{scale(350), scale(41), scale(386), scale(64)};
        RECT preview_label{scale(410), scale(41), scale(552), scale(64)};
        DrawTextW(memory, L"Saturation and brightness", -1, &sv_label,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        DrawTextW(memory, L"Hue", -1, &hue_label,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        DrawTextW(memory, L"Preview", -1, &preview_label,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        const RECT sv = saturation_value_rect();
        draw_gradient(memory, sv, [this](int x, int y, int width, int height) {
            HsvColor color = hsv_;
            color.saturation = width <= 1 ? 0.0
                : static_cast<double>(x) / (width - 1);
            color.value = height <= 1 ? 1.0
                : 1.0 - static_cast<double>(y) / (height - 1);
            return hsv_to_rgb(color);
        });
        const RECT hue = hue_rect();
        draw_gradient(memory, hue, [](int, int y, int, int height) {
            const double fraction = height <= 1 ? 0.0
                : static_cast<double>(y) / (height - 1);
            return hsv_to_rgb({fraction * 359.999, 1.0, 1.0});
        });

        HBRUSH border = CreateSolidBrush(palette_.border);
        FrameRect(memory, &sv, border);
        FrameRect(memory, &hue, border);
        const RECT preview = preview_rect();
        HBRUSH preview_brush = CreateSolidBrush(color_);
        FillRect(memory, &preview, preview_brush);
        DeleteObject(preview_brush);
        FrameRect(memory, &preview, border);
        DeleteObject(border);

        const int sv_width = std::max(
            1, static_cast<int>(sv.right - sv.left - 1));
        const int sv_height = std::max(
            1, static_cast<int>(sv.bottom - sv.top - 1));
        const int marker_x = sv.left +
            static_cast<int>(std::lround(hsv_.saturation * sv_width));
        const int marker_y = sv.top +
            static_cast<int>(std::lround((1.0 - hsv_.value) * sv_height));
        HPEN outer_pen = CreatePen(PS_SOLID, scale(3), RGB(0, 0, 0));
        HPEN inner_pen = CreatePen(PS_SOLID, scale(1), RGB(255, 255, 255));
        HGDIOBJ old_pen = SelectObject(memory, outer_pen);
        HGDIOBJ old_brush = SelectObject(memory, GetStockObject(NULL_BRUSH));
        const int radius = scale(6);
        Ellipse(memory, marker_x - radius, marker_y - radius,
                marker_x + radius + 1, marker_y + radius + 1);
        SelectObject(memory, inner_pen);
        Ellipse(memory, marker_x - radius, marker_y - radius,
                marker_x + radius + 1, marker_y + radius + 1);
        SelectObject(memory, old_brush);
        SelectObject(memory, old_pen);
        DeleteObject(inner_pen);
        DeleteObject(outer_pen);

        const int hue_height = std::max(
            1, static_cast<int>(hue.bottom - hue.top - 1));
        const int hue_y = hue.top +
            static_cast<int>(std::lround((hsv_.hue / 359.999) * hue_height));
        HPEN hue_outer = CreatePen(PS_SOLID, scale(3), RGB(0, 0, 0));
        HPEN hue_inner = CreatePen(PS_SOLID, scale(1), RGB(255, 255, 255));
        old_pen = SelectObject(memory, hue_outer);
        MoveToEx(memory, hue.left - scale(4), hue_y, nullptr);
        LineTo(memory, hue.right + scale(4), hue_y);
        SelectObject(memory, hue_inner);
        MoveToEx(memory, hue.left - scale(4), hue_y, nullptr);
        LineTo(memory, hue.right + scale(4), hue_y);
        SelectObject(memory, old_pen);
        DeleteObject(hue_inner);
        DeleteObject(hue_outer);

        RECT hex_label{scale(410), scale(142), scale(552), scale(166)};
        DrawTextW(memory, L"Hex color", -1, &hex_label,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT hint{scale(410), scale(208), scale(552), scale(261)};
        SetTextColor(memory, palette_.muted);
        DrawTextW(memory, L"Drag the palettes, use arrow keys, or type #RRGGBB.",
                  -1, &hint, DT_LEFT | DT_TOP | DT_WORDBREAK |
                                 DT_NOPREFIX | DT_EDITCONTROL);
        SelectObject(memory, old_font);

        BitBlt(target, 0, 0, client.right, client.bottom,
               memory, 0, 0, SRCCOPY);
        SelectObject(memory, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
    }

    void accept() {
        const auto parsed = color_from_hex(window_text(hex_edit_));
        if (!parsed) {
            MessageBeep(MB_ICONWARNING);
            SetFocus(hex_edit_);
            SendMessageW(hex_edit_, EM_SETSEL, 0, -1);
            return;
        }
        color_ = *parsed;
        accepted_ = true;
        destroy_modal_dialog(window_);
        window_ = nullptr;
    }

    void cancel() {
        accepted_ = false;
        destroy_modal_dialog(window_);
        window_ = nullptr;
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE:
                set_dark_title(window_, palette_.dark);
                apply_axiom_window_icons(window_, instance_);
                create_controls();
                return 0;
            case WM_GETDLGCODE:
                return DLGC_WANTARROWS;
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wparam);
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                apply_axiom_window_icons(window_, instance_);
                create_font();
                for (HWND control : {hex_edit_, accept_, cancel_}) {
                    if (control != nullptr) {
                        SendMessageW(control, WM_SETFONT,
                                     reinterpret_cast<WPARAM>(font_), TRUE);
                    }
                }
                tooltip_.update_dpi(dpi_);
                layout();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(window_, &paint);
                RECT client{};
                GetClientRect(window_, &client);
                draw_picker(dc, client);
                EndPaint(window_, &paint);
                return 0;
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
                draw_dialog_button(
                    *reinterpret_cast<DRAWITEMSTRUCT*>(lparam), palette_.dark);
                return TRUE;
            case WM_LBUTTONDOWN: {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                const RECT saturation_value = saturation_value_rect();
                const RECT hue = hue_rect();
                if (PtInRect(&saturation_value, point)) {
                    dragging_saturation_value_ = true;
                    SetCapture(window_);
                    SetFocus(window_);
                    update_saturation_value(point);
                    return 0;
                }
                if (PtInRect(&hue, point)) {
                    dragging_hue_ = true;
                    SetCapture(window_);
                    SetFocus(window_);
                    update_hue(point);
                    return 0;
                }
                break;
            }
            case WM_MOUSEMOVE:
                if (GetCapture() == window_) {
                    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                    if (dragging_saturation_value_) {
                        update_saturation_value(point);
                    } else if (dragging_hue_) {
                        update_hue(point);
                    }
                    return 0;
                }
                break;
            case WM_LBUTTONUP:
                if (GetCapture() == window_) ReleaseCapture();
                dragging_saturation_value_ = false;
                dragging_hue_ = false;
                return 0;
            case WM_CAPTURECHANGED:
                dragging_saturation_value_ = false;
                dragging_hue_ = false;
                return 0;
            case WM_KEYDOWN:
                if (wparam == VK_ESCAPE) {
                    cancel();
                    return 0;
                }
                if (wparam == VK_LEFT || wparam == VK_RIGHT ||
                    wparam == VK_UP || wparam == VK_DOWN) {
                    const bool adjust_hue = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    if (adjust_hue) {
                        const double direction =
                            (wparam == VK_LEFT || wparam == VK_UP) ? -1.0 : 1.0;
                        hsv_.hue = std::fmod(hsv_.hue + direction + 360.0, 360.0);
                    } else if (wparam == VK_LEFT || wparam == VK_RIGHT) {
                        const double direction = wparam == VK_LEFT ? -1.0 : 1.0;
                        hsv_.saturation = std::clamp(
                            hsv_.saturation + direction / 255.0, 0.0, 1.0);
                    } else {
                        const double direction = wparam == VK_UP ? 1.0 : -1.0;
                        hsv_.value = std::clamp(
                            hsv_.value + direction / 255.0, 0.0, 1.0);
                    }
                    set_color(hsv_to_rgb(hsv_), false);
                    return 0;
                }
                break;
            case WM_COMMAND: {
                const int id = LOWORD(wparam);
                if (id == kHexEdit && HIWORD(wparam) == EN_CHANGE &&
                    !updating_edit_) {
                    if (const auto parsed =
                            color_from_hex(window_text(hex_edit_))) {
                        color_ = *parsed;
                        hsv_ = rgb_to_hsv(color_);
                        InvalidateRect(window_, nullptr, FALSE);
                    }
                    return 0;
                }
                if (id == kAcceptColor) {
                    accept();
                    return 0;
                }
                if (id == kCancelColor) {
                    cancel();
                    return 0;
                }
                break;
            }
            case WM_CLOSE:
                cancel();
                return 0;
            case WM_NCDESTROY:
                SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
                if (font_ != nullptr) {
                    DeleteObject(font_);
                    font_ = nullptr;
                }
                if (window_brush_ != nullptr) {
                    DeleteObject(window_brush_);
                    window_brush_ = nullptr;
                }
                if (edit_brush_ != nullptr) {
                    DeleteObject(edit_brush_);
                    edit_brush_ = nullptr;
                }
                return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        AccentColorPicker* self = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<AccentColorPicker*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(
                window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<AccentColorPicker*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return self != nullptr
            ? self->handle(message, wparam, lparam)
            : DefWindowProcW(window, message, wparam, lparam);
    }

    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    bool owner_was_enabled_ = false;
    bool accepted_ = false;
    bool updating_edit_ = false;
    bool dragging_saturation_value_ = false;
    bool dragging_hue_ = false;
    Palette palette_;
    COLORREF color_ = RGB(255, 185, 60);
    HsvColor hsv_;
    HFONT font_ = nullptr;
    HBRUSH window_brush_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    TooltipManager tooltip_;
    HWND hex_edit_ = nullptr;
    HWND accept_ = nullptr;
    HWND cancel_ = nullptr;
};

std::optional<COLORREF> choose_accent_color(HWND owner, COLORREF initial) {
    AccentColorPicker picker;
    return picker.show(owner, initial);
}

class FileColumnsDialog {
public:
    explicit FileColumnsDialog(ThemePalette theme) : theme_(theme) {}

    ~FileColumnsDialog() {
        if (font_ != nullptr) DeleteObject(font_);
        if (window_brush_ != nullptr) DeleteObject(window_brush_);
        if (edit_brush_ != nullptr) DeleteObject(edit_brush_);
    }

    bool show(HWND owner, std::vector<FileListColumnSetting>& columns) {
        owner_ = owner;
        instance_ = reinterpret_cast<HINSTANCE>(
            GetWindowLongPtrW(owner, GWLP_HINSTANCE));
        dpi_ = GetDpiForWindow(owner);
        columns_ = normalize_file_list_columns(columns);
        if (!register_class()) return false;

        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU |
                            WS_THICKFRAME | WS_CLIPCHILDREN;
        const DWORD ex_style = WS_EX_DLGMODALFRAME;
        const SIZE size = dialog_window_size_for_client(
            760, 520, style, ex_style, dpi_);
        RECT owner_rect{};
        GetWindowRect(owner, &owner_rect);
        const int x = owner_rect.left +
            ((owner_rect.right - owner_rect.left) - size.cx) / 2;
        const int y = owner_rect.top +
            ((owner_rect.bottom - owner_rect.top) - size.cy) / 2;
        window_ = CreateWindowExW(
            ex_style, class_name(), L"Customize file-list columns", style,
            x, y, size.cx, size.cy, owner, nullptr, instance_, this);
        if (window_ == nullptr) return false;
        const int show_command =
            restore_named_window_placement(window_, owner, L"FileListColumns");
        owner_was_enabled_ = disable_dialog_owner(owner, window_);
        ShowWindow(window_, show_command);
        UpdateWindow(window_);

        MSG message{};
        while (IsWindow(window_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_SPACE &&
                message.hwnd == table_.hwnd()) {
                toggle_selected();
                continue;
            }
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (accepted_) columns = std::move(columns_);
        return accepted_;
    }

private:
    enum : int {
        kTable = 2860,
        kToggle = 2861,
        kMoveUp = 2862,
        kMoveDown = 2863,
        kDefaults = 2864,
        kAccept = IDOK,
        kCancel = IDCANCEL,
    };

    static const wchar_t* class_name() {
        return L"AxiomFileColumnsDialog";
    }

    bool register_class() const {
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        if (GetClassInfoExW(instance_, class_name(), &existing)) return true;
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.hInstance = instance_;
        window_class.lpfnWndProc = &FileColumnsDialog::window_proc;
        window_class.lpszClassName = class_name();
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        assign_axiom_window_class_icons(window_class, instance_);
        return RegisterClassExW(&window_class) != 0;
    }

    int scale(int value) const {
        return MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
    }

    HWND button(const wchar_t* text, int id) {
        HWND control = CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        apply_dialog_control_theme(control, palette_.dark);
        return control;
    }

    void rebuild_font() {
        if (font_ != nullptr) DeleteObject(font_);
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if (!SystemParametersInfoForDpi(
                SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
            SystemParametersInfoW(
                SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        }
        font_ = CreateFontIndirectW(&metrics.lfMessageFont);
        EnumChildWindows(window_, [](HWND child, LPARAM font) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(font_));
        table_.set_font(font_);
        table_.set_dpi(dpi_);
    }

    void create_controls() {
        palette_ = make_palette();
        window_brush_ = CreateSolidBrush(palette_.window);
        edit_brush_ = CreateSolidBrush(palette_.edit);
        set_dark_title(window_, palette_.dark);
        apply_axiom_window_icons(window_, instance_);
        rebuild_font();
        tooltip_.create(window_, dpi_, palette_.dark);

        description_ = CreateWindowExW(
            0, L"STATIC",
            L"Select fields to show and arrange their left-to-right order. "
            L"You can also drag headers directly in the main file list.",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_EDITCONTROL,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        SendMessageW(description_, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font_), TRUE);

        table_.create(window_, instance_, kTable);
        table_.set_font(font_);
        table_.set_dpi(dpi_);
        table_.set_theme(theme_);
        table_.set_options({true, false, true, false});
        table_.set_columns({
            {L"Column", 330},
            {L"Visibility", 130},
            {L"Current width", 130},
        });
        table_.set_sort_indicator(-1, true);
        refresh_rows(0);

        toggle_ = button(L"Show / hide", kToggle);
        move_up_ = button(L"Move up", kMoveUp);
        move_down_ = button(L"Move down", kMoveDown);
        defaults_ = button(L"Defaults", kDefaults);
        accept_ = button(L"OK", kAccept);
        cancel_ = button(L"Cancel", kCancel);
        add_dialog_tooltip(
            tooltip_, table_.hwnd(),
            L"Select a column to change. Double-click or press Space to show or hide it; the Name column is always visible and stays first.");
        add_dialog_tooltip(
            tooltip_, toggle_,
            L"Show or hide the selected file-list column. The Name column cannot be hidden.");
        add_dialog_tooltip(
            tooltip_, move_up_,
            L"Move the selected column one position to the left. The Name column stays first.");
        add_dialog_tooltip(
            tooltip_, move_down_,
            L"Move the selected column one position to the right.");
        add_dialog_tooltip(
            tooltip_, defaults_,
            L"Restore Axiom's default visible columns, widths, and order in this editor.");
        layout();
    }

    void refresh_rows(int selected_row = -1) {
        columns_ = normalize_file_list_columns(columns_);
        const int focused =
            selected_row >= 0 ? selected_row : table_.focused_index();
        const int scroll_y = table_.vertical_scroll_position();
        std::vector<std::vector<std::wstring>> rows;
        rows.reserve(columns_.size());
        for (const auto& column : columns_) {
            const auto* info = file_list_column_info(column.id);
            if (info == nullptr) continue;
            rows.push_back({
                info->title,
                column.visible ? L"Shown" : L"Hidden",
                std::to_wstring(column.width) + L" px",
            });
        }
        table_.set_rows(std::move(rows), {}, nullptr);
        if (focused >= 0 && focused < static_cast<int>(columns_.size())) {
            table_.set_selection_and_scroll({focused}, focused, 0, scroll_y);
        }
    }

    void toggle_selected() {
        const int row = table_.focused_index();
        if (row < 0 || row >= static_cast<int>(columns_.size())) return;
        auto& column = columns_[static_cast<std::size_t>(row)];
        if (column.id == FileListColumnId::name) return;
        column.visible = !column.visible;
        refresh_rows(row);
    }

    void move_selected(int direction) {
        const int row = table_.focused_index();
        const int target = row + direction;
        if (row <= 0 || target <= 0 ||
            row >= static_cast<int>(columns_.size()) ||
            target >= static_cast<int>(columns_.size())) {
            return;
        }
        std::swap(columns_[static_cast<std::size_t>(row)],
                  columns_[static_cast<std::size_t>(target)]);
        refresh_rows(target);
    }

    void layout() {
        if (window_ == nullptr) return;
        RECT client{};
        GetClientRect(window_, &client);
        const int margin = scale(16);
        const int gap = scale(10);
        const int button_height = scale(30);
        const int button_width = scale(86);
        const int description_height = scale(42);
        const int footer_y = client.bottom - margin - button_height;
        const int description_width =
            std::max(1, static_cast<int>(client.right) - margin * 2);
        const int table_y = margin + description_height + gap;
        const int table_height =
            std::max(scale(160), footer_y - gap - table_y);

        const int toggle_width = scale(104);
        const int move_up_width = scale(82);
        const int move_down_width = scale(92);
        const int defaults_width = scale(104);
        int left = margin;
        const int toggle_x = left;
        left += toggle_width + gap;
        const int move_up_x = left;
        left += move_up_width + gap;
        const int move_down_x = left;
        left += move_down_width + gap;
        const int defaults_x = left;

        int right = client.right - margin;
        const int cancel_x = right - button_width;
        right = cancel_x - gap;
        const int accept_x = right - button_width;

        // Batch the resize so the footer never exposes intermediate,
        // overlapping button positions while the user drags the frame.
        HDWP positions = BeginDeferWindowPos(8);
        const auto place = [&positions](
                               HWND control, int x, int y,
                               int width, int height) {
            if (positions == nullptr) return;
            positions = DeferWindowPos(
                positions, control, nullptr, x, y, width, height,
                SWP_NOACTIVATE | SWP_NOZORDER);
        };
        place(description_, margin, margin, description_width,
              description_height);
        place(table_.hwnd(), margin, table_y, description_width, table_height);
        place(toggle_, toggle_x, footer_y, toggle_width, button_height);
        place(move_up_, move_up_x, footer_y, move_up_width, button_height);
        place(move_down_, move_down_x, footer_y, move_down_width, button_height);
        place(defaults_, defaults_x, footer_y, defaults_width, button_height);
        place(accept_, accept_x, footer_y, button_width, button_height);
        place(cancel_, cancel_x, footer_y, button_width, button_height);
        if (positions != nullptr) EndDeferWindowPos(positions);
    }

    void close(bool accepted) {
        accepted_ = accepted;
        save_named_window_placement(L"FileListColumns", window_);
        HWND owner = owner_;
        const bool owner_was_enabled = owner_was_enabled_;
        owner_was_enabled_ = false;
        if (window_ != nullptr && IsWindow(window_)) {
            destroy_modal_dialog(window_);
        }
        restore_dialog_owner(owner, owner_was_enabled);
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE:
                create_controls();
                return 0;
            case WM_SIZE:
                layout();
                return 0;
            case WM_GETMINMAXINFO: {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
                const DWORD style = static_cast<DWORD>(
                    GetWindowLongPtrW(window_, GWL_STYLE));
                const DWORD ex_style = static_cast<DWORD>(
                    GetWindowLongPtrW(window_, GWL_EXSTYLE));
                const SIZE minimum = dialog_window_size_for_client(
                    700, 430, style, ex_style, dpi_);
                limits->ptMinTrackSize = {minimum.cx, minimum.cy};
                return 0;
            }
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wparam);
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                rebuild_font();
                apply_axiom_window_icons(window_, instance_);
                tooltip_.update_dpi(dpi_);
                layout();
                return 0;
            }
            case WM_ERASEBKGND: {
                RECT client{};
                GetClientRect(window_, &client);
                FillRect(reinterpret_cast<HDC>(wparam), &client, window_brush_);
                return 1;
            }
            case WM_CTLCOLORSTATIC:
                SetBkColor(reinterpret_cast<HDC>(wparam), palette_.window);
                SetTextColor(reinterpret_cast<HDC>(wparam), palette_.text);
                return reinterpret_cast<LRESULT>(window_brush_);
            case WM_DRAWITEM:
                draw_dialog_button(
                    *reinterpret_cast<DRAWITEMSTRUCT*>(lparam), palette_.dark);
                return TRUE;
            case kTableActivateMessage:
                toggle_selected();
                return 0;
            case kTableSelectionChangedMessage:
            case kTableSortMessage:
            case kTableParentMessage:
                return 0;
            case WM_COMMAND:
                switch (LOWORD(wparam)) {
                    case kToggle:
                        toggle_selected();
                        return 0;
                    case kMoveUp:
                        move_selected(-1);
                        return 0;
                    case kMoveDown:
                        move_selected(1);
                        return 0;
                    case kDefaults:
                        columns_ = default_file_list_columns();
                        refresh_rows(0);
                        return 0;
                    case kAccept:
                        close(true);
                        return 0;
                    case kCancel:
                        close(false);
                        return 0;
                }
                break;
            case WM_CLOSE:
                close(false);
                return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(
        HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        FileColumnsDialog* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<FileColumnsDialog*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(
                window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<FileColumnsDialog*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return self != nullptr
            ? self->handle(message, wparam, lparam)
            : DefWindowProcW(window, message, wparam, lparam);
    }

    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    bool owner_was_enabled_ = false;
    bool accepted_ = false;
    Palette palette_;
    ThemePalette theme_;
    HBRUSH window_brush_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    HFONT font_ = nullptr;
    TooltipManager tooltip_;
    HWND description_ = nullptr;
    DarkTableView table_;
    HWND toggle_ = nullptr;
    HWND move_up_ = nullptr;
    HWND move_down_ = nullptr;
    HWND defaults_ = nullptr;
    HWND accept_ = nullptr;
    HWND cancel_ = nullptr;
    std::vector<FileListColumnSetting> columns_;
};

enum class DialogMode { create_archive, extract_archive, settings };

class OptionsDialog {
public:
    explicit OptionsDialog(DialogMode mode) : mode_(mode) {}
    ~OptionsDialog() {
        stop_compression_curve_worker();
        if (font_) DeleteObject(font_);
        if (window_brush_) DeleteObject(window_brush_);
        if (edit_brush_) DeleteObject(edit_brush_);
        if (toolbar_image_list_ != nullptr) ImageList_Destroy(toolbar_image_list_);
    }

    bool show(HWND owner) {
        owner_ = owner;
        instance_ = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
        dpi_ = GetDpiForWindow(owner);
        // Establish the non-client appearance before USER32 creates the frame.
        // Applying it later in WM_CREATE permits one compositor frame with the
        // default light border on a dark desktop.
        palette_ = make_palette();
        if (!register_class()) return false;
        const DWORD window_style = WS_POPUP | WS_CAPTION | WS_SYSMENU |
            WS_CLIPCHILDREN |
            (mode_ == DialogMode::create_archive || mode_ == DialogMode::settings
                 ? WS_THICKFRAME | WS_MAXIMIZEBOX : 0);
        // Thick-frame dialogs already have a complete non-client frame.
        // WS_EX_DLGMODALFRAME adds a second legacy edge which can briefly paint
        // in the system light color before DWM applies the requested theme.
        const DWORD extended_style =
            WS_EX_LAYERED |
            (mode_ == DialogMode::extract_archive ? WS_EX_DLGMODALFRAME : 0);
        const SIZE initial_size = dialog_window_size_for_client(
            mode_ == DialogMode::create_archive
                ? kCreateInitialClientWidth
                : mode_ == DialogMode::settings ? 920 : 540,
            mode_ == DialogMode::create_archive
                ? kCreateInitialClientHeight
                : mode_ == DialogMode::settings ? 650 : 290,
            window_style, extended_style, dpi_);
        int width = initial_size.cx;
        int height = initial_size.cy;
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
        window_ = CreateWindowExW(extended_style, class_name(), title,
                                  window_style,
                                  x, y, width, height, owner, nullptr, instance_, this);
        if (!window_) return false;
        // Unlike DWMWA_CLOAK, layered alpha is applied synchronously by USER32
        // before a window can become visible.  Keep the complete window,
        // including its DWM-owned non-client frame, transparent until its
        // first dark frame has been realized.
        layered_reveal_pending_ =
            SetLayeredWindowAttributes(window_, 0, 0, LWA_ALPHA) != FALSE;
        const int show_command =
            restore_named_window_placement(window_, owner, layout_name());
        enforce_minimum_window_size();
        owner_was_enabled_ = disable_dialog_owner(owner, window_);

        ShowWindow(window_, show_command);
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                         RDW_ALLCHILDREN | RDW_UPDATENOW);
        if (layered_reveal_pending_) {
            // Showing a thick-frame window queues additional non-client work.
            // Let the nested modal loop process that work before revealing the
            // surface.  A one-shot timer runs only after higher-priority
            // messages and prevents the otherwise visible light frame.
            DwmFlush();
            SetTimer(window_, kCompositorRevealTimer, 1, nullptr);
        }
        MSG message{};
        while (IsWindow(window_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (mode_ == DialogMode::settings &&
                toolbar_list_.hwnd() != nullptr &&
                message.message == WM_KEYDOWN &&
                message.wParam == VK_SPACE &&
                (message.hwnd == toolbar_list_.hwnd() ||
                 IsChild(toolbar_list_.hwnd(), message.hwnd))) {
                toggle_toolbar_settings_row(toolbar_list_.focused_index());
                continue;
            }
            const HWND page_viewport = mode_ == DialogMode::settings
                ? settings_viewport_
                : mode_ == DialogMode::create_archive
                    ? create_viewport_
                    : nullptr;
            if (page_viewport != nullptr &&
                (message.hwnd == page_viewport ||
                 IsChild(page_viewport, message.hwnd))) {
                wchar_t class_name[32]{};
                GetClassNameW(message.hwnd, class_name,
                              static_cast<int>(std::size(class_name)));
                const bool combo =
                    lstrcmpiW(class_name, L"ComboBox") == 0 ||
                    lstrcmpiW(class_name, L"ComboLBox") == 0;
                const bool table =
                    message.hwnd == toolbar_list_.hwnd() ||
                    (toolbar_list_.hwnd() != nullptr &&
                     IsChild(toolbar_list_.hwnd(), message.hwnd));
                if (message.message == WM_MOUSEWHEEL && !combo && !table) {
                    SendMessageW(page_viewport, WM_MOUSEWHEEL,
                                 message.wParam, message.lParam);
                    continue;
                }
                if (message.message == WM_KEYDOWN && !combo && !table &&
                    (message.wParam == VK_PRIOR ||
                     message.wParam == VK_NEXT)) {
                    SendMessageW(page_viewport, WM_KEYDOWN,
                                 message.wParam, message.lParam);
                    continue;
                }
            }
            const bool handled = IsDialogMessageW(window_, &message) != FALSE;
            if (page_viewport != nullptr &&
                message.message == WM_KEYDOWN &&
                message.wParam == VK_TAB) {
                ensure_viewport_focus_visible(page_viewport, GetFocus());
            }
            if (!handled) {
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
    std::vector<fs::path> estimate_inputs;
    fs::path archive_path;

private:
    static const wchar_t* class_name() { return L"AxiomDarkOptionsDialog"; }
    static constexpr UINT_PTR kCompositorRevealTimer = 0xA710;
    static constexpr int kCreateInitialClientWidth = 1000;
    static constexpr int kCreateInitialClientHeight = 650;
    static constexpr int kCreateMinimumClientWidth = 900;
    static constexpr int kCreateMinimumClientHeight = 600;

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
            assign_axiom_window_class_icons(wc, instance_);
            atom = RegisterClassExW(&wc);
            dialog_registered = atom != 0 ||
                                GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        return dialog_registered && register_settings_nav_class(instance_) &&
               register_settings_viewport_class(instance_);
    }

    int scale(int value) const {
        return MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
    }

    SIZE minimum_track_size() const {
        const DWORD style = static_cast<DWORD>(
            GetWindowLongPtrW(window_, GWL_STYLE));
        const DWORD ex_style = static_cast<DWORD>(
            GetWindowLongPtrW(window_, GWL_EXSTYLE));
        SIZE minimum = dialog_window_size_for_client(
            mode_ == DialogMode::settings
                ? 780 : kCreateMinimumClientWidth,
            mode_ == DialogMode::settings
                ? 520 : kCreateMinimumClientHeight,
            style, ex_style, dpi_);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(
                MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                &monitor)) {
            minimum.cx = std::min(
                minimum.cx, monitor.rcWork.right - monitor.rcWork.left);
            minimum.cy = std::min(
                minimum.cy, monitor.rcWork.bottom - monitor.rcWork.top);
        }
        return minimum;
    }

    void enforce_minimum_window_size() {
        if (mode_ != DialogMode::create_archive) return;
        RECT bounds{};
        if (!GetWindowRect(window_, &bounds)) return;
        const SIZE minimum = minimum_track_size();
        const int width = std::max(
            static_cast<int>(bounds.right - bounds.left),
            static_cast<int>(minimum.cx));
        const int height = std::max(
            static_cast<int>(bounds.bottom - bounds.top),
            static_cast<int>(minimum.cy));
        if (width == bounds.right - bounds.left &&
            height == bounds.bottom - bounds.top) {
            return;
        }

        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(
                MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                &monitor)) {
            SetWindowPos(
                window_, nullptr, 0, 0, width, height,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            return;
        }
        const int x = std::clamp(
            static_cast<int>(bounds.left),
            static_cast<int>(monitor.rcWork.left),
            std::max(
                static_cast<int>(monitor.rcWork.left),
                static_cast<int>(monitor.rcWork.right) - width));
        const int y = std::clamp(
            static_cast<int>(bounds.top),
            static_cast<int>(monitor.rcWork.top),
            std::max(
                static_cast<int>(monitor.rcWork.top),
                static_cast<int>(monitor.rcWork.bottom) - height));
        SetWindowPos(
            window_, nullptr, x, y, width, height,
            SWP_NOZORDER | SWP_NOACTIVATE);
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
        const auto hardware_threads = static_cast<unsigned int>(
            axiom::core::logical_processor_count());
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
        if (create_viewport_ != nullptr) {
            SetParent(result, create_viewport_);
        }
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
        const auto hardware_threads = static_cast<unsigned>(
            axiom::core::logical_processor_count());
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
        if (settings_viewport_ != nullptr) {
            SetParent(result, settings_viewport_);
        }
        settings_controls_.push_back({result, page, x, y, width, height, wrapped});
        if (page != settings_page_) {
            ShowWindow(result, SW_HIDE);
        }
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
        const auto hardware_threads = static_cast<unsigned>(
            axiom::core::logical_processor_count());
        for (unsigned index = 1; index <= hardware_threads; ++index) {
            const std::wstring value = std::to_wstring(index);
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
        }
        return combo;
    }

    ThemePalette toolbar_table_theme() const {
        return make_theme(application_options.theme_mode,
                          application_options.accent_color_mode,
                          application_options.custom_accent_color);
    }

    HBITMAP render_toolbar_settings_icon(ToolbarIcon icon, int size) const {
        if (size <= 0) return nullptr;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size;
        info.bmiHeader.biHeight = -size;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                          &pixels, nullptr, 0);
        if (bitmap == nullptr || pixels == nullptr) {
            if (bitmap != nullptr) DeleteObject(bitmap);
            return nullptr;
        }

        HDC screen = GetDC(window_);
        HDC memory_dc = CreateCompatibleDC(screen);
        if (memory_dc == nullptr) {
            if (screen != nullptr) ReleaseDC(window_, screen);
            DeleteObject(bitmap);
            return nullptr;
        }
        HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
        RECT bounds{0, 0, size, size};
        HBRUSH background = CreateSolidBrush(palette_.edit);
        FillRect(memory_dc, &bounds, background);
        DeleteObject(background);
        const int icon_mode = std::clamp(application_options.toolbar_icon_style, 0, 2);
        const COLORREF color = icon_mode == 2 ? palette_.accent : palette_.text;
        const ToolbarIconStyle style = icon_mode == 1
            ? ToolbarIconStyle::colorful
            : ToolbarIconStyle::monochrome;
        draw_toolbar_icon(memory_dc, icon, bounds, color, dpi_, 18, style);
        SelectObject(memory_dc, old_bitmap);
        DeleteDC(memory_dc);
        if (screen != nullptr) ReleaseDC(window_, screen);
        return bitmap;
    }

    void rebuild_toolbar_settings_image_list() {
        if (toolbar_image_list_ != nullptr) {
            ImageList_Destroy(toolbar_image_list_);
            toolbar_image_list_ = nullptr;
        }
        const int icon_size = scale(18);
        toolbar_image_list_ = ImageList_Create(
            icon_size, icon_size, ILC_COLOR32 | ILC_MASK,
            static_cast<int>(kToolbarCommandCatalog.size()), 0);
        if (toolbar_image_list_ == nullptr) return;
        ImageList_SetBkColor(toolbar_image_list_, CLR_NONE);
        for (const ToolbarCommandInfo& command : kToolbarCommandCatalog) {
            HBITMAP bitmap = render_toolbar_settings_icon(command.icon, icon_size);
            if (bitmap == nullptr) {
                ImageList_Add(toolbar_image_list_, nullptr, nullptr);
                continue;
            }
            ImageList_AddMasked(toolbar_image_list_, bitmap, palette_.edit);
            DeleteObject(bitmap);
        }
    }

    bool toolbar_command_enabled(std::wstring_view command_id) const {
        const auto commands =
            normalize_toolbar_commands(application_options.toolbar_commands);
        return std::any_of(commands.begin(), commands.end(),
                           [&](const std::wstring& command) {
                               return command == command_id;
                           });
    }

    void set_toolbar_command_enabled(int row, bool enabled) {
        if (row < 0 || row >= static_cast<int>(kToolbarCommandCatalog.size())) return;
        const wchar_t* command_id =
            kToolbarCommandCatalog[static_cast<std::size_t>(row)].id;
        auto commands =
            normalize_toolbar_commands(application_options.toolbar_commands);
        commands.erase(std::remove_if(commands.begin(), commands.end(),
                                      [&](const std::wstring& command) {
                                          return command == command_id;
                                      }),
                       commands.end());
        if (enabled) {
            commands.emplace_back(command_id);
        }
        application_options.toolbar_commands = normalize_toolbar_commands(commands);
    }

    void refresh_toolbar_settings_list(int selected_row = -1) {
        if (toolbar_list_.hwnd() == nullptr) return;
        const int focused = selected_row >= 0 ? selected_row : toolbar_list_.focused_index();
        const int scroll_y = toolbar_list_.vertical_scroll_position();
        const int scroll_x = toolbar_list_.horizontal_scroll_position();
        std::vector<std::vector<std::wstring>> rows;
        std::vector<int> icons;
        rows.reserve(kToolbarCommandCatalog.size());
        icons.reserve(kToolbarCommandCatalog.size());
        for (std::size_t index = 0; index < kToolbarCommandCatalog.size(); ++index) {
            const ToolbarCommandInfo& command = kToolbarCommandCatalog[index];
            const bool enabled = toolbar_command_enabled(command.id);
            rows.push_back({
                L"",
                command.label,
                command.button_text,
                enabled ? L"Enabled" : L"Hidden",
            });
            icons.push_back(static_cast<int>(index));
        }
        toolbar_list_.set_rows(std::move(rows), std::move(icons), toolbar_image_list_);
        if (focused >= 0 && focused < static_cast<int>(kToolbarCommandCatalog.size())) {
            toolbar_list_.set_selection_and_scroll({focused}, focused, scroll_x, scroll_y);
        }
        sync_toolbar_status_combo();
    }

    void create_toolbar_settings_list(int page, int x, int y, int width, int height) {
        toolbar_list_.create(settings_viewport_, instance_, kToolbarList);
        toolbar_list_.set_font(font_);
        toolbar_list_.set_dpi(dpi_);
        toolbar_list_.set_theme(toolbar_table_theme());
        toolbar_list_.set_options({
            true,
            true,
            true,
            false,
        });
        toolbar_list_.set_columns({
            {L"Icon", 58},
            {L"Command", 300},
            {L"Button text", 170},
            {L"Status", 110},
        });
        rebuild_toolbar_settings_image_list();
        refresh_toolbar_settings_list();
        settings_controls_.push_back({toolbar_list_.hwnd(), page, x, y, width, height, false});
        add_dialog_tooltip(
            tooltip_, toolbar_list_.hwnd(),
            L"Select a command to show or hide it on the main toolbar. Double-click a row to toggle its status.");

        toolbar_status_combo_ = control(
            L"COMBOBOX", L"",
            WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | CBS_NOINTEGRALHEIGHT,
            kToolbarStatusCombo);
        SetParent(toolbar_status_combo_, settings_viewport_);
        SendMessageW(toolbar_status_combo_, CB_SETITEMHEIGHT, 0, scale(24));
        SendMessageW(toolbar_status_combo_, CB_SETITEMHEIGHT,
                     static_cast<WPARAM>(-1), scale(24));
        for (const wchar_t* item : kToolbarStatusNames) {
            SendMessageW(toolbar_status_combo_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(item));
        }
        SendMessageW(toolbar_status_combo_, CB_SETMINVISIBLE,
                     static_cast<WPARAM>(kToolbarStatusNames.size()), 0);
        ShowWindow(toolbar_status_combo_, SW_HIDE);
        add_dialog_tooltip(
            tooltip_, toolbar_status_combo_,
            L"Choose whether the selected command is shown on or hidden from the main toolbar.");
    }

    void ensure_toolbar_settings_list() {
        if (toolbar_list_.hwnd() != nullptr) return;
        // Icon rasterization and table population are the most expensive part
        // of settings construction. Defer them until the Toolbar page is
        // actually requested so opening Settings is independent of toolbar
        // catalog size and does not initialize GDI+ on the first frame.
        create_toolbar_settings_list(9, 0, 110, 2000, 420);
    }

    void toggle_toolbar_settings_row(int row) {
        if (row < 0 || row >= static_cast<int>(kToolbarCommandCatalog.size())) return;
        const ToolbarCommandInfo& command = kToolbarCommandCatalog[static_cast<std::size_t>(row)];
        set_toolbar_command_enabled(row, !toolbar_command_enabled(command.id));
        refresh_toolbar_settings_list(row);
    }

    void sync_toolbar_status_combo() {
        if (toolbar_status_combo_ == nullptr) return;
        if (settings_page_ != 9 || toolbar_list_.hwnd() == nullptr) {
            ShowWindow(toolbar_status_combo_, SW_HIDE);
            return;
        }
        const int row = toolbar_list_.focused_index();
        if (row < 0 || row >= static_cast<int>(kToolbarCommandCatalog.size())) {
            ShowWindow(toolbar_status_combo_, SW_HIDE);
            return;
        }
        const auto rect = toolbar_list_.cell_rect(row, 3);
        if (!rect || rect->right <= rect->left || rect->bottom <= rect->top) {
            ShowWindow(toolbar_status_combo_, SW_HIDE);
            return;
        }
        const bool enabled =
            toolbar_command_enabled(kToolbarCommandCatalog[static_cast<std::size_t>(row)].id);
        SendMessageW(toolbar_status_combo_, CB_SETCURSEL, enabled ? 0 : 1, 0);
        MoveWindow(toolbar_status_combo_,
                   rect->left + scale(3), rect->top + scale(1),
                   std::max(scale(86), static_cast<int>(rect->right - rect->left) - scale(6)),
                   scale(120), TRUE);
        ShowWindow(toolbar_status_combo_, SW_SHOWNA);
        BringWindowToTop(toolbar_status_combo_);
    }

    void apply_toolbar_status_combo_selection() {
        if (toolbar_status_combo_ == nullptr) return;
        const int row = toolbar_list_.focused_index();
        if (row < 0 || row >= static_cast<int>(kToolbarCommandCatalog.size())) return;
        const LRESULT selection = SendMessageW(toolbar_status_combo_, CB_GETCURSEL, 0, 0);
        if (selection == CB_ERR) return;
        set_toolbar_command_enabled(row, selection == 0);
        refresh_toolbar_settings_list(row);
    }

    void customize_file_columns() {
        FileColumnsDialog dialog(toolbar_table_theme());
        auto columns = application_options.file_list_columns;
        if (dialog.show(window_, columns)) {
            application_options.file_list_columns =
                normalize_file_list_columns(columns);
        }
    }

    void create_settings_controls() {
        settings_tabs_ = control(kSettingsNavClass, L"", WS_TABSTOP | WS_GROUP,
                                 kSettingsTabs);
        settings_viewport_ = CreateWindowExW(
            WS_EX_CONTROLPARENT, kSettingsViewportClass, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        SendMessageW(settings_viewport_, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font_), TRUE);

        setting_label(0, L"Application behavior", 0, 0, 660);
        setting_label(0, L"Theme", 0, 42, 170);
        setting_combo(0, kThemeMode, kThemeModeNames, 180, 36, 260);
        setting_label(0, L"Accent color", 0, 84, 170);
        setting_combo(0, kAccentColorMode, kAccentColorNames, 180, 78, 260);
        setting_label(0, L"Custom accent", 0, 126, 170);
        setting_edit(0, kCustomAccentColor, 180, 120, 150);
        setting_control(0, L"BUTTON", L"Pick color...",
                        WS_TABSTOP | BS_OWNERDRAW,
                        kPickAccentColor, 340, 120, 110, 30);
        setting_label(0, L"Pick a color or enter #RRGGBB; select Custom above to use it.",
                      464, 126, 300, 38, true);
        setting_label(0, L"Button icons", 0, 168, 170);
        setting_combo(0, kToolbarIconStyle, kToolbarIconStyleNames, 180, 162, 260);
        setting_label(0, L"Startup location", 0, 210, 170);
        setting_combo(0, kStartupMode, kStartupLocationNames, 180, 204, 260);
        setting_label(0, L"Startup folder path", 0, 252, 170);
        setting_edit(0, kStartupCustomPath, 180, 246, 470);
        setting_browse(0, kBrowseStartupCustomPath, 660, 246);
        setting_checkbox(0, kRestoreWindowPlacement,
                         L"Restore main window size and position", 180, 290);
        setting_checkbox(0, kCenterChildWindows,
                         L"Center child windows on the main window", 180, 324);
        setting_checkbox(0, kConfirmDelete, L"Confirm before deleting files", 180, 358);
        setting_checkbox(0, kConfirmOverwrite,
                         L"Confirm before overwriting existing files", 180, 392);
        setting_label(0, L"Recent count (integer)", 0, 436, 170);
        setting_edit(0, kRecentLocationCount, 180, 430, 95, ES_NUMBER | ES_AUTOHSCROLL);
        setting_label(0, L"0 disables recent path entries; 12 matches the current default.",
                      288, 436, 430, 38, true);

        setting_label(1, L"Default Add-to-archive options", 0, 0, 660);
        setting_label(1, L"Compression method", 0, 42, 170);
        setting_combo(1, kCompressionMethod, kCompressionMethodNames, 180, 36, 260);
        setting_label(1, L"Method level", 0, 84, 170);
        setting_combo(1, kLevel, kLevelNames, 180, 78, 260);
        setting_label(1, L"Dictionary size", 0, 126, 170);
        setting_combo(1, kDictionarySize, kDictionaryNames, 180, 120, 260);
        setting_label(1, L"Word size", 0, 168, 170);
        setting_combo(1, kWordSize, kWordSizeNames, 180, 162, 260);
        setting_label(1, L"Solid block size", 0, 210, 170);
        setting_combo(1, kSolidBlockSize, kSolidBlockNames, 180, 204, 260);
        setting_label(1, L"Method option", 0, 252, 170);
        setting_combo(1, kThreadModel, kThreadModelNames, 180, 246, 260);
        setting_label(1, L"CPU threads (integer)", 0, 294, 170);
        setting_thread_combo(1, 180, 288, 190);
        setting_label(1, L"Update mode", 0, 336, 170);
        setting_combo(1, kDefaultUpdateMode, kUpdateModeNames, 180, 330, 330);
        setting_label(1, L"Volume size (integer)", 0, 378, 170);
        setting_edit(1, kDefaultVolumeSize, 180, 372, 160,
                     ES_NUMBER | ES_AUTOHSCROLL);
        setting_combo(1, kDefaultVolumeUnit, kVolumeUnitNames, 350, 372, 105);
        setting_label(1, L"Recovery (integer 0-100)", 0, 420, 170);
        setting_edit(1, kDefaultRecoveryPercent, 180, 414, 80, ES_NUMBER | ES_AUTOHSCROLL);
        setting_label(1, L"%", 268, 420, 24);
        setting_checkbox(1, kDefaultRecoveryVolumes,
                         L"Create recovery volumes when split volumes are enabled", 180, 456);
        setting_checkbox(1, kDefaultCreateSfx,
                         L"Create self-extracting .exe by default", 180, 490);
        setting_checkbox(1, kDefaultSignArchive,
                         L"Sign archives by default", 180, 524);
        setting_label(1, L"Signing key file path", 0, 566, 170);
        setting_edit(1, kDefaultSigningKey, 180, 560, 470);
        setting_browse(1, kBrowseDefaultSigningKey, 660, 560);

        setting_label(2, L"Default folders", 0, 0, 660);
        setting_label(2, L"Archive output", 0, 42, 170);
        setting_combo(2, kArchiveOutputMode, kFolderPolicyNames, 180, 36, 260);
        setting_label(2, L"Archive folder path", 0, 84, 170);
        setting_edit(2, kArchiveOutputFolder, 180, 78, 470);
        setting_browse(2, kBrowseArchiveOutputFolder, 660, 78);
        setting_label(2, L"Extraction output", 0, 126, 170);
        setting_combo(2, kExtractDestinationMode, kFolderPolicyNames, 180, 120, 260);
        setting_label(2, L"Extraction folder path", 0, 168, 170);
        setting_edit(2, kExtractDestinationFolder, 180, 162, 470);
        setting_browse(2, kBrowseExtractDestinationFolder, 660, 162);
        setting_label(2, L"Temporary files", 0, 210, 170);
        setting_combo(2, kTempFolderMode, kTempFolderModeNames, 180, 204, 300);
        setting_label(2, L"Temporary folder path", 0, 252, 170);
        setting_edit(2, kTempFolder, 180, 246, 470);
        setting_browse(2, kBrowseTempFolder, 660, 246);
        setting_label(2, L"Cleanup days (integer)", 0, 294, 170);
        setting_edit(2, kTempCleanupDays, 180, 288, 80, ES_NUMBER | ES_AUTOHSCROLL);
        setting_label(2,
                      L"Last-used folders are remembered after successful dialog confirmation. "
                      L"Temporary cleanup removes old Axiom staging folders on startup. "
                      L"Use Tools > Delete Axiom temporary files for immediate cleanup.",
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
        setting_label(3, L"File-list columns", 0, 352, 660);
        setting_label(
            3,
            L"Choose visible fields and their order in a dedicated editor. "
            L"Visible headers can also be dragged directly in the main file list.",
            0, 382, 700, 48, true);
        setting_control(3, L"BUTTON", L"Customize columns...",
                        WS_TABSTOP | BS_OWNERDRAW,
                        kCustomizeFileColumns, 0, 442, 170, 30);

        setting_label(4, L"Viewing files from archives", 0, 0, 660);
        setting_label(4, L"Double-click action", 0, 42, 170);
        setting_combo(4, kFileOpenMode, kFileOpenModeNames, 180, 36, 280);
        setting_label(4, L"Viewer executable path", 0, 84, 170);
        setting_edit(4, kExternalViewer, 180, 78, 470);
        setting_browse(4, kBrowseExternalViewer, 660, 78);
        setting_label(4, L"Editor executable path", 0, 126, 170);
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
        setting_label(5, L"Trusted keys folder path", 0, 198, 170);
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
        setting_checkbox(6, kContextOpen, L"Axiom submenu: Open archives", 0, 300);
        setting_checkbox(6, kContextAdd, L"Axiom submenu: Add files or folders to archive", 0, 334);
        setting_checkbox(6, kContextExtract, L"Axiom submenu: Extract archives", 0, 368);
        setting_checkbox(6, kContextTest, L"Axiom submenu: Test archives", 0, 402);
        setting_label(6,
                      L"Integration is written per-user under HKCU and applies/removes Axiom's "
                      L"own file association and Explorer context submenu entries. Read-only "
                      L"formats can be opened, tested, and extracted but not edited.",
                      0, 456, 660, 64, true);

        setting_label(7, L"Updates", 0, 0, 660);
        setting_checkbox(7, kAutomaticUpdateChecks,
                         L"Silently check for updates at startup (at most once every 24 hours)",
                         0, 42, 660);
        setting_label(7, L"Release channel", 0, 92, 170);
        setting_combo(7, kUpdateChannel, kUpdateChannelNames, 180, 86, 260);
        setting_label(7, L"Update URL (HTTPS)", 0, 134, 170);
        setting_edit(7, kUpdateUrl, 180, 128, 470);
        setting_label(7,
                      L"Leave this empty to use Axiom's official GitHub release feed. Custom "
                      L"feeds must use HTTPS and may include {channel}.",
                      0, 184, 660, 54, true);

        setting_label(8, L"Keyboard shortcuts", 0, 0, 660);
        setting_label(8, L"Command", 0, 42, 170);
        setting_shortcut_command_combo(8, 180, 36, 470);
        setting_label(8, L"Shortcut (key chord)", 0, 92, 170);
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

        setting_label(9, L"Toolbar buttons", 0, 0, 660);
        setting_label(9,
                      L"Choose which commands appear on the main command toolbar. "
                      L"Buttons keep this order and wrap when needed. Hover a button "
                      L"to preview its active state.",
                      0, 30, 760, 38, true);
        setting_label(9, L"Button labels", 0, 72, 170);
        setting_combo(9, kToolbarDisplayMode, kToolbarDisplayModeNames,
                      180, 66, 220);
        setting_control(9, L"BUTTON", L"Restore default toolbar",
                        WS_TABSTOP | BS_OWNERDRAW,
                        kToolbarResetDefaults, 0, 552, 180, 30);

        setting_label(10, L"Advanced", 0, 0, 660);
        setting_label(10, L"Worker priority", 0, 42, 170);
        setting_combo(10, kWorkerPriority, kWorkerPriorityNames, 180, 36, 240);
        setting_checkbox(10, kVerboseLogging, L"Enable verbose operation logging", 180, 80);
        setting_label(10, L"Log folder path", 0, 126, 170);
        setting_edit(10, kLogFolder, 180, 120, 470);
        setting_browse(10, kBrowseLogFolder, 660, 120);
        setting_label(10, L"I/O buffer (byte size)", 0, 168, 170);
        setting_combo(10, kIoBufferMode, kAutomaticCustomNames, 180, 162, 180);
        setting_edit(10, kIoBufferSize, 370, 162, 160);
        setting_label(10, L"Memory limit (byte size)", 0, 210, 170);
        setting_combo(10, kMemoryLimitMode, kAutomaticCustomNames, 180, 204, 180);
        setting_edit(10, kMemoryLimit, 370, 204, 160);
        setting_label(10,
                      L"Worker priority, I/O buffer size, and memory limit are applied to GUI "
                      L"operations. Automatic I/O uses 1 MiB; custom values must be 64 KiB "
                      L"through 64 MiB.",
                      0, 264, 660, 72, true);

        apply_dialog_input_filter(item(kCustomAccentColor),
                                  DialogInputFilter::hexadecimal_color, 7);
        apply_dialog_input_filter(item(kRecentLocationCount),
                                  DialogInputFilter::unsigned_integer, 2);
        apply_dialog_input_filter(item(kThreads),
                                  DialogInputFilter::unsigned_integer, 5);
        apply_dialog_input_filter(item(kDefaultVolumeSize),
                                  DialogInputFilter::unsigned_integer, 20);
        apply_dialog_input_filter(item(kDefaultRecoveryPercent),
                                  DialogInputFilter::unsigned_integer, 3);
        apply_dialog_input_filter(item(kTempCleanupDays),
                                  DialogInputFilter::unsigned_integer, 3);
        apply_dialog_input_filter(item(kIoBufferSize),
                                  DialogInputFilter::byte_size, 24);
        apply_dialog_input_filter(item(kMemoryLimit),
                                  DialogInputFilter::byte_size, 24);

        for (const int id : {kStartupCustomPath, kDefaultSigningKey,
                             kArchiveOutputFolder, kExtractDestinationFolder,
                             kTempFolder, kExternalViewer, kExternalEditor,
                             kTrustedKeysFolder, kLogFolder}) {
            SendMessageW(item(id), EM_SETLIMITTEXT, 32767, 0);
        }
        SendMessageW(item(kUpdateUrl), EM_SETLIMITTEXT, 2048, 0);
        SendMessageW(item(kShortcutValue), EM_SETLIMITTEXT, 64, 0);

        add_dialog_tooltip(
            tooltip_, item(kThemeMode),
            L"Choose whether Axiom follows the Windows app theme or always uses dark or light controls.");
        add_dialog_tooltip(
            tooltip_, item(kAccentColorMode),
            L"Choose the color source used for selections, progress indicators, focus, and other accents.");
        add_dialog_tooltip(tooltip_, item(kCustomAccentColor),
                           L"Hexadecimal RGB color. Enter exactly #RRGGBB, for example #FFB93C.");
        add_dialog_tooltip(tooltip_, item(kPickAccentColor),
                           L"Open Axiom's accent color picker and select a custom RGB color.");
        add_dialog_tooltip(
            tooltip_, item(kToolbarIconStyle),
            L"Choose whether command icons follow the current theme, keep their full colors, or use the selected accent color.");
        add_dialog_tooltip(
            tooltip_, item(kStartupMode),
            L"Choose the location shown when Axiom starts. Custom uses the folder path below.");
        add_dialog_tooltip(tooltip_, item(kStartupCustomPath),
                           L"Windows folder path used when Startup location is Custom.");
        add_dialog_tooltip(tooltip_, item(kBrowseStartupCustomPath),
                           L"Choose an existing startup folder.");
        add_dialog_tooltip(tooltip_, item(kRecentLocationCount),
                           L"Unsigned integer from 0 through 50. Zero disables recent locations.");
        add_dialog_tooltip(tooltip_, item(kCenterChildWindows),
                           L"When selected, dialogs open centered on the main Axiom window. Clear it to remember each dialog's last position.");
        add_dialog_tooltip(
            tooltip_, item(kRestoreWindowPlacement),
            L"Restore the main window's saved size, position, and maximized state the next time Axiom starts.");
        add_dialog_tooltip(
            tooltip_, item(kConfirmDelete),
            L"Ask for confirmation before deleting filesystem items or entries from an archive.");
        add_dialog_tooltip(
            tooltip_, item(kConfirmOverwrite),
            L"Ask before an archive, extracted file, or other output replaces an existing file.");
        add_dialog_tooltip(tooltip_, item(kThreads),
                           L"Unsigned integer from 0 through the available logical processor count. Zero uses all processors.");
        add_dialog_tooltip(
            tooltip_, item(kCompressionMethod),
            L"Choose the default codec for new AXAR archives. ZIP supports Deflate or Store.");
        add_dialog_tooltip(
            tooltip_, item(kLevel),
            L"Choose the default method-specific compression level. Higher values generally trade time and memory for a smaller archive.");
        add_dialog_tooltip(
            tooltip_, item(kDictionarySize),
            L"Choose the default history dictionary. Larger values can improve compression but require more memory.");
        add_dialog_tooltip(
            tooltip_, item(kWordSize),
            L"Choose the default match-search word size. Useful values depend on the selected codec and level.");
        add_dialog_tooltip(
            tooltip_, item(kSolidBlockSize),
            L"Choose how much input is compressed as one solid block. Larger blocks can improve ratio but make random extraction more expensive.");
        add_dialog_tooltip(
            tooltip_, item(kThreadModel),
            L"Choose the codec-specific threading strategy used by default. Available choices depend on the compression method.");
        add_dialog_tooltip(
            tooltip_, item(kDefaultUpdateMode),
            L"Choose how Add to archive treats existing entries: replace matches, update only newer inputs, freshen existing names, or synchronize the archive.");
        add_dialog_tooltip(tooltip_, item(kDefaultVolumeSize),
                           L"Maximum size of each output part. Select KiB, MiB, GiB, or TiB; leave empty for one archive file. Splitting only succeeds when the completed archive is larger than this value.");
        add_dialog_tooltip(tooltip_, item(kDefaultRecoveryPercent),
                           L"Unsigned integer percentage from 0 through 100.");
        add_dialog_tooltip(
            tooltip_, item(kDefaultVolumeUnit),
            L"Binary unit applied to the default positive integer volume size.");
        add_dialog_tooltip(
            tooltip_, item(kDefaultRecoveryVolumes),
            L"Create .rev files that can reconstruct missing or corrupt split-archive parts. Requires a volume size and a recovery percentage.");
        add_dialog_tooltip(
            tooltip_, item(kDefaultCreateSfx),
            L"Enable creation of a self-extracting Windows executable by default when the selected archive format supports it.");
        add_dialog_tooltip(
            tooltip_, item(kDefaultSignArchive),
            L"Sign completed AXAR archives with the default key below so recipients can verify authenticity.");
        add_dialog_tooltip(tooltip_, item(kDefaultSigningKey),
                           L"Windows file path to an Axiom signing-key file.");
        add_dialog_tooltip(tooltip_, item(kBrowseDefaultSigningKey),
                           L"Choose an existing Axiom signing-key file.");
        add_dialog_tooltip(
            tooltip_, item(kArchiveOutputMode),
            L"Choose whether new archives are written beside the source, in the last-used folder, or in the custom folder below.");
        add_dialog_tooltip(tooltip_, item(kArchiveOutputFolder),
                           L"Windows folder path for newly created archives when the custom-folder policy is selected.");
        add_dialog_tooltip(tooltip_, item(kBrowseArchiveOutputFolder),
                           L"Choose an existing custom archive-output folder.");
        add_dialog_tooltip(
            tooltip_, item(kExtractDestinationMode),
            L"Choose whether extraction starts beside the archive, in the last-used folder, or in the custom folder below.");
        add_dialog_tooltip(tooltip_, item(kExtractDestinationFolder),
                           L"Windows folder path for extracted files when the custom-folder policy is selected.");
        add_dialog_tooltip(tooltip_, item(kBrowseExtractDestinationFolder),
                           L"Choose an existing custom extraction folder.");
        add_dialog_tooltip(
            tooltip_, item(kTempFolderMode),
            L"Choose whether Axiom stages temporary files in the Windows temporary folder or in the custom folder below.");
        add_dialog_tooltip(tooltip_, item(kTempFolder),
                           L"Windows folder path for Axiom temporary and staging files.");
        add_dialog_tooltip(tooltip_, item(kBrowseTempFolder),
                           L"Choose an existing custom temporary folder.");
        add_dialog_tooltip(tooltip_, item(kTempCleanupDays),
                           L"Unsigned integer from 0 through 365 days.");
        add_dialog_tooltip(
            tooltip_, item(kShowAddressShellLocations),
            L"Include This PC, known folders, and drives in the address-bar dropdown.");
        add_dialog_tooltip(
            tooltip_, item(kShowAddressRecentLocations),
            L"Include recently visited filesystem and archive locations in the address-bar dropdown.");
        add_dialog_tooltip(
            tooltip_, item(kShowAddressArchiveChildren),
            L"Include child folders from the current filesystem or archive location in the address-bar dropdown.");
        add_dialog_tooltip(
            tooltip_, item(kCustomizeFileColumns),
            L"Open the column editor to choose visible file-list fields and their left-to-right order.");
        add_dialog_tooltip(
            tooltip_, item(kFileOpenMode),
            L"Choose what double-clicking a file inside an archive does: open with Windows, use Axiom's viewer setting, or prompt.");
        add_dialog_tooltip(tooltip_, item(kExternalViewer),
                           L"Windows file path to the executable used to view extracted files.");
        add_dialog_tooltip(tooltip_, item(kBrowseExternalViewer),
                           L"Choose an existing viewer executable file.");
        add_dialog_tooltip(tooltip_, item(kExternalEditor),
                           L"Windows file path to the executable used to edit extracted files.");
        add_dialog_tooltip(tooltip_, item(kBrowseExternalEditor),
                           L"Choose an existing editor executable file.");
        add_dialog_tooltip(
            tooltip_, item(kWarnExecutableOpen),
            L"Show a safety warning before launching executable or script content extracted from an archive.");
        add_dialog_tooltip(
            tooltip_, item(kKeepViewedFilesUntilExit),
            L"Keep selectively extracted viewing files available until Axiom exits instead of cleaning them up immediately.");
        add_dialog_tooltip(
            tooltip_, item(kPasswordPromptMode),
            L"Choose when Axiom asks for an archive password instead of waiting for an encrypted operation to require it.");
        add_dialog_tooltip(
            tooltip_, item(kCachePasswords),
            L"Reuse passwords only in memory while their archive remains open. Passwords are never written to settings.");
        add_dialog_tooltip(
            tooltip_, item(kVerifySignatures),
            L"Verify a signed AXAR archive against trusted public keys before extraction begins.");
        add_dialog_tooltip(
            tooltip_, item(kWipeEncryptedTempFiles),
            L"Overwrite temporary plaintext files produced from encrypted archives before deleting them.");
        add_dialog_tooltip(tooltip_, item(kTrustedKeysFolder),
                           L"Windows folder path containing trusted Axiom public-key files.");
        add_dialog_tooltip(tooltip_, item(kBrowseTrustedKeysFolder),
                           L"Choose an existing trusted-keys folder.");
        add_dialog_tooltip(tooltip_, item(kUpdateUrl),
                           L"Absolute HTTPS URL for a custom update feed. Leave empty to use Axiom's official GitHub release feed.");
        add_dialog_tooltip(
            tooltip_, item(kUpdateChannel),
            L"Choose which release channel automatic and manual update checks follow.");
        add_dialog_tooltip(
            tooltip_, item(kAutomaticUpdateChecks),
            L"Check the selected release feed at startup, at most once every 24 hours. Updates are not installed automatically.");
        add_dialog_tooltip(
            tooltip_, item(kShortcutCommand),
            L"Choose the Axiom command whose keyboard shortcut you want to inspect or change.");
        add_dialog_tooltip(tooltip_, item(kShortcutValue),
                           L"Key-chord text such as Ctrl+O, Alt+Left, F5, Delete, or None.");
        add_dialog_tooltip(
            tooltip_, item(kShortcutAssign),
            L"Assign the typed key chord to the selected command. Duplicate non-contextual shortcuts are rejected.");
        add_dialog_tooltip(
            tooltip_, item(kShortcutClear),
            L"Remove the custom shortcut from the selected command.");
        add_dialog_tooltip(
            tooltip_, item(kShortcutResetAll),
            L"Restore every keyboard shortcut to Axiom's defaults.");
        add_dialog_tooltip(tooltip_, item(kToolbarDisplayMode),
                           L"Choose whether the main command toolbar shows labels beside icons or uses icons only. Full command names remain available as tooltips.");
        add_dialog_tooltip(
            tooltip_, item(kToolbarResetDefaults),
            L"Restore the default set and order of commands on the main toolbar.");
        add_dialog_tooltip(
            tooltip_, item(kWorkerPriority),
            L"Choose the Windows process priority used while archive operations run. Lower priority keeps other applications more responsive.");
        add_dialog_tooltip(
            tooltip_, item(kVerboseLogging),
            L"Write detailed operation diagnostics to the selected log folder. Logs may contain file paths.");
        add_dialog_tooltip(tooltip_, item(kLogFolder),
                           L"Windows folder path for verbose operation logs.");
        add_dialog_tooltip(tooltip_, item(kBrowseLogFolder),
                           L"Choose an existing log folder.");
        add_dialog_tooltip(
            tooltip_, item(kIoBufferMode),
            L"Use Axiom's 1 MiB I/O buffer or enable the custom byte-size field beside this list.");
        add_dialog_tooltip(tooltip_, item(kIoBufferSize),
                           L"Positive byte size from 64 KiB through 64 MiB. Accepted suffixes: B, KiB, MiB, GiB, or TiB.");
        add_dialog_tooltip(tooltip_, item(kMemoryLimit),
                           L"Positive byte size. Accepted suffixes: B, KiB, MiB, GiB, or TiB.");
        add_dialog_tooltip(
            tooltip_, item(kMemoryLimitMode),
            L"Use Axiom's automatic operation memory budget or enable the custom byte-size field beside this list.");

        accept_ = control(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW, kAccept);
        cancel_ = control(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, kCancel);
        apply_ = control(L"BUTTON", L"Apply", WS_TABSTOP | BS_OWNERDRAW, kApply);
        defaults_ = control(L"BUTTON", L"Defaults...", WS_TABSTOP | BS_OWNERDRAW, kDefaults);
        load_settings_values();
        select_settings_page(0);
    }

    void create_create_controls() {
        create_navigation_ = control(kSettingsNavClass, L"",
                                     WS_TABSTOP | WS_GROUP,
                                     kCreateNavigation);
        create_viewport_ = CreateWindowExW(
            WS_EX_CONTROLPARENT, kSettingsViewportClass, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        SendMessageW(create_viewport_, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font_), TRUE);
        create_page_heading_ = control(L"STATIC", L"",
                                        SS_LEFT | SS_NOPREFIX,
                                        0);
        SetParent(create_page_heading_, create_viewport_);

        summary_ = label(L"");
        path_label_ = label(L"Output file path");
        path_edit_ = control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kPathEdit);
        browse_ = control(L"BUTTON", L"Browse...", WS_TABSTOP | BS_OWNERDRAW, kBrowse);
        SendMessageW(path_edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(scale(6), scale(6)));
        format_label_ = label(L"Format");
        format_combo_ = archive_format_combo();

        update_mode_label_ = page_label(1, L"Update mode");
        update_mode_combo_ = page_combo(1, kUpdateMode, kUpdateModeNames);
        comment_label_ = page_label(1, L"Archive comment (text)");
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

        compression_profile_label_ = page_label(0, L"Compression profile");
        compression_profile_combo_ = page_control(
            0, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN |
                CBS_AUTOHSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
            kCompressionProfile);
        SendMessageW(compression_profile_combo_, CB_SETITEMHEIGHT, 0, scale(24));
        SendMessageW(compression_profile_combo_, CB_SETITEMHEIGHT,
                     static_cast<WPARAM>(-1), scale(24));
        save_compression_profile_ = page_control(
            0, L"BUTTON", L"Save", WS_TABSTOP | BS_OWNERDRAW,
            kSaveCompressionProfile);
        delete_compression_profile_ = page_control(
            0, L"BUTTON", L"Delete", WS_TABSTOP | BS_OWNERDRAW,
            kDeleteCompressionProfile);
        method_label_ = page_label(0, L"Compression method");
        method_combo_ = page_combo(0, kCompressionMethod, kCompressionMethodNames);
        level_label_ = page_label(0, L"Method level");
        level_combo_ = page_combo(0, kLevel, kLevelNames);
        dictionary_label_ = page_label(0, L"Dictionary size");
        dictionary_combo_ = page_combo(0, kDictionarySize, kDictionaryNames);
        word_size_label_ = page_label(0, L"Word size");
        word_size_combo_ = page_combo(0, kWordSize, kWordSizeNames);
        solid_block_label_ = page_label(0, L"Solid block size");
        solid_block_combo_ = page_combo(0, kSolidBlockSize, kSolidBlockNames);
        threads_label_ = page_label(0, L"Threads (integer; 0 = all)");
        threads_combo_ = page_thread_combo(0);
        thread_model_label_ = page_label(0, L"Threading model");
        thread_model_combo_ = page_combo(0, kThreadModel, kThreadModelNames);
        compression_info_ = page_label(
            0,
            L"Default values follow the selected compression level. Larger dictionaries and "
            L"solid blocks can improve ratio but increase memory use.", true);
        compression_preview_ = page_control(
            0, L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY,
            kCompressionPreview);

        encrypt_data_ = page_checkbox(2, kEncryptData, L"Encrypt file data");
        encrypt_names_ = page_checkbox(2, kEncryptNames,
                                       L"Encrypt file names and archive directory");
        password_label_ = page_label(2, L"Password (text)");
        password_edit_ = page_edit(2, kPassword, ES_PASSWORD | ES_AUTOHSCROLL);
        confirm_password_label_ = page_label(2, L"Confirm password (text)");
        confirm_password_edit_ = page_edit(
            2, kConfirmPassword, ES_PASSWORD | ES_AUTOHSCROLL);
        show_password_ = page_checkbox(2, kShowPassword, L"Show password");
        security_info_ = page_label(
            2,
            L"Axiom uses Argon2id key derivation and XChaCha20-Poly1305. Passwords are never "
            L"saved in GUI settings and are cleared when this dialog closes.", true);

        volume_size_label_ = page_label(3, L"Volume size (integer > 0)");
        volume_size_edit_ = page_edit(3, kVolumeSize, ES_NUMBER | ES_AUTOHSCROLL);
        volume_unit_combo_ = page_combo(3, kVolumeUnit, kVolumeUnitNames);
        recovery_percent_label_ = page_label(3, L"Recovery (integer 0-100)");
        recovery_percent_edit_ = page_edit(3, kRecoveryPercent,
                                           ES_NUMBER | ES_AUTOHSCROLL);
        recovery_percent_suffix_ = page_label(3, L"% of archive data");
        recovery_volumes_ = page_checkbox(
            3, kRecoveryVolumes,
            L"Create .rev recovery volumes for missing-volume reconstruction");
        recovery_info_ = page_label(
            3,
            L"Split size is the maximum part size and must be smaller than the completed archive; "
            L"Axiom verifies this after compression. A recovery record repairs bounded damage "
            L"inside an archive; .rev volumes reconstruct missing or corrupt parts.", true);

        // Signing is an authenticity control, so it lives with the other
        // security settings rather than sharing a tab with SFX output.
        sign_archive_ = page_checkbox(2, kSignArchive,
                                      L"Sign the completed archive");
        signing_key_label_ = page_label(2, L"Signing key file path");
        signing_key_edit_ = page_edit(2, kSigningKey);
        browse_signing_key_ = page_control(2, L"BUTTON", L"Browse...",
                                           WS_TABSTOP | BS_OWNERDRAW, kBrowseSigningKey);

        create_sfx_ = page_checkbox(4, kCreateSfx,
                                    L"Create one self-extracting Windows executable");
        sfx_stub_tier_label_ = page_label(4, L"Extractor type");
        sfx_stub_tier_combo_ = page_combo(4, kSfxStubTier, kSfxStubTierNames);
        sfx_title_label_ = page_label(4, L"Window title (text)");
        sfx_title_edit_ = page_edit(4, kSfxTitle);
        sfx_default_path_label_ = page_label(4, L"Default destination (path)");
        sfx_default_path_combo_ = page_combo(
            4, kSfxDefaultPath, kSfxDefaultPathNames, true);
        sfx_description_label_ = page_label(4, L"Description (text)");
        sfx_description_edit_ = page_edit(4, kSfxDescription);
        sfx_overwrite_label_ = page_label(4, L"Existing files");
        sfx_overwrite_combo_ = page_combo(4, kSfxOverwrite, kSfxOverwriteNames);
        sfx_mode_label_ = page_label(4, L"Interface");
        sfx_mode_combo_ = page_combo(4, kSfxMode, kSfxModeNames);
        sfx_elevation_label_ = page_label(4, L"Elevation");
        sfx_elevation_combo_ = page_combo(4, kSfxElevation, kSfxElevationNames);
        sfx_run_program_label_ = page_label(4, L"Run after extracting (path)");
        sfx_run_program_edit_ = page_edit(4, kSfxRunProgram);
        sfx_run_arguments_label_ = page_label(4, L"Run arguments (text)");
        sfx_run_arguments_edit_ = page_edit(4, kSfxRunArguments);
        sfx_theme_label_ = page_label(4, L"Appearance");
        sfx_theme_combo_ = page_combo(4, kSfxTheme, kSfxThemeNames);
        sfx_license_label_ = page_label(4, L"License text");
        sfx_license_edit_ = page_edit(
            4, kSfxLicenseText,
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL);
        sfx_allow_path_change_ = page_checkbox(
            4, kSfxAllowPathChange, L"Let the user change the destination");
        sfx_require_accept_ = page_checkbox(
            4, kSfxRequireAccept, L"Require the license to be accepted");
        sfx_open_destination_ = page_checkbox(
            4, kSfxOpenDestination, L"Open the destination when finished");

        content_dedup_ = page_checkbox(
            5, kContentDedup,
            L"Store repeated file content once using live deduplication");
        dedup_min_chunk_label_ = page_label(5, L"Minimum chunk size");
        dedup_min_chunk_edit_ = page_edit(5, kDedupMinChunk);
        dedup_average_chunk_label_ = page_label(5, L"Average chunk size");
        dedup_average_chunk_edit_ = page_edit(5, kDedupAverageChunk);
        dedup_max_chunk_label_ = page_label(5, L"Maximum chunk size");
        dedup_max_chunk_edit_ = page_edit(5, kDedupMaxChunk);
        dedup_info_ = page_label(
            5,
            L"Live deduplication splits files at content-defined boundaries so unchanged and repeated regions share storage. The defaults suit general backups; smaller chunks find more overlap but add directory overhead.",
            true);

        SendMessageW(path_edit_, EM_SETLIMITTEXT, 32767, 0);
        SendMessageW(comment_edit_, EM_SETLIMITTEXT, 65535, 0);
        SendMessageW(password_edit_, EM_SETLIMITTEXT, 1024, 0);
        SendMessageW(confirm_password_edit_, EM_SETLIMITTEXT, 1024, 0);
        SendMessageW(signing_key_edit_, EM_SETLIMITTEXT, 32767, 0);
        apply_dialog_input_filter(threads_combo_,
                                  DialogInputFilter::unsigned_integer, 5);
        apply_dialog_input_filter(volume_size_edit_,
                                  DialogInputFilter::unsigned_integer, 20);
        apply_dialog_input_filter(recovery_percent_edit_,
                                  DialogInputFilter::unsigned_integer, 3);
        apply_dialog_input_filter(dedup_min_chunk_edit_,
                                  DialogInputFilter::byte_size, 24);
        apply_dialog_input_filter(dedup_average_chunk_edit_,
                                  DialogInputFilter::byte_size, 24);
        apply_dialog_input_filter(dedup_max_chunk_edit_,
                                  DialogInputFilter::byte_size, 24);
        add_dialog_tooltip(tooltip_, path_edit_,
                           L"Windows file path for the archive or self-extracting executable.");
        add_dialog_tooltip(tooltip_, browse_,
                           L"Choose the output archive or self-extracting executable file path.");
        add_dialog_tooltip(tooltip_, format_combo_,
                           L"Archive format selection. Available options determine which other fields are enabled.");
        add_dialog_tooltip(
            tooltip_, compression_profile_combo_,
            L"Choose a built-in or saved compression profile. To create one, adjust the compression settings, type a profile name, and select Save.");
        add_dialog_tooltip(tooltip_, save_compression_profile_,
                           L"Save the current compression-tab settings under the typed profile name.");
        add_dialog_tooltip(tooltip_, delete_compression_profile_,
                           L"Delete the selected user profile. Built-in profiles cannot be deleted.");
        add_dialog_tooltip(
            tooltip_, method_combo_,
            L"Select the codec stored in AXAR solid blocks. ZIP supports Deflate or Store.");
        add_dialog_tooltip(tooltip_, level_combo_,
                           L"Method-specific compression level. Available values change with the selected codec.");
        add_dialog_tooltip(
            tooltip_, dictionary_combo_,
            L"Compression history dictionary. Larger values can improve ratio but require more memory; available values depend on the codec.");
        add_dialog_tooltip(
            tooltip_, word_size_combo_,
            L"Match-search word size used by the selected codec. Available values depend on the method and compression level.");
        add_dialog_tooltip(
            tooltip_, solid_block_combo_,
            L"Amount of input compressed as one solid block. Larger blocks can improve ratio but make random extraction more expensive.");
        add_dialog_tooltip(
            tooltip_, thread_model_combo_,
            L"Codec-specific threading strategy. Available choices depend on the selected compression method.");
        add_dialog_tooltip(
            tooltip_, update_mode_combo_,
            L"Choose whether to replace matching entries, update only newer inputs, freshen existing names, or synchronize removals with the source.");
        add_dialog_tooltip(
            tooltip_, compression_preview_,
            L"Live compression prognosis. Select a point to choose that codec level; the shaded band shows estimate uncertainty.");
        add_dialog_tooltip(tooltip_, threads_combo_,
                           L"Unsigned integer from 0 through the available logical processor count. Zero uses all processors.");
        add_dialog_tooltip(tooltip_, comment_edit_,
                           L"Unicode text stored as the archive comment.");
        add_dialog_tooltip(
            tooltip_, lock_archive_,
            L"Mark the completed AXAR archive as locked so later update operations refuse to modify it.");
        add_dialog_tooltip(
            tooltip_, repack_after_update_,
            L"Rewrite the archive after updating to remove superseded data and recover unused space. This adds another full archive pass.");
        add_dialog_tooltip(
            tooltip_, encrypt_data_,
            L"Encrypt stored file content with XChaCha20-Poly1305. A non-empty password is required.");
        add_dialog_tooltip(
            tooltip_, encrypt_names_,
            L"Encrypt the AXAR directory so file names and paths are hidden until the password is supplied. This also encrypts file data.");
        add_dialog_tooltip(tooltip_, password_edit_,
                           L"Unicode password text. Passwords are never saved in GUI settings.");
        add_dialog_tooltip(tooltip_, confirm_password_edit_,
                           L"Repeat the password exactly; both text fields must match.");
        add_dialog_tooltip(
            tooltip_, show_password_,
            L"Temporarily show or mask both password fields. This does not store the password.");
        add_dialog_tooltip(tooltip_, volume_size_edit_,
                           L"Maximum size of each output part. Select KiB, MiB, GiB, or TiB; leave empty to disable splitting. The completed archive must be larger than this value.");
        add_dialog_tooltip(tooltip_, volume_unit_combo_,
                           L"Binary unit applied to the positive integer volume size.");
        add_dialog_tooltip(tooltip_, recovery_percent_edit_,
                           L"Unsigned integer percentage from 0 through 100.");
        add_dialog_tooltip(
            tooltip_, recovery_volumes_,
            L"Create .rev files that can reconstruct missing or corrupt split-archive parts. Requires splitting and a recovery percentage.");
        add_dialog_tooltip(
            tooltip_, sign_archive_,
            L"Sign the completed AXAR archive so its authenticity can be verified with the matching public key.");
        add_dialog_tooltip(tooltip_, signing_key_edit_,
                           L"Windows file path to an Axiom signing-key file.");
        add_dialog_tooltip(tooltip_, browse_signing_key_,
                           L"Choose an existing Axiom signing-key file.");
        add_dialog_tooltip(tooltip_, create_sfx_,
                           L"Build one Windows .exe containing the selected archive format and extraction stub.");
        add_dialog_tooltip(
            tooltip_, content_dedup_,
            L"Select the AXAR v5 live content-deduplication profile for a new archive. Existing archives preserve their established profile automatically.");
        add_dialog_tooltip(
            tooltip_, dedup_min_chunk_edit_,
            L"Smallest content-defined chunk. Enter 4 KiB through 64 MiB; it cannot exceed the average size.");
        add_dialog_tooltip(
            tooltip_, dedup_average_chunk_edit_,
            L"Target content-defined chunk size. It must be between the minimum and maximum sizes.");
        add_dialog_tooltip(
            tooltip_, dedup_max_chunk_edit_,
            L"Largest content-defined chunk. Enter up to 64 MiB and no less than the average size.");
        SendMessageW(sfx_title_edit_, EM_SETLIMITTEXT, 1024, 0);
        SendMessageW(sfx_description_edit_, EM_SETLIMITTEXT, 4096, 0);
        SendMessageW(sfx_default_path_combo_, CB_LIMITTEXT, 32767, 0);
        SendMessageW(sfx_run_program_edit_, EM_SETLIMITTEXT, 32767, 0);
        SendMessageW(sfx_run_arguments_edit_, EM_SETLIMITTEXT, 32767, 0);
        SendMessageW(sfx_license_edit_, EM_SETLIMITTEXT, 65535, 0);
        add_dialog_tooltip(
            tooltip_, sfx_stub_tier_combo_,
            L"Full window shows dialogs when the extractor runs. Console only uses the smaller decode-only runtime and never prompts; window-only settings are disabled for that tier.");
        add_dialog_tooltip(tooltip_, sfx_title_edit_,
                           L"Unicode text shown as the extractor's window title. Leave empty to use the Axiom default.");
        add_dialog_tooltip(tooltip_, sfx_description_edit_,
                           L"Unicode text shown to the user under the heading when the extractor runs. Replaces the archive comment there.");
        add_dialog_tooltip(tooltip_, sfx_theme_combo_,
                           L"Appearance of the extractor. Follow the system tracks the Windows light or dark setting; the other two pin it.");
        add_dialog_tooltip(
            tooltip_, sfx_default_path_combo_,
            L"Editable destination template. Choose a common location or type an absolute path. Accepts %ProgramFiles%, %ProgramFiles(x86)%, %LOCALAPPDATA%, %APPDATA%, %USERPROFILE%, %DESKTOP%, %DOCUMENTS%, %TEMP%, %SFXDIR%, and %SFXNAME%. Leave empty to extract beside the executable.");
        add_dialog_tooltip(tooltip_, sfx_overwrite_combo_,
                           L"What the extractor does when a target file already exists.");
        add_dialog_tooltip(
            tooltip_, sfx_mode_combo_,
            L"Interactive shows the extraction dialog. Silent shows progress and errors only. No window runs without any interface, and then a destination and any password must come from the command line.");
        add_dialog_tooltip(
            tooltip_, sfx_elevation_combo_,
            L"When the extractor requests administrator rights. Elevate when needed tests whether the destination is actually writable before asking.");
        add_dialog_tooltip(
            tooltip_, sfx_run_program_edit_,
            L"Archive-relative path to a program to run after extracting, such as setup\\install.exe. It must be a file the extraction produced; absolute paths are rejected. Leave empty to run nothing.");
        add_dialog_tooltip(tooltip_, sfx_run_arguments_edit_,
                           L"Command-line text passed to the program above. Requires a program.");
        add_dialog_tooltip(
            tooltip_, sfx_license_edit_,
            L"Unicode text shown before extraction when acceptance is required. Leave empty for no license step.");
        add_dialog_tooltip(tooltip_, sfx_allow_path_change_,
                           L"When cleared, the extractor shows the destination but does not let the user edit it.");
        add_dialog_tooltip(tooltip_, sfx_require_accept_,
                           L"Show the license and require the user to accept before extracting. Needs license text.");
        add_dialog_tooltip(tooltip_, sfx_open_destination_,
                           L"Open the destination folder in Explorer once extraction finishes.");
        accept_ = control(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW, kAccept);
        cancel_ = control(L"BUTTON", L"Cancel", WS_TABSTOP | BS_OWNERDRAW, kCancel);
        load_create_values();
        select_create_page(0);
        update_create_dependencies();
        schedule_compression_curve();
    }

    void create_controls() {
        palette_ = make_palette();
        set_dark_title(window_, palette_.dark);
        apply_axiom_window_icons(window_, instance_);
        tooltip_.create(window_, dpi_, palette_.dark);
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
            path_label_ = label(mode_ == DialogMode::create_archive
                                    ? L"Archive file path"
                                    : L"Destination folder path");
            path_edit_ = control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kPathEdit);
            browse_ = control(L"BUTTON", L"Browse...", WS_TABSTOP | BS_OWNERDRAW, kBrowse);
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
        threads_label_ = label(L"Threads (integer; 0 = all)");
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
        SendMessageW(path_edit_, EM_SETLIMITTEXT, 32767, 0);
        apply_dialog_input_filter(threads_combo_,
                                  DialogInputFilter::unsigned_integer, 5);
        add_dialog_tooltip(tooltip_, path_edit_,
                           mode_ == DialogMode::extract_archive
                               ? L"Windows folder path that will receive the extracted items."
                               : L"Windows file path for the new archive.");
        add_dialog_tooltip(tooltip_, browse_,
                           mode_ == DialogMode::extract_archive
                               ? L"Choose an extraction destination folder."
                               : L"Choose an output archive file path.");
        add_dialog_tooltip(tooltip_, threads_combo_,
                           L"Unsigned integer from 0 through the available logical processor count. Zero uses all processors.");
        add_dialog_tooltip(tooltip_, overwrite_,
                           L"Allow extracted files to replace existing files with the same path.");
        add_dialog_tooltip(tooltip_, restore_time_,
                           L"Restore each extracted item's stored last-modified timestamp.");
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
        set_thread_count(threads_combo_, threads);
        if (mode_ == DialogMode::create_archive) {
            rebuild_codec_parameter_controls(
                dictionary_combo_, word_size_combo_, create_options.method,
                create_options.dictionary_size, create_options.word_size);
            SendMessageW(solid_block_combo_, CB_SETCURSEL,
                         value_index(kSolidBlockValues, create_options.solid_block_size), 0);
        }
    }

    HWND item(int id) const {
        if (HWND result = GetDlgItem(window_, id)) return result;
        if (settings_viewport_ != nullptr) {
            if (HWND result = GetDlgItem(settings_viewport_, id)) return result;
        }
        return create_viewport_ != nullptr
            ? GetDlgItem(create_viewport_, id) : nullptr;
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

    std::optional<int> edit_int(int id, int minimum, int maximum) const {
        const std::wstring text = window_text(item(id));
        if (text.empty()) return std::nullopt;
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long value = std::wcstoul(text.c_str(), &end, 10);
        if (errno == ERANGE || end == text.c_str() || *end != L'\0' ||
            value > static_cast<unsigned long>(maximum) ||
            value < static_cast<unsigned long>(minimum)) {
            return std::nullopt;
        }
        return static_cast<int>(value);
    }

    std::optional<std::size_t> thread_count_from(HWND combo) const {
        if (combo == nullptr) return std::nullopt;
        const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        const auto maximum = static_cast<unsigned long long>(
            axiom::core::logical_processor_count());
        // Every valid thread count has a list entry at the same index. Reading
        // that index also handles the descriptive "0 (all processors)" row.
        if (selection != CB_ERR && selection >= 0 &&
            static_cast<unsigned long long>(selection) <= maximum) {
            return static_cast<std::size_t>(selection);
        }
        std::wstring text = window_text(combo);
        if (text.empty()) {
            COMBOBOXINFO info{sizeof(info)};
            if (GetComboBoxInfo(combo, &info) && info.hwndItem != nullptr) {
                text = window_text(info.hwndItem);
            }
        }
        if (text.empty()) return std::nullopt;
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long long value = _wcstoui64(text.c_str(), &end, 10);
        if (errno == ERANGE || end == text.c_str() || *end != L'\0' ||
            value > maximum) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(value);
    }

    void set_thread_count(HWND combo, std::size_t count) const {
        if (combo == nullptr) return;
        const std::size_t maximum = axiom::core::logical_processor_count();
        const std::size_t normalized = std::min(count, maximum);
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(normalized), 0);
    }

    std::optional<CompressionProfile> compression_profile_from_controls(
        std::wstring name) const {
        const auto threads = thread_count_from(threads_combo_);
        if (!threads) return std::nullopt;
        const auto method = method_from_combo(method_combo_);
        const int method_level = level_from_combo(level_combo_, 5);
        const int option = static_cast<int>(std::clamp<LRESULT>(
            SendMessageW(thread_model_combo_, CB_GETCURSEL, 0, 0), 0, 1));
        return CompressionProfile{
            std::move(name),
            method == axiom::CompressionMethod::axiom
                ? std::clamp(method_level, 1, 9)
                : std::clamp(create_options.level, 1, 9),
            *threads,
            value_from_combo(dictionary_combo_),
            value_from_combo(word_size_combo_),
            selected_combo_value(solid_block_combo_, kSolidBlockValues),
            method == axiom::CompressionMethod::axiom ? option : 0,
            method,
            method == axiom::CompressionMethod::axiom
                ? axiom::kAutomaticCodecLevel
                : method_level,
            method == axiom::CompressionMethod::lzma2 ? option == 1 : true,
        };
    }

    static bool compression_profile_settings_equal(const CompressionProfile& left,
                                                   const CompressionProfile& right) {
        return left.level == right.level &&
               left.thread_count == right.thread_count &&
               left.dictionary_size == right.dictionary_size &&
               left.word_size == right.word_size &&
               left.solid_block_size == right.solid_block_size &&
               left.thread_model == right.thread_model &&
               left.method == right.method &&
               left.codec_level == right.codec_level &&
               left.lzma_binary_tree == right.lzma_binary_tree;
    }

    void update_compression_profile_actions() {
        if (delete_compression_profile_ == nullptr ||
            compression_profile_combo_ == nullptr) return;
        const std::wstring name = trim_color_text(window_text(compression_profile_combo_));
        const bool custom = std::any_of(
            create_options.compression_profiles.begin(),
            create_options.compression_profiles.end(),
            [&](const CompressionProfile& profile) {
                return profile_names_equal(profile.name, name);
            });
        EnableWindow(delete_compression_profile_, custom);
    }

    void rebuild_compression_profile_combo(bool match_current_settings = false) {
        if (compression_profile_combo_ == nullptr) return;
        SendMessageW(compression_profile_combo_, CB_RESETCONTENT, 0, 0);
        for (const CompressionProfile& profile : built_in_compression_profiles()) {
            SendMessageW(compression_profile_combo_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(profile.name.c_str()));
        }
        for (const CompressionProfile& profile : create_options.compression_profiles) {
            SendMessageW(compression_profile_combo_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(profile.name.c_str()));
        }

        LRESULT selection = CB_ERR;
        if (match_current_settings) {
            if (const auto current = compression_profile_from_controls(L""); current) {
                std::size_t index = 0;
                for (const CompressionProfile& profile : built_in_compression_profiles()) {
                    if (compression_profile_settings_equal(*current, profile)) {
                        selection = static_cast<LRESULT>(index);
                        break;
                    }
                    ++index;
                }
                if (selection == CB_ERR) {
                    for (const CompressionProfile& profile : create_options.compression_profiles) {
                        if (compression_profile_settings_equal(*current, profile)) {
                            selection = static_cast<LRESULT>(index);
                            break;
                        }
                        ++index;
                    }
                }
            }
        }
        SendMessageW(compression_profile_combo_, CB_SETCURSEL, selection, 0);
        if (selection == CB_ERR) {
            SetWindowTextW(compression_profile_combo_, L"Custom settings");
        }
        update_compression_profile_actions();
    }

    void mark_compression_profile_custom() {
        if (applying_compression_profile_ || compression_profile_combo_ == nullptr) return;
        SendMessageW(compression_profile_combo_, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
        SetWindowTextW(compression_profile_combo_, L"Custom settings");
        update_compression_profile_actions();
    }

    void apply_selected_compression_profile() {
        const LRESULT selection = SendMessageW(
            compression_profile_combo_, CB_GETCURSEL, 0, 0);
        if (selection == CB_ERR) {
            update_compression_profile_actions();
            return;
        }
        const std::size_t built_in_count = built_in_compression_profiles().size();
        const CompressionProfile* profile = nullptr;
        if (static_cast<std::size_t>(selection) < built_in_count) {
            profile = &built_in_compression_profiles()[static_cast<std::size_t>(selection)];
        } else {
            const std::size_t custom_index =
                static_cast<std::size_t>(selection) - built_in_count;
            if (custom_index < create_options.compression_profiles.size()) {
                profile = &create_options.compression_profiles[custom_index];
            }
        }
        if (profile == nullptr) return;

        applying_compression_profile_ = true;
        rebuild_method_combo(method_combo_, selected_format_is_native(),
                             profile->method);
        create_options.method = method_from_combo(method_combo_);
        rebuild_method_controls(
            level_combo_, thread_model_combo_, create_options.method,
            profile->level, profile->codec_level, profile->thread_model,
            profile->lzma_binary_tree);
        rebuild_codec_parameter_controls(
            dictionary_combo_, word_size_combo_, create_options.method,
            profile->dictionary_size, profile->word_size);
        set_thread_count(threads_combo_, profile->thread_count);
        SendMessageW(solid_block_combo_, CB_SETCURSEL,
                     value_index(kSolidBlockValues, profile->solid_block_size), 0);
        applying_compression_profile_ = false;
        update_create_dependencies();
        update_compression_profile_actions();
    }

    void save_compression_profile() {
        std::wstring name = trim_color_text(window_text(compression_profile_combo_));
        const bool invalid_character = std::any_of(
            name.begin(), name.end(), [](wchar_t character) {
                return character < L' ' || character == L'\t';
            });
        if (name.empty() || name.size() > 64 || invalid_character ||
            profile_names_equal(name, L"Custom settings")) {
            show_message_dialog(
                window_, instance_, dpi_, palette_.dark, L"Compression profiles",
                L"Type a profile name from 1 through 64 characters in the Profile field, then select Save.",
                MessageDialogIcon::warning);
            SetFocus(compression_profile_combo_);
            return;
        }
        if (is_built_in_profile_name(name)) {
            show_message_dialog(
                window_, instance_, dpi_, palette_.dark, L"Compression profiles",
                L"Built-in profiles cannot be replaced. Type a different profile name.",
                MessageDialogIcon::warning);
            SetFocus(compression_profile_combo_);
            return;
        }
        auto profile = compression_profile_from_controls(name);
        if (!profile) {
            show_message_dialog(
                window_, instance_, dpi_, palette_.dark, L"Compression profiles",
                L"The current thread count is invalid. Correct it before saving the profile.",
                MessageDialogIcon::warning);
            SetFocus(threads_combo_);
            return;
        }
        auto existing = std::find_if(
            create_options.compression_profiles.begin(),
            create_options.compression_profiles.end(),
            [&](const CompressionProfile& value) {
                return profile_names_equal(value.name, name);
            });
        if (existing != create_options.compression_profiles.end()) {
            if (show_message_dialog(
                    window_, instance_, dpi_, palette_.dark,
                    L"Replace compression profile",
                    L"Replace the saved profile \"" + existing->name +
                        L"\" with the current compression settings?",
                    MessageDialogIcon::question, MessageDialogButtons::yes_no,
                    IDNO) != IDYES) {
                return;
            }
            *existing = *profile;
        } else {
            if (create_options.compression_profiles.size() >= 32) {
                show_message_dialog(
                    window_, instance_, dpi_, palette_.dark, L"Compression profiles",
                    L"A maximum of 32 user compression profiles can be saved.",
                    MessageDialogIcon::warning);
                return;
            }
            create_options.compression_profiles.push_back(*profile);
        }
        create_options.compression_profiles_changed = true;
        rebuild_compression_profile_combo(false);
        const std::size_t index = built_in_compression_profiles().size() +
            static_cast<std::size_t>(std::distance(
                create_options.compression_profiles.begin(),
                std::find_if(create_options.compression_profiles.begin(),
                             create_options.compression_profiles.end(),
                             [&](const CompressionProfile& value) {
                                 return profile_names_equal(value.name, name);
                             })));
        SendMessageW(compression_profile_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(index), 0);
        update_compression_profile_actions();
    }

    void delete_compression_profile() {
        const std::wstring name = trim_color_text(window_text(compression_profile_combo_));
        const auto profile = std::find_if(
            create_options.compression_profiles.begin(),
            create_options.compression_profiles.end(),
            [&](const CompressionProfile& value) {
                return profile_names_equal(value.name, name);
            });
        if (profile == create_options.compression_profiles.end()) return;
        if (show_message_dialog(
                window_, instance_, dpi_, palette_.dark, L"Delete compression profile",
                L"Delete the saved profile \"" + profile->name + L"\"?",
                MessageDialogIcon::question, MessageDialogButtons::yes_no,
                IDNO) != IDYES) {
            return;
        }
        create_options.compression_profiles.erase(profile);
        create_options.compression_profiles_changed = true;
        rebuild_compression_profile_combo(false);
    }

    void load_settings_values() {
        set_selected_index(kThemeMode, std::clamp(application_options.theme_mode, 0, 2));
        set_selected_index(kAccentColorMode,
                           std::clamp(application_options.accent_color_mode, 0, 6));
        set_window_text(item(kCustomAccentColor),
                        color_to_hex(application_options.custom_accent_color));
        set_selected_index(kToolbarIconStyle,
                           std::clamp(application_options.toolbar_icon_style, 0, 2));
        set_selected_index(kToolbarDisplayMode,
                           std::clamp(application_options.toolbar_display_mode, 0, 1));
        application_options.toolbar_commands =
            normalize_toolbar_commands(application_options.toolbar_commands);
        refresh_toolbar_settings_list();
        application_options.file_list_columns =
            normalize_file_list_columns(application_options.file_list_columns);
        set_selected_index(kStartupMode,
                           std::clamp(application_options.startup_location_mode, 0, 3));
        set_window_text(item(kStartupCustomPath), application_options.startup_custom_path);
        set_window_text(item(kRecentLocationCount),
                        std::to_wstring(application_options.recent_location_count));

        rebuild_method_combo(item(kCompressionMethod), true,
                             application_options.default_method);
        rebuild_method_controls(
            item(kLevel), item(kThreadModel), application_options.default_method,
            application_options.default_level,
            application_options.default_codec_level,
            application_options.default_thread_model,
            application_options.default_lzma_binary_tree);
        rebuild_codec_parameter_controls(
            item(kDictionarySize), item(kWordSize),
            application_options.default_method,
            application_options.default_dictionary_size,
            application_options.default_word_size);
        SendMessageW(item(kSolidBlockSize), CB_SETCURSEL,
                     value_index(kSolidBlockValues,
                                 application_options.default_solid_block_size), 0);
        set_thread_count(item(kThreads), application_options.default_thread_count);
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
        const auto method = method_from_combo(item(kCompressionMethod));
        const bool dictionary =
            method == axiom::CompressionMethod::axiom ||
            method == axiom::CompressionMethod::lzma2;
        EnableWindow(item(kLevel), method != axiom::CompressionMethod::store);
        EnableWindow(item(kDictionarySize), dictionary);
        EnableWindow(item(kWordSize), dictionary);
        EnableWindow(item(kThreadModel),
                     method == axiom::CompressionMethod::axiom ||
                     method == axiom::CompressionMethod::lzma2);
        EnableWindow(item(kCustomAccentColor),
                     selected_index(kAccentColorMode, 0) == 6);
        EnableWindow(item(kPickAccentColor),
                     selected_index(kAccentColorMode, 0) == 6);
        EnableWindow(item(kStartupCustomPath),
                     selected_index(kStartupMode, 0) == 3);
        EnableWindow(item(kBrowseStartupCustomPath),
                     selected_index(kStartupMode, 0) == 3);
        EnableWindow(item(kArchiveOutputFolder),
                     selected_index(kArchiveOutputMode, 0) == 2);
        EnableWindow(item(kBrowseArchiveOutputFolder),
                     selected_index(kArchiveOutputMode, 0) == 2);
        EnableWindow(item(kExtractDestinationFolder),
                     selected_index(kExtractDestinationMode, 0) == 2);
        EnableWindow(item(kBrowseExtractDestinationFolder),
                     selected_index(kExtractDestinationMode, 0) == 2);
        EnableWindow(item(kTempFolder),
                     selected_index(kTempFolderMode, 0) == 2);
        EnableWindow(item(kBrowseTempFolder),
                     selected_index(kTempFolderMode, 0) == 2);
        EnableWindow(item(kDefaultSigningKey),
                     checkbox_checked(kDefaultSignArchive));
        EnableWindow(item(kBrowseDefaultSigningKey),
                     checkbox_checked(kDefaultSignArchive));
        const std::wstring default_volume =
            trim_dialog_input(window_text(item(kDefaultVolumeSize)));
        const bool default_sfx = checkbox_checked(kDefaultCreateSfx);
        EnableWindow(item(kDefaultVolumeSize), !default_sfx);
        EnableWindow(item(kDefaultVolumeUnit), !default_sfx);
        const bool default_split_enabled =
            !default_volume.empty() &&
            !default_sfx;
        if (!default_split_enabled &&
            application_options.default_recovery_volumes) {
            application_options.default_recovery_volumes = false;
            InvalidateRect(item(kDefaultRecoveryVolumes), nullptr, FALSE);
        }
        EnableWindow(item(kDefaultRecoveryVolumes), default_split_enabled);
        EnableWindow(item(kLogFolder), checkbox_checked(kVerboseLogging));
        EnableWindow(item(kBrowseLogFolder), checkbox_checked(kVerboseLogging));
        EnableWindow(item(kIoBufferSize), selected_index(kIoBufferMode, 0) == 1);
        EnableWindow(item(kMemoryLimit), selected_index(kMemoryLimitMode, 0) == 1);
        layout_settings();
    }

    bool apply_settings_values() {
        if (!commit_shortcut_edit(true)) return false;
        const auto reject_field = [&](int page, int id, const std::wstring& message) {
            select_settings_page(page);
            if (HWND control = item(id)) {
                SetFocus(control);
                SendMessageW(control, EM_SETSEL, 0, -1);
            }
            show_message_dialog(window_, instance_, dpi_, palette_.dark,
                                L"Axiom settings", message,
                                MessageDialogIcon::warning);
            return false;
        };
        const auto recent_count = edit_int(kRecentLocationCount, 0, 50);
        if (!recent_count) {
            return reject_field(0, kRecentLocationCount,
                                L"Recent count must be an integer from 0 through 50.");
        }
        const auto default_threads = thread_count_from(item(kThreads));
        if (!default_threads) {
            return reject_field(
                1, kThreads,
                L"CPU threads must be an integer from 0 through the available logical processor count.");
        }
        const std::wstring default_volume =
            trim_dialog_input(window_text(item(kDefaultVolumeSize)));
        const int default_volume_unit = selected_index(kDefaultVolumeUnit, 2);
        if (!default_volume.empty() &&
            !parse_integer_size_with_unit(default_volume, default_volume_unit)) {
            return reject_field(
                1, kDefaultVolumeSize,
                L"Split volume size must be a positive integer. Select its unit in the adjacent list.");
        }
        if (checkbox_checked(kDefaultRecoveryVolumes) && default_volume.empty()) {
            return reject_field(
                1, kDefaultVolumeSize,
                L"Recovery volumes require a split volume size. Enter a maximum volume size first.");
        }
        const auto recovery_percent = edit_int(kDefaultRecoveryPercent, 0, 100);
        if (!recovery_percent) {
            return reject_field(1, kDefaultRecoveryPercent,
                                L"Recovery must be an integer percentage from 0 through 100.");
        }
        const auto cleanup_days = edit_int(kTempCleanupDays, 0, 365);
        if (!cleanup_days) {
            return reject_field(2, kTempCleanupDays,
                                L"Cleanup days must be an integer from 0 through 365.");
        }
        const std::wstring io_buffer =
            trim_dialog_input(window_text(item(kIoBufferSize)));
        if (selected_index(kIoBufferMode, 0) == 1) {
            const auto size = parse_size_text(io_buffer);
            if (!size || *size < kMinIoBufferSize || *size > kMaxIoBufferSize) {
                return reject_field(
                    10, kIoBufferSize,
                    L"Custom I/O buffer size must be between 64 KiB and 64 MiB. Examples: 1 MiB, 4 MiB, 8388608.");
            }
        }
        const std::wstring memory_limit =
            trim_dialog_input(window_text(item(kMemoryLimit)));
        if (selected_index(kMemoryLimitMode, 0) == 1) {
            const auto size = parse_size_text(memory_limit);
            if (!size || *size < kMinMemoryLimitSize ||
                *size > static_cast<std::uint64_t>(
                            std::numeric_limits<std::size_t>::max())) {
                return reject_field(
                    10, kMemoryLimit,
                    L"Custom memory limit must be at least 64 KiB and fit this build's address space. Examples: 512 MiB or 4 GiB.");
            }
        }
        const std::wstring update_url =
            trim_dialog_input(window_text(item(kUpdateUrl)));
        if (!update_url.empty() && !valid_https_url(update_url)) {
            return reject_field(
                7, kUpdateUrl,
                L"Update URL must be a complete absolute HTTPS URL with a host name, or empty to use Axiom's official release feed.");
        }
        const auto existing_folder = [&](int mode_id, int custom_index,
                                         int path_id, int page,
                                         const wchar_t* label) {
            if (selected_index(mode_id, 0) != custom_index) return true;
            const auto result = validate_dialog_path(
                window_text(item(path_id)), DialogPathKind::existing_folder);
            if (result) {
                set_window_text(item(path_id), result.path.wstring());
                return true;
            }
            return reject_field(
                page, path_id,
                std::wstring(label) + L":\n\n" + result.error);
        };
        if (!existing_folder(kStartupMode, 3, kStartupCustomPath, 0,
                             L"Custom startup folder is not valid") ||
            !existing_folder(kArchiveOutputMode, 2, kArchiveOutputFolder, 2,
                             L"Custom archive output folder is not valid") ||
            !existing_folder(kExtractDestinationMode, 2, kExtractDestinationFolder, 2,
                             L"Custom extraction folder is not valid") ||
            !existing_folder(kTempFolderMode, 2, kTempFolder, 2,
                             L"Custom temporary folder is not valid")) {
            return false;
        }
        if (checkbox_checked(kDefaultSignArchive)) {
            const auto result = validate_dialog_path(
                window_text(item(kDefaultSigningKey)),
                DialogPathKind::existing_file);
            if (!result || !valid_signing_secret_key(result.path)) {
                return reject_field(
                    1, kDefaultSigningKey,
                    !result
                        ? L"Default signing key is not valid:\n\n" + result.error
                        : L"Default signing key must be a 64-byte Axiom secret key. Public keys and unrelated files cannot sign archives.");
            }
            set_window_text(item(kDefaultSigningKey), result.path.wstring());
        }
        const auto optional_file = [&](int path_id, int page,
                                       const wchar_t* label) {
            const std::wstring text =
                trim_dialog_input(window_text(item(path_id)));
            if (text.empty()) return true;
            const auto result =
                validate_dialog_path(text, DialogPathKind::existing_file);
            if (!result) {
                return reject_field(
                    page, path_id,
                    std::wstring(label) + L":\n\n" + result.error);
            }
            set_window_text(item(path_id), result.path.wstring());
            return true;
        };
        const auto optional_folder = [&](int path_id, int page,
                                         const wchar_t* label,
                                         DialogPathKind kind) {
            const std::wstring text =
                trim_dialog_input(window_text(item(path_id)));
            if (text.empty()) return true;
            const auto result = validate_dialog_path(text, kind);
            if (!result) {
                return reject_field(
                    page, path_id,
                    std::wstring(label) + L":\n\n" + result.error);
            }
            set_window_text(item(path_id), result.path.wstring());
            return true;
        };
        if (!optional_file(kExternalViewer, 4,
                           L"External viewer is not a valid executable or file") ||
            !optional_file(kExternalEditor, 4,
                           L"External editor is not a valid executable or file") ||
            !optional_folder(kTrustedKeysFolder, 5,
                             L"Trusted keys folder is not valid",
                             DialogPathKind::existing_folder) ||
            (checkbox_checked(kVerboseLogging) &&
             !optional_folder(kLogFolder, 10,
                              L"Log folder is not valid",
                              DialogPathKind::destination_folder))) {
            return false;
        }
        application_options.theme_mode = selected_index(kThemeMode, 0);
        application_options.accent_color_mode = selected_index(kAccentColorMode, 0);
        if (const auto color = color_from_hex(window_text(item(kCustomAccentColor)))) {
            application_options.custom_accent_color = *color;
            set_window_text(item(kCustomAccentColor), color_to_hex(*color));
        } else if (application_options.accent_color_mode == 6) {
            return reject_field(
                0, kCustomAccentColor,
                L"Custom accent color must use #RRGGBB, for example #FFB93C.");
        } else {
            set_window_text(item(kCustomAccentColor),
                            color_to_hex(application_options.custom_accent_color));
        }
        application_options.toolbar_icon_style = selected_index(kToolbarIconStyle, 0);
        application_options.toolbar_display_mode =
            selected_index(kToolbarDisplayMode, 0);
        application_options.toolbar_commands =
            normalize_toolbar_commands(application_options.toolbar_commands);
        application_options.startup_location_mode = selected_index(kStartupMode, 0);
        application_options.startup_custom_path =
            trim_dialog_input(window_text(item(kStartupCustomPath)));
        application_options.recent_location_count = *recent_count;

        application_options.default_method =
            method_from_combo(item(kCompressionMethod));
        const int selected_method_level =
            level_from_combo(item(kLevel), 5);
        if (application_options.default_method ==
            axiom::CompressionMethod::axiom) {
            application_options.default_level =
                std::clamp(selected_method_level, 1, 9);
            application_options.default_codec_level =
                axiom::kAutomaticCodecLevel;
        } else {
            application_options.default_codec_level = selected_method_level;
        }
        application_options.default_dictionary_size =
            value_from_combo(item(kDictionarySize));
        application_options.default_word_size =
            value_from_combo(item(kWordSize));
        application_options.default_solid_block_size =
            selected_combo_value(item(kSolidBlockSize), kSolidBlockValues);
        if (application_options.default_method ==
            axiom::CompressionMethod::axiom) {
            application_options.default_thread_model =
                selected_index(kThreadModel, 0);
        } else if (application_options.default_method ==
                   axiom::CompressionMethod::lzma2) {
            application_options.default_lzma_binary_tree =
                selected_index(kThreadModel, 1) == 1;
        }
        application_options.default_thread_count = *default_threads;
        application_options.default_update_mode = selected_index(kDefaultUpdateMode, 0);
        application_options.default_volume_size = default_volume;
        application_options.default_volume_unit = default_volume_unit;
        application_options.default_recovery_percent = *recovery_percent;
        application_options.default_signing_key =
            trim_dialog_input(window_text(item(kDefaultSigningKey)));

        application_options.archive_output_mode = selected_index(kArchiveOutputMode, 0);
        application_options.archive_output_folder =
            trim_dialog_input(window_text(item(kArchiveOutputFolder)));
        application_options.extract_destination_mode =
            selected_index(kExtractDestinationMode, 0);
        application_options.extract_destination_folder =
            trim_dialog_input(window_text(item(kExtractDestinationFolder)));
        application_options.temp_folder_mode = selected_index(kTempFolderMode, 0);
        application_options.temp_folder =
            trim_dialog_input(window_text(item(kTempFolder)));
        application_options.temp_cleanup_days = *cleanup_days;

        application_options.file_open_mode = selected_index(kFileOpenMode, 0);
        application_options.external_viewer =
            trim_dialog_input(window_text(item(kExternalViewer)));
        application_options.external_editor =
            trim_dialog_input(window_text(item(kExternalEditor)));

        application_options.password_prompt_mode = selected_index(kPasswordPromptMode, 0);
        application_options.trusted_keys_folder =
            trim_dialog_input(window_text(item(kTrustedKeysFolder)));

        application_options.update_channel = selected_index(kUpdateChannel, 0);
        application_options.update_url = update_url;

        application_options.worker_priority = selected_index(kWorkerPriority, 0);
        application_options.log_folder =
            trim_dialog_input(window_text(item(kLogFolder)));
        application_options.io_buffer_mode = selected_index(kIoBufferMode, 0);
        application_options.io_buffer_size = io_buffer;
        application_options.memory_limit_mode = selected_index(kMemoryLimitMode, 0);
        application_options.memory_limit = memory_limit;
        return true;
    }

    void refresh_settings_appearance() {
        if (mode_ != DialogMode::settings || window_ == nullptr) return;
        set_dialog_appearance({
            application_options.theme_mode,
            application_options.accent_color_mode,
            application_options.custom_accent_color,
            application_options.toolbar_icon_style,
            application_options.center_child_windows,
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
            // Owner-drawn hidden combo boxes can still send WM_DRAWITEM while
            // their native theme is reset. Defer those pages until they are
            // selected so they cannot paint onto the active page's surface.
            if (IsWindowVisible(child)) {
                apply_dialog_control_theme(child, self->palette_.dark);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(this));
        if (toolbar_list_.hwnd() != nullptr) {
            toolbar_list_.set_theme(toolbar_table_theme());
            rebuild_toolbar_settings_image_list();
            refresh_toolbar_settings_list();
        }
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
        tooltip_.apply_theme(palette_.dark);
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
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
        const int label_width = scale(180);
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
        MoveWindow(cancel_, client.right - margin - button_width, button_y, button_width, row_height, TRUE);
        MoveWindow(accept_, client.right - margin - button_width * 2 - scale(8),
                   button_y, button_width, row_height, TRUE);
        tooltip_.update_layout();
    }

    void layout_settings() {
        if (settings_viewport_ == nullptr) return;
        RECT client{};
        GetClientRect(window_, &client);
        const SIZE client_size{
            static_cast<LONG>(client.right - client.left),
            static_cast<LONG>(client.bottom - client.top)};
        const bool resized =
            client_size.cx != settings_layout_client_size_.cx ||
            client_size.cy != settings_layout_client_size_.cy;
        settings_layout_client_size_ = client_size;
        if (resized) {
            // Moving a clipped child viewport can otherwise make USER32 copy
            // pixels from its former client area into the newly exposed
            // footer. Suppress intermediate paints and produce one complete
            // frame after every live-resize layout.
            SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
        }
        const int margin = scale(14);
        const int row = scale(30);
        const int button_width = scale(86);
        const int button_y = client.bottom - margin - row;
        HDWP footer = BeginDeferWindowPos(5);
        footer = DeferWindowPos(footer, defaults_, nullptr,
                                margin, button_y, scale(104), row,
                                SWP_NOZORDER | SWP_NOACTIVATE |
                                    SWP_NOCOPYBITS);
        footer = DeferWindowPos(footer, cancel_, nullptr,
                                client.right - margin - button_width, button_y,
                                button_width, row,
                                SWP_NOZORDER | SWP_NOACTIVATE |
                                    SWP_NOCOPYBITS);
        footer = DeferWindowPos(footer, apply_, nullptr,
                                client.right - margin - button_width * 2 - scale(8),
                                button_y, button_width, row,
                                SWP_NOZORDER | SWP_NOACTIVATE |
                                    SWP_NOCOPYBITS);
        footer = DeferWindowPos(footer, accept_, nullptr,
                                client.right - margin - button_width * 3 - scale(16),
                                button_y, button_width, row,
                                SWP_NOZORDER | SWP_NOACTIVATE |
                                    SWP_NOCOPYBITS);

        const int bottom_limit = button_y - scale(12);
        const int nav_width = scale(154);
        const int nav_gap = scale(16);
        footer = DeferWindowPos(
            footer, settings_tabs_, nullptr, margin, margin, nav_width,
            std::max(row, bottom_limit - margin),
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        if (footer != nullptr) EndDeferWindowPos(footer);

        const int content_left = margin + nav_width + nav_gap;
        const int viewport_width = std::max(
            scale(300), static_cast<int>(client.right) - margin - content_left);
        const int viewport_height = std::max(row, bottom_limit - margin);
        SetWindowPos(settings_viewport_, nullptr, content_left, margin,
                     viewport_width, viewport_height,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);

        RECT viewport{};
        GetClientRect(settings_viewport_, &viewport);
        const int scrollbar_gutter = scale(16);
        const int usable_width = std::max(
            scale(260), static_cast<int>(viewport.right) - scrollbar_gutter);
        const int design_width = scale(760);
        const int horizontal_numerator = std::min(usable_width, design_width);

        const bool custom_accent = selected_index(kAccentColorMode, 0) == 6;
        const bool custom_startup = selected_index(kStartupMode, 0) == 3;
        const bool custom_archive = selected_index(kArchiveOutputMode, 0) == 2;
        const bool custom_extract =
            selected_index(kExtractDestinationMode, 0) == 2;
        const bool custom_temp = selected_index(kTempFolderMode, 0) == 2;
        const bool signing_enabled = checkbox_checked(kDefaultSignArchive);
        const bool custom_io = selected_index(kIoBufferMode, 0) == 1;
        const bool custom_memory = selected_index(kMemoryLimitMode, 0) == 1;

        struct Placement {
            const SettingControl* control = nullptr;
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            int extent_height = 0;
            bool visible = false;
        };
        std::vector<Placement> placements;
        placements.reserve(settings_controls_.size());
        int content_height = 0;

        const auto is_combo = [](HWND window) {
            wchar_t class_name[32]{};
            GetClassNameW(window, class_name,
                          static_cast<int>(std::size(class_name)));
            return lstrcmpiW(class_name, L"ComboBox") == 0;
        };

        for (const SettingControl& control : settings_controls_) {
            bool dependency_visible = true;
            int compact_y = control.y;
            const int id = GetDlgCtrlID(control.window);
            if (control.page == 0) {
                if (!custom_accent) {
                    if (control.y >= 120 && control.y <= 150) {
                        dependency_visible = false;
                    } else if (control.y > 150) {
                        compact_y -= 42;
                    }
                }
                if (!custom_startup) {
                    if (control.y >= 246 && control.y <= 276) {
                        dependency_visible = false;
                    } else if (control.y > 276) {
                        compact_y -= 42;
                    }
                }
            } else if (control.page == 1 && !signing_enabled &&
                       control.y >= 518 && control.y <= 548) {
                dependency_visible = false;
            } else if (control.page == 2) {
                if (!custom_archive) {
                    if (control.y >= 78 && control.y <= 108) {
                        dependency_visible = false;
                    } else if (control.y > 108) {
                        compact_y -= 42;
                    }
                }
                if (!custom_extract) {
                    if (control.y >= 162 && control.y <= 192) {
                        dependency_visible = false;
                    } else if (control.y > 192) {
                        compact_y -= 42;
                    }
                }
                if (!custom_temp) {
                    if (control.y >= 246 && control.y <= 276) {
                        dependency_visible = false;
                    } else if (control.y > 276) {
                        compact_y -= 42;
                    }
                }
            } else if (control.page == 10) {
                if (id == kIoBufferSize && !custom_io) {
                    dependency_visible = false;
                }
                if (id == kMemoryLimit && !custom_memory) {
                    dependency_visible = false;
                }
            }

            int x = MulDiv(scale(control.x), horizontal_numerator, design_width);
            int y = MulDiv(scale(compact_y), 9, 10);
            int width = MulDiv(scale(control.width),
                               horizontal_numerator, design_width);
            width = std::min(width, std::max(scale(64), usable_width - x));
            int height = scale(control.height);
            int extent_height = is_combo(control.window)
                ? row : height;

            if (control.window == toolbar_list_.hwnd()) {
                x = 0;
                y = scale(110);
                width = usable_width;
                height = std::max(
                    scale(150), static_cast<int>(viewport.bottom) -
                                    y - scale(48));
                extent_height = height;
            } else if (id == kToolbarResetDefaults) {
                x = 0;
                y = std::max(scale(226),
                             static_cast<int>(viewport.bottom) - scale(34));
                width = std::min(scale(180), usable_width);
                height = row;
                extent_height = row;
            } else if (control.wrapped) {
                height = wrapped_height(control.window, width, height);
                extent_height = height;
            }

            const bool visible =
                control.page == settings_page_ && dependency_visible;
            placements.push_back({
                &control, x, y, width, height, extent_height, visible});
            if (control.page == settings_page_ && dependency_visible) {
                content_height = std::max(content_height, y + extent_height);
            }
        }

        if (settings_page_ == 9) {
            content_height = viewport.bottom;
        } else {
            content_height += scale(10);
        }
        const SettingsViewportMetrics metrics{
            content_height, scale(32)};
        SendMessageW(settings_viewport_, kSettingsViewportSetMetrics, 0,
                     reinterpret_cast<LPARAM>(&metrics));
        int scroll_offset = static_cast<int>(
            SendMessageW(settings_viewport_, kSettingsViewportGetOffset, 0, 0));
        const int desired_offset =
            settings_scroll_offsets_[static_cast<std::size_t>(settings_page_)];
        if (desired_offset != scroll_offset) {
            SendMessageW(settings_viewport_, kSettingsViewportScrollBy,
                         static_cast<WPARAM>(desired_offset - scroll_offset), 0);
            scroll_offset = static_cast<int>(
                SendMessageW(settings_viewport_,
                             kSettingsViewportGetOffset, 0, 0));
        }
        settings_scroll_offsets_[static_cast<std::size_t>(settings_page_)] =
            scroll_offset;

        SendMessageW(settings_viewport_, WM_SETREDRAW, FALSE, 0);
        HDWP controls = BeginDeferWindowPos(
            static_cast<int>(placements.size()));
        for (const Placement& placement : placements) {
            const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE |
                SWP_NOCOPYBITS |
                (placement.visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
            controls = DeferWindowPos(
                controls, placement.control->window, nullptr,
                placement.x, placement.y - scroll_offset,
                placement.width, placement.height, flags);
        }
        if (controls != nullptr) EndDeferWindowPos(controls);
        SendMessageW(settings_viewport_, WM_SETREDRAW, TRUE, 0);
        sync_toolbar_status_combo();
        if (resized) {
            SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(window_, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                             RDW_ALLCHILDREN | RDW_UPDATENOW);
        } else {
            RedrawWindow(settings_viewport_, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        }
        tooltip_.update_layout();
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
        if (window_ == nullptr || accept_ == nullptr ||
            create_navigation_ == nullptr || create_viewport_ == nullptr) {
            return;
        }

        SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
        RECT client{};
        GetClientRect(window_, &client);
        const int margin = scale(14);
        const int row = scale(30);
        const int gap = scale(8);
        const int browse_width = scale(86);
        const int label_width = scale(170);

        // Keep the archive identity and output controls fixed while the page
        // content scrolls. The format selector shares the first header row
        // with the item count to reclaim the vertical space used by the old
        // three-row header and horizontal tab strip.
        const int header_y = margin;
        const int format_width = scale(240);
        const int format_label_width = scale(62);
        const int format_x = client.right - margin - format_width;
        MoveWindow(summary_, margin, header_y,
                   std::max(scale(180), format_x - margin - gap -
                            format_label_width), row, TRUE);
        MoveWindow(format_label_, format_x - gap - format_label_width,
                   header_y + scale(6), format_label_width, row, TRUE);
        MoveWindow(format_combo_, format_x, header_y, format_width,
                   scale(240), TRUE);

        const int path_y = header_y + row + gap;
        const int path_label_width = scale(112);
        const int path_edit_x = margin + path_label_width + gap;
        MoveWindow(path_label_, margin, path_y + scale(6), path_label_width,
                   row, TRUE);
        MoveWindow(path_edit_, path_edit_x, path_y,
                   std::max(scale(120), static_cast<int>(client.right) -
                            margin - browse_width - gap - path_edit_x),
                   row, TRUE);
        MoveWindow(browse_, client.right - margin - browse_width, path_y,
                   browse_width, row, TRUE);

        const int button_y = client.bottom - margin - row;
        const int button_width = scale(86);
        MoveWindow(cancel_, client.right - margin - button_width, button_y,
                   button_width, row, TRUE);
        MoveWindow(accept_, client.right - margin - button_width * 2 - gap,
                   button_y, button_width, row, TRUE);

        const int page_top = path_y + row + scale(12);
        const int page_bottom = button_y - scale(12);
        const int navigation_width = scale(154);
        const int navigation_gap = scale(14);
        const int viewport_x = margin + navigation_width + navigation_gap;
        const int page_height = std::max(row, page_bottom - page_top);
        const int viewport_width = std::max(
            scale(300), static_cast<int>(client.right) - margin - viewport_x);
        MoveWindow(create_navigation_, margin, page_top, navigation_width,
                   page_height, TRUE);
        MoveWindow(create_viewport_, viewport_x, page_top, viewport_width,
                   page_height, TRUE);

        RECT viewport{};
        GetClientRect(create_viewport_, &viewport);
        const int scrollbar_gutter = scale(16);
        const int content_width = std::max(
            scale(260), static_cast<int>(viewport.right) - scrollbar_gutter);
        const int content_top = scale(42);

        const auto move_page_control = [&](HWND control, int x, int y,
                                           int width, int height,
                                           int scroll_offset) {
            if (control == nullptr) return;
            MoveWindow(control, x, y - scroll_offset, width, height, TRUE);
        };
        const auto move_page_wrapped = [&](HWND control, int x, int& y,
                                           int width, int minimum,
                                           int scroll_offset, int trailing_gap) {
            const int height = wrapped_height(control, width, scale(minimum));
            move_page_control(control, x, y, width, height, scroll_offset);
            y += height + scale(trailing_gap);
        };

        const auto layout_page = [&](int scroll_offset) {
            move_page_control(create_page_heading_, 0, 0, content_width,
                              row, scroll_offset);
            int y = content_top;
            int content_height = content_top;
            const int value_x = label_width;

            const auto row_pair = [&](HWND label_window, HWND value,
                                      int value_width = 340,
                                      bool combo = true) {
                move_page_control(label_window, 0, y + scale(6), label_width,
                                  row, scroll_offset);
                move_page_control(value, value_x, y,
                                  std::min(scale(value_width),
                                           std::max(scale(120),
                                                    content_width - value_x)),
                                  combo ? scale(240) : row, scroll_offset);
                y += row + gap;
            };

            switch (create_page_) {
                case 0: {
                    const int preview_gap = scale(14);
                    const bool show_preview = content_width >= scale(760);
                    int form_width = content_width;
                    if (show_preview) {
                        form_width = std::min(
                            scale(460), content_width - preview_gap - scale(270));
                    }
                    const int preview_width = content_width - form_width - preview_gap;
                    const int compression_label_width = std::min(
                        label_width, std::max(scale(140),
                                             form_width - scale(250)));

                    if (show_preview && compression_preview_ != nullptr) {
                        move_page_control(compression_preview_,
                                          form_width + preview_gap, content_top,
                                          std::max(scale(240), preview_width),
                                          scale(360), scroll_offset);
                        ShowWindow(compression_preview_, SW_SHOWNA);
                    } else if (compression_preview_ != nullptr) {
                        ShowWindow(compression_preview_, SW_HIDE);
                    }

                    move_page_control(compression_profile_label_, 0,
                                      y + scale(6), compression_label_width,
                                      row, scroll_offset);
                    const int action_width = scale(56);
                    const int action_gap = scale(6);
                    const int profile_value_x = compression_label_width;
                    const auto compression_value_width =
                        [&](int requested_width = 260) {
                            return std::min(
                                scale(requested_width),
                                std::max(scale(120),
                                         form_width - compression_label_width));
                        };
                    // Keep the editable profile field aligned with every other
                    // compression value. Its actions use a compact row below it
                    // instead of squeezing the field beside two buttons.
                    const int profile_combo_width = compression_value_width();
                    move_page_control(compression_profile_combo_,
                                      profile_value_x, y, profile_combo_width,
                                      scale(260), scroll_offset);
                    const int profile_action_width = action_width * 2 + action_gap;
                    const int profile_actions_x = profile_value_x + std::max(
                        0, profile_combo_width - profile_action_width);
                    const int profile_actions_y = y + row + gap;
                    move_page_control(save_compression_profile_,
                                      profile_actions_x, profile_actions_y,
                                      action_width, row, scroll_offset);
                    move_page_control(delete_compression_profile_,
                                      profile_actions_x + action_width + action_gap,
                                      profile_actions_y, action_width, row,
                                      scroll_offset);
                    y = profile_actions_y + row + gap;

                    const auto compression_row = [&](HWND label_window,
                                                     HWND value,
                                                     int value_width = 260) {
                        move_page_control(label_window, 0, y + scale(6),
                                          compression_label_width, row,
                                          scroll_offset);
                        move_page_control(value, compression_label_width, y,
                                          compression_value_width(value_width),
                                          scale(240), scroll_offset);
                        y += row + gap;
                    };
                    compression_row(method_label_, method_combo_);
                    compression_row(level_label_, level_combo_);
                    compression_row(dictionary_label_, dictionary_combo_);
                    compression_row(word_size_label_, word_size_combo_);
                    compression_row(solid_block_label_, solid_block_combo_);
                    compression_row(threads_label_, threads_combo_);
                    compression_row(thread_model_label_, thread_model_combo_);
                    y += scale(4);
                    move_page_wrapped(compression_info_, 0, y,
                                      show_preview ? form_width : content_width,
                                      54, scroll_offset, 10);
                    content_height = std::max(
                        y, show_preview ? content_top + scale(360) : y);
                    break;
                }
                case 1: {
                    row_pair(update_mode_label_, update_mode_combo_, 340);
                    move_page_control(comment_label_, 0, y + scale(5),
                                      label_width, row, scroll_offset);
                    const int comment_height = scale(86);
                    move_page_control(comment_edit_, value_x, y,
                                      std::max(scale(160), content_width - value_x),
                                      comment_height, scroll_offset);
                    y += comment_height + gap;
                    move_page_control(lock_archive_, value_x, y,
                                      std::max(scale(160), content_width - value_x),
                                      row, scroll_offset);
                    y += row + scale(4);
                    move_page_control(repack_after_update_, value_x, y,
                                      std::max(scale(160), content_width - value_x),
                                      row, scroll_offset);
                    y += row + scale(12);
                    move_page_control(metadata_heading_, 0, y, content_width,
                                      row, scroll_offset);
                    y += row + scale(4);
                    move_page_wrapped(metadata_info_, 0, y, content_width,
                                      38, scroll_offset, 10);
                    content_height = y;
                    break;
                }
                case 2: {
                    move_page_control(encrypt_data_, 0, y, content_width, row,
                                      scroll_offset);
                    y += row + scale(4);
                    move_page_control(encrypt_names_, 0, y, content_width, row,
                                      scroll_offset);
                    y += row + gap;
                    row_pair(password_label_, password_edit_, 340, false);
                    row_pair(confirm_password_label_, confirm_password_edit_,
                             340, false);
                    move_page_control(show_password_, value_x, y,
                                      std::max(scale(160), content_width - value_x),
                                      row, scroll_offset);
                    y += row + scale(12);
                    move_page_control(sign_archive_, 0, y, content_width, row,
                                      scroll_offset);
                    y += row + gap;
                    move_page_control(signing_key_label_, 0, y + scale(6),
                                      label_width, row, scroll_offset);
                    const int signing_edit_width = std::max(
                        scale(120), content_width - value_x - browse_width - gap);
                    move_page_control(signing_key_edit_, value_x, y,
                                      signing_edit_width, row, scroll_offset);
                    move_page_control(browse_signing_key_,
                                      value_x + signing_edit_width + gap, y,
                                      browse_width, row, scroll_offset);
                    y += row + scale(12);
                    move_page_wrapped(security_info_, 0, y, content_width,
                                      46, scroll_offset, 10);
                    content_height = y;
                    break;
                }
                case 3: {
                    move_page_control(volume_size_label_, 0, y + scale(6),
                                      label_width, row, scroll_offset);
                    const int volume_edit_width = scale(160);
                    move_page_control(volume_size_edit_, value_x, y,
                                      volume_edit_width, row, scroll_offset);
                    move_page_control(volume_unit_combo_,
                                      value_x + volume_edit_width + gap, y,
                                      scale(105), scale(180), scroll_offset);
                    y += row + gap;
                    move_page_control(recovery_percent_label_, 0, y + scale(6),
                                      label_width, row, scroll_offset);
                    const int recovery_edit_width = scale(80);
                    move_page_control(recovery_percent_edit_, value_x, y,
                                      recovery_edit_width, row, scroll_offset);
                    move_page_control(recovery_percent_suffix_,
                                      value_x + recovery_edit_width + gap, y + scale(6),
                                      scale(170), row, scroll_offset);
                    y += row + scale(10);
                    move_page_control(recovery_volumes_, value_x, y,
                                      std::max(scale(160), content_width - value_x),
                                      row, scroll_offset);
                    y += row + scale(12);
                    move_page_wrapped(recovery_info_, 0, y, content_width,
                                      52, scroll_offset, 10);
                    content_height = y;
                    break;
                }
                case 4: {
                    move_page_control(create_sfx_, 0, y, content_width, row,
                                      scroll_offset);
                    y += row + scale(10);

                    const int sfx_label_width = std::min(
                        label_width, std::max(scale(130),
                                             content_width - scale(120)));
                    const int column_gap = scale(16);
                    const bool two_columns = content_width >=
                        sfx_label_width * 2 + scale(120) * 2 + column_gap;
                    const int column_width = two_columns
                        ? (content_width - column_gap) / 2
                        : content_width;
                    const int sfx_value_width = column_width - sfx_label_width;
                    const int right_left = column_width + column_gap;
                    const int form_top = y;
                    const int pitch = row + gap;

                    const auto cell = [&](HWND label_window, HWND value,
                                          int left, int top, bool combo) {
                        move_page_control(label_window, left, top + scale(6),
                                          sfx_label_width, row, scroll_offset);
                        move_page_control(value, left + sfx_label_width, top,
                                          sfx_value_width,
                                          combo ? scale(240) : row,
                                          scroll_offset);
                    };

                    int left_y = form_top;
                    cell(sfx_stub_tier_label_, sfx_stub_tier_combo_, 0, left_y, true);
                    left_y += pitch;
                    cell(sfx_title_label_, sfx_title_edit_, 0, left_y, false);
                    left_y += pitch;
                    cell(sfx_description_label_, sfx_description_edit_, 0,
                         left_y, false);
                    left_y += pitch;
                    cell(sfx_default_path_label_, sfx_default_path_combo_, 0,
                         left_y, true);
                    left_y += pitch;
                    cell(sfx_overwrite_label_, sfx_overwrite_combo_, 0, left_y, true);
                    left_y += pitch;

                    int right_y = two_columns ? form_top : left_y;
                    const int right_x = two_columns ? right_left : 0;
                    cell(sfx_mode_label_, sfx_mode_combo_, right_x, right_y, true);
                    right_y += pitch;
                    cell(sfx_elevation_label_, sfx_elevation_combo_, right_x,
                         right_y, true);
                    right_y += pitch;
                    cell(sfx_run_program_label_, sfx_run_program_edit_, right_x,
                         right_y, false);
                    right_y += pitch;
                    cell(sfx_run_arguments_label_, sfx_run_arguments_edit_, right_x,
                         right_y, false);
                    right_y += pitch;
                    cell(sfx_theme_label_, sfx_theme_combo_, right_x, right_y, true);
                    right_y += pitch;
                    y = std::max(left_y, right_y) + scale(4);

                    move_page_control(sfx_license_label_, 0, y + scale(6),
                                      sfx_label_width, row, scroll_offset);
                    const int license_height = scale(88);
                    move_page_control(sfx_license_edit_, sfx_label_width, y,
                                      std::max(scale(160), content_width -
                                               sfx_label_width),
                                      license_height, scroll_offset);
                    y += license_height + gap;

                    const int checkbox_gap = scale(8);
                    const int checkbox_width = content_width >= scale(600)
                        ? (content_width - checkbox_gap) / 2
                        : content_width;
                    move_page_control(sfx_allow_path_change_, 0, y,
                                      checkbox_width, row, scroll_offset);
                    move_page_control(sfx_require_accept_,
                                      checkbox_width + checkbox_gap, y,
                                      checkbox_width, row, scroll_offset);
                    y += row + scale(4);
                    move_page_control(sfx_open_destination_, 0, y,
                                      checkbox_width, row, scroll_offset);
                    y += row + scale(10);
                    content_height = y;
                    break;
                }
                case 5: {
                    move_page_control(content_dedup_, 0, y, content_width, row,
                                      scroll_offset);
                    y += row + scale(14);
                    row_pair(dedup_min_chunk_label_, dedup_min_chunk_edit_,
                             240, false);
                    row_pair(dedup_average_chunk_label_,
                             dedup_average_chunk_edit_, 240, false);
                    row_pair(dedup_max_chunk_label_, dedup_max_chunk_edit_,
                             240, false);
                    y += scale(8);
                    move_page_wrapped(dedup_info_, 0, y, content_width,
                                      72, scroll_offset, 10);
                    content_height = y;
                    break;
                }
            }
            return std::max(content_height, y + scale(10));
        };

        int content_height = layout_page(0);
        const SettingsViewportMetrics metrics{content_height, scale(32)};
        SendMessageW(create_viewport_, kSettingsViewportSetMetrics, 0,
                     reinterpret_cast<LPARAM>(&metrics));
        const int current_offset = static_cast<int>(SendMessageW(
            create_viewport_, kSettingsViewportGetOffset, 0, 0));
        const int desired_offset = create_scroll_offsets_[
            static_cast<std::size_t>(create_page_)];
        if (desired_offset != current_offset) {
            SendMessageW(create_viewport_, kSettingsViewportScrollBy,
                         static_cast<WPARAM>(desired_offset - current_offset), 0);
        }
        const int scroll_offset = static_cast<int>(SendMessageW(
            create_viewport_, kSettingsViewportGetOffset, 0, 0));
        if (scroll_offset != 0) {
            content_height = layout_page(scroll_offset);
        }
        create_scroll_offsets_[static_cast<std::size_t>(create_page_)] =
            scroll_offset;

        SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
        tooltip_.update_layout();
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
        }
    }

    void pick_settings_accent_color() {
        COLORREF initial = application_options.custom_accent_color;
        if (const auto parsed =
                color_from_hex(window_text(item(kCustomAccentColor)))) {
            initial = *parsed;
        }
        if (const auto selected = choose_accent_color(window_, initial)) {
            application_options.custom_accent_color = *selected;
            set_window_text(item(kCustomAccentColor), color_to_hex(*selected));
            set_selected_index(kAccentColorMode, 6);
            InvalidateRect(item(kCustomAccentColor), nullptr, TRUE);
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

    std::optional<std::size_t> thread_count() const {
        return thread_count_from(threads_combo_);
    }

    static axiom::CompressionMethod method_from_combo(
        HWND combo,
        axiom::CompressionMethod fallback = axiom::CompressionMethod::axiom) {
        if (combo == nullptr) return fallback;
        const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        if (selection == CB_ERR) return fallback;
        const LRESULT value = SendMessageW(
            combo, CB_GETITEMDATA, static_cast<WPARAM>(selection), 0);
        return value >= 0 && value <= 4
            ? static_cast<axiom::CompressionMethod>(value)
            : fallback;
    }

    static int level_from_combo(HWND combo, int fallback) {
        if (combo == nullptr) return fallback;
        const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        if (selection == CB_ERR) return fallback;
        const LRESULT value = SendMessageW(
            combo, CB_GETITEMDATA, static_cast<WPARAM>(selection), 0);
        return static_cast<int>(value);
    }

    static std::size_t value_from_combo(HWND combo) {
        if (combo == nullptr) return 0;
        const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        if (selection == CB_ERR) return 0;
        const LRESULT value = SendMessageW(
            combo, CB_GETITEMDATA, static_cast<WPARAM>(selection), 0);
        return value == CB_ERR || value < 0 ? 0 : static_cast<std::size_t>(value);
    }

    static void select_combo_value(HWND combo, std::size_t wanted) {
        if (combo == nullptr) return;
        LRESULT selected = 0;
        const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
        for (LRESULT index = 0; index < count; ++index) {
            if (SendMessageW(combo, CB_GETITEMDATA,
                             static_cast<WPARAM>(index), 0) ==
                static_cast<LRESULT>(wanted)) {
                selected = index;
                break;
            }
        }
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    }

    void rebuild_method_combo(HWND combo, bool native,
                              axiom::CompressionMethod selected) const {
        if (combo == nullptr) return;
        SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        const auto add = [&](axiom::CompressionMethod method) {
            const auto index = SendMessageW(
                combo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(
                    kCompressionMethodNames[static_cast<std::size_t>(method)]));
            SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index),
                         static_cast<LPARAM>(method));
            return index;
        };
        if (native) {
            for (int value = 0; value <= 4; ++value) {
                add(static_cast<axiom::CompressionMethod>(value));
            }
        } else {
            add(axiom::CompressionMethod::deflate);
            add(axiom::CompressionMethod::store);
            if (selected != axiom::CompressionMethod::deflate &&
                selected != axiom::CompressionMethod::store) {
                selected = axiom::CompressionMethod::deflate;
            }
        }
        for (LRESULT index = 0;
             index < SendMessageW(combo, CB_GETCOUNT, 0, 0); ++index) {
            if (SendMessageW(combo, CB_GETITEMDATA,
                             static_cast<WPARAM>(index), 0) ==
                static_cast<LRESULT>(selected)) {
                SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
                break;
            }
        }
        SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(combo, nullptr, TRUE);
    }

    void rebuild_method_controls(HWND level_combo, HWND option_combo,
                                 axiom::CompressionMethod method,
                                 int portable_level, int codec_level,
                                 int thread_model, bool lzma_binary_tree) const {
        SendMessageW(level_combo, WM_SETREDRAW, FALSE, 0);
        SendMessageW(level_combo, CB_RESETCONTENT, 0, 0);
        const auto add_level = [&](const std::wstring& text, int value) {
            const auto index = SendMessageW(
                level_combo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(text.c_str()));
            SendMessageW(level_combo, CB_SETITEMDATA,
                         static_cast<WPARAM>(index), value);
            return index;
        };

        int wanted = codec_level;
        if (method == axiom::CompressionMethod::axiom) {
            wanted = std::clamp(portable_level, 1, 9);
            for (int level = 1; level <= 9; ++level) {
                add_level(kLevelNames[static_cast<std::size_t>(level - 1)], level);
            }
        } else if (method == axiom::CompressionMethod::zstandard) {
            if (wanted == axiom::kAutomaticCodecLevel) wanted = 3;
            wanted = std::clamp(wanted, -5, 22);
            for (int level = -5; level <= 22; ++level) {
                add_level(L"Level " + std::to_wstring(level), level);
            }
        } else if (method == axiom::CompressionMethod::lzma2 ||
                   method == axiom::CompressionMethod::deflate) {
            if (wanted == axiom::kAutomaticCodecLevel) wanted = 5;
            wanted = std::clamp(wanted, 0, 9);
            for (int level = 0; level <= 9; ++level) {
                add_level(L"Level " + std::to_wstring(level), level);
            }
        } else {
            wanted = 0;
            add_level(L"No compression", 0);
        }
        for (LRESULT index = 0;
             index < SendMessageW(level_combo, CB_GETCOUNT, 0, 0); ++index) {
            if (SendMessageW(level_combo, CB_GETITEMDATA,
                             static_cast<WPARAM>(index), 0) == wanted) {
                SendMessageW(level_combo, CB_SETCURSEL,
                             static_cast<WPARAM>(index), 0);
                break;
            }
        }
        SendMessageW(level_combo, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(level_combo, nullptr, TRUE);

        SendMessageW(option_combo, WM_SETREDRAW, FALSE, 0);
        SendMessageW(option_combo, CB_RESETCONTENT, 0, 0);
        if (method == axiom::CompressionMethod::axiom) {
            for (const auto* name : kThreadModelNames) {
                SendMessageW(option_combo, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(name));
            }
            SendMessageW(option_combo, CB_SETCURSEL,
                         static_cast<WPARAM>(std::clamp(thread_model, 0, 1)), 0);
        } else if (method == axiom::CompressionMethod::lzma2) {
            for (const auto* name : kLzmaMatchFinderNames) {
                SendMessageW(option_combo, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(name));
            }
            SendMessageW(option_combo, CB_SETCURSEL,
                         lzma_binary_tree ? 1 : 0, 0);
        } else {
            SendMessageW(option_combo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(L"Not applicable"));
            SendMessageW(option_combo, CB_SETCURSEL, 0, 0);
        }
        SendMessageW(option_combo, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(option_combo, nullptr, TRUE);
    }

    void rebuild_codec_parameter_controls(
        HWND dictionary_combo, HWND word_combo,
        axiom::CompressionMethod method,
        std::size_t selected_dictionary,
        std::size_t selected_word) const {
        if (dictionary_combo == nullptr || word_combo == nullptr) return;
        SendMessageW(dictionary_combo, WM_SETREDRAW, FALSE, 0);
        SendMessageW(word_combo, WM_SETREDRAW, FALSE, 0);
        SendMessageW(dictionary_combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(word_combo, CB_RESETCONTENT, 0, 0);

        const auto add = [](HWND combo, const wchar_t* text, std::size_t value) {
            const LRESULT index = SendMessageW(
                combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
            if (index != CB_ERR && index != CB_ERRSPACE) {
                SendMessageW(combo, CB_SETITEMDATA,
                             static_cast<WPARAM>(index),
                             static_cast<LPARAM>(value));
            }
        };

        if (method == axiom::CompressionMethod::axiom) {
            for (std::size_t index = 0; index < kDictionaryNames.size(); ++index) {
                add(dictionary_combo, kDictionaryNames[index],
                    kDictionaryValues[index]);
            }
            for (std::size_t index = 0; index < kWordSizeNames.size(); ++index) {
                add(word_combo, kWordSizeNames[index], kWordSizeValues[index]);
            }
        } else if (method == axiom::CompressionMethod::lzma2) {
            constexpr std::array<const wchar_t*, 18> names{
                L"Default for LZMA2 level", L"4 KiB", L"16 KiB", L"64 KiB",
                L"256 KiB", L"1 MiB", L"2 MiB", L"4 MiB", L"8 MiB",
                L"16 MiB", L"32 MiB", L"64 MiB", L"128 MiB", L"256 MiB",
                L"512 MiB", L"1 GiB", L"2 GiB", L"4 GiB"};
            constexpr std::array<std::size_t, 18> values{
                0, 4u << 10, 16u << 10, 64u << 10, 256u << 10,
                1u << 20, 2u << 20, 4u << 20, 8u << 20, 16u << 20,
                32u << 20, 64u << 20, 128u << 20, 256u << 20,
                512u << 20, 1u << 30, 2u << 30,
                axiom::kMaxLzmaDictionarySize};
            constexpr std::array<const wchar_t*, 8> fast_names{
                L"Default for LZMA2 level", L"5", L"16", L"32",
                L"64", L"128", L"192", L"273"};
            constexpr std::array<std::size_t, 8> fast_values{
                0, 5, 16, 32, 64, 128, 192, 273};
            for (std::size_t index = 0; index < names.size(); ++index) {
                add(dictionary_combo, names[index], values[index]);
            }
            for (std::size_t index = 0; index < fast_names.size(); ++index) {
                add(word_combo, fast_names[index], fast_values[index]);
            }
        } else if (method == axiom::CompressionMethod::deflate) {
            add(dictionary_combo, L"Fixed 32 KiB window", 0);
            add(word_combo, L"Fixed 258-byte maximum match", 0);
        } else if (method == axiom::CompressionMethod::zstandard) {
            add(dictionary_combo, L"Managed by Zstandard", 0);
            add(word_combo, L"Managed by Zstandard", 0);
        } else {
            add(dictionary_combo, L"Not used", 0);
            add(word_combo, L"Not used", 0);
        }

        select_combo_value(dictionary_combo, selected_dictionary);
        select_combo_value(word_combo, selected_word);
        SendMessageW(dictionary_combo, WM_SETREDRAW, TRUE, 0);
        SendMessageW(word_combo, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(dictionary_combo, nullptr, TRUE);
        InvalidateRect(word_combo, nullptr, TRUE);
    }

    void load_create_values() {
        create_options.archive_format =
            archive_format_from_path(create_options.archive_path, create_options.archive_format);
        SendMessageW(format_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(creatable_archive_format_index(
                         create_options.archive_format)), 0);
        rebuild_method_combo(method_combo_, selected_format_is_native(),
                             create_options.method);
        create_options.method = method_from_combo(method_combo_);
        level_ = std::clamp(create_options.level, 1, 9);
        rebuild_method_controls(
            level_combo_, thread_model_combo_, create_options.method,
            level_, create_options.codec_level, create_options.thread_model,
            create_options.lzma_binary_tree);
        rebuild_codec_parameter_controls(
            dictionary_combo_, word_size_combo_, create_options.method,
            create_options.dictionary_size, create_options.word_size);
        SendMessageW(solid_block_combo_, CB_SETCURSEL,
                     value_index(kSolidBlockValues, create_options.solid_block_size), 0);
        set_thread_count(threads_combo_, create_options.thread_count);
        SendMessageW(update_mode_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(create_options.features.update_mode), 0);
        SendMessageW(volume_unit_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(std::clamp(create_options.features.volume_unit, 0, 3)), 0);
        SendMessageW(sfx_stub_tier_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(std::clamp(
                         create_options.features.sfx_stub_tier, 0, 1)), 0);
        SendMessageW(sfx_overwrite_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(std::clamp(
                         create_options.features.sfx_overwrite, 0, 2)), 0);
        SendMessageW(sfx_mode_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(std::clamp(
                         create_options.features.sfx_mode, 0, 2)), 0);
        SendMessageW(sfx_elevation_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(std::clamp(
                         create_options.features.sfx_elevation, 0, 2)), 0);
        SendMessageW(sfx_theme_combo_, CB_SETCURSEL,
                     static_cast<WPARAM>(std::clamp(
                         create_options.features.sfx_theme, 0, 2)), 0);
        set_window_text(sfx_title_edit_, create_options.features.sfx_title);
        set_window_text(sfx_description_edit_,
                        create_options.features.sfx_description);
        set_window_text(sfx_default_path_combo_,
                        create_options.features.sfx_default_path);
        set_window_text(sfx_run_program_edit_,
                        create_options.features.sfx_run_program);
        set_window_text(sfx_run_arguments_edit_,
                        create_options.features.sfx_run_arguments);
        set_window_text(sfx_license_edit_, create_options.features.sfx_license_text);
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
        set_window_text(
            dedup_min_chunk_edit_,
            format_size_text(create_options.features.dedup_min_chunk_size));
        set_window_text(
            dedup_average_chunk_edit_,
            format_size_text(create_options.features.dedup_average_chunk_size));
        set_window_text(
            dedup_max_chunk_edit_,
            format_size_text(create_options.features.dedup_max_chunk_size));
        clear_options_unsupported_by_selected_format();
        rebuild_compression_profile_combo(true);
    }

    void select_create_page(int page) {
        if (create_viewport_ != nullptr) {
            create_scroll_offsets_[static_cast<std::size_t>(create_page_)] =
                static_cast<int>(SendMessageW(
                    create_viewport_, kSettingsViewportGetOffset, 0, 0));
        }
        create_page_ = std::clamp(page, 0, static_cast<int>(kCreateTabNames.size()) - 1);
        for (const auto& item : page_controls_) {
            ShowWindow(item.window,
                       item.page == create_page_ ? SW_SHOWNA : SW_HIDE);
        }
        if (create_page_heading_ != nullptr) {
            SetWindowTextW(create_page_heading_,
                           kCreateTabNames[static_cast<std::size_t>(create_page_)]);
        }
        layout();
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW | RDW_UPDATENOW);
        SendMessageW(create_navigation_, kPageNavigationSetSelection,
                     static_cast<WPARAM>(create_page_), 0);
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                         RDW_ERASENOW | RDW_UPDATENOW);
    }

    void select_settings_page(int page) {
        if (settings_viewport_ != nullptr) {
            settings_scroll_offsets_[static_cast<std::size_t>(settings_page_)] =
                static_cast<int>(SendMessageW(
                    settings_viewport_, kSettingsViewportGetOffset, 0, 0));
        }
        settings_page_ = std::clamp(page, 0, static_cast<int>(kSettingsTabNames.size()) - 1);
        if (settings_page_ == 9) {
            ensure_toolbar_settings_list();
        }
        for (const SettingControl& control : settings_controls_) {
            if (control.page != settings_page_) {
                ShowWindow(control.window, SW_HIDE);
                continue;
            }
            // A page that was hidden during a live theme change still carries
            // its previous native theme. Suppress its first paint until the
            // new theme and restored edge styles are installed.
            SendMessageW(control.window, WM_SETREDRAW, FALSE, 0);
            apply_dialog_control_theme(control.window, palette_.dark);
            SendMessageW(control.window, WM_SETREDRAW, TRUE, 0);
        }
        SendMessageW(settings_tabs_, kPageNavigationSetSelection,
                     static_cast<WPARAM>(settings_page_), 0);
        layout_settings();
        RedrawWindow(settings_viewport_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void ensure_viewport_focus_visible(HWND viewport_window, HWND focus) {
        if (viewport_window == nullptr || focus == nullptr) return;
        HWND settings_control = focus;
        while (settings_control != nullptr &&
               GetParent(settings_control) != viewport_window) {
            settings_control = GetParent(settings_control);
        }
        if (settings_control == nullptr) return;
        RECT control_rect{};
        GetWindowRect(settings_control, &control_rect);
        MapWindowPoints(nullptr, viewport_window,
                        reinterpret_cast<POINT*>(&control_rect), 2);
        RECT viewport_rect{};
        GetClientRect(viewport_window, &viewport_rect);
        const int padding = scale(8);
        int delta = 0;
        if (control_rect.top < viewport_rect.top + padding) {
            delta = control_rect.top - viewport_rect.top - padding;
        } else if (control_rect.bottom > viewport_rect.bottom - padding) {
            delta = control_rect.bottom - viewport_rect.bottom + padding;
        }
        if (delta != 0) {
            SendMessageW(viewport_window, kSettingsViewportScrollBy,
                         static_cast<WPARAM>(delta), 0);
        }
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
            available.volumes = zip && available.volumes;
            available.recovery = false;
            available.authenticity = false;
            available.sfx = zip;
            available.posix_metadata = false;
            available.update = zip && available.update;
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
        if (!zip) {
            create_options.features.volume_size.clear();
            set_window_text(volume_size_edit_, L"");
        }
        create_options.features.recovery_percent = 0;
        create_options.features.create_recovery_volumes = false;
        create_options.features.sign_archive = false;
        create_options.features.signing_key.clear();
        create_options.features.enable_content_dedup = false;
        if (!zip) {
            create_options.features.create_sfx = false;
            create_options.features.sfx_destination.clear();
        }
        set_window_text(comment_edit_, L"");
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
        rebuild_method_combo(method_combo_, selected_format_is_native(),
                             create_options.method);
        create_options.method = method_from_combo(method_combo_);
        rebuild_method_controls(
            level_combo_, thread_model_combo_, create_options.method,
            create_options.level, create_options.codec_level,
            create_options.thread_model, create_options.lzma_binary_tree);
        rebuild_codec_parameter_controls(
            dictionary_combo_, word_size_combo_, create_options.method, 0, 0);
        clear_options_unsupported_by_selected_format();
        apply_selected_format_extension(true);
        update_create_dependencies();
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
                rebuild_method_combo(method_combo_, selected_format_is_native(),
                                     create_options.method);
                create_options.method = method_from_combo(method_combo_);
                rebuild_method_controls(
                    level_combo_, thread_model_combo_, create_options.method,
                    create_options.level, create_options.codec_level,
                    create_options.thread_model,
                    create_options.lzma_binary_tree);
                rebuild_codec_parameter_controls(
                    dictionary_combo_, word_size_combo_,
                    create_options.method, 0, 0);
                clear_options_unsupported_by_selected_format();
                update_create_dependencies();
                schedule_compression_curve();
            }
        }
    }

    int combo_selection(HWND combo, int lowest, int highest) const {
        const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        if (selection == CB_ERR) return lowest;
        return std::clamp(static_cast<int>(selection), lowest, highest);
    }

    // Runs the same checks the extractor applies to an embedded configuration,
    // so an impossible combination is reported here rather than shipping in an
    // executable that refuses to start.
    bool sfx_options_valid(std::string& error) const {
        const auto config = sfx_config_from_features(create_options.features);
        if (!axiom::sfx::sfx_validate_path_template(config.default_path, error)) {
            return false;
        }
        return axiom::sfx::sfx_validate_config(config, error);
    }

    void update_create_dependencies() {
        const auto available = selected_format_availability();
        const bool updating = create_options.features.update_mode != ArchiveUpdateMode::create_new;
        const bool native = selected_format_is_native();
        const std::wstring previous_compression_info =
            window_text(compression_info_);
        const std::wstring previous_dedup_info = window_text(dedup_info_);
        const auto method = method_from_combo(
            method_combo_, native ? axiom::CompressionMethod::axiom
                                  : axiom::CompressionMethod::deflate);
        const bool axiom_method = method == axiom::CompressionMethod::axiom;
        const bool lzma_method = method == axiom::CompressionMethod::lzma2;
        EnableWindow(format_combo_, !create_options.fixed_archive_format);
        EnableWindow(method_combo_, TRUE);
        EnableWindow(level_combo_, method != axiom::CompressionMethod::store);
        EnableWindow(dictionary_combo_, native && (axiom_method || lzma_method));
        EnableWindow(word_size_combo_, native && (axiom_method || lzma_method));
        EnableWindow(solid_block_combo_, native);
        set_window_text(
            dictionary_label_,
            method == axiom::CompressionMethod::deflate
                ? L"Window size"
                : L"Dictionary size");
        set_window_text(
            word_size_label_,
            lzma_method
                ? L"Fast bytes"
                : method == axiom::CompressionMethod::deflate
                    ? L"Maximum match"
                    : L"Word size");
        // Level 7's path-dependent lazy tree parse is the one preset that has no
        // safe segmented form. Keep the control available everywhere else.
        const int selected_level = level_from_combo(level_combo_, 5);
        const bool swarm_applicable =
            native && axiom_method && selected_level != 7;
        set_window_text(thread_model_label_,
                        axiom_method ? L"Threading model"
                                     : lzma_method ? L"Match finder"
                                                   : L"Method option");
        EnableWindow(thread_model_label_, swarm_applicable || lzma_method);
        EnableWindow(thread_model_combo_, swarm_applicable || lzma_method);
        set_window_text(
            compression_info_,
            !native
                ? L"ZIP supports standard Deflate or Store. Axiom-only dictionary, word, "
                  L"solid-block, recovery, and encrypted-name controls do not apply."
            : method == axiom::CompressionMethod::zstandard
                ? L"Zstandard levels -5 through 22 trade speed for ratio. AXAR chunks remain "
                  L"bounded and independently cancellable; dictionary and word size do not apply."
            : lzma_method
                ? L"LZMA2 uses a 4 KiB to 4 GiB dictionary, 5 to 273 fast bytes, and the "
                  L"HC4/BT4 match finder. The effective dictionary cannot exceed the selected "
                  L"solid-block size. Large dictionaries can require several times their size "
                  L"in encoder memory; BT4 favors ratio and HC4 favors speed. Solid blocks "
                  L"above 4 GiB are staged on disk and use 512 MiB codec chunks by default; "
                  L"select a larger solid block to enable a larger independently decoded chunk. "
                  L"file-aware filters, encryption, and recovery records are unavailable "
                  L"for that profile."
            : method == axiom::CompressionMethod::deflate
                ? L"Deflate levels 0 through 9 use the format-defined 32 KiB window and "
                  L"258-byte maximum match. Those fixed values are shown but cannot be changed."
            : method == axiom::CompressionMethod::store
                ? L"Store writes data without compression. Archive encryption, recovery, "
                  L"signing, metadata, and volume features remain available."
            : selected_level == 1
                ? L"Swarm replaces level 1's fastest byte-token parser with a cooperative "
                  L"full-window hash parse. It improves ratio, but is intentionally slower."
            : selected_level >= 8
                ? L"Swarm parallelizes the preliminary tree parse. At level 9, exact global "
                  L"candidate discovery also runs ahead of the DP; output remains identical."
            : swarm_applicable
                ? L"Split blocks compress independent regions in parallel. Swarm lets all cores "
                  L"share each large block, preserving its full-window ratio."
                : L"Level 7 uses a path-dependent lazy tree parse, so swarm is unavailable. "
                  L"Choose Split blocks or another compression level.");
        EnableWindow(update_mode_combo_, available.update);
        EnableWindow(comment_edit_, available.comments);
        EnableWindow(lock_archive_, available.lock);
        EnableWindow(repack_after_update_, available.update && updating);

        const bool can_select_dedup =
            native && !updating && !create_options.existing_archive;
        EnableWindow(content_dedup_, can_select_dedup);
        const bool can_tune_dedup =
            can_select_dedup && create_options.features.enable_content_dedup;
        for (HWND control : {dedup_min_chunk_label_, dedup_min_chunk_edit_,
                             dedup_average_chunk_label_,
                             dedup_average_chunk_edit_,
                             dedup_max_chunk_label_, dedup_max_chunk_edit_}) {
            EnableWindow(control, can_tune_dedup);
        }
        set_window_text(
            dedup_info_,
            create_options.existing_archive
                ? create_options.features.enable_content_dedup
                    ? L"This archive already uses live content deduplication. Its persisted chunk geometry is preserved automatically for add, update, synchronize, delete, move, and repack operations."
                    : L"This archive uses ordinary AXAR storage. Its content profile cannot be converted from this update dialog; create a new archive to opt into live deduplication."
            : !native
                ? L"Live content deduplication is an AXAR-only profile. Choose Axiom archive format to enable it."
            : updating
                ? L"Live content deduplication is selected only while creating a new archive. Existing archives preserve their established profile automatically."
            : create_options.features.enable_content_dedup
                ? L"Files will be split at content-defined boundaries and repeated chunks stored once. Valid geometry is 4 KiB <= minimum <= average <= maximum <= 64 MiB."
                : L"Enable live deduplication to share repeated and unchanged file regions. The default 256 KiB / 1 MiB / 4 MiB geometry suits general backup archives.");

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
        const std::wstring volume_text =
            trim_dialog_input(window_text(volume_size_edit_));
        const int volume_unit = static_cast<int>(
            SendMessageW(volume_unit_combo_, CB_GETCURSEL, 0, 0));
        const bool valid_split_size =
            split_enabled && parse_integer_size_with_unit(
                                 volume_text, volume_unit).has_value();
        if (!valid_split_size &&
            create_options.features.create_recovery_volumes) {
            create_options.features.create_recovery_volumes = false;
            InvalidateRect(recovery_volumes_, nullptr, FALSE);
        }
        EnableWindow(recovery_volumes_,
                     available.recovery && valid_split_size);

        EnableWindow(sign_archive_, available.authenticity);
        const bool key_enabled = available.authenticity && create_options.features.sign_archive;
        EnableWindow(signing_key_edit_, key_enabled);
        EnableWindow(browse_signing_key_, key_enabled);
        EnableWindow(create_sfx_, available.sfx);
        // The whole SFX options page only means anything for an SFX build.
        const bool sfx_options_enabled =
            available.sfx && create_options.features.create_sfx;
        const bool mini_stub =
            sfx_options_enabled && combo_selection(sfx_stub_tier_combo_, 0, 1) == 1;
        // Mini has no dialog stack. Keep extraction and command-line behavior
        // configurable, but make dialog-only settings visibly unavailable.
        if (mini_stub) {
            SendMessageW(sfx_mode_combo_, CB_SETCURSEL, 2, 0);
        }
        for (HWND control : {sfx_stub_tier_label_, sfx_stub_tier_combo_,
                             sfx_default_path_label_, sfx_default_path_combo_,
                             sfx_overwrite_label_, sfx_overwrite_combo_,
                             sfx_elevation_label_, sfx_elevation_combo_,
                             sfx_license_label_, sfx_license_edit_}) {
            EnableWindow(control, sfx_options_enabled);
        }
        const bool dialog_options_enabled = sfx_options_enabled && !mini_stub;
        for (HWND control : {sfx_title_label_, sfx_title_edit_,
                             sfx_description_label_, sfx_description_edit_,
                             sfx_theme_label_, sfx_theme_combo_,
                             sfx_mode_label_, sfx_mode_combo_}) {
            EnableWindow(control, dialog_options_enabled);
        }
        EnableWindow(sfx_allow_path_change_, dialog_options_enabled);
        EnableWindow(sfx_open_destination_, dialog_options_enabled);
        // Arguments need a program, and acceptance needs something to accept.
        const bool run_configured =
            sfx_options_enabled &&
            !trim_dialog_input(window_text(sfx_run_program_edit_)).empty();
        EnableWindow(sfx_run_program_label_, sfx_options_enabled);
        EnableWindow(sfx_run_program_edit_, sfx_options_enabled);
        EnableWindow(sfx_run_arguments_label_, run_configured);
        EnableWindow(sfx_run_arguments_edit_, run_configured);
        EnableWindow(sfx_require_accept_,
                     sfx_options_enabled &&
                         !trim_dialog_input(window_text(sfx_license_edit_)).empty());
        if (mode_ == DialogMode::create_archive &&
            (previous_compression_info != window_text(compression_info_) ||
             previous_dedup_info != window_text(dedup_info_))) {
            // The description is wrapped and its height depends on the active
            // method and available width. Recompute the page metrics after a
            // method/format change so the viewport cannot clip the last line.
            layout_create();
        }
        InvalidateRect(window_, nullptr, TRUE);
    }

    std::wstring compression_curve_key() const {
        if (method_combo_ == nullptr) return {};
        const auto method = method_from_combo(method_combo_);
        const auto threads = thread_count().value_or(0);
        const auto dictionary = value_from_combo(dictionary_combo_);
        const auto word_size = value_from_combo(word_size_combo_);
        const auto solid_block = selected_combo_value(
            solid_block_combo_, kSolidBlockValues);
        const int method_option = static_cast<int>(
            std::clamp<LRESULT>(
                SendMessageW(thread_model_combo_, CB_GETCURSEL, 0, 0),
                0, 1));
        std::wostringstream key;
        key << static_cast<int>(selected_archive_format()) << L':'
            << static_cast<int>(method) << L':'
            << threads << L':' << dictionary << L':' << word_size << L':'
            << solid_block << L':' << method_option;
        return key.str();
    }

    CompressionOptions compression_curve_options(
        CompressionMethod method, int level) const {
        CompressionOptions options;
        if (method == CompressionMethod::axiom) {
            apply_compression_level(options, std::clamp(level, 1, 9));
            options.codec_level = kAutomaticCodecLevel;
        } else {
            // External codecs still use the portable preset for chunk geometry;
            // their native effort is carried independently in codec_level.
            apply_compression_level(
                options, std::clamp(create_options.level, 1, 9));
            options.codec_level = level;
        }
        options.method = method;
        options.thread_count = thread_count().value_or(0);

        const std::size_t dictionary =
            value_from_combo(dictionary_combo_);
        const std::size_t word_size =
            value_from_combo(word_size_combo_);
        if (method == CompressionMethod::lzma2) {
            options.lzma_dictionary_size = dictionary;
            options.lzma_fast_bytes = word_size;
        } else if (method == CompressionMethod::axiom) {
            if (dictionary != 0) options.window_size = dictionary;
            if (word_size != 0) options.nice_length = word_size;
        }
        const std::size_t solid_block = selected_combo_value(
            solid_block_combo_, kSolidBlockValues);
        if (solid_block != 0) {
            options.block_size = solid_block;
            options.auto_block_size_for_threads = false;
        }
        const int method_option = static_cast<int>(
            std::clamp<LRESULT>(
                SendMessageW(thread_model_combo_, CB_GETCURSEL, 0, 0),
                0, 1));
        options.swarm_parse =
            method == CompressionMethod::axiom && method_option == 1;
        options.lzma_binary_tree =
            method != CompressionMethod::lzma2 || method_option == 1;
        options.recovery_percent = 0;
        return options;
    }

    std::vector<CompressionEstimateCurveCandidate>
    compression_curve_candidates() const {
        const auto method = method_from_combo(method_combo_);
        const int selected = level_from_combo(
            level_combo_, method == CompressionMethod::axiom ? 5 : 0);
        std::vector<int> levels;
        if (method == CompressionMethod::axiom) {
            for (int level = 1; level <= 9; ++level) {
                levels.push_back(level);
            }
        } else if (method == CompressionMethod::zstandard) {
            for (int level = -5; level <= 22; ++level) {
                levels.push_back(level);
            }
        } else if (method == CompressionMethod::lzma2 ||
                   method == CompressionMethod::deflate) {
            for (int level = 0; level <= 9; ++level) {
                levels.push_back(level);
            }
        } else {
            levels.push_back(0);
        }

        // The worker publishes candidates in this order. Put the selected
        // result and the curve's bounds first so useful context appears before
        // the remaining points are filled in.
        std::vector<int> ordered;
        const auto add = [&](int level) {
            if (std::find(levels.begin(), levels.end(), level) != levels.end() &&
                std::find(ordered.begin(), ordered.end(), level) ==
                    ordered.end()) {
                ordered.push_back(level);
            }
        };
        add(selected);
        add(levels.front());
        add(levels.back());
        for (int level : levels) add(level);

        std::vector<CompressionEstimateCurveCandidate> candidates;
        candidates.reserve(ordered.size());
        for (int level : ordered) {
            candidates.push_back({
                level, compression_curve_options(method, level)});
        }
        return candidates;
    }

    void schedule_compression_curve() {
        if (mode_ != DialogMode::create_archive ||
            window_ == nullptr || estimate_inputs.empty()) {
            return;
        }
        KillTimer(window_, kCompressionCurveDebounceTimer);
        SetTimer(window_, kCompressionCurveDebounceTimer, 400, nullptr);
    }

    void stop_compression_curve_worker() {
        if (window_ != nullptr && IsWindow(window_)) {
            KillTimer(window_, kCompressionCurveDebounceTimer);
        }
        if (compression_curve_operation_) {
            compression_curve_operation_->request_cancel();
        }
        if (compression_curve_worker_.joinable()) {
            compression_curve_worker_.join();
        }
        compression_curve_operation_.reset();
        compression_curve_running_ = false;
    }

    void start_compression_curve() {
        if (mode_ != DialogMode::create_archive ||
            estimate_inputs.empty() || window_ == nullptr) {
            return;
        }
        const ArchiveFormat format = selected_archive_format();
        if (format != ArchiveFormat::axar &&
            format != ArchiveFormat::zip) {
            std::lock_guard lock(compression_curve_mutex_);
            compression_curve_result_.reset();
            compression_curve_error_ =
                L"Compression preview is unavailable for this archive format.";
            compression_curve_status_ = compression_curve_error_;
            InvalidateRect(compression_preview_, nullptr, FALSE);
            return;
        }

        const std::wstring key = compression_curve_key();
        {
            std::lock_guard lock(compression_curve_mutex_);
            if (compression_curve_result_ &&
                compression_curve_result_->complete &&
                compression_curve_result_key_ == key) {
                InvalidateRect(compression_preview_, nullptr, FALSE);
                return;
            }
        }
        if (compression_curve_running_) {
            if (compression_curve_active_key_ == key) return;
            compression_curve_restart_pending_ = true;
            if (compression_curve_operation_) {
                compression_curve_operation_->request_cancel();
            }
            {
                std::lock_guard lock(compression_curve_mutex_);
                compression_curve_status_ = L"Updating compression preview...";
            }
            InvalidateRect(compression_preview_, nullptr, FALSE);
            return;
        }
        if (compression_curve_worker_.joinable()) {
            compression_curve_worker_.join();
        }

        const auto candidates = compression_curve_candidates();
        auto estimate_options = CompressionEstimateOptions{};
        estimate_options.format = format;
        // Keep enough headroom to reach high confidence on heterogeneous
        // inputs, while bounding the total work across every curve point.
        estimate_options.sample_budget = candidates.size() > 16
            ? 16u << 20
            : 32u << 20;
        estimate_options.sample_chunk_size = 256u << 10;
        estimate_options.time_budget = std::chrono::seconds(30);
        estimate_options.stop_when_high_confidence = true;
        compression_curve_operation_ =
            std::make_shared<OperationControl>();
        estimate_options.compression.operation =
            compression_curve_operation_;
        const auto inputs = estimate_inputs;
        const HWND target = window_;
        compression_curve_active_key_ = key;
        compression_curve_restart_pending_ = false;
        compression_curve_running_ = true;
        compression_curve_update_posted_.store(
            false, std::memory_order_release);
        {
            std::lock_guard lock(compression_curve_mutex_);
            compression_curve_result_.reset();
            compression_curve_error_.clear();
            compression_curve_status_ = L"Scanning selected files...";
        }
        InvalidateRect(compression_preview_, nullptr, FALSE);

        compression_curve_worker_ = std::jthread(
            [this, target, inputs, estimate_options, candidates, key] {
                try {
                    auto result = estimate_compression_curve(
                        inputs, estimate_options, candidates,
                        [this, target, &key](
                            const CompressionEstimateCurveResult& value) {
                            {
                                std::lock_guard lock(
                                    compression_curve_mutex_);
                                compression_curve_result_ = value;
                                compression_curve_result_key_ = key;
                                compression_curve_status_ =
                                    !value.complete
                                        ? L"Estimating codec levels..."
                                        : value.reached_high_confidence ||
                                              value.sampled_bytes >=
                                                  value.planned_sample_bytes
                                            ? L"Compression preview complete"
                                            : L"Preview sample limit reached; confidence is bounded";
                            }
                            if (!compression_curve_update_posted_.exchange(
                                    true, std::memory_order_acq_rel)) {
                                PostMessageW(
                                    target, kCompressionCurveUpdated, 0, 0);
                            }
                        });
                    {
                        std::lock_guard lock(compression_curve_mutex_);
                        compression_curve_result_ = std::move(result);
                        compression_curve_result_key_ = key;
                        compression_curve_status_ =
                            compression_curve_result_->reached_high_confidence ||
                                    compression_curve_result_->sampled_bytes >=
                                        compression_curve_result_->planned_sample_bytes
                                ? L"Compression preview complete"
                                : L"Preview sample limit reached; confidence is bounded";
                    }
                } catch (const OperationCancelled&) {
                    // A settings change or dialog close superseded this curve.
                } catch (const std::exception& exception) {
                    std::lock_guard lock(compression_curve_mutex_);
                    compression_curve_error_ =
                        widen_ascii(exception.what());
                    compression_curve_status_ =
                        L"Compression preview unavailable";
                } catch (...) {
                    std::lock_guard lock(compression_curve_mutex_);
                    compression_curve_error_ =
                        L"Unknown compression estimation failure.";
                    compression_curve_status_ =
                        L"Compression preview unavailable";
                }
                PostMessageW(
                    target, kCompressionCurveFinished, 0, 0);
            });
    }

    void finish_compression_curve() {
        if (compression_curve_worker_.joinable()) {
            compression_curve_worker_.join();
        }
        compression_curve_running_ = false;
        compression_curve_operation_.reset();
        compression_curve_update_posted_.store(
            false, std::memory_order_release);
        InvalidateRect(compression_preview_, nullptr, FALSE);
        if (compression_curve_restart_pending_) {
            compression_curve_restart_pending_ = false;
            start_compression_curve();
        }
    }

    void draw_compression_curve(const DRAWITEMSTRUCT& draw) const {
        RECT bounds = draw.rcItem;
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        if (width <= 0 || height <= 0) return;

        HDC memory = CreateCompatibleDC(draw.hDC);
        HBITMAP bitmap = CreateCompatibleBitmap(draw.hDC, width, height);
        HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
        RECT local{0, 0, width, height};
        HBRUSH background = CreateSolidBrush(palette_.button);
        FillRect(memory, &local, background);
        DeleteObject(background);
        HPEN border_pen = CreatePen(PS_SOLID, 1, palette_.border);
        HGDIOBJ old_pen = SelectObject(memory, border_pen);
        HGDIOBJ old_brush = SelectObject(
            memory, GetStockObject(HOLLOW_BRUSH));
        Rectangle(memory, 0, 0, width, height);
        SelectObject(memory, old_brush);
        SelectObject(memory, old_pen);
        DeleteObject(border_pen);

        std::optional<CompressionEstimateCurveResult> result;
        std::wstring status;
        std::wstring error;
        {
            std::lock_guard lock(compression_curve_mutex_);
            result = compression_curve_result_;
            status = compression_curve_status_;
            error = compression_curve_error_;
        }

        HFONT old_font = reinterpret_cast<HFONT>(
            SelectObject(memory, font_));
        SetBkMode(memory, TRANSPARENT);
        SetTextColor(memory, palette_.text);
        const int inset = scale(16);
        RECT title_rect{inset, scale(12), width - inset, scale(36)};
        const auto method = method_from_combo(method_combo_);
        std::wstring title =
            std::wstring(
                kCompressionMethodNames[
                    static_cast<std::size_t>(method)]) +
            L" compression preview";
        DrawTextW(
            memory, title.c_str(), -1, &title_rect,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        std::vector<CompressionEstimateCurvePoint> points;
        if (result) {
            for (const auto& point : result->points) {
                if (point.completed_probes != 0 ||
                    result->source_bytes == 0) {
                    points.push_back(point);
                }
            }
            std::sort(
                points.begin(), points.end(),
                [](const auto& left, const auto& right) {
                    return left.level < right.level;
                });
        }
        const int selected_level = level_from_combo(
            level_combo_, method == CompressionMethod::axiom ? 5 : 0);
        const auto selected = std::find_if(
            points.begin(), points.end(),
            [selected_level](const auto& point) {
                return point.level == selected_level;
            });

        SetTextColor(memory, palette_.muted);
        RECT summary_rect{
            inset, scale(38), width - inset, scale(66)};
        std::wstring summary = status;
        if (selected != points.end() && result &&
            result->source_bytes != 0) {
            const double compressed_percent =
                100.0 *
                static_cast<double>(
                    selected->estimated_archive_bytes) /
                static_cast<double>(result->source_bytes);
            summary =
                L"Level " + std::to_wstring(selected->level) +
                L"  \u2022  " +
                format_preview_bytes(
                    selected->estimated_archive_bytes) +
                L"  \u2022  " +
                format_preview_percent(compressed_percent) +
                L" of original  \u2022  " +
                format_preview_percent(
                    selected->estimated_savings_percent) +
                L" saved";
        }
        DrawTextW(
            memory, summary.c_str(), -1, &summary_rect,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        RECT detail_rect{
            inset, scale(62), width - inset, scale(86)};
        std::wstring detail;
        if (selected != points.end() && result) {
            detail =
                estimate_confidence_name(selected->confidence) +
                std::wstring(L"  \u2022  sampled ") +
                format_preview_bytes(result->sampled_bytes);
            if (result->planned_sample_bytes != 0 &&
                result->sampled_bytes < result->planned_sample_bytes) {
                if (result->reached_high_confidence) {
                    detail += L"  •  high confidence reached";
                } else {
                    detail += L" of " +
                        format_preview_bytes(
                            result->planned_sample_bytes);
                }
            }
            if (!result->warnings.empty()) {
                detail += L"  \u2022  " +
                    std::to_wstring(result->warnings.size()) +
                    L" warning";
                if (result->warnings.size() != 1) detail += L"s";
            }
        } else if (!error.empty()) {
            detail = error;
        } else {
            detail =
                L"Each point uses the same sampled source regions.";
        }
        DrawTextW(
            memory, detail.c_str(), -1, &detail_rect,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        const RECT chart{
            scale(48), scale(96),
            width - scale(18), height - scale(42)};
        if (points.empty() || !result ||
            result->source_bytes == 0 ||
            chart.right <= chart.left ||
            chart.bottom <= chart.top) {
            RECT empty_rect{
                inset, scale(105), width - inset,
                height - scale(20)};
            SetTextColor(memory, palette_.muted);
            const std::wstring empty_text =
                !error.empty() ? error
                               : status.empty()
                                   ? L"Preparing compression preview..."
                                   : status;
            DrawTextW(
                memory, empty_text.c_str(), -1, &empty_rect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                    DT_END_ELLIPSIS);
            SelectObject(memory, old_font);
            BitBlt(
                draw.hDC, bounds.left, bounds.top, width, height,
                memory, 0, 0, SRCCOPY);
            SelectObject(memory, old_bitmap);
            DeleteObject(bitmap);
            DeleteDC(memory);
            return;
        }

        double maximum_percent = 100.0;
        for (const auto& point : points) {
            maximum_percent = std::max(
                maximum_percent,
                100.0 *
                    static_cast<double>(
                        point.estimated_high_bytes) /
                    static_cast<double>(result->source_bytes));
        }
        maximum_percent =
            std::max(100.0, std::ceil(maximum_percent / 10.0) * 10.0);
        const auto x_for = [&](std::size_t index) {
            if (points.size() <= 1) {
                return (chart.left + chart.right) / 2;
            }
            return chart.left +
                static_cast<int>(
                    static_cast<long long>(
                        chart.right - chart.left) *
                    index /
                    static_cast<long long>(points.size() - 1));
        };
        const auto y_for = [&](double percent) {
            return chart.bottom -
                static_cast<int>(
                    std::clamp(
                        percent / maximum_percent, 0.0, 1.0) *
                    (chart.bottom - chart.top));
        };

        HPEN grid_pen = CreatePen(
            PS_SOLID, 1, blend_color(
                palette_.button, palette_.border, 70));
        old_pen = SelectObject(memory, grid_pen);
        SetTextColor(memory, palette_.muted);
        for (int tick = 0; tick <= 2; ++tick) {
            const double value =
                maximum_percent * tick / 2.0;
            const int y = y_for(value);
            MoveToEx(memory, chart.left, y, nullptr);
            LineTo(memory, chart.right, y);
            RECT label{
                scale(3), y - scale(10),
                chart.left - scale(5), y + scale(10)};
            const std::wstring text =
                format_preview_percent(value);
            DrawTextW(
                memory, text.c_str(), -1, &label,
                DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        }
        SelectObject(memory, old_pen);
        DeleteObject(grid_pen);

        std::vector<Gdiplus::PointF> curve;
        std::vector<Gdiplus::PointF> band;
        curve.reserve(points.size());
        band.reserve(points.size() * 2);
        for (std::size_t index = 0; index < points.size(); ++index) {
            const double percent =
                100.0 *
                static_cast<double>(
                    points[index].estimated_archive_bytes) /
                static_cast<double>(result->source_bytes);
            curve.push_back({
                static_cast<Gdiplus::REAL>(x_for(index)),
                static_cast<Gdiplus::REAL>(y_for(percent))});
            const double high =
                100.0 *
                static_cast<double>(
                    points[index].estimated_high_bytes) /
                static_cast<double>(result->source_bytes);
            band.push_back({
                static_cast<Gdiplus::REAL>(x_for(index)),
                static_cast<Gdiplus::REAL>(y_for(high))});
        }
        for (std::size_t reverse = points.size(); reverse-- > 0;) {
            const double low =
                100.0 *
                static_cast<double>(
                    points[reverse].estimated_low_bytes) /
                static_cast<double>(result->source_bytes);
            band.push_back({
                static_cast<Gdiplus::REAL>(x_for(reverse)),
                static_cast<Gdiplus::REAL>(y_for(low))});
        }

        if (compression_graph_gdiplus_ready()) {
            Gdiplus::Graphics graphics(memory);
            graphics.SetSmoothingMode(
                Gdiplus::SmoothingModeAntiAlias);
            graphics.SetPixelOffsetMode(
                Gdiplus::PixelOffsetModeHighQuality);
            graphics.SetCompositingQuality(
                Gdiplus::CompositingQualityGammaCorrected);
            if (curve.size() >= 2) {
                const Gdiplus::REAL original_baseline =
                    static_cast<Gdiplus::REAL>(y_for(100.0));
                const Gdiplus::REAL zero_baseline =
                    static_cast<Gdiplus::REAL>(y_for(0.0));

                std::vector<Gdiplus::PointF> compressed_area =
                    curve;
                compressed_area.push_back({
                    static_cast<Gdiplus::REAL>(
                        chart.right),
                    zero_baseline});
                compressed_area.push_back({
                    static_cast<Gdiplus::REAL>(
                        chart.left),
                    zero_baseline});
                Gdiplus::SolidBrush compressed(
                    graph_color(palette_.accent, 92));
                graphics.FillPolygon(
                    &compressed, compressed_area.data(),
                    static_cast<INT>(
                        compressed_area.size()));

                std::vector<Gdiplus::PointF> saved_area = curve;
                saved_area.push_back({
                    static_cast<Gdiplus::REAL>(
                        chart.right),
                    original_baseline});
                saved_area.push_back({
                    static_cast<Gdiplus::REAL>(
                        chart.left),
                    original_baseline});
                Gdiplus::SolidBrush saved(
                    graph_color(
                        compression_graph_saved_color(
                            palette_.accent),
                        105));
                graphics.FillPolygon(
                    &saved, saved_area.data(),
                    static_cast<INT>(saved_area.size()));

                if (band.size() >= 3) {
                    Gdiplus::SolidBrush uncertainty(
                        graph_color(palette_.text, 28));
                    graphics.FillPolygon(
                        &uncertainty, band.data(),
                        static_cast<INT>(band.size()));
                }
                Gdiplus::Pen line(
                    graph_color(palette_.accent), 2.0f);
                line.SetLineJoin(Gdiplus::LineJoinRound);
                graphics.DrawLines(
                    &line, curve.data(),
                    static_cast<INT>(curve.size()));
            }
            for (std::size_t index = 0;
                 index < points.size(); ++index) {
                const bool selected_point =
                    points[index].level == selected_level;
                const Gdiplus::REAL radius =
                    selected_point ? 5.0f : 2.5f;
                Gdiplus::SolidBrush fill(
                    selected_point
                        ? graph_color(palette_.selection_text)
                        : graph_color(palette_.accent));
                Gdiplus::Pen outline(
                    graph_color(palette_.accent),
                    selected_point ? 3.0f : 1.0f);
                graphics.FillEllipse(
                    &fill,
                    curve[index].X - radius,
                    curve[index].Y - radius,
                    radius * 2, radius * 2);
                graphics.DrawEllipse(
                    &outline,
                    curve[index].X - radius,
                    curve[index].Y - radius,
                    radius * 2, radius * 2);
            }
        } else if (curve.size() >= 2) {
            std::vector<POINT> line;
            line.reserve(curve.size());
            for (const auto& point : curve) {
                line.push_back({
                    static_cast<LONG>(point.X),
                    static_cast<LONG>(point.Y)});
            }
            HPEN curve_pen = CreatePen(
                PS_SOLID, scale(2), palette_.accent);
            old_pen = SelectObject(memory, curve_pen);
            Polyline(
                memory, line.data(),
                static_cast<int>(line.size()));
            SelectObject(memory, old_pen);
            DeleteObject(curve_pen);
        }

        const int legend_y = chart.top + scale(8);
        int legend_x = std::max(
            chart.left + scale(8), chart.right - scale(172));
        RECT legend_box{
            legend_x, legend_y,
            legend_x + scale(9), legend_y + scale(9)};
        HBRUSH legend_brush = CreateSolidBrush(palette_.accent);
        FillRect(memory, &legend_box, legend_brush);
        DeleteObject(legend_brush);
        SetTextColor(memory, palette_.text);
        RECT legend_text{
            legend_box.right + scale(5), legend_y - scale(5),
            legend_box.right + scale(80), legend_y + scale(15)};
        DrawTextW(
            memory, L"Compressed", -1, &legend_text,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        legend_x += scale(91);
        legend_box = {
            legend_x, legend_y,
            legend_x + scale(9), legend_y + scale(9)};
        legend_brush = CreateSolidBrush(
            compression_graph_saved_color(palette_.accent));
        FillRect(memory, &legend_box, legend_brush);
        DeleteObject(legend_brush);
        legend_text = {
            legend_box.right + scale(5), legend_y - scale(5),
            chart.right, legend_y + scale(15)};
        DrawTextW(
            memory, L"Saved", -1, &legend_text,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        const std::size_t tick_stride = std::max<std::size_t>(
            1, (points.size() + 6) / 7);
        SetTextColor(memory, palette_.muted);
        for (std::size_t index = 0;
             index < points.size(); ++index) {
            if (index != 0 &&
                index + 1 != points.size() &&
                points[index].level != selected_level &&
                index % tick_stride != 0) {
                continue;
            }
            RECT tick{
                x_for(index) - scale(24),
                chart.bottom + scale(7),
                x_for(index) + scale(24),
                chart.bottom + scale(29)};
            const std::wstring label =
                std::to_wstring(points[index].level);
            DrawTextW(
                memory, label.c_str(), -1, &tick,
                DT_CENTER | DT_SINGLELINE | DT_TOP);
        }

        SelectObject(memory, old_font);
        BitBlt(
            draw.hDC, bounds.left, bounds.top, width, height,
            memory, 0, 0, SRCCOPY);
        SelectObject(memory, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
    }

    void select_compression_curve_level() {
        std::optional<CompressionEstimateCurveResult> result;
        {
            std::lock_guard lock(compression_curve_mutex_);
            result = compression_curve_result_;
        }
        if (!result || result->source_bytes == 0) return;
        std::vector<CompressionEstimateCurvePoint> points;
        for (const auto& point : result->points) {
            if (point.completed_probes != 0) points.push_back(point);
        }
        std::sort(
            points.begin(), points.end(),
            [](const auto& left, const auto& right) {
                return left.level < right.level;
            });
        if (points.empty()) return;

        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(compression_preview_, &cursor);
        RECT client{};
        GetClientRect(compression_preview_, &client);
        const int left = scale(48);
        const int right = client.right - scale(18);
        if (cursor.x < left || cursor.x > right ||
            cursor.y < scale(88) ||
            cursor.y > client.bottom - scale(20)) {
            return;
        }
        std::size_t nearest = 0;
        int nearest_distance = std::numeric_limits<int>::max();
        for (std::size_t index = 0;
             index < points.size(); ++index) {
            const int x = points.size() <= 1
                ? (left + right) / 2
                : left +
                    static_cast<int>(
                        static_cast<long long>(right - left) *
                        index /
                        static_cast<long long>(
                            points.size() - 1));
            const int distance = std::abs(cursor.x - x);
            if (distance < nearest_distance) {
                nearest = index;
                nearest_distance = distance;
            }
        }
        const LRESULT count =
            SendMessageW(level_combo_, CB_GETCOUNT, 0, 0);
        for (LRESULT index = 0; index < count; ++index) {
            if (SendMessageW(
                    level_combo_, CB_GETITEMDATA,
                    static_cast<WPARAM>(index), 0) ==
                points[nearest].level) {
                SendMessageW(
                    level_combo_, CB_SETCURSEL,
                    static_cast<WPARAM>(index), 0);
                mark_compression_profile_custom();
                update_create_dependencies();
                InvalidateRect(
                    compression_preview_, nullptr, FALSE);
                break;
            }
        }
    }

    void convert_output_mode(bool sfx) {
        fs::path output = window_text(path_edit_);
        if (output.empty()) return;
        if (sfx) {
            output.replace_extension(L".exe");
        } else if (lstrcmpiW(output.extension().c_str(), L".exe") == 0) {
            output.replace_extension(
                widen_ascii(archive_format_info(selected_archive_format()).default_extension));
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
        if (mode_ == DialogMode::extract_archive) {
            const auto result = validate_dialog_path(
                window_text(path_edit_), DialogPathKind::destination_folder);
            if (!result) {
                show_message_dialog(
                    window_, instance_, dpi_, palette_.dark,
                    L"Extraction destination", result.error,
                    MessageDialogIcon::warning);
                SetFocus(path_edit_);
                SendMessageW(path_edit_, EM_SETSEL, 0, -1);
                return;
            }
            set_window_text(path_edit_, result.path.wstring());
        }
        const auto threads = thread_count();
        if (!threads) {
            show_message_dialog(
                window_, instance_, dpi_, palette_.dark, L"Axiom",
                L"Threads must be an integer from 0 through the available logical processor count.",
                MessageDialogIcon::warning);
            SetFocus(threads_combo_);
            return;
        }
        const LRESULT level_selection = SendMessageW(level_combo_, CB_GETCURSEL, 0, 0);
        if (level_selection != CB_ERR) level_ = static_cast<int>(level_selection) + 1;
        if (mode_ == DialogMode::create_archive) {
            create_options.archive_path = window_text(path_edit_);
            create_options.level = level_;
            create_options.thread_count = *threads;
            create_options.dictionary_size = value_from_combo(dictionary_combo_);
            create_options.word_size = value_from_combo(word_size_combo_);
            create_options.solid_block_size = selected_combo_value(
                solid_block_combo_, kSolidBlockValues);
        } else if (mode_ == DialogMode::extract_archive) {
            extract_options.destination = window_text(path_edit_);
            extract_options.thread_count = *threads;
            extract_options.overwrite = overwrite_checked_;
            extract_options.restore_mtime = restore_time_checked_;
        } else {
            application_options.default_level = level_;
            application_options.default_thread_count = *threads;
            application_options.confirm_delete = confirm_delete_checked_;
            application_options.show_hidden = show_hidden_checked_;
        }
        accepted_ = true;
        close_dialog();
    }

    void accept_create() {
        fs::path requested_output =
            trim_dialog_input(window_text(path_edit_));
        if (!requested_output.empty()) {
            if (create_options.features.create_sfx) {
                requested_output.replace_extension(L".exe");
            } else if (requested_output.extension().empty() ||
                       is_known_archive_extension(requested_output)) {
                requested_output.replace_extension(
                    widen_ascii(archive_format_info(selected_archive_format())
                                    .default_extension));
            }
        }
        const auto output_result = validate_dialog_path(
            requested_output.wstring(), DialogPathKind::output_file);
        if (!output_result) {
            show_message_dialog(window_, instance_, dpi_, palette_.dark,
                                L"Archive output", output_result.error,
                                MessageDialogIcon::warning);
            SetFocus(path_edit_);
            SendMessageW(path_edit_, EM_SETSEL, 0, -1);
            return;
        }
        fs::path displayed_output = output_result.path;
        set_window_text(path_edit_, displayed_output.wstring());

        create_options.method = method_from_combo(method_combo_);
        const int method_level = level_from_combo(level_combo_, 5);
        if (create_options.method == axiom::CompressionMethod::axiom) {
            level_ = std::clamp(method_level, 1, 9);
            create_options.level = level_;
            create_options.codec_level = axiom::kAutomaticCodecLevel;
        } else {
            create_options.codec_level = method_level;
        }
        const auto threads = thread_count();
        if (!threads) {
            select_create_page(0);
            show_message_dialog(
                window_, instance_, dpi_, palette_.dark, L"Compression options",
                L"Threads must be an integer from 0 through the available logical processor count.",
                MessageDialogIcon::warning);
            SetFocus(threads_combo_);
            return;
        }
        create_options.thread_count = *threads;
        create_options.dictionary_size = value_from_combo(dictionary_combo_);
        create_options.word_size = value_from_combo(word_size_combo_);
        create_options.solid_block_size = selected_combo_value(
            solid_block_combo_, kSolidBlockValues);
        const int method_option = static_cast<int>(
            std::clamp<LRESULT>(
                SendMessageW(thread_model_combo_, CB_GETCURSEL, 0, 0), 0, 1));
        if (create_options.method == axiom::CompressionMethod::axiom) {
            create_options.thread_model = method_option;
        } else if (create_options.method == axiom::CompressionMethod::lzma2) {
            create_options.lzma_binary_tree = method_option == 1;
        }
        create_options.archive_format = selected_archive_format();
        clear_options_unsupported_by_selected_format();

        if (const LRESULT selection = SendMessageW(update_mode_combo_, CB_GETCURSEL, 0, 0);
            selection != CB_ERR) {
            create_options.features.update_mode = static_cast<ArchiveUpdateMode>(selection);
        }
        const bool create_with_dedup =
            create_options.archive_format == axiom::ArchiveFormat::axar &&
            create_options.features.update_mode == ArchiveUpdateMode::create_new &&
            !create_options.existing_archive &&
            create_options.features.enable_content_dedup;
        if (create_with_dedup) {
            const auto minimum = parse_size_text(window_text(dedup_min_chunk_edit_));
            const auto average = parse_size_text(window_text(dedup_average_chunk_edit_));
            const auto maximum = parse_size_text(window_text(dedup_max_chunk_edit_));
            constexpr std::uint64_t kMinimumChunkSize = 4u << 10;
            constexpr std::uint64_t kMaximumChunkSize = 64u << 20;
            const std::uint64_t size_t_limit =
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
            if (!minimum || !average || !maximum ||
                *minimum < kMinimumChunkSize || *minimum > *average ||
                *average > *maximum || *maximum > kMaximumChunkSize ||
                *maximum > size_t_limit) {
                select_create_page(5);
                show_message_dialog(
                    window_, instance_, dpi_, palette_.dark,
                    L"Deduplication",
                    L"Chunk sizes must satisfy 4 KiB <= minimum <= average <= maximum <= 64 MiB. Examples: 256 KiB, 1 MiB, and 4 MiB.",
                    MessageDialogIcon::warning);
                SetFocus(!minimum ? dedup_min_chunk_edit_
                                  : !average ? dedup_average_chunk_edit_
                                             : dedup_max_chunk_edit_);
                return;
            }
            create_options.features.dedup_min_chunk_size =
                static_cast<std::size_t>(*minimum);
            create_options.features.dedup_average_chunk_size =
                static_cast<std::size_t>(*average);
            create_options.features.dedup_max_chunk_size =
                static_cast<std::size_t>(*maximum);
        } else {
            create_options.features.enable_content_dedup = false;
        }
        create_options.features.comment = window_text(comment_edit_);
        create_options.features.volume_size =
            trim_dialog_input(window_text(volume_size_edit_));
        if (const LRESULT selection =
                SendMessageW(volume_unit_combo_, CB_GETCURSEL, 0, 0);
            selection != CB_ERR) {
            create_options.features.volume_unit = static_cast<int>(selection);
        }
        if (!create_options.features.volume_size.empty() &&
            !parse_integer_size_with_unit(
                create_options.features.volume_size,
                create_options.features.volume_unit)) {
            select_create_page(3);
            show_message_dialog(
                window_, instance_, dpi_, palette_.dark,
                L"Recovery and volumes",
                L"Volume size must be a positive integer. Select KiB, MiB, GiB, or TiB in the adjacent list.",
                MessageDialogIcon::warning);
            SetFocus(volume_size_edit_);
            SendMessageW(volume_size_edit_, EM_SETSEL, 0, -1);
            return;
        }
        if (create_options.features.create_recovery_volumes &&
            create_options.features.volume_size.empty()) {
            select_create_page(3);
            show_message_dialog(
                window_, instance_, dpi_, palette_.dark,
                L"Recovery and volumes",
                L"Recovery volumes require a split volume size. Enter a maximum volume size first.",
                MessageDialogIcon::warning);
            SetFocus(volume_size_edit_);
            return;
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

        const bool large_solid_block =
            static_cast<std::uint64_t>(create_options.solid_block_size) >
            (std::uint64_t{4} << 30);
        if (create_options.archive_format == axiom::ArchiveFormat::axar &&
            large_solid_block &&
            create_options.method != axiom::CompressionMethod::lzma2) {
            select_create_page(0);
            show_message_dialog(
                window_, instance_, dpi_, palette_.dark,
                L"Compression options",
                L"Solid blocks above 4 GiB currently require the LZMA2 method.",
                MessageDialogIcon::warning);
            return;
        }
        if (create_options.archive_format == axiom::ArchiveFormat::axar &&
            large_solid_block && create_options.features.recovery_percent != 0) {
            select_create_page(3);
            show_message_dialog(
                window_, instance_, dpi_, palette_.dark,
                L"Recovery and volumes",
                L"Recovery records are not available with solid blocks above 4 GiB yet.",
                MessageDialogIcon::warning);
            return;
        }
        if (create_options.archive_format == axiom::ArchiveFormat::axar &&
            large_solid_block && create_options.features.encrypt_data) {
            select_create_page(2);
            show_message_dialog(
                window_, instance_, dpi_, palette_.dark,
                L"Archive security",
                L"Encryption is not available with solid blocks above 4 GiB yet.",
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
            if (create_options.archive_format == axiom::ArchiveFormat::axar &&
                large_solid_block) {
                select_create_page(0);
                show_message_dialog(
                    window_, instance_, dpi_, palette_.dark,
                    L"Compression options",
                    L"Encryption is not available with solid blocks above 4 GiB yet.",
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

        create_options.features.signing_key =
            trim_dialog_input(window_text(signing_key_edit_));
        if (create_options.features.sign_archive &&
            create_options.features.signing_key.empty()) {
            select_create_page(2);
            show_message_dialog(window_, instance_, dpi_, palette_.dark,
                                L"Archive signing", L"Choose an Axiom signing key.",
                                MessageDialogIcon::warning);
            return;
        }
        if (create_options.features.sign_archive &&
            !valid_signing_secret_key(create_options.features.signing_key)) {
            select_create_page(2);
            show_message_dialog(window_, instance_, dpi_, palette_.dark,
                                L"Archive signing",
                                L"Signing key must be an existing 64-byte Axiom secret key. Public keys and unrelated files cannot sign archives.",
                                MessageDialogIcon::warning);
            SetFocus(signing_key_edit_);
            return;
        }
        if (create_options.features.create_sfx) {
            create_options.features.sfx_stub_tier =
                combo_selection(sfx_stub_tier_combo_, 0, 1);
            create_options.features.sfx_overwrite =
                combo_selection(sfx_overwrite_combo_, 0, 2);
            create_options.features.sfx_mode = combo_selection(sfx_mode_combo_, 0, 2);
            create_options.features.sfx_elevation =
                combo_selection(sfx_elevation_combo_, 0, 2);
            create_options.features.sfx_theme =
                combo_selection(sfx_theme_combo_, 0, 2);
            create_options.features.sfx_title =
                trim_dialog_input(window_text(sfx_title_edit_));
            create_options.features.sfx_description =
                trim_dialog_input(window_text(sfx_description_edit_));
            create_options.features.sfx_default_path =
                trim_dialog_input(window_text(sfx_default_path_combo_));
            create_options.features.sfx_run_program =
                trim_dialog_input(window_text(sfx_run_program_edit_));
            create_options.features.sfx_run_arguments =
                trim_dialog_input(window_text(sfx_run_arguments_edit_));
            create_options.features.sfx_license_text = window_text(sfx_license_edit_);
            if (create_options.features.sfx_license_text.empty()) {
                create_options.features.sfx_require_accept = false;
            }
            if (create_options.features.sfx_run_program.empty()) {
                create_options.features.sfx_run_arguments.clear();
            }
            // The stub validates the same rules; catching them here points at
            // the offending control instead of failing on the user's machine.
            std::string sfx_error;
            if (!sfx_options_valid(sfx_error)) {
                select_create_page(4);
                show_message_dialog(window_, instance_, dpi_, palette_.dark,
                                    L"SFX options",
                                    widen_ascii(sfx_error),
                                    MessageDialogIcon::warning);
                return;
            }
            displayed_output.replace_extension(L".exe");
            create_options.features.sfx_destination = displayed_output.wstring();
            create_options.archive_path = displayed_output;
            create_options.archive_path.replace_extension(
                widen_ascii(archive_format_info(create_options.archive_format)
                                .default_extension));
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
        HWND owner = owner_;
        const bool owner_was_enabled = owner_was_enabled_;
        owner_was_enabled_ = false;
        if (window_ != nullptr && IsWindow(window_)) {
            destroy_modal_dialog(window_);
        }
        restore_dialog_owner(owner, owner_was_enabled);
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
            case kCenterChildWindows: return application_options.center_child_windows;
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
            case kContentDedup: return create_options.features.enable_content_dedup;
            case kEncryptData: return create_options.features.encrypt_data;
            case kEncryptNames: return create_options.features.encrypt_names;
            case kShowPassword: return create_show_password_;
            case kRecoveryVolumes: return create_options.features.create_recovery_volumes;
            case kSignArchive: return create_options.features.sign_archive;
            case kCreateSfx: return create_options.features.create_sfx;
            case kSfxAllowPathChange:
                return create_options.features.sfx_allow_path_change;
            case kSfxRequireAccept:
                return create_options.features.sfx_require_accept;
            case kSfxOpenDestination:
                return create_options.features.sfx_open_destination;
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
            case kCenterChildWindows:
                application_options.center_child_windows =
                    !application_options.center_child_windows;
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
                if (application_options.default_create_sfx) {
                    application_options.default_recovery_volumes = false;
                    application_options.default_volume_size.clear();
                    SetWindowTextW(item(kDefaultVolumeSize), L"");
                }
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
            case kContentDedup:
                create_options.features.enable_content_dedup =
                    !create_options.features.enable_content_dedup;
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
            case kSfxAllowPathChange:
                create_options.features.sfx_allow_path_change =
                    !create_options.features.sfx_allow_path_change;
                break;
            case kSfxRequireAccept:
                create_options.features.sfx_require_accept =
                    !create_options.features.sfx_require_accept;
                break;
            case kSfxOpenDestination:
                create_options.features.sfx_open_destination =
                    !create_options.features.sfx_open_destination;
                break;
            case kCreateSfx:
                create_options.features.create_sfx = !create_options.features.create_sfx;
                if (create_options.features.create_sfx) {
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
        } else if (mode_ == DialogMode::settings) {
            update_settings_dependencies();
        }
    }

    bool is_checkbox_id(int id) const {
        switch (id) {
            case kOverwrite:
            case kRestoreTime:
            case kConfirmDelete:
            case kShowHidden:
            case kRestoreWindowPlacement:
            case kCenterChildWindows:
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
            case kContentDedup:
            case kEncryptData:
            case kEncryptNames:
            case kShowPassword:
            case kRecoveryVolumes:
            case kSignArchive:
            case kCreateSfx:
            case kSfxAllowPathChange:
            case kSfxRequireAccept:
            case kSfxOpenDestination:
                return true;
            default:
                return false;
        }
    }

    void draw_button(const DRAWITEMSTRUCT& draw) const {
        const int id = GetDlgCtrlID(draw.hwndItem);
        const bool checkbox = is_checkbox_id(id);
        if (!checkbox) {
            draw_dialog_button(draw, palette_.dark);
            return;
        }
        draw_dialog_checkbox(draw, palette_.dark, checkbox_checked(id));
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
        if (toolbar_list_.hwnd() != nullptr) {
            toolbar_list_.set_font(font_);
            toolbar_list_.set_dpi(dpi_);
            rebuild_toolbar_settings_image_list();
            refresh_toolbar_settings_list();
        }
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE: create_controls(); return 0;
            case WM_TIMER:
                if (wparam == kCompositorRevealTimer &&
                    layered_reveal_pending_) {
                    KillTimer(window_, kCompositorRevealTimer);
                    RedrawWindow(window_, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                                     RDW_ALLCHILDREN | RDW_UPDATENOW);
                    DwmFlush();
                    SetLayeredWindowAttributes(
                        window_, 0, 255, LWA_ALPHA);
                    DwmFlush();
                    // Layering is only an opening-frame gate.  Keeping
                    // WS_EX_LAYERED on a control-heavy, resizable dialog makes
                    // USER32 redirect every live-resize paint and causes the
                    // controls to flicker.  Return to the normal DWM path once
                    // the fully dark first frame has been committed.
                    const LONG_PTR extended_style =
                        GetWindowLongPtrW(window_, GWL_EXSTYLE);
                    SetWindowLongPtrW(
                        window_, GWL_EXSTYLE,
                        extended_style & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
                    SetWindowPos(
                        window_, nullptr, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                            SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                            SWP_FRAMECHANGED);
                    RedrawWindow(window_, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                                     RDW_ALLCHILDREN | RDW_UPDATENOW);
                    DwmFlush();
                    layered_reveal_pending_ = false;
                    return 0;
                }
                if (wparam == kCompressionCurveDebounceTimer &&
                    mode_ == DialogMode::create_archive) {
                    KillTimer(
                        window_, kCompressionCurveDebounceTimer);
                    start_compression_curve();
                    return 0;
                }
                break;
            case kCompressionCurveUpdated:
                compression_curve_update_posted_.store(
                    false, std::memory_order_release);
                if (compression_preview_ != nullptr) {
                    InvalidateRect(
                        compression_preview_, nullptr, FALSE);
                }
                return 0;
            case kCompressionCurveFinished:
                finish_compression_curve();
                return 0;
            case WM_SIZE: layout(); return 0;
            case kSettingsViewportOffsetChanged:
                if (mode_ == DialogMode::settings) {
                    settings_scroll_offsets_[
                        static_cast<std::size_t>(settings_page_)] =
                        static_cast<int>(wparam);
                    layout_settings();
                    return 0;
                }
                if (mode_ == DialogMode::create_archive) {
                    create_scroll_offsets_[
                        static_cast<std::size_t>(create_page_)] =
                        static_cast<int>(wparam);
                    layout_create();
                    return 0;
                }
                break;
            case kTableActivateMessage:
                if (mode_ == DialogMode::settings && settings_page_ == 9) {
                    sync_toolbar_status_combo();
                    if (toolbar_status_combo_ != nullptr) {
                        SetFocus(toolbar_status_combo_);
                        SendMessageW(toolbar_status_combo_, CB_SHOWDROPDOWN, TRUE, 0);
                    }
                    return 0;
                }
                break;
            case kTableSortMessage:
            case kTableParentMessage:
                if (mode_ == DialogMode::settings && settings_page_ == 9) {
                    return 0;
                }
                break;
            case kTableSelectionChangedMessage:
                if (mode_ == DialogMode::settings && settings_page_ == 9) {
                    sync_toolbar_status_combo();
                    return 0;
                }
                break;
            case WM_GETMINMAXINFO:
                if (mode_ == DialogMode::create_archive || mode_ == DialogMode::settings) {
                    auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
                    const SIZE minimum = minimum_track_size();
                    limits->ptMinTrackSize = {minimum.cx, minimum.cy};
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
                apply_axiom_window_icons(window_, instance_);
                rebuild_font_for_dpi();
                tooltip_.update_dpi(dpi_);
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
                if (draw.hwndItem == compression_preview_) {
                    draw_compression_curve(draw);
                } else if (draw.CtlType == ODT_COMBOBOX) {
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
                    (id == kAccentColorMode || id == kStartupMode ||
                     id == kCompressionMethod ||
                     id == kArchiveOutputMode ||
                     id == kExtractDestinationMode ||
                     id == kTempFolderMode ||
                     id == kIoBufferMode || id == kMemoryLimitMode) &&
                    HIWORD(wparam) == CBN_SELCHANGE) {
                    if (id == kCompressionMethod) {
                        const auto method =
                            method_from_combo(item(kCompressionMethod));
                        rebuild_method_controls(
                            item(kLevel), item(kThreadModel), method,
                            application_options.default_level,
                            axiom::kAutomaticCodecLevel,
                            application_options.default_thread_model,
                            application_options.default_lzma_binary_tree);
                        rebuild_codec_parameter_controls(
                            item(kDictionarySize), item(kWordSize),
                            method, 0, 0);
                    }
                    update_settings_dependencies();
                    return 0;
                }
                if (mode_ == DialogMode::settings &&
                    id == kShortcutCommand && HIWORD(wparam) == CBN_SELCHANGE) {
                    update_shortcut_controls();
                    return 0;
                }
                if (mode_ == DialogMode::settings &&
                    id == kToolbarStatusCombo && HIWORD(wparam) == CBN_SELCHANGE) {
                    apply_toolbar_status_combo_selection();
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
                    schedule_compression_curve();
                    return 0;
                }
                if (mode_ == DialogMode::create_archive &&
                    id == kCompressionMethod &&
                    HIWORD(wparam) == CBN_SELCHANGE) {
                    create_options.method = method_from_combo(method_combo_);
                    create_options.codec_level = axiom::kAutomaticCodecLevel;
                    rebuild_method_controls(
                        level_combo_, thread_model_combo_, create_options.method,
                        create_options.level, create_options.codec_level,
                        create_options.thread_model,
                        create_options.lzma_binary_tree);
                    rebuild_codec_parameter_controls(
                        dictionary_combo_, word_size_combo_,
                        create_options.method, 0, 0);
                    mark_compression_profile_custom();
                    update_create_dependencies();
                    schedule_compression_curve();
                    return 0;
                }
                if (mode_ == DialogMode::create_archive &&
                    id == kCompressionProfile && HIWORD(wparam) == CBN_SELCHANGE) {
                    apply_selected_compression_profile();
                    schedule_compression_curve();
                    return 0;
                }
                if (mode_ == DialogMode::settings &&
                    id == kDefaultVolumeSize && HIWORD(wparam) == EN_CHANGE) {
                    update_settings_dependencies();
                    return 0;
                }
                if (mode_ == DialogMode::create_archive &&
                    id == kCompressionProfile && HIWORD(wparam) == CBN_EDITCHANGE) {
                    update_compression_profile_actions();
                    return 0;
                }
                if (mode_ == DialogMode::create_archive &&
                    (id == kLevel || id == kDictionarySize || id == kWordSize ||
                     id == kSolidBlockSize || id == kThreadModel || id == kThreads) &&
                    (HIWORD(wparam) == CBN_SELCHANGE ||
                     HIWORD(wparam) == CBN_EDITCHANGE)) {
                    mark_compression_profile_custom();
                    if (id == kLevel || id == kThreadModel) {
                        update_create_dependencies();
                    }
                    if (id == kLevel) {
                        InvalidateRect(
                            compression_preview_, nullptr, FALSE);
                    } else {
                        schedule_compression_curve();
                    }
                    return 0;
                }
                if (mode_ == DialogMode::create_archive &&
                    (id == kPathEdit || id == kVolumeSize) &&
                    HIWORD(wparam) == EN_CHANGE) {
                    if (id == kPathEdit) {
                        sync_archive_format_from_path();
                    }
                    if (id == kVolumeSize) {
                        update_create_dependencies();
                    }
                    return 0;
                }
                // Two SFX controls gate on the contents of an edit box: the
                // acceptance checkbox needs license text, and run arguments
                // need a program. Without this they never re-evaluate as the
                // user types, so the dependent control stays disabled.
                if (mode_ == DialogMode::create_archive &&
                    (id == kSfxLicenseText || id == kSfxRunProgram) &&
                    HIWORD(wparam) == EN_CHANGE) {
                    update_create_dependencies();
                    return 0;
                }
                if (mode_ == DialogMode::create_archive &&
                    id == kSfxStubTier && HIWORD(wparam) == CBN_SELCHANGE) {
                    update_create_dependencies();
                    return 0;
                }
                if (mode_ == DialogMode::create_archive &&
                    id == kVolumeUnit && HIWORD(wparam) == CBN_SELCHANGE) {
                    update_create_dependencies();
                    return 0;
                }
                switch (id) {
                    case kCompressionPreview:
                        if (mode_ == DialogMode::create_archive &&
                            HIWORD(wparam) == STN_CLICKED) {
                            select_compression_curve_level();
                        }
                        return 0;
                    case kBrowse: browse(); return 0;
                    case kPickAccentColor:
                        if (mode_ == DialogMode::settings) {
                            pick_settings_accent_color();
                        }
                        return 0;
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
                    case kBrowseSigningKey:
                        if (const auto key = browse_signing_key(window_)) {
                            set_window_text(signing_key_edit_, key->wstring());
                        }
                        return 0;
                    case kSaveCompressionProfile:
                        if (mode_ == DialogMode::create_archive) save_compression_profile();
                        return 0;
                    case kDeleteCompressionProfile:
                        if (mode_ == DialogMode::create_archive) delete_compression_profile();
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
                    case kToolbarResetDefaults:
                        if (mode_ == DialogMode::settings) {
                            application_options.toolbar_commands =
                                default_toolbar_commands();
                            load_settings_values();
                            InvalidateRect(window_, nullptr, TRUE);
                        }
                        return 0;
                    case kCustomizeFileColumns:
                        if (mode_ == DialogMode::settings) {
                            customize_file_columns();
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
                    case kContentDedup:
                        toggle(kContentDedup, content_dedup_); return 0;
                    case kEncryptData: toggle(kEncryptData, encrypt_data_); return 0;
                    case kEncryptNames: toggle(kEncryptNames, encrypt_names_); return 0;
                    case kShowPassword: toggle(kShowPassword, show_password_); return 0;
                    case kRecoveryVolumes:
                        toggle(kRecoveryVolumes, recovery_volumes_); return 0;
                    case kSignArchive: toggle(kSignArchive, sign_archive_); return 0;
                    case kCreateSfx: toggle(kCreateSfx, create_sfx_); return 0;
                    case kSfxAllowPathChange:
                        toggle(kSfxAllowPathChange, sfx_allow_path_change_);
                        return 0;
                    case kSfxRequireAccept:
                        toggle(kSfxRequireAccept, sfx_require_accept_);
                        return 0;
                    case kSfxOpenDestination:
                        toggle(kSfxOpenDestination, sfx_open_destination_);
                        return 0;
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
            set_dark_title(window, self->palette_.dark);
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
    bool layered_reveal_pending_ = false;
    Palette palette_;
    HBRUSH window_brush_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    HFONT font_ = nullptr;
    TooltipManager tooltip_;
    bool accepted_ = false;
    int level_ = 5;
    bool overwrite_checked_ = false;
    bool restore_time_checked_ = true;
    bool confirm_delete_checked_ = true;
    bool show_hidden_checked_ = true;
    bool create_show_password_ = false;
    int create_page_ = 0;
    int settings_page_ = 0;
    HWND create_navigation_ = nullptr;
    HWND create_viewport_ = nullptr;
    HWND create_page_heading_ = nullptr;
    HWND settings_tabs_ = nullptr;
    HWND settings_viewport_ = nullptr;
    SIZE settings_layout_client_size_{};
    std::array<int, kSettingsTabNames.size()> settings_scroll_offsets_{};
    std::array<int, kCreateTabNames.size()> create_scroll_offsets_{};
    std::vector<PageControl> page_controls_;
    std::vector<SettingControl> settings_controls_;
    DarkTableView toolbar_list_;
    HIMAGELIST toolbar_image_list_ = nullptr;
    HWND toolbar_status_combo_ = nullptr;
    HWND summary_ = nullptr;
    HWND path_label_ = nullptr;
    HWND path_edit_ = nullptr;
    HWND browse_ = nullptr;
    HWND format_label_ = nullptr;
    HWND format_combo_ = nullptr;
    HWND compression_profile_label_ = nullptr;
    HWND compression_profile_combo_ = nullptr;
    HWND save_compression_profile_ = nullptr;
    HWND delete_compression_profile_ = nullptr;
    HWND method_label_ = nullptr;
    HWND method_combo_ = nullptr;
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
    HWND thread_model_label_ = nullptr;
    HWND thread_model_combo_ = nullptr;
    HWND compression_info_ = nullptr;
    HWND compression_preview_ = nullptr;
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
    HWND sfx_stub_tier_label_ = nullptr;
    HWND sfx_stub_tier_combo_ = nullptr;
    HWND sfx_title_label_ = nullptr;
    HWND sfx_title_edit_ = nullptr;
    HWND sfx_default_path_label_ = nullptr;
    HWND sfx_default_path_combo_ = nullptr;
    HWND sfx_overwrite_label_ = nullptr;
    HWND sfx_overwrite_combo_ = nullptr;
    HWND sfx_mode_label_ = nullptr;
    HWND sfx_mode_combo_ = nullptr;
    HWND sfx_elevation_label_ = nullptr;
    HWND sfx_elevation_combo_ = nullptr;
    HWND sfx_run_program_label_ = nullptr;
    HWND sfx_run_program_edit_ = nullptr;
    HWND sfx_run_arguments_label_ = nullptr;
    HWND sfx_run_arguments_edit_ = nullptr;
    HWND sfx_license_label_ = nullptr;
    HWND sfx_license_edit_ = nullptr;
    HWND sfx_allow_path_change_ = nullptr;
    HWND sfx_require_accept_ = nullptr;
    HWND sfx_open_destination_ = nullptr;
    HWND sfx_description_label_ = nullptr;
    HWND sfx_description_edit_ = nullptr;
    HWND sfx_theme_label_ = nullptr;
    HWND sfx_theme_combo_ = nullptr;
    HWND content_dedup_ = nullptr;
    HWND dedup_min_chunk_label_ = nullptr;
    HWND dedup_min_chunk_edit_ = nullptr;
    HWND dedup_average_chunk_label_ = nullptr;
    HWND dedup_average_chunk_edit_ = nullptr;
    HWND dedup_max_chunk_label_ = nullptr;
    HWND dedup_max_chunk_edit_ = nullptr;
    HWND dedup_info_ = nullptr;
    HWND overwrite_ = nullptr;
    HWND restore_time_ = nullptr;
    HWND confirm_delete_ = nullptr;
    HWND show_hidden_ = nullptr;
    HWND accept_ = nullptr;
    HWND cancel_ = nullptr;
    HWND apply_ = nullptr;
    HWND defaults_ = nullptr;
    HWND settings_shortcut_default_label_ = nullptr;
    bool applying_compression_profile_ = false;
    mutable std::mutex compression_curve_mutex_;
    std::optional<CompressionEstimateCurveResult>
        compression_curve_result_;
    std::wstring compression_curve_result_key_;
    std::wstring compression_curve_active_key_;
    std::wstring compression_curve_status_;
    std::wstring compression_curve_error_;
    std::shared_ptr<OperationControl>
        compression_curve_operation_;
    std::jthread compression_curve_worker_;
    std::atomic_bool compression_curve_update_posted_{false};
    bool compression_curve_running_ = false;
    bool compression_curve_restart_pending_ = false;
};

}  // namespace

bool show_create_archive_dialog(HWND owner,
                                const std::vector<fs::path>& inputs,
                                CreateArchiveDialogOptions& options) {
    OptionsDialog dialog(DialogMode::create_archive);
    dialog.create_options = options;
    dialog.input_count = inputs.size();
    dialog.estimate_inputs = inputs;
    const bool accepted = dialog.show(owner);
    if (!accepted) {
        options.compression_profiles =
            std::move(dialog.create_options.compression_profiles);
        options.compression_profiles_changed =
            dialog.create_options.compression_profiles_changed;
        return false;
    }
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

axiom::sfx::SfxConfig sfx_config_from_features(const ArchiveFeatureOptions& features) {
    axiom::sfx::SfxConfig config;
    config.title = narrow_utf8(features.sfx_title);
    config.description = narrow_utf8(features.sfx_description);
    config.default_path = narrow_utf8(features.sfx_default_path);
    config.license_text = narrow_utf8(features.sfx_license_text);
    config.run_program = narrow_utf8(features.sfx_run_program);
    config.run_arguments = narrow_utf8(features.sfx_run_arguments);
    config.allow_path_change = features.sfx_allow_path_change;
    config.require_accept =
        features.sfx_require_accept && !config.license_text.empty();
    config.open_destination = features.sfx_open_destination;
    switch (std::clamp(features.sfx_overwrite, 0, 2)) {
        case 1: config.overwrite = ExtractOptions::Overwrite::skip; break;
        case 2: config.overwrite = ExtractOptions::Overwrite::fail; break;
        default: config.overwrite = ExtractOptions::Overwrite::overwrite; break;
    }
    switch (std::clamp(features.sfx_mode, 0, 2)) {
        case 1: config.mode = axiom::sfx::SfxMode::silent; break;
        case 2: config.mode = axiom::sfx::SfxMode::very_silent; break;
        default: config.mode = axiom::sfx::SfxMode::interactive; break;
    }
    switch (std::clamp(features.sfx_theme, 0, 2)) {
        case 1: config.theme = axiom::sfx::SfxTheme::light; break;
        case 2: config.theme = axiom::sfx::SfxTheme::dark; break;
        default: config.theme = axiom::sfx::SfxTheme::automatic; break;
    }
    switch (std::clamp(features.sfx_elevation, 0, 2)) {
        case 1: config.elevation = axiom::sfx::SfxElevation::automatic; break;
        case 2: config.elevation = axiom::sfx::SfxElevation::require; break;
        default: config.elevation = axiom::sfx::SfxElevation::none; break;
    }
    if (config.run_program.empty()) config.run_arguments.clear();
    return config;
}

axiom::sfx::SfxStubTier sfx_stub_tier_from_features(
    const ArchiveFeatureOptions& features) {
    return features.sfx_stub_tier == 1 ? axiom::sfx::SfxStubTier::mini
                                       : axiom::sfx::SfxStubTier::full;
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
