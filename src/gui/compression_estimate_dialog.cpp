#define NOMINMAX
#include "gui/compression_estimate_dialog.hpp"

#include "gui/dialog_support.hpp"

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace axiom::gui {
namespace {

constexpr wchar_t kEstimateDialogClass[] = L"AxiomCompressionEstimateDialog";
constexpr int kEstimateButton = 6101;
constexpr int kCloseButton = 6102;
constexpr int kCompressionLevel = 6103;
constexpr UINT kSnapshotMessage = WM_APP + 61;
constexpr UINT kDoneMessage = WM_APP + 62;
constexpr UINT kMetadataProgressMessage = WM_APP + 63;
constexpr UINT kMetadataDoneMessage = WM_APP + 64;
constexpr int kDialogClientWidth = 680;
constexpr int kDialogClientHeight = 600;
constexpr DWORD kEstimateDialogStyle =
    WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
constexpr DWORD kEstimateDialogExStyle = WS_EX_WINDOWEDGE | WS_EX_CONTROLPARENT;
constexpr auto kMetadataRefreshInterval = std::chrono::milliseconds(50);
constexpr std::array<const wchar_t*, 9> kCompressionLevelNames{
    L"1 - Fastest", L"2 - Very fast", L"3 - Fast", L"4 - Normal",
    L"5 - Balanced", L"6 - Strong", L"7 - High", L"8 - Very high",
    L"9 - Maximum"};

std::wstring format_bytes(std::uint64_t bytes) {
    constexpr const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::wstringstream stream;
    stream << std::fixed << std::setprecision(unit == 0 ? 0 : 1)
           << value << L' ' << units[unit];
    return stream.str();
}

std::wstring format_duration(std::uint64_t seconds) {
    const std::uint64_t hours = seconds / 3600;
    const std::uint64_t minutes = (seconds % 3600) / 60;
    const std::uint64_t remainder = seconds % 60;
    std::wstringstream stream;
    if (hours != 0) stream << hours << L"h ";
    if (hours != 0 || minutes != 0) stream << minutes << L"m ";
    stream << remainder << L's';
    return stream.str();
}

std::wstring format_decimal(double value, int precision, wchar_t suffix = L'\0') {
    std::wstringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    if (suffix != L'\0') stream << suffix;
    return stream.str();
}

std::wstring widen_utf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0);
    if (length <= 0) return L"Estimation failed.";
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), length);
    return result;
}

void fill_rect(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void frame_rect(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}

struct MetadataProgress {
    std::uint64_t logical_bytes = 0;
    std::uint64_t allocated_bytes = 0;
    std::uint64_t file_count = 0;
    std::uint64_t folder_count = 0;
    std::uint64_t other_count = 0;
    std::uint64_t warning_count = 0;
};

struct FilesystemInformation : MetadataProgress {
    bool contains_directories = false;
    std::wstring type;
    std::wstring location;
    std::wstring created;
    std::wstring modified;
    std::wstring accessed;
    std::wstring attributes;
};

std::uint64_t saturated_add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

std::optional<std::uint64_t> allocation_size(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    FILE_STANDARD_INFO information{};
    const bool loaded = GetFileInformationByHandleEx(
        file, FileStandardInfo, &information, sizeof(information)) != FALSE;
    CloseHandle(file);
    if (!loaded) return std::nullopt;
    return static_cast<std::uint64_t>(information.AllocationSize.QuadPart);
}

std::wstring format_file_time(const FILETIME& utc) {
    FILETIME local{};
    SYSTEMTIME value{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &value)) {
        return L"-";
    }
    wchar_t date[96]{};
    wchar_t time[96]{};
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &value,
                        nullptr, date, static_cast<int>(std::size(date)), nullptr) == 0 ||
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &value,
                        nullptr, time, static_cast<int>(std::size(time))) == 0) {
        return L"-";
    }
    return std::wstring(date) + L"  " + time;
}

std::wstring attribute_text(DWORD attributes) {
    std::wstring result;
    const auto add = [&](std::wstring_view name) {
        if (!result.empty()) result += L", ";
        result += name;
    };
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) add(L"Read-only");
    if ((attributes & FILE_ATTRIBUTE_HIDDEN) != 0) add(L"Hidden");
    if ((attributes & FILE_ATTRIBUTE_SYSTEM) != 0) add(L"System");
    if ((attributes & FILE_ATTRIBUTE_ARCHIVE) != 0) add(L"Archive");
    if ((attributes & FILE_ATTRIBUTE_COMPRESSED) != 0) add(L"Compressed");
    if ((attributes & FILE_ATTRIBUTE_ENCRYPTED) != 0) add(L"Encrypted");
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) add(L"Reparse point");
    return result.empty() ? L"None" : result;
}

std::wstring shell_type_name(const std::filesystem::path& path, DWORD attributes) {
    SHFILEINFOW information{};
    if (SHGetFileInfoW(path.c_str(), attributes, &information, sizeof(information),
                       SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) != 0 &&
        information.szTypeName[0] != L'\0') {
        return information.szTypeName;
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? L"File folder" : L"File";
}

FilesystemInformation scan_filesystem_information(
    const std::vector<std::filesystem::path>& inputs,
    const std::shared_ptr<OperationControl>& operation,
    const std::function<void(const MetadataProgress&)>& progress) {
    FilesystemInformation result;
    if (inputs.size() == 1) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(inputs.front().c_str(), GetFileExInfoStandard, &data)) {
            result.type = shell_type_name(inputs.front(), data.dwFileAttributes);
            result.location = inputs.front().parent_path().wstring();
            result.created = format_file_time(data.ftCreationTime);
            result.modified = format_file_time(data.ftLastWriteTime);
            result.accessed = format_file_time(data.ftLastAccessTime);
            result.attributes = attribute_text(data.dwFileAttributes);
        }
    } else {
        result.type = L"Multiple items";
        std::filesystem::path common = inputs.empty()
            ? std::filesystem::path{} : inputs.front().parent_path();
        for (const auto& input : inputs) {
            if (input.parent_path() != common) {
                common.clear();
                break;
            }
        }
        result.location = common.empty() ? L"Multiple locations" : common.wstring();
        result.created = result.modified = result.accessed = L"Multiple values";
        result.attributes = L"Multiple values";
    }
    if (result.type.empty()) result.type = L"Unavailable";
    if (result.location.empty()) result.location = L"-";
    if (result.created.empty()) result.created = L"-";
    if (result.modified.empty()) result.modified = L"-";
    if (result.accessed.empty()) result.accessed = L"-";
    if (result.attributes.empty()) result.attributes = L"-";

    std::set<std::filesystem::path> seen;
    std::uint64_t since_progress = 0;
    auto last_progress = std::chrono::steady_clock::now();
    const auto record = [&](const std::filesystem::path& path,
                            const std::filesystem::file_status& status) {
        if (!seen.insert(path.lexically_normal()).second) return;
        if (std::filesystem::is_regular_file(status)) {
            std::error_code error;
            const std::uint64_t size = std::filesystem::file_size(path, error);
            if (error) {
                ++result.warning_count;
            } else {
                result.logical_bytes = saturated_add(result.logical_bytes, size);
                const auto allocated = allocation_size(path);
                if (allocated) {
                    result.allocated_bytes = saturated_add(
                        result.allocated_bytes, *allocated);
                } else {
                    ++result.warning_count;
                }
            }
            ++result.file_count;
        } else if (std::filesystem::is_directory(status)) {
            ++result.folder_count;
        } else {
            ++result.other_count;
        }
        if (++since_progress >= 128) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_progress >= kMetadataRefreshInterval) {
                since_progress = 0;
                last_progress = now;
                progress(result);
            }
        }
    };

    for (const auto& input : inputs) {
        if (operation) operation->checkpoint();
        std::error_code error;
        const auto status = std::filesystem::symlink_status(input, error);
        if (error) {
            ++result.warning_count;
            continue;
        }
        if (!std::filesystem::is_directory(status) ||
            std::filesystem::is_symlink(status)) {
            record(input, status);
            continue;
        }
        result.contains_directories = true;
        std::filesystem::recursive_directory_iterator iterator(
            input, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        if (error) {
            ++result.warning_count;
            continue;
        }
        while (iterator != end) {
            if (operation) operation->checkpoint();
            std::error_code status_error;
            const auto entry_status = iterator->symlink_status(status_error);
            if (status_error) {
                ++result.warning_count;
            } else {
                record(iterator->path(), entry_status);
            }
            iterator.increment(error);
            if (error) {
                ++result.warning_count;
                error.clear();
            }
        }
    }
    progress(result);
    return result;
}

struct EstimateDialogState {
    HWND hwnd{};
    HWND owner{};
    HWND estimate_button{};
    HWND close_button{};
    HWND level_combo{};
    HWND tooltip{};
    HINSTANCE instance{};
    HFONT font{};
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    bool dark{};
    bool owner_was_enabled{};
    bool running{};
    bool metadata_running{};
    bool closing{};
    std::atomic_bool cancelled{false};
    std::vector<std::filesystem::path> inputs;
    CompressionEstimateOptions options;
    int compression_level = 5;
    std::shared_ptr<OperationControl> operation;
    std::shared_ptr<OperationControl> metadata_operation;
    std::jthread worker;
    std::jthread metadata_worker;
    std::mutex result_mutex;
    std::optional<CompressionEstimateSnapshot> snapshot;
    std::optional<CompressionEstimateResult> result;
    std::optional<MetadataProgress> metadata_progress;
    std::optional<FilesystemInformation> information;
    std::wstring error;
};

int scale(const EstimateDialogState& state, int value) {
    return scale_for_dialog_dpi(value, state.dpi);
}

SIZE minimum_dialog_window_size(const EstimateDialogState& state) {
    return dialog_window_size_for_client(
        kDialogClientWidth, kDialogClientHeight,
        kEstimateDialogStyle, kEstimateDialogExStyle, state.dpi);
}

void enforce_minimum_dialog_size(EstimateDialogState& state) {
    RECT current{};
    if (!GetWindowRect(state.hwnd, &current)) return;
    const SIZE minimum = minimum_dialog_window_size(state);
    const int width = (std::max)(current.right - current.left, minimum.cx);
    const int height = (std::max)(current.bottom - current.top, minimum.cy);
    SetWindowPos(state.hwnd, nullptr, current.left, current.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void layout_dialog(EstimateDialogState& state) {
    if (state.hwnd == nullptr) return;
    RECT client{};
    GetClientRect(state.hwnd, &client);
    const int margin = scale(state, 24);
    const int button_width = scale(state, 106);
    const int button_height = scale(state, 32);
    const int bottom = client.bottom - margin - button_height;
    MoveWindow(state.estimate_button, margin, bottom,
               button_width, button_height, TRUE);
    MoveWindow(state.close_button, client.right - margin - button_width, bottom,
               button_width, button_height, TRUE);
    MoveWindow(state.level_combo, client.right - margin - scale(state, 150),
               scale(state, 326), scale(state, 150), scale(state, 250), TRUE);
    // Match the About dialog: a DPI relayout must repaint every owner-drawn row,
    // otherwise pixels rendered with the previous monitor's scale can survive.
    InvalidateRect(state.hwnd, nullptr, TRUE);
}

void apply_theme(EstimateDialogState& state) {
    apply_dialog_dark_frame(state.hwnd, state.dark);
    apply_dialog_control_theme(state.estimate_button, state.dark);
    apply_dialog_control_theme(state.close_button, state.dark);
    apply_dialog_control_theme(state.level_combo, state.dark);
    InvalidateRect(state.hwnd, nullptr, TRUE);
}

void rebuild_font(EstimateDialogState& state) {
    delete_dialog_font(state.font);
    state.font = create_dialog_font(state.dpi);
    set_dialog_control_font(state.estimate_button, state.font);
    set_dialog_control_font(state.close_button, state.font);
    set_dialog_control_font(state.level_combo, state.font);
    SendMessageW(state.level_combo, CB_SETITEMHEIGHT, 0, scale(state, 24));
    SendMessageW(state.level_combo, CB_SETITEMHEIGHT,
                 static_cast<WPARAM>(-1), scale(state, 24));
}

void invalidate_dialog_band(const EstimateDialogState& state, int top, int bottom) {
    RECT client{};
    GetClientRect(state.hwnd, &client);
    RECT band{0, scale(state, top), client.right, scale(state, bottom)};
    InvalidateRect(state.hwnd, &band, FALSE);
}

void draw_text_line(HDC dc, HFONT font, COLORREF color, const std::wstring& text,
                    RECT rect, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                             DT_NOPREFIX | DT_END_ELLIPSIS) {
    HGDIOBJ old_font = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), -1, &rect, flags);
    SelectObject(dc, old_font);
}

void paint_dialog(EstimateDialogState& state) {
    PAINTSTRUCT paint{};
    HDC paint_dc = BeginPaint(state.hwnd, &paint);
    RECT client{};
    GetClientRect(state.hwnd, &client);
    HDC buffer_dc = CreateCompatibleDC(paint_dc);
    HBITMAP buffer_bitmap = buffer_dc == nullptr ? nullptr : CreateCompatibleBitmap(
        paint_dc, (std::max)(1L, client.right), (std::max)(1L, client.bottom));
    HGDIOBJ old_bitmap = buffer_bitmap == nullptr
        ? nullptr : SelectObject(buffer_dc, buffer_bitmap);
    const bool buffered = buffer_dc != nullptr && buffer_bitmap != nullptr &&
                          old_bitmap != nullptr && old_bitmap != HGDI_ERROR;
    HDC dc = buffered ? buffer_dc : paint_dc;
    const DialogColors colors = dialog_colors(state.dark);
    fill_rect(dc, client, colors.background);

    const int margin = scale(state, 24);
    const int right = client.right - margin;
    draw_text_line(dc, state.font, colors.text, L"Filesystem information",
                   {margin, scale(state, 18), right, scale(state, 44)});

    const std::wstring selection = state.inputs.size() == 1
        ? state.inputs.front().wstring()
        : std::to_wstring(state.inputs.size()) + L" selected inputs";
    draw_text_line(dc, state.font, colors.disabled_text, selection,
                   {margin, scale(state, 46), right, scale(state, 70)});
    CompressionEstimateSnapshot snapshot;
    CompressionEstimateResult result;
    MetadataProgress metadata;
    FilesystemInformation information;
    bool has_snapshot = false;
    bool has_result = false;
    bool has_metadata = false;
    bool has_information = false;
    std::wstring error;
    {
        std::lock_guard lock(state.result_mutex);
        if (state.snapshot) {
            snapshot = *state.snapshot;
            has_snapshot = true;
        }
        if (state.result) {
            result = *state.result;
            has_result = true;
        }
        if (state.metadata_progress) {
            metadata = *state.metadata_progress;
            has_metadata = true;
        }
        if (state.information) {
            information = *state.information;
            metadata = information;
            has_metadata = true;
            has_information = true;
        }
        error = state.error;
    }

    const std::wstring type = has_information ? information.type : L"Scanning...";
    const std::wstring location = has_information ? information.location
        : state.inputs.size() == 1 ? state.inputs.front().parent_path().wstring()
                                   : L"Calculating common location...";
    const std::wstring size_text = has_metadata
        ? format_bytes(metadata.logical_bytes) + L"  (" +
              format_bytes(metadata.allocated_bytes) + L" on disk)"
        : L"Scanning...";
    const std::wstring contains_label = has_information && information.contains_directories
        ? L"Contains: " : L"Items: ";
    const std::wstring contains = has_metadata
        ? std::to_wstring(metadata.file_count) + L" files, " +
              std::to_wstring(metadata.folder_count) + L" folders" +
              (metadata.other_count == 0 ? L"" : L", " +
                   std::to_wstring(metadata.other_count) + L" other items")
        : L"Scanning...";
    draw_text_line(dc, state.font, colors.text, L"Type: " + type,
                   {margin, scale(state, 80), right, scale(state, 104)});
    draw_text_line(dc, state.font, colors.text, L"Location: " + location,
                   {margin, scale(state, 104), right, scale(state, 128)});
    draw_text_line(dc, state.font, colors.text, L"Size: " + size_text,
                   {margin, scale(state, 128), right, scale(state, 152)});
    draw_text_line(dc, state.font, colors.text, contains_label + contains,
                   {margin, scale(state, 152), right, scale(state, 176)});
    draw_text_line(dc, state.font, colors.disabled_text,
                   L"Created: " + (has_information ? information.created : L"Scanning..."),
                   {margin, scale(state, 182), right, scale(state, 206)});
    draw_text_line(dc, state.font, colors.disabled_text,
                   L"Modified: " + (has_information ? information.modified : L"Scanning..."),
                   {margin, scale(state, 206), right, scale(state, 230)});
    draw_text_line(dc, state.font, colors.disabled_text,
                   L"Accessed: " + (has_information ? information.accessed : L"Scanning..."),
                   {margin, scale(state, 230), right, scale(state, 254)});
    draw_text_line(dc, state.font, colors.disabled_text,
                   L"Attributes: " + (has_information ? information.attributes : L"Scanning..."),
                   {margin, scale(state, 254), right, scale(state, 278)});
    const std::wstring scan_status = state.metadata_running
        ? L"Scanning contents..."
        : has_information ? L"Filesystem scan complete  |  " +
              std::to_wstring(information.warning_count) + L" warnings"
                          : L"Filesystem information unavailable";
    draw_text_line(dc, state.font, colors.disabled_text, scan_status,
                   {margin, scale(state, 284), right, scale(state, 308)});

    RECT separator{margin, scale(state, 320), right, scale(state, 321)};
    fill_rect(dc, separator, colors.border);
    const wchar_t* format = state.options.format == ArchiveFormat::zip ? L"ZIP" : L"AXAR";
    draw_text_line(dc, state.font, colors.disabled_text,
                   std::wstring(L"Compression estimate: ") + format,
                   {margin, scale(state, 330), scale(state, 430), scale(state, 354)});
    draw_text_line(dc, state.font, colors.disabled_text, L"Level",
                   {right - scale(state, 205), scale(state, 330),
                    right - scale(state, 158), scale(state, 354)});

    double savings = has_result ? result.estimated_savings_percent
                                : has_snapshot ? snapshot.estimated_savings_percent : 0.0;
    std::wstring headline = state.metadata_running
        ? L"Estimate becomes available after the filesystem scan"
        : L"Ready to estimate compression";
    if (state.running && !has_snapshot) headline = L"Calibrating samples...";
    if (has_result) {
        const double compressed = 100.0 - savings;
        headline = savings >= 0.0
            ? L"Estimate complete - compressed: " + format_decimal(compressed, 1, L'%') +
                  L"  |  saved: " + format_decimal(savings, 1, L'%')
            : L"Estimate complete - compressed: " + format_decimal(compressed, 1, L'%') +
                  L"  |  growth: " + format_decimal(-savings, 1, L'%');
    } else if (has_snapshot) {
        const double compressed = 100.0 - savings;
        headline = savings >= 0.0
            ? L"Provisional - compressed: " + format_decimal(compressed, 1, L'%') +
                  L"  |  saved: " + format_decimal(savings, 1, L'%')
            : L"Provisional - compressed: " + format_decimal(compressed, 1, L'%') +
                  L"  |  growth: " + format_decimal(-savings, 1, L'%');
    }
    if (state.cancelled.load(std::memory_order_acquire)) headline = L"Estimation cancelled";
    if (!error.empty()) headline = L"Estimation failed";
    draw_text_line(dc, state.font, colors.text, headline,
                   {margin, scale(state, 364), right, scale(state, 388)});

    RECT track{margin, scale(state, 396), right, scale(state, 424)};
    fill_rect(dc, track, colors.control_background);
    frame_rect(dc, track, colors.border);
    RECT inner = track;
    InflateRect(&inner, -scale(state, 2), -scale(state, 2));
    const bool has_estimate = has_snapshot || has_result;
    const COLORREF compressed_color = state.dark ? RGB(0, 122, 220) : RGB(0, 102, 190);
    const COLORREF saved_color = state.dark ? RGB(46, 178, 93) : RGB(38, 148, 76);
    fill_rect(dc, inner, has_estimate ? saved_color : colors.control_background);
    if (has_estimate) {
        const double compressed_fraction = std::clamp(1.0 - savings / 100.0, 0.0, 1.0);
        RECT compressed = inner;
        compressed.right = compressed.left + static_cast<int>(
            (inner.right - inner.left) * compressed_fraction);
        if (compressed.right > compressed.left) {
            fill_rect(dc, compressed,
                      savings >= 0.0 ? compressed_color : RGB(210, 135, 55));
        }
        if (compressed.right > inner.left && compressed.right < inner.right) {
            const int divider_width = (std::max)(1, scale(state, 2));
            RECT divider{compressed.right - divider_width / 2, inner.top,
                         compressed.right + (divider_width + 1) / 2, inner.bottom};
            fill_rect(dc, divider, colors.background);
        }
    }

    std::wstring archive = L"Estimated archive: -";
    std::wstring ratio = L"Compression ratio: -";
    std::wstring sampling = L"Sampling: not started";
    std::wstring confidence = L"Confidence: -";
    if (has_snapshot) {
        archive = L"Estimated archive: " + format_bytes(snapshot.estimated_archive_bytes);
        ratio = L"Compression ratio: " + format_decimal(snapshot.estimated_ratio, 2) + L"x";
        sampling = L"Sampled " + format_bytes(snapshot.sampled_bytes) + L" of " +
                   format_bytes(snapshot.planned_sample_bytes);
    }
    if (has_result) {
        archive = L"Estimated archive: " + format_bytes(result.estimated_archive_bytes) +
                  L"  (range " + format_bytes(result.estimated_low_bytes) + L" - " +
                  format_bytes(result.estimated_high_bytes) + L")";
        ratio = L"Compression ratio: " + format_decimal(result.estimated_ratio, 2) +
                L"x  |  Estimated time: " + format_duration(result.estimated_seconds);
        sampling = L"Sampled " + format_bytes(result.sampled_bytes) + L"  |  " +
                   std::to_wstring(result.warnings.size()) + L" warnings";
        confidence = std::wstring(L"Confidence: ") +
            (result.confidence == EstimateConfidence::high ? L"High"
             : result.confidence == EstimateConfidence::medium ? L"Medium" : L"Low") +
            L"  |  95% margin: +/- " +
            format_decimal(result.confidence_margin_percent, 1) + L" percentage points";
    }
    if (!error.empty()) {
        archive = error;
        ratio.clear();
        sampling.clear();
        confidence.clear();
    }
    draw_text_line(dc, state.font, colors.text, archive,
                   {margin, scale(state, 434), right, scale(state, 458)});
    draw_text_line(dc, state.font, colors.disabled_text, ratio,
                   {margin, scale(state, 462), right, scale(state, 486)});
    draw_text_line(dc, state.font, colors.disabled_text, sampling,
                   {margin, scale(state, 490), right, scale(state, 514)});
    draw_text_line(dc, state.font, colors.disabled_text, confidence,
                   {margin, scale(state, 518), right, scale(state, 538)});

    if (buffered) {
        BitBlt(paint_dc, paint.rcPaint.left, paint.rcPaint.top,
               paint.rcPaint.right - paint.rcPaint.left,
               paint.rcPaint.bottom - paint.rcPaint.top,
               buffer_dc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
        SelectObject(buffer_dc, old_bitmap);
    }
    if (buffer_bitmap != nullptr) DeleteObject(buffer_bitmap);
    if (buffer_dc != nullptr) DeleteDC(buffer_dc);

    EndPaint(state.hwnd, &paint);
}

void finish_worker(EstimateDialogState& state) {
    if (state.worker.joinable()) state.worker.join();
    state.running = false;
    state.operation.reset();
    if (state.closing && !state.metadata_running) {
        save_named_window_placement(L"CompressionEstimateDialog", state.hwnd);
        DestroyWindow(state.hwnd);
        return;
    }
    if (!state.closing) {
        SetWindowTextW(state.estimate_button, L"Re-estimate");
        EnableWindow(state.estimate_button, TRUE);
        EnableWindow(state.level_combo, TRUE);
    }
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

void finish_metadata_worker(EstimateDialogState& state) {
    if (state.metadata_worker.joinable()) state.metadata_worker.join();
    state.metadata_running = false;
    state.metadata_operation.reset();
    if (state.closing && !state.running) {
        save_named_window_placement(L"CompressionEstimateDialog", state.hwnd);
        DestroyWindow(state.hwnd);
        return;
    }
    if (!state.closing) {
        SetWindowTextW(state.estimate_button, L"Estimate");
        EnableWindow(state.estimate_button, TRUE);
    }
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

void start_metadata_scan(EstimateDialogState& state) {
    if (state.metadata_running) return;
    if (state.metadata_worker.joinable()) state.metadata_worker.join();
    state.metadata_operation = std::make_shared<OperationControl>();
    state.metadata_running = true;
    SetWindowTextW(state.estimate_button, L"Scanning...");
    EnableWindow(state.estimate_button, FALSE);
    InvalidateRect(state.hwnd, nullptr, FALSE);

    const HWND target = state.hwnd;
    const auto operation = state.metadata_operation;
    const auto inputs = state.inputs;
    state.metadata_worker = std::jthread(
        [&state, target, operation, inputs] {
            try {
                auto information = scan_filesystem_information(
                    inputs, operation, [&state, target](const MetadataProgress& value) {
                        {
                            std::lock_guard lock(state.result_mutex);
                            state.metadata_progress = value;
                        }
                        PostMessageW(target, kMetadataProgressMessage, 0, 0);
                    });
                std::lock_guard lock(state.result_mutex);
                state.metadata_progress = information;
                state.information = std::move(information);
            } catch (const OperationCancelled&) {
                // Closing the dialog cancels a potentially long directory walk.
            } catch (...) {
                FilesystemInformation information;
                information.type = L"Unavailable";
                information.location = inputs.size() == 1
                    ? inputs.front().parent_path().wstring() : L"Multiple locations";
                information.created = information.modified = information.accessed = L"-";
                information.attributes = L"-";
                information.warning_count = 1;
                std::lock_guard lock(state.result_mutex);
                state.information = std::move(information);
            }
            PostMessageW(target, kMetadataDoneMessage, 0, 0);
        });
}

void start_estimate(EstimateDialogState& state) {
    if (state.metadata_running || state.closing) return;
    if (state.running) {
        if (state.operation) state.operation->request_cancel();
        SetWindowTextW(state.estimate_button, L"Cancelling...");
        EnableWindow(state.estimate_button, FALSE);
        return;
    }
    if (state.worker.joinable()) state.worker.join();
    {
        std::lock_guard lock(state.result_mutex);
        state.snapshot.reset();
        state.result.reset();
        state.error.clear();
    }
    state.cancelled.store(false, std::memory_order_release);
    state.operation = std::make_shared<OperationControl>();
    state.running = true;
    SetWindowTextW(state.estimate_button, L"Cancel");
    EnableWindow(state.estimate_button, TRUE);
    EnableWindow(state.level_combo, FALSE);
    InvalidateRect(state.hwnd, nullptr, FALSE);

    const HWND target = state.hwnd;
    const auto operation = state.operation;
    const auto inputs = state.inputs;
    const auto base_options = state.options;
    state.worker = std::jthread([&state, target, operation, inputs, base_options] {
        auto options = base_options;
        options.compression.operation = operation;
        options.progress_callback = [&state, target](const CompressionEstimateSnapshot& value) {
            {
                std::lock_guard lock(state.result_mutex);
                state.snapshot = value;
            }
            PostMessageW(target, kSnapshotMessage, 0, 0);
        };
        try {
            auto result = estimate_compression(inputs, options);
            std::lock_guard lock(state.result_mutex);
            state.result = std::move(result);
        } catch (const OperationCancelled&) {
            state.cancelled.store(true, std::memory_order_release);
        } catch (const std::exception& exception) {
            std::lock_guard lock(state.result_mutex);
            state.error = widen_utf8(exception.what());
        } catch (...) {
            std::lock_guard lock(state.result_mutex);
            state.error = L"Unknown estimation failure.";
        }
        PostMessageW(target, kDoneMessage, 0, 0);
    });
}

void request_close(EstimateDialogState& state) {
    if (!state.running && !state.metadata_running) {
        save_named_window_placement(L"CompressionEstimateDialog", state.hwnd);
        DestroyWindow(state.hwnd);
        return;
    }
    state.closing = true;
    if (state.operation) state.operation->request_cancel();
    if (state.metadata_operation) state.metadata_operation->request_cancel();
    SetWindowTextW(state.estimate_button, L"Closing...");
    EnableWindow(state.estimate_button, FALSE);
    EnableWindow(state.close_button, FALSE);
    EnableWindow(state.level_combo, FALSE);
}

void select_compression_level(EstimateDialogState& state) {
    if (state.running) return;
    const LRESULT selection = SendMessageW(state.level_combo, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) return;
    const int level = std::clamp(static_cast<int>(selection) + 1, 1, 9);
    if (level == state.compression_level) return;

    // Rebuild the level preset from defaults so moving down from a high level
    // cannot retain its larger window or optimal-parser fields. Execution and
    // file-handling choices are independent of the selected effort preset.
    const CompressionOptions previous = state.options.compression;
    CompressionOptions compression;
    apply_compression_level(compression, level);
    compression.thread_count = previous.thread_count;
    compression.io_buffer_size = previous.io_buffer_size;
    compression.swarm_parse = level == 7 ? false : previous.swarm_parse;
    compression.force_parallel_blocks = previous.force_parallel_blocks;
    compression.skip_unreadable_files = previous.skip_unreadable_files;
    compression.input_open_retries = previous.input_open_retries;
    compression.enable_file_filters = previous.enable_file_filters;
    state.options.compression = std::move(compression);
    state.options.sample_budget = level >= 8 ? 24u << 20
        : level >= 6 ? 48u << 20 : 64u << 20;
    state.compression_level = level;
    {
        std::lock_guard lock(state.result_mutex);
        state.snapshot.reset();
        state.result.reset();
        state.error.clear();
    }
    state.cancelled.store(false, std::memory_order_release);
    SetWindowTextW(state.estimate_button, L"Estimate");
    invalidate_dialog_band(state, 324, 540);
}

LRESULT CALLBACK estimate_dialog_proc(HWND hwnd, UINT message,
                                      WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<EstimateDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = create == nullptr
            ? nullptr : static_cast<EstimateDialogState*>(create->lpCreateParams);
        if (state == nullptr) return FALSE;
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);

    switch (message) {
        case WM_CREATE:
            state->font = create_dialog_font(state->dpi);
            state->level_combo = CreateWindowExW(
                0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                    WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCompressionLevel)),
                state->instance, nullptr);
            SendMessageW(state->level_combo, CB_SETITEMHEIGHT, 0, scale(*state, 24));
            SendMessageW(state->level_combo, CB_SETITEMHEIGHT,
                         static_cast<WPARAM>(-1), scale(*state, 24));
            for (const wchar_t* level : kCompressionLevelNames) {
                SendMessageW(state->level_combo, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(level));
            }
            SendMessageW(state->level_combo, CB_SETCURSEL,
                         static_cast<WPARAM>(std::clamp(state->compression_level, 1, 9) - 1), 0);
            state->estimate_button = CreateWindowExW(
                0, L"BUTTON", L"Estimate", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEstimateButton)),
                state->instance, nullptr);
            state->close_button = CreateWindowExW(
                0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseButton)),
                state->instance, nullptr);
            set_dialog_control_font(state->estimate_button, state->font);
            set_dialog_control_font(state->close_button, state->font);
            set_dialog_control_font(state->level_combo, state->font);
            state->tooltip = create_dialog_tooltip(hwnd);
            add_dialog_tooltip(
                state->tooltip, state->level_combo,
                L"Select compression level 1 (fastest) through 9 (maximum ratio) for the next estimate.");
            SetWindowTextW(state->estimate_button, L"Scanning...");
            EnableWindow(state->estimate_button, FALSE);
            apply_theme(*state);
            layout_dialog(*state);
            return 0;
        case WM_SIZE:
            layout_dialog(*state);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            const SIZE minimum = minimum_dialog_window_size(*state);
            limits->ptMinTrackSize.x = minimum.cx;
            limits->ptMinTrackSize.y = minimum.cy;
            return 0;
        }
        case WM_DPICHANGED: {
            state->dpi = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            const SIZE window_size = minimum_dialog_window_size(*state);
            // This dialog is not user-resizable. Recreate its fixed logical
            // client size at the destination DPI instead of carrying a legacy
            // or work-area-constrained physical size to the new monitor.
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         window_size.cx, window_size.cy,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            apply_axiom_window_icons(hwnd, state->instance);
            rebuild_font(*state);
            layout_dialog(*state);
            return 0;
        }
        case WM_PAINT:
            paint_dialog(*state);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM:
            if (const auto& draw = *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
                draw.CtlType == ODT_COMBOBOX) {
                draw_dialog_combo_item(draw, state->dark);
            } else {
                draw_dialog_button(draw, state->dark);
            }
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wparam) == kCompressionLevel &&
                HIWORD(wparam) == CBN_SELCHANGE) {
                select_compression_level(*state);
                return 0;
            }
            if (LOWORD(wparam) == kEstimateButton) {
                start_estimate(*state);
                return 0;
            }
            if (LOWORD(wparam) == kCloseButton) {
                request_close(*state);
                return 0;
            }
            break;
        case kSnapshotMessage:
            invalidate_dialog_band(*state, 360, 540);
            return 0;
        case kDoneMessage:
            finish_worker(*state);
            return 0;
        case kMetadataProgressMessage:
            invalidate_dialog_band(*state, 124, 180);
            return 0;
        case kMetadataDoneMessage:
            finish_metadata_worker(*state);
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            state->dark = dialog_should_use_dark();
            apply_theme(*state);
            return 0;
        case WM_CLOSE:
            request_close(*state);
            return 0;
        case WM_DESTROY:
            delete_dialog_font(state->font);
            state->font = nullptr;
            return 0;
        case WM_NCDESTROY:
            state->hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool register_estimate_dialog(HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = estimate_dialog_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = kEstimateDialogClass;
    assign_axiom_window_class_icons(window_class, instance);
    return RegisterClassExW(&window_class) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace

void show_filesystem_information_dialog(
    HWND owner,
    std::vector<std::filesystem::path> inputs,
    CompressionEstimateOptions estimate_options,
    int compression_level) {
    EstimateDialogState state;
    state.owner = owner;
    state.instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (state.instance == nullptr) state.instance = GetModuleHandleW(nullptr);
    state.dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    state.dark = dialog_should_use_dark();
    state.inputs = std::move(inputs);
    state.options = std::move(estimate_options);
    state.compression_level = compression_level;
    if (!register_estimate_dialog(state.instance)) return;

    const SIZE window_size = dialog_window_size_for_client(
        kDialogClientWidth, kDialogClientHeight,
        kEstimateDialogStyle, kEstimateDialogExStyle, state.dpi);
    const int width = window_size.cx;
    const int height = window_size.cy;
    const POINT position = centered_window_position(owner, width, height);
    HWND dialog = CreateWindowExW(
        kEstimateDialogExStyle, kEstimateDialogClass,
        L"Information - Axiom", kEstimateDialogStyle,
        position.x, position.y, width, height, owner, nullptr,
        state.instance, &state);
    if (dialog == nullptr) return;
    apply_axiom_window_icons(dialog, state.instance);
    restore_named_window_placement(dialog, owner, L"CompressionEstimateDialog");
    // Older builds persisted a much shorter estimate window. Never let that
    // placement cover the result rows or action buttons after the dialog grows.
    enforce_minimum_dialog_size(state);
    state.owner_was_enabled = disable_dialog_owner(owner, dialog);
    start_metadata_scan(state);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    MSG message{};
    while (IsWindow(dialog)) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (state.operation) state.operation->request_cancel();
            if (state.metadata_operation) state.metadata_operation->request_cancel();
            if (state.worker.joinable()) state.worker.join();
            if (state.metadata_worker.joinable()) state.metadata_worker.join();
            if (IsWindow(dialog)) DestroyWindow(dialog);
            if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (message_targets_window(dialog, message) &&
            message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            request_close(state);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (state.worker.joinable()) state.worker.join();
    if (state.metadata_worker.joinable()) state.metadata_worker.join();
    restore_dialog_owner(owner, state.owner_was_enabled);
}

}  // namespace axiom::gui
