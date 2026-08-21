#define NOMINMAX
#include "gui/operation_progress_window.hpp"

#include "gui/dialog_support.hpp"
#include "gui/toolbar_icons.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <sstream>
#include <utility>

namespace axiom::gui {

namespace {

constexpr wchar_t kWindowClass[] = L"AxiomOperationProgressWindow";
constexpr int kPauseButton = 1;
constexpr int kCancelButton = 2;
constexpr int kDetailsButton = 3;
constexpr UINT_PTR kAnimationTimer = 1;
// Growing the window is deferred through the queue rather than done inside the
// timer handler, so a resize never interleaves with the paint that is already
// in flight for the old geometry.
constexpr UINT kResizeMessage = WM_APP + 1;

// Layout grid, in 96-DPI logical units. The collapsed view stops after the
// activity row; Details adds the two diagnostic rows below it.
constexpr int kBarHeight = 22;
constexpr int kOverallBarTop = 66;
constexpr int kOverallBarBottom = kOverallBarTop + kBarHeight;
constexpr int kFileBarTop = 162;
constexpr int kFileBarBottom = kFileBarTop + kBarHeight;
constexpr int kRowHeight = 22;
// Compression figures and the sync plan only exist for some operations. Their
// rows are reserved on demand so an extraction or a drag transfer does not
// leave a band of empty dialog above the buttons.
constexpr int kOptionalRowTop = kFileBarBottom + 26;
constexpr int kDetailRowHeight = 20;

std::wstring format_bytes_of(std::uint64_t completed, std::uint64_t total);

std::wstring format_size(std::uint64_t bytes) {
    constexpr const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::wstringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(unit == 0 ? 0 : 1);
    stream << value << L' ' << units[unit];
    return stream.str();
}

// "170 MB of 400 MB". One phrase instead of three labelled fields.
std::wstring format_bytes_of(std::uint64_t completed, std::uint64_t total) {
    return format_size(completed) + L" of " + format_size(total);
}

std::wstring format_count(std::uint64_t value) {
    std::wstring digits = std::to_wstring(value);
    for (std::size_t position = digits.size(); position > 3;) {
        position -= 3;
        digits.insert(position, 1, L',');
    }
    return digits;
}

std::wstring format_percent(std::uint64_t completed, std::uint64_t total) {
    std::wstringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(1);
    stream << (total == 0 ? 0.0
                          : std::min(100.0, static_cast<double>(completed) *
                                                100.0 / total))
           << L'%';
    return stream.str();
}

std::wstring format_duration(std::uint64_t seconds) {
    const std::uint64_t hours = seconds / 3600;
    const std::uint64_t minutes = (seconds % 3600) / 60;
    const std::uint64_t remaining_seconds = seconds % 60;
    std::wstringstream stream;
    if (hours != 0) stream << hours << L"h ";
    if (hours != 0 || minutes != 0) stream << minutes << L"m ";
    stream << remaining_seconds << L's';
    return stream.str();
}

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

std::wstring stage_text(OperationStage stage) {
    switch (stage) {
        case OperationStage::scanning: return L"Scanning";
        case OperationStage::estimating: return L"Estimating compression";
        case OperationStage::comparing: return L"Comparing";
        case OperationStage::reading: return L"Reading";
        case OperationStage::copying: return L"Copying unchanged data";
        case OperationStage::compressing: return L"Compressing";
        case OperationStage::writing: return L"Writing";
        case OperationStage::testing: return L"Testing";
        case OperationStage::extracting: return L"Extracting";
        case OperationStage::transferring: return L"Transferring";
        case OperationStage::recovering: return L"Building recovery data";
        case OperationStage::committing: return L"Committing";
        case OperationStage::finalizing: return L"Finalizing";
    }
    return L"Working";
}

std::pair<std::uint64_t, std::uint64_t> overall_progress(
    const OperationProgress& progress) {
    const std::uint64_t completed = progress.total_bytes > 0
        ? progress.completed_bytes : progress.completed_items;
    const std::uint64_t total = progress.total_bytes > 0
        ? progress.total_bytes : progress.total_items;
    if (progress.phase_count == 0) return {completed, total};

    constexpr std::uint64_t kPhaseScale = 1'000'000;
    const std::uint64_t bounded_phase =
        std::min<std::uint64_t>(progress.phase_index, progress.phase_count - 1);
    const std::uint64_t phase_fraction = total == 0
        ? 0
        : std::min<std::uint64_t>(
              kPhaseScale,
              static_cast<std::uint64_t>(
                  static_cast<long double>(std::min(completed, total)) *
                  kPhaseScale / total));
    return {
        bounded_phase * kPhaseScale + phase_fraction,
        static_cast<std::uint64_t>(progress.phase_count) * kPhaseScale,
    };
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

COLORREF blend_theme_color(COLORREF base, COLORREF overlay, int overlay_percent) {
    const int base_percent = 100 - overlay_percent;
    return RGB(
        (GetRValue(base) * base_percent + GetRValue(overlay) * overlay_percent) / 100,
        (GetGValue(base) * base_percent + GetGValue(overlay) * overlay_percent) / 100,
        (GetBValue(base) * base_percent + GetBValue(overlay) * overlay_percent) / 100);
}

void refresh_operation_theme(OperationWindowTheme& theme) {
    theme.dark = dialog_should_use_dark();
    const DialogColors colors = dialog_colors(theme.dark);
    theme.background = colors.background;
    theme.panel = colors.control_background;
    theme.text = colors.text;
    theme.muted_text = colors.disabled_text;
    theme.border = colors.border;
    theme.button = colors.control_background;
    theme.button_hot =
        blend_theme_color(colors.control_background, colors.focus_border,
                          theme.dark ? 10 : 8);
    theme.button_pressed =
        blend_theme_color(colors.control_background, colors.focus_border,
                          theme.dark ? 18 : 16);
    theme.progress_track = colors.control_background;
    theme.progress_fill = colors.focus_border;
}

} // namespace

OperationProgressWindow::~OperationProgressWindow() {
    close();
    release_back_buffer();
    if (background_brush_ != nullptr) DeleteObject(background_brush_);
    if (font_ != nullptr && font_ != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(font_);
}

bool OperationProgressWindow::create(HWND owner,
                                     HINSTANCE instance,
                                     std::wstring title,
                                     std::filesystem::path output_path,
                                     const OperationWindowTheme& theme,
                                     PauseHandler pause_handler,
                                     CancelHandler cancel_handler,
                                     bool pause_available,
                                     const Placement* placement) {
    close();
    owner_ = owner;
    instance_ = instance;
    title_ = std::move(title);
    output_path_ = std::move(output_path);
    theme_ = theme;
    pause_handler_ = std::move(pause_handler);
    cancel_handler_ = std::move(cancel_handler);
    pause_available_ = pause_available;
    follow_system_theme_ = placement == nullptr || placement->follow_system_theme;
    started_ = std::chrono::steady_clock::now();
    last_progress_time_ = started_;
    last_heartbeat_paint_ = started_;
    progress_source_.reset();
    rate_.reset();
    last_progress_sequence_ = 0;
    details_expanded_ = false;
    file_bar_active_ = false;
    reserved_optional_rows_ = 0;
    paused_ = false;
    cancelling_ = false;
    has_progress_ = false;
    progress_dirty_ = false;
    telemetry_dirty_ = true;
    for (auto& text : telemetry_text_) text.clear();
    pulse_ = 0;
    dpi_ = placement != nullptr && placement->dpi != 0
               ? placement->dpi
               : (owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem());
    if (!register_class()) return false;

    constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                   WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT window_rect{0, 0, scale(640), scale(collapsed_height())};
    AdjustWindowRectExForDpi(&window_rect, kWindowStyle, FALSE, 0, dpi_);
    const int width = window_rect.right - window_rect.left;
    const int height = window_rect.bottom - window_rect.top;
    RECT anchor_rect{};
    if (placement != nullptr && !IsRectEmpty(&placement->anchor)) {
        anchor_rect = placement->anchor;
    } else if (owner == nullptr || !GetWindowRect(owner, &anchor_rect)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor_rect, 0);
    }
    const int x = anchor_rect.left + (anchor_rect.right - anchor_rect.left - width) / 2;
    const int y = anchor_rect.top + (anchor_rect.bottom - anchor_rect.top - height) / 2;
    const std::wstring caption = title_ + L" - Axiom";
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, caption.c_str(),
                            kWindowStyle,
                            x, y, width, height, owner, nullptr, instance, this);
    if (hwnd_ == nullptr) return false;
    apply_axiom_window_icons(hwnd_, instance_);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    if (placement != nullptr && placement->topmost) {
        // An ownerless window has nothing keeping it above the window the user
        // just dropped onto. Raise it without taking focus, so the drop target
        // keeps activation.
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    UpdateWindow(hwnd_);
    return true;
}

bool OperationProgressWindow::register_class() const {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &OperationProgressWindow::window_proc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = kWindowClass;
    return RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int OperationProgressWindow::scale(int value) const {
    return MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
}

void OperationProgressWindow::rebuild_font() {
    if (font_ != nullptr && font_ != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(font_);
    }
    font_ = nullptr;
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
        font_ = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (font_ == nullptr) font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

void OperationProgressWindow::create_controls() {
    constexpr DWORD telemetry_style =
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS;
    for (HWND& control : telemetry_fields_) {
        control = CreateWindowExW(0, L"STATIC", L"", telemetry_style,
                                  0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    pause_button_ = CreateWindowExW(0, L"BUTTON", L"Pause", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    0, 0, 0, 0, hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPauseButton)), instance_, nullptr);
    cancel_button_ = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 0, 0, hwnd_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButton)), instance_, nullptr);
    details_button_ = CreateWindowExW(0, L"BUTTON", L"Details",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 0, 0, hwnd_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailsButton)),
                                      instance_, nullptr);
    SendMessageW(pause_button_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(cancel_button_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(details_button_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    if (!pause_available_) ShowWindow(pause_button_, SW_HIDE);
    update_telemetry_fields();
}

void OperationProgressWindow::apply_theme() {
    apply_dialog_dark_frame(hwnd_, theme_.dark);
    if (background_brush_ != nullptr) DeleteObject(background_brush_);
    background_brush_ = CreateSolidBrush(theme_.background);
    apply_dialog_control_theme(pause_button_, theme_.dark);
    apply_dialog_control_theme(cancel_button_, theme_.dark);
    apply_dialog_control_theme(details_button_, theme_.dark);
    InvalidateRect(hwnd_, nullptr, TRUE);
    for (HWND control : telemetry_fields_) {
        if (control != nullptr) InvalidateRect(control, nullptr, TRUE);
    }
    InvalidateRect(pause_button_, nullptr, TRUE);
    InvalidateRect(cancel_button_, nullptr, TRUE);
    InvalidateRect(details_button_, nullptr, TRUE);
}

void OperationProgressWindow::set_theme(const OperationWindowTheme& theme) {
    theme_ = theme;
    if (hwnd_ != nullptr) apply_theme();
}

int OperationProgressWindow::activity_row() const {
    // The trailing gap keeps the status line from sitting flush against the
    // last content row when no optional rows are reserved.
    return kOptionalRowTop + reserved_optional_rows_ * kRowHeight + 12;
}

int OperationProgressWindow::collapsed_height() const {
    return activity_row() + 18 + 14 + 32 + 20;
}

int OperationProgressWindow::expanded_height() const {
    return collapsed_height() + kDetailRowHeight * 2 + 6;
}

void OperationProgressWindow::apply_window_size() {
    if (hwnd_ == nullptr) return;
    RECT window_rect{0, 0, scale(640),
                     scale(details_expanded_ ? expanded_height()
                                             : collapsed_height())};
    AdjustWindowRectExForDpi(&window_rect,
                             static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)),
                             FALSE, 0, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0,
                 window_rect.right - window_rect.left,
                 window_rect.bottom - window_rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    layout();
    // Moving a child STATIC leaves its old pixels on the parent until the
    // parent repaints that band, and the cached back buffer can otherwise blit
    // the pre-move frame straight back over it. Drop the buffer and force the
    // parent and every child to redraw now, rather than invalidating and
    // hoping the next paint covers the vacated rows.
    release_back_buffer();
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

// Reserved rows only ever grow within one operation. Releasing a row again
// when a counter briefly returns to zero would make the dialog change height
// underneath the reader, which is worse than a little unused space.
void OperationProgressWindow::reserve_optional_rows(int needed) {
    if (needed <= reserved_optional_rows_ || hwnd_ == nullptr) return;
    reserved_optional_rows_ = needed;
    PostMessageW(hwnd_, kResizeMessage, 0, 0);
}

void OperationProgressWindow::layout() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int margin = scale(20);
    const int gap = scale(8);
    const int button_width = scale(92);
    const int details_width = scale(120);
    const int button_height = scale(32);
    const int bottom = client.bottom - margin;
    const int content_width = client.right - margin * 2;
    const auto place = [&](TelemetryField field_id, int y, int height = 20) {
        MoveWindow(field(field_id), margin, scale(y), content_width,
                   scale(height), TRUE);
    };

    place(TelemetryField::stage, 14, 24);
    place(TelemetryField::output_path, 38, 18);
    place(TelemetryField::overall_summary, kOverallBarBottom + 4);
    place(TelemetryField::overall_rate, kOverallBarBottom + 26);
    place(TelemetryField::current_path, 140, 18);
    place(TelemetryField::file_summary, kFileBarBottom + 4);
    place(TelemetryField::result_summary, kOptionalRowTop);
    place(TelemetryField::plan_summary, kOptionalRowTop + kRowHeight);

    const int activity_y = activity_row();
    place(TelemetryField::activity, activity_y, 18);
    place(TelemetryField::detail_timing, activity_y + 26, 18);
    place(TelemetryField::detail_archive, activity_y + 26 + kDetailRowHeight, 18);

    ShowWindow(field(TelemetryField::result_summary),
               reserved_optional_rows_ >= 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(field(TelemetryField::plan_summary),
               reserved_optional_rows_ >= 2 ? SW_SHOW : SW_HIDE);
    const int show = details_expanded_ ? SW_SHOW : SW_HIDE;
    ShowWindow(field(TelemetryField::detail_timing), show);
    ShowWindow(field(TelemetryField::detail_archive), show);

    MoveWindow(cancel_button_, client.right - margin - button_width,
               bottom - button_height, button_width, button_height, TRUE);
    if (pause_available_) {
        MoveWindow(pause_button_, client.right - margin - button_width * 2 - gap,
                   bottom - button_height, button_width, button_height, TRUE);
    }
    MoveWindow(details_button_, margin, bottom - button_height, details_width,
               button_height, TRUE);
}

void OperationProgressWindow::toggle_details() {
    details_expanded_ = !details_expanded_;
    SetWindowTextW(details_button_,
                   details_expanded_ ? L"Hide details" : L"Details");
    apply_window_size();
}

HWND OperationProgressWindow::field(TelemetryField field_id) const {
    return telemetry_fields_[static_cast<std::size_t>(field_id)];
}

void OperationProgressWindow::set_field_text(TelemetryField field_id,
                                             std::wstring text) {
    auto& previous = telemetry_text_[static_cast<std::size_t>(field_id)];
    if (previous == text) return;
    previous = std::move(text);
    const HWND control = field(field_id);
    if (control != nullptr) SetWindowTextW(control, previous.c_str());
}

bool OperationProgressWindow::muted_field(HWND control) const {
    return control == field(TelemetryField::current_path) ||
           control == field(TelemetryField::output_path) ||
           control == field(TelemetryField::activity) ||
           control == field(TelemetryField::detail_timing) ||
           control == field(TelemetryField::detail_archive);
}

std::pair<std::uint64_t, std::uint64_t>
OperationProgressWindow::displayed_file_progress() const {
    if (!has_progress_) return {0, 0};
    if (progress_.current_file_total_bytes > 0) {
        return {progress_.current_file_completed_bytes,
                progress_.current_file_total_bytes};
    }
    // External providers may expose only operation-wide counters. For a
    // one-item operation those counters are exactly the current file and are a
    // safe fallback rather than leaving the file bar pinned at zero.
    if (progress_.total_items == 1 && progress_.total_bytes > 0 &&
        !progress_.current_path.empty()) {
        return {progress_.completed_bytes, progress_.total_bytes};
    }
    return {progress_.current_file_completed_bytes, 0};
}

void OperationProgressWindow::update_telemetry_fields() {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_seconds = static_cast<std::uint64_t>(
        std::chrono::duration<double>(now - started_).count());
    const auto checkpoint_seconds = static_cast<std::uint64_t>(
        std::chrono::duration<double>(now - last_progress_time_).count());
    constexpr const wchar_t* kSeparator = L"  \x2022  ";

    // A multi-phase operation reports two different scopes at once: the bar and
    // its percentage span the whole operation, while the byte and item counters
    // describe only the current phase. Say which is which rather than letting
    // one row imply both are the same measurement.
    const bool phased = has_progress_ && progress_.phase_count != 0;

    std::wstring stage = has_progress_ ? stage_text(progress_.stage)
                                       : std::wstring{L"Preparing"};
    if (phased) {
        stage = L"Stage " + std::to_wstring(progress_.phase_index + 1) +
                L" of " + std::to_wstring(progress_.phase_count) + L": " + stage;
    }
    set_field_text(TelemetryField::stage, stage);

    set_field_text(TelemetryField::output_path,
                   output_path_.empty() ? std::wstring{}
                                        : output_path_.wstring());

    const bool byte_total = has_progress_ && progress_.total_bytes > 0;
    const bool item_total = has_progress_ && progress_.total_items > 0;
    const auto [overall_completed, overall_total] =
        has_progress_ ? overall_progress(progress_)
                      : std::pair<std::uint64_t, std::uint64_t>{0, 0};

    std::wstring summary = format_percent(overall_completed, overall_total);
    if (phased) summary += L" overall";
    if (byte_total) {
        summary += kSeparator +
                   format_bytes_of(progress_.completed_bytes,
                                   progress_.total_bytes);
        if (phased) summary += L" in this stage";
    } else if (has_progress_ && progress_.completed_bytes > 0) {
        summary += kSeparator + format_size(progress_.completed_bytes);
    }
    if (item_total) {
        summary += kSeparator + format_count(progress_.completed_items) +
                   L" of " + format_count(progress_.total_items) + L" items";
    } else if (has_progress_ && progress_.completed_items > 0) {
        summary += kSeparator + format_count(progress_.completed_items) +
                   L" items";
    }
    set_field_text(TelemetryField::overall_summary, summary);

    // Time remaining is derived from the current phase's byte counters, so it
    // is a stage estimate whenever phases are in play. Labelling it plainly
    // "ETA" during a five-phase sync overstates what it knows.
    const double rate = rate_.rate();
    std::wstring rate_line =
        format_size(static_cast<std::uint64_t>(std::max(0.0, rate))) + L"/s";
    if (byte_total && progress_.completed_bytes >= progress_.total_bytes) {
        rate_line += std::wstring{kSeparator} +
                     (phased ? L"stage complete" : L"finishing");
    } else if (byte_total && rate > 0.0) {
        const double seconds =
            static_cast<double>(progress_.total_bytes - progress_.completed_bytes) /
            rate;
        // Integer-truncating this rendered "0s left" for most of the last
        // second, which reads as a stall rather than as nearly done.
        if (seconds < 1.0) {
            rate_line += std::wstring{kSeparator} + L"finishing";
        } else {
            rate_line += kSeparator +
                         format_duration(static_cast<std::uint64_t>(seconds + 0.5)) +
                         (phased ? L" left in this stage" : L" left");
        }
    } else if (has_progress_) {
        rate_line += std::wstring{kSeparator} + L"estimating time remaining";
    }
    set_field_text(TelemetryField::overall_rate, rate_line);

    set_field_text(TelemetryField::current_path,
                   has_progress_ && !progress_.current_path.empty()
                       ? widen(progress_.current_path)
                       : std::wstring{L"Waiting for the first item"});

    const auto [file_completed, file_total_bytes] = displayed_file_progress();
    set_field_text(TelemetryField::file_summary,
                   file_total_bytes > 0
                       ? format_bytes_of(file_completed, file_total_bytes)
                       : (file_completed > 0 ? format_size(file_completed)
                                             : std::wstring{}));

    std::wstring result;
    if (has_progress_ && progress_.compressed_bytes != 0) {
        std::wstringstream ratio;
        ratio.setf(std::ios::fixed);
        ratio.precision(2);
        ratio << static_cast<double>(progress_.compressed_source_bytes) /
                     static_cast<double>(progress_.compressed_bytes);
        result = L"Compressed " + format_size(progress_.compressed_bytes) +
                 L" (" + ratio.str() + L"x)";
    }
    if (has_progress_ && progress_.reused_items != 0) {
        if (!result.empty()) result += kSeparator;
        result += L"reused " + format_size(progress_.reused_bytes) + L" in " +
                  format_count(progress_.reused_items) + L" items";
    }
    set_field_text(TelemetryField::result_summary, result);

    // The plan counters get their own row. They used to be written into the
    // compressed-size and ratio fields, which meant two controls changed
    // meaning depending on the operation.
    std::wstring plan;
    if (has_progress_ &&
        (progress_.planned_added_items != 0 ||
         progress_.planned_updated_items != 0 ||
         progress_.planned_removed_items != 0 ||
         progress_.planned_unchanged_items != 0)) {
        plan = L"Planned: " + format_count(progress_.planned_added_items) +
               L" added" + kSeparator +
               format_count(progress_.planned_updated_items) + L" updated" +
               kSeparator + format_count(progress_.planned_removed_items) +
               L" removed" + kSeparator +
               format_count(progress_.planned_unchanged_items) + L" unchanged";
    }
    set_field_text(TelemetryField::plan_summary, plan);
    reserve_optional_rows((result.empty() ? 0 : 1) + (plan.empty() ? 0 : 1));

    std::wstring activity = L"Preparing";
    if (cancelling_) {
        activity = L"Cancelling";
    } else if (paused_) {
        activity = L"Paused";
    } else if (has_progress_ && checkpoint_seconds >= 2) {
        activity = L"Waiting for the next checkpoint";
    } else if (has_progress_) {
        activity = L"Active";
    }
    set_field_text(TelemetryField::activity, activity);

    set_field_text(TelemetryField::detail_timing,
                   L"Elapsed " + format_duration(elapsed_seconds) + kSeparator +
                       L"last update " + format_duration(checkpoint_seconds) +
                       L" ago");
    set_field_text(
        TelemetryField::detail_archive,
        has_progress_ && progress_.archive_bytes_read > 0
            ? L"Archive bytes read " + format_size(progress_.archive_bytes_read)
            : std::wstring{L"Archive bytes read not reported by this backend"});
}

void OperationProgressWindow::set_progress(const OperationProgress& progress) {
    const auto now = std::chrono::steady_clock::now();
    progress_ = progress;
    has_progress_ = true;
    progress_dirty_ = true;
    telemetry_dirty_ = true;
    last_progress_time_ = now;
    last_progress_sequence_ = progress.sequence;
    rate_.update(progress, now);
    if (displayed_file_progress().second > 0) file_bar_active_ = true;
}

void OperationProgressWindow::set_progress_source(
    std::shared_ptr<OperationControl> source) {
    progress_source_ = std::move(source);
    if (progress_source_) {
        if (auto snapshot = progress_source_->latest_progress()) set_progress(*snapshot);
    }
}

void OperationProgressWindow::invalidate_progress_area() {
    if (hwnd_ == nullptr) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    RECT progress_area{0, 0, client.right,
                       std::min(client.bottom,
                                static_cast<LONG>(scale(kFileBarBottom + 4)))};
    InvalidateRect(hwnd_, &progress_area, FALSE);
}

void OperationProgressWindow::release_back_buffer() {
    if (back_buffer_dc_ != nullptr && back_buffer_old_bitmap_ != nullptr) {
        SelectObject(back_buffer_dc_, back_buffer_old_bitmap_);
    }
    if (back_buffer_bitmap_ != nullptr) DeleteObject(back_buffer_bitmap_);
    if (back_buffer_dc_ != nullptr) DeleteDC(back_buffer_dc_);
    back_buffer_dc_ = nullptr;
    back_buffer_bitmap_ = nullptr;
    back_buffer_old_bitmap_ = nullptr;
    back_buffer_size_ = {};
}

bool OperationProgressWindow::ensure_back_buffer(HDC reference, int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (back_buffer_dc_ != nullptr && back_buffer_bitmap_ != nullptr &&
        back_buffer_size_.cx == width && back_buffer_size_.cy == height) {
        return true;
    }

    release_back_buffer();
    back_buffer_dc_ = CreateCompatibleDC(reference);
    if (back_buffer_dc_ == nullptr) return false;
    back_buffer_bitmap_ = CreateCompatibleBitmap(reference, width, height);
    if (back_buffer_bitmap_ == nullptr) {
        release_back_buffer();
        return false;
    }
    back_buffer_old_bitmap_ = SelectObject(back_buffer_dc_, back_buffer_bitmap_);
    if (back_buffer_old_bitmap_ == nullptr || back_buffer_old_bitmap_ == HGDI_ERROR) {
        back_buffer_old_bitmap_ = nullptr;
        release_back_buffer();
        return false;
    }
    back_buffer_size_ = SIZE{width, height};
    return true;
}

void OperationProgressWindow::set_cancelling() {
    if (cancelling_) return;
    cancelling_ = true;
    paused_ = false;
    SetWindowTextW(pause_button_, L"Pause");
    EnableWindow(pause_button_, FALSE);
    EnableWindow(cancel_button_, FALSE);
    // Details stays usable while cancelling; it only reveals text.
    telemetry_dirty_ = true;
    update_telemetry_fields();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void OperationProgressWindow::toggle_pause() {
    if (!pause_available_) return;
    if (cancelling_) return;
    paused_ = !paused_;
    SetWindowTextW(pause_button_, paused_ ? L"Resume" : L"Pause");
    InvalidateRect(pause_button_, nullptr, TRUE);
    if (pause_handler_) pause_handler_(paused_);
    telemetry_dirty_ = true;
    update_telemetry_fields();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void OperationProgressWindow::request_cancel() {
    if (cancelling_) return;
    set_cancelling();
    if (cancel_handler_) cancel_handler_();
}

void OperationProgressWindow::draw_button(const DRAWITEMSTRUCT& draw) const {
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    RECT rect = draw.rcItem;
    fill_rect(draw.hDC, rect, pressed ? theme_.button_pressed : hot ? theme_.button_hot : theme_.button);
    frame_rect(draw.hDC, rect, (focused || hot || pressed) ? theme_.button_hot : theme_.border);
    if (pressed) OffsetRect(&rect, scale(1), scale(1));

    const std::wstring text = [&] {
        const int length = GetWindowTextLengthW(draw.hwndItem);
        std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
        if (length > 0) GetWindowTextW(draw.hwndItem, value.data(), length + 1);
        value.resize(static_cast<std::size_t>(length));
        return value;
    }();
    HGDIOBJ old_font = SelectObject(draw.hDC, font_);
    SetBkMode(draw.hDC, TRANSPARENT);
    const COLORREF content_color = disabled ? theme_.muted_text : theme_.text;
    SetTextColor(draw.hDC, content_color);
    SIZE text_size{};
    GetTextExtentPoint32W(draw.hDC, text.c_str(), static_cast<int>(text.size()), &text_size);
    // Details is a plain disclosure toggle, so it carries no command icon.
    const bool with_icon = draw.CtlID != kDetailsButton;
    const int icon_size = with_icon ? scale(18) : 0;
    const int gap = with_icon ? scale(5) : 0;
    const int content_width = icon_size + gap + static_cast<int>(text_size.cx);
    const int left = rect.left + (rect.right - rect.left - content_width) / 2;
    RECT icon_rect{left, rect.top, left + icon_size, rect.bottom};
    if (with_icon) {
        const ToolbarIcon icon = draw.CtlID == kCancelButton ? ToolbarIcon::cancel
            : paused_ ? ToolbarIcon::resume : ToolbarIcon::pause;
        draw_toolbar_icon(draw.hDC, icon, icon_rect, content_color, dpi_);
    }
    RECT text_rect{icon_rect.right + gap, rect.top,
                   icon_rect.right + gap + static_cast<int>(text_size.cx), rect.bottom};
    DrawTextW(draw.hDC, text.c_str(), -1, &text_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(draw.hDC, old_font);
}

void OperationProgressWindow::paint() {
    PAINTSTRUCT paint_info{};
    HDC paint_dc = BeginPaint(hwnd_, &paint_info);
    RECT client{};
    GetClientRect(hwnd_, &client);

    const bool buffered = ensure_back_buffer(
        paint_dc, static_cast<int>(client.right), static_cast<int>(client.bottom));
    HDC dc = buffered ? back_buffer_dc_ : paint_dc;

    fill_rect(dc, client, theme_.background);
    const int margin = scale(20);

    const auto draw_progress_bar = [&](RECT track, std::uint64_t completed,
                                       std::uint64_t total,
                                       bool pulse_when_unknown = true) {
        fill_rect(dc, track, theme_.progress_track);
        frame_rect(dc, track, theme_.border);
        RECT inner = track;
        InflateRect(&inner, -scale(2), -scale(2));
        if (total == 0 && !pulse_when_unknown) return;
        if (total > 0) {
            const double fraction = std::clamp(
                static_cast<double>(completed) / total, 0.0, 1.0);
            RECT filled = inner;
            filled.right = filled.left +
                static_cast<int>((inner.right - inner.left) * fraction);
            fill_rect(dc, filled, theme_.progress_fill);
            return;
        }
        const int width = inner.right - inner.left;
        const int block_width = std::max(scale(40), width / 4);
        const int travel = width + block_width;
        const int offset = travel > 0 ? pulse_ % travel - block_width : 0;
        RECT block{inner.left + offset, inner.top, inner.left + offset + block_width, inner.bottom};
        RECT clipped{};
        if (IntersectRect(&clipped, &inner, &block)) fill_rect(dc, clipped, theme_.progress_fill);
    };

    const auto [stage_completed, stage_total] =
        has_progress_ ? overall_progress(progress_)
                      : std::pair<std::uint64_t, std::uint64_t>{0, 0};
    draw_progress_bar({margin, scale(kOverallBarTop), client.right - margin,
                       scale(kOverallBarBottom)},
                      stage_completed, stage_total);
    // Plenty of backends report no per-item size at all, and pulsing an
    // indeterminate second bar under a blank label is just motion that means
    // nothing. But whether a *given* snapshot carries a per-file size is not
    // stable: an empty file, or the first report of an operation, momentarily
    // has none. Deciding per frame made the bar blink out mid-transfer, so the
    // decision is made once per operation and then held.
    const auto [file_completed, file_total] = displayed_file_progress();
    if (file_bar_active_) {
        draw_progress_bar({margin, scale(kFileBarTop), client.right - margin,
                           scale(kFileBarBottom)},
                          file_completed, file_total,
                          /*pulse_when_unknown=*/false);
    }
    if (buffered) {
        const RECT& dirty = paint_info.rcPaint;
        BitBlt(paint_dc, dirty.left, dirty.top,
               dirty.right - dirty.left, dirty.bottom - dirty.top,
               back_buffer_dc_, dirty.left, dirty.top, SRCCOPY);
    }
    EndPaint(hwnd_, &paint_info);
}

void OperationProgressWindow::close() {
    if (hwnd_ != nullptr) DestroyWindow(hwnd_);
    progress_source_.reset();
}

LRESULT OperationProgressWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            rebuild_font();
            create_controls();
            apply_theme();
            layout();
            SetTimer(hwnd_, kAnimationTimer, 33, nullptr);
            return 0;
        case WM_SIZE: layout(); return 0;
        case kResizeMessage: apply_window_size(); return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            apply_axiom_window_icons(hwnd_, instance_);
            rebuild_font();
            for (HWND control : telemetry_fields_) {
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            }
            SendMessageW(pause_button_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            SendMessageW(cancel_button_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            SendMessageW(details_button_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            layout();
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_TIMER: {
            if (progress_source_) {
                if (auto snapshot = progress_source_->latest_progress();
                    snapshot && snapshot->sequence != last_progress_sequence_) {
                    set_progress(*snapshot);
                }
            }
            pulse_ += scale(5);
            if (!has_progress_ || progress_.total_bytes == 0) {
                progress_dirty_ = true;
            }
            // A heartbeat repaint makes liveness explicit even during backend
            // calls that cannot expose byte-level checkpoints.
            const auto now = std::chrono::steady_clock::now();
            if (now - last_heartbeat_paint_ >= std::chrono::milliseconds(250)) {
                last_heartbeat_paint_ = now;
                progress_dirty_ = true;
                telemetry_dirty_ = true;
            }
            if (telemetry_dirty_) {
                telemetry_dirty_ = false;
                update_telemetry_fields();
            }
            if (progress_dirty_) {
                progress_dirty_ = false;
                invalidate_progress_area();
            }
            return 0;
        }
        case WM_DRAWITEM:
            draw_button(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        case WM_CTLCOLORSTATIC: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            const HWND control = reinterpret_cast<HWND>(lparam);
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, theme_.background);
            SetTextColor(dc, muted_field(control) ? theme_.muted_text : theme_.text);
            return reinterpret_cast<LRESULT>(
                background_brush_ != nullptr ? background_brush_
                                             : GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == kPauseButton) { toggle_pause(); return 0; }
            if (LOWORD(wparam) == kCancelButton) { request_cancel(); return 0; }
            if (LOWORD(wparam) == kDetailsButton) { toggle_details(); return 0; }
            break;
        case WM_SETTINGCHANGE:
            if (!follow_system_theme_) return 0;
            handle_dialog_theme_setting_change(lparam);
            refresh_operation_theme(theme_);
            apply_theme();
            return 0;
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            if (!follow_system_theme_) return 0;
            refresh_operation_theme(theme_);
            apply_theme();
            return 0;
        case WM_CLOSE: request_cancel(); return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kAnimationTimer);
            release_back_buffer();
            return 0;
        case WM_NCDESTROY:
            hwnd_ = nullptr;
            pause_button_ = nullptr;
            cancel_button_ = nullptr;
            telemetry_fields_.fill(nullptr);
            return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

LRESULT CALLBACK OperationProgressWindow::window_proc(HWND hwnd, UINT message,
                                                       WPARAM wparam, LPARAM lparam) {
    OperationProgressWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        window = static_cast<OperationProgressWindow*>(
            reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        window->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    } else {
        window = reinterpret_cast<OperationProgressWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return window != nullptr ? window->handle_message(message, wparam, lparam)
                             : DefWindowProcW(hwnd, message, wparam, lparam);
}

ThreadedOperationProgressWindow::~ThreadedOperationProgressWindow() { stop(); }

bool ThreadedOperationProgressWindow::start(HWND anchor,
                                            HINSTANCE instance,
                                            std::wstring title,
                                            std::filesystem::path output_path,
                                            const OperationWindowTheme& theme,
                                            PauseHandler pause_handler,
                                            CancelHandler cancel_handler,
                                            bool pause_available,
                                            std::shared_ptr<OperationControl> source) {
    stop();

    // Resolve everything that must be read from the calling thread's window
    // before handing off. The progress thread never touches `anchor`.
    OperationProgressWindow::Placement placement;
    placement.topmost = true;
    placement.follow_system_theme = false;
    if (anchor != nullptr) {
        placement.dpi = GetDpiForWindow(anchor);
        if (!GetWindowRect(anchor, &placement.anchor)) placement.anchor = {};
    }

    std::promise<bool> ready;
    std::future<bool> ready_future = ready.get_future();

    thread_ = std::thread([this, instance, title = std::move(title),
                           output_path = std::move(output_path), theme,
                           pause_handler = std::move(pause_handler),
                           cancel_handler = std::move(cancel_handler),
                           pause_available, source = std::move(source),
                           placement, ready = std::move(ready)]() mutable {
        thread_id_ = GetCurrentThreadId();
        // Force a message queue to exist before start() returns, so a stop()
        // that arrives immediately cannot post to a queue that is not there.
        MSG bootstrap{};
        PeekMessageW(&bootstrap, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        OperationProgressWindow window;
        const bool created = window.create(nullptr, instance, std::move(title),
                                           std::move(output_path), theme,
                                           std::move(pause_handler),
                                           std::move(cancel_handler),
                                           pause_available, &placement);
        if (created) window.set_progress_source(std::move(source));
        ready.set_value(created);
        if (!created) return;

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        // `window` is destroyed here, on the thread that created it, which is
        // the only thread allowed to call DestroyWindow on it.
    });

    bool created = false;
    try {
        created = ready_future.get();
    } catch (...) {
        created = false;
    }
    if (!created) stop();
    return created;
}

void ThreadedOperationProgressWindow::stop() {
    if (!thread_.joinable()) return;
    if (thread_id_ != 0) {
        // WM_QUIT ends the loop; the window is then torn down on its own
        // thread as the local OperationProgressWindow goes out of scope.
        PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    }
    thread_.join();
    thread_id_ = 0;
}

} // namespace axiom::gui
