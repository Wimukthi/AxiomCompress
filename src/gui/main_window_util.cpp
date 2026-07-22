// General helpers shared by the main-window translation units:
// text conversion and formatting, registry access, shell icons, and file
// pickers.

#include "gui/main_window_internal.hpp"

namespace axiom::gui {

std::wstring widen(std::string_view text, UINT code_page) {
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
std::wstring digits = std::to_wstring(count);
for (std::ptrdiff_t pos = static_cast<std::ptrdiff_t>(digits.size()) - 3;
     pos > 0;
     pos -= 3) {
    digits.insert(static_cast<std::size_t>(pos), 1, L',');
}
stream << digits << L' ' << (count == 1 ? singular : plural);
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

std::wstring format_packed_size(const std::optional<std::uint64_t>& size,
                            bool estimated) {
if (!size) return {};
return estimated ? (L"\u2248 " + format_size(*size)) : format_size(*size);
}

std::wstring format_ratio(std::uint64_t packed, std::uint64_t unpacked) {
if (unpacked == 0) return L"n/a";
std::wstringstream stream;
const double stored_percent =
    (static_cast<double>(packed) * 100.0) / static_cast<double>(unpacked);
stream << std::fixed << std::setprecision(1) << stored_percent << L"% of original";
if (packed != 0) {
    const double ratio =
        static_cast<double>(unpacked) / static_cast<double>(packed);
    stream << L" (" << std::fixed << std::setprecision(2) << ratio << L"x)";
}
return stream.str();
}

std::uint64_t saturated_add(std::uint64_t left, std::uint64_t right) {
const auto max = std::numeric_limits<std::uint64_t>::max();
return right > max - left ? max : left + right;
}

ArchiveEntryTotals summarize_archive_entries(
const std::vector<axiom::ArchiveEntry>& entries) {
ArchiveEntryTotals totals;
for (const auto& entry : entries) {
    if (entry.is_directory) {
        ++totals.directories;
        continue;
    }
    if (entry.is_symlink || entry.is_hardlink) {
        continue;
    }
    ++totals.files;
    totals.unpacked = saturated_add(totals.unpacked, entry.size);
    if (entry.packed_size) {
        totals.has_packed = true;
        totals.packed = saturated_add(totals.packed, *entry.packed_size);
        totals.packed_estimated =
            totals.packed_estimated || entry.packed_size_estimated;
    }
}
return totals;
}

std::wstring format_crc32(const std::optional<std::uint32_t>& crc) {
if (!crc) return {};
wchar_t text[9]{};
swprintf_s(text, L"%08X", *crc);
return text;
}

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

std::size_t configured_io_buffer_size(
const axiom::gui::ApplicationDialogOptions& options) {
if (options.io_buffer_mode != 1) {
    return 0;
}
const auto parsed = parse_size_setting(options.io_buffer_size);
if (!parsed) {
    return 0;
}
return static_cast<std::size_t>(
    std::min<std::uint64_t>(*parsed, std::numeric_limits<std::size_t>::max()));
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

ShellIconRef shell_icon_for_path(const fs::path& path, bool drive) {
axiom::gui::BrowserItem item;
if (drive) {
    item.kind = axiom::gui::BrowserItemKind::drive;
} else {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.kind = directory ? axiom::gui::BrowserItemKind::directory
                              : axiom::gui::BrowserItemKind::file;
    } else {
        // Existing callers pass "folder" when they need the generic folder
        // shell icon. For real filesystem paths, do not assume directory:
        // the tree now contains files as first-class items.
        item.kind = path.has_extension() ? axiom::gui::BrowserItemKind::file
                                         : axiom::gui::BrowserItemKind::directory;
    }
}
item.filesystem_path = path;
item.name = path.filename().wstring();
return shell_icon_for_item(item);
}

std::wstring lowercase_extension_for_icon(const fs::path& path) {
std::wstring extension = path.extension().wstring();
std::transform(extension.begin(), extension.end(), extension.begin(),
               [](wchar_t value) {
                   return static_cast<wchar_t>(std::towlower(value));
               });
return extension;
}

bool extension_can_have_unique_shell_icon(std::wstring_view extension) {
static constexpr std::array<std::wstring_view, 11> extensions{
    L".exe", L".com", L".scr", L".ico", L".cur", L".ani",
    L".lnk", L".url", L".appref-ms", L".msi", L".msp",
};
return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

bool item_needs_real_shell_icon(const axiom::gui::BrowserItem& item) {
if (item.filesystem_path.empty()) return false;
if (item.kind == axiom::gui::BrowserItemKind::drive ||
    item.kind == axiom::gui::BrowserItemKind::directory ||
    item.kind == axiom::gui::BrowserItemKind::parent) {
    return false;
}
return extension_can_have_unique_shell_icon(
    lowercase_extension_for_icon(item.filesystem_path));
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
const bool needs_real_file = item_needs_real_shell_icon(item);
if (item.kind != axiom::gui::BrowserItemKind::drive && !needs_real_file) {
    flags |= SHGFI_USEFILEATTRIBUTES;
}
auto image_list = reinterpret_cast<HIMAGELIST>(
    SHGetFileInfoW(query.c_str(), attributes, &info, sizeof(info), flags));
if (image_list == nullptr && needs_real_file) {
    flags |= SHGFI_USEFILEATTRIBUTES;
    image_list = reinterpret_cast<HIMAGELIST>(
        SHGetFileInfoW(query.c_str(), attributes, &info, sizeof(info), flags));
}
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
std::wstring combined_pattern;
std::wstring combined_name = L"Supported archives";
bool first = true;
for (const auto& format : axiom::supported_archive_formats()) {
    if (!first) {
        combined_pattern += L";";
    }
    combined_pattern += format.open_filter_pattern;
    first = false;
}
// Split/recovery volumes are validated before normal provider probing.
if (!combined_pattern.empty()) combined_pattern += L";";
combined_pattern += L"*.rev*;*.part*.axar";
combined_name += L" (" + combined_pattern + L")";

COMDLG_FILTERSPEC filters[] = {
    {combined_name.c_str(), combined_pattern.c_str()},
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

bool high_contrast_enabled() {
HIGHCONTRASTW high_contrast{};
high_contrast.cbSize = sizeof(high_contrast);
return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(high_contrast),
                             &high_contrast, 0) &&
       (high_contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

COLORREF blend_color(COLORREF base, COLORREF overlay, int overlay_percent) {
overlay_percent = std::clamp(overlay_percent, 0, 100);
const int base_percent = 100 - overlay_percent;
return RGB((GetRValue(base) * base_percent + GetRValue(overlay) * overlay_percent) / 100,
           (GetGValue(base) * base_percent + GetGValue(overlay) * overlay_percent) / 100,
           (GetBValue(base) * base_percent + GetBValue(overlay) * overlay_percent) / 100);
}

COLORREF readable_selection_text(COLORREF background) {
const int luminance = GetRValue(background) * 299 +
                      GetGValue(background) * 587 +
                      GetBValue(background) * 114;
return luminance > 150000 ? RGB(0, 0, 0) : RGB(255, 255, 255);
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
    case kUpdateArchive:
    case kSynchronizeArchive:
    case kDeleteArchiveEntries:
    case kRepackArchive:
    case kEditArchiveComment:
    case kLockArchive:
    case kRepairArchive:
    case kVerifyArchiveSignature:
    case kCreateSfx:
    case kFreshenArchive:
    case kBenchmark:
    case kFind:
    case kCopyPath:
    case kCopyCrc32:
    case kAddFavorite:
    case kRemoveFavorite:
    case kToggleTreePane:
    case kNavigateBack:
    case kNavigateForward:
    case kNavigateUp:
    case kNavigateRefresh:
    case kAddressGo:
    case kView:
    case kDelete:
    case kSelectAll:
    case kInfo:
    case kSettings:
    case kCheckUpdates:
        return true;
    default:
        return false;
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

std::wstring folded_text(std::wstring_view text) {
std::wstring result(text);
std::transform(result.begin(), result.end(), result.begin(),
               [](wchar_t value) {
                   return static_cast<wchar_t>(std::towlower(value));
               });
return result;
}

bool starts_with_folded(std::wstring_view text, std::wstring_view prefix) {
if (prefix.size() > text.size()) return false;
return folded_text(text.substr(0, prefix.size())) == folded_text(prefix);
}

bool contains_folded(std::wstring_view text, std::wstring_view needle) {
if (needle.empty()) return true;
return folded_text(text).find(folded_text(needle)) != std::wstring::npos;
}

}  // namespace axiom::gui
