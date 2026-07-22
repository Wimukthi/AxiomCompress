#define NOMINMAX
#include "gui/operation_progress_window.hpp"

#include "gui/dialog_support.hpp"
#include "gui/toolbar_icons.hpp"

#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <utility>

namespace axiom::gui {

namespace {

constexpr wchar_t kWindowClass[] = L"AxiomOperationProgressWindow";
constexpr int kPauseButton = 1;
constexpr int kCancelButton = 2;
constexpr UINT_PTR kAnimationTimer = 1;

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
        case OperationStage::reading: return L"Reading";
        case OperationStage::compressing: return L"Compressing";
        case OperationStage::writing: return L"Writing";
        case OperationStage::testing: return L"Testing";
        case OperationStage::extracting: return L"Extracting";
        case OperationStage::transferring: return L"Transferring";
        case OperationStage::finalizing: return L"Finalizing";
    }
    return L"Working";
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

void apply_dark_title_bar(HWND hwnd, bool dark) {
    constexpr DWORD kImmersiveDarkMode = 20;
    BOOL enabled = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, kImmersiveDarkMode, &enabled, sizeof(enabled)))) {
        constexpr DWORD kOlderImmersiveDarkMode = 19;
        DwmSetWindowAttribute(hwnd, kOlderImmersiveDarkMode, &enabled, sizeof(enabled));
    }
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
                                     bool pause_available) {
    close();
    owner_ = owner;
    instance_ = instance;
    title_ = std::move(title);
    output_path_ = std::move(output_path);
    theme_ = theme;
    pause_handler_ = std::move(pause_handler);
    cancel_handler_ = std::move(cancel_handler);
    pause_available_ = pause_available;
    started_ = std::chrono::steady_clock::now();
    last_progress_time_ = started_;
    last_heartbeat_paint_ = started_;
    progress_source_.reset();
    rate_samples_.clear();
    last_progress_sequence_ = 0;
    current_rate_ = 0.0;
    paused_ = false;
    cancelling_ = false;
    has_progress_ = false;
    progress_dirty_ = false;
    telemetry_dirty_ = true;
    for (auto& text : telemetry_text_) text.clear();
    pulse_ = 0;
    dpi_ = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    if (!register_class()) return false;

    constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                   WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT window_rect{0, 0, scale(660), scale(360)};
    AdjustWindowRectExForDpi(&window_rect, kWindowStyle, FALSE, 0, dpi_);
    const int width = window_rect.right - window_rect.left;
    const int height = window_rect.bottom - window_rect.top;
    RECT anchor_rect{};
    if (owner == nullptr || !GetWindowRect(owner, &anchor_rect)) {
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
    SendMessageW(pause_button_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(cancel_button_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    if (!pause_available_) ShowWindow(pause_button_, SW_HIDE);
    update_telemetry_fields();
}

void OperationProgressWindow::apply_theme() {
    apply_dark_title_bar(hwnd_, theme_.dark);
    if (background_brush_ != nullptr) DeleteObject(background_brush_);
    background_brush_ = CreateSolidBrush(theme_.background);
    SetWindowTheme(pause_button_, theme_.dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    SetWindowTheme(cancel_button_, theme_.dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    InvalidateRect(hwnd_, nullptr, TRUE);
    for (HWND control : telemetry_fields_) {
        if (control != nullptr) InvalidateRect(control, nullptr, TRUE);
    }
    InvalidateRect(pause_button_, nullptr, TRUE);
    InvalidateRect(cancel_button_, nullptr, TRUE);
}

void OperationProgressWindow::set_theme(const OperationWindowTheme& theme) {
    theme_ = theme;
    if (hwnd_ != nullptr) apply_theme();
}

void OperationProgressWindow::layout() {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int margin = scale(20);
    const int gap = scale(8);
    const int button_width = scale(92);
    const int button_height = scale(32);
    const int bottom = client.bottom - margin;
    const int column_gap = scale(12);
    const int content_width = client.right - margin * 2;
    const int column_width = std::max(1, (content_width - column_gap * 2) / 3);
    const int x0 = margin;
    const int x1 = x0 + column_width + column_gap;
    const int x2 = x1 + column_width + column_gap;
    const auto place = [&](TelemetryField field_id, int x, int y, int width,
                           int height = 22) {
        MoveWindow(field(field_id), x, scale(y), width, scale(height), TRUE);
    };

    place(TelemetryField::stage, margin, 14, content_width, 26);
    place(TelemetryField::current_path, margin, 40, content_width);
    place(TelemetryField::output_path, margin, 62, content_width);

    place(TelemetryField::overall_percent, x0, 117, column_width);
    place(TelemetryField::overall_completed, x1, 117, column_width);
    place(TelemetryField::overall_total, x2, 117, column_width);
    place(TelemetryField::items_completed, x0, 140, column_width);
    place(TelemetryField::items_total, x1, 140, column_width);
    place(TelemetryField::speed, x2, 140, column_width);

    place(TelemetryField::file_percent, x0, 201, column_width);
    place(TelemetryField::file_completed, x1, 201, column_width);
    place(TelemetryField::file_total, x2, 201, column_width);
    place(TelemetryField::eta, x0, 224, column_width);
    place(TelemetryField::elapsed, x1, 224, column_width);
    place(TelemetryField::checkpoint_age, x2, 224, column_width);
    place(TelemetryField::compressed_size, x0, 250, column_width);
    place(TelemetryField::compression_ratio, x1, 250, column_width);
    place(TelemetryField::activity, x2, 250, column_width);

    MoveWindow(cancel_button_, client.right - margin - button_width,
               bottom - button_height, button_width, button_height, TRUE);
    if (pause_available_) {
        MoveWindow(pause_button_, client.right - margin - button_width * 2 - gap,
                   bottom - button_height, button_width, button_height, TRUE);
    }
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
           control == field(TelemetryField::checkpoint_age) ||
           control == field(TelemetryField::activity);
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
    const auto percentage = [](std::uint64_t completed, std::uint64_t total) {
        std::wstringstream stream;
        stream.setf(std::ios::fixed);
        stream.precision(1);
        stream << (total == 0 ? 0.0
                              : static_cast<double>(completed) * 100.0 / total)
               << L'%';
        return stream.str();
    };

    set_field_text(TelemetryField::stage,
                   L"Stage: " + (has_progress_ ? stage_text(progress_.stage)
                                                 : std::wstring{L"Preparing"}));
    set_field_text(TelemetryField::current_path,
                   L"Current item: " +
                       (has_progress_ && !progress_.current_path.empty()
                            ? widen(progress_.current_path)
                            : std::wstring{L"Waiting for backend"}));
    set_field_text(TelemetryField::output_path,
                   L"Output: " + (output_path_.empty()
                                       ? std::wstring{L"Not applicable"}
                                       : output_path_.wstring()));

    const bool byte_total = has_progress_ && progress_.total_bytes > 0;
    const bool item_total = has_progress_ && progress_.total_items > 0;
    set_field_text(TelemetryField::overall_percent,
                   L"Overall progress: " +
                       (byte_total ? percentage(progress_.completed_bytes,
                                                progress_.total_bytes)
                                   : item_total
                                         ? percentage(progress_.completed_items,
                                                      progress_.total_items)
                                         : percentage(0, 0)));
    set_field_text(TelemetryField::overall_completed,
                   L"Completed bytes: " + format_size(
                       has_progress_ ? progress_.completed_bytes : 0));
    set_field_text(TelemetryField::overall_total,
                   L"Total bytes: " + format_size(
                       byte_total ? progress_.total_bytes : 0));
    set_field_text(TelemetryField::items_completed,
                   L"Completed items: " + std::to_wstring(
                       has_progress_ ? progress_.completed_items : 0));
    set_field_text(TelemetryField::items_total,
                   L"Total items: " + std::to_wstring(
                       item_total ? progress_.total_items : 0));
    set_field_text(TelemetryField::speed,
                   L"Speed: " + format_size(static_cast<std::uint64_t>(
                       std::max(0.0, current_rate_))) + L"/s");
    set_field_text(TelemetryField::compressed_size,
                   L"Compressed size: " + format_size(
                       has_progress_ ? progress_.compressed_bytes : 0));
    std::wstringstream ratio;
    ratio.setf(std::ios::fixed);
    ratio.precision(2);
    ratio << (has_progress_ && progress_.compressed_bytes != 0
                  ? static_cast<double>(progress_.compressed_source_bytes) /
                        static_cast<double>(progress_.compressed_bytes)
                  : 0.0)
          << L'x';
    set_field_text(TelemetryField::compression_ratio,
                   L"Compression ratio: " + ratio.str());

    const auto [file_completed, file_total_bytes] = displayed_file_progress();
    const bool file_total = file_total_bytes > 0;
    set_field_text(TelemetryField::file_percent,
                   L"File progress: " +
                       (file_total ? percentage(file_completed, file_total_bytes)
                                   : percentage(0, 0)));
    set_field_text(TelemetryField::file_completed,
                   L"File completed: " + format_size(file_completed));
    set_field_text(TelemetryField::file_total,
                   L"File total: " + format_size(file_total_bytes));

    std::wstring eta = L"Calculating";
    if (byte_total && progress_.total_bytes > progress_.completed_bytes &&
        current_rate_ > 0.0) {
        const auto speed = static_cast<std::uint64_t>(current_rate_);
        if (speed != 0) {
            eta = format_duration((progress_.total_bytes - progress_.completed_bytes) /
                                  speed);
        }
    } else if (byte_total && progress_.completed_bytes >= progress_.total_bytes) {
        eta = L"0s";
    }
    set_field_text(TelemetryField::eta, L"ETA: " + eta);
    set_field_text(TelemetryField::elapsed,
                   L"Elapsed: " + format_duration(elapsed_seconds));
    set_field_text(TelemetryField::checkpoint_age,
                   L"Checkpoint age: " + format_duration(checkpoint_seconds));

    std::wstring activity = L"Preparing";
    if (cancelling_) {
        activity = L"Cancelling";
    } else if (paused_) {
        activity = L"Paused";
    } else if (has_progress_ && checkpoint_seconds >= 2) {
        activity = L"Waiting for checkpoint";
    } else if (has_progress_) {
        activity = L"Active";
    }
    set_field_text(TelemetryField::activity, L"Activity: " + activity);
}

void OperationProgressWindow::set_progress(const OperationProgress& progress) {
    const auto now = std::chrono::steady_clock::now();
    const auto rate_counter = [](const OperationProgress& value) {
        return value.throughput_bytes != 0
            ? value.throughput_bytes : value.completed_bytes;
    };
    const auto sample = rate_counter(progress);
    const auto previous_sample = rate_counter(progress_);
    // Archive pipelines legitimately alternate between compressing and writing
    // while the same byte counter advances. Resetting on every stage flip made
    // the sampling window collapse to one point and displayed 0 B/s despite
    // visible progress. Only a counter restart or a new total begins a new rate
    // epoch.
    const bool new_rate_epoch = !has_progress_ ||
        sample < previous_sample ||
        progress.total_bytes != progress_.total_bytes;
    progress_ = progress;
    has_progress_ = true;
    progress_dirty_ = true;
    telemetry_dirty_ = true;
    last_progress_time_ = now;
    last_progress_sequence_ = progress.sequence;
    if (new_rate_epoch) {
        rate_samples_.clear();
        current_rate_ = 0.0;
    }
    if (rate_samples_.empty() ||
        rate_samples_.back().second != sample) {
        rate_samples_.emplace_back(now, sample);
    }
    while (rate_samples_.size() > 2 &&
           now - rate_samples_.front().first > std::chrono::seconds(4)) {
        rate_samples_.pop_front();
    }
    if (rate_samples_.size() >= 2) {
        const double seconds = std::chrono::duration<double>(
            rate_samples_.back().first - rate_samples_.front().first).count();
        const auto first = rate_samples_.front().second;
        const auto last = rate_samples_.back().second;
        if (seconds > 0.1 && last >= first) {
            current_rate_ = static_cast<double>(last - first) / seconds;
        }
    }
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
                       std::min(client.bottom, static_cast<LONG>(scale(210)))};
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
    const int icon_size = scale(18);
    const int gap = scale(5);
    const int content_width = icon_size + gap + static_cast<int>(text_size.cx);
    const int left = rect.left + (rect.right - rect.left - content_width) / 2;
    RECT icon_rect{left, rect.top, left + icon_size, rect.bottom};
    const ToolbarIcon icon = draw.CtlID == kCancelButton ? ToolbarIcon::cancel
        : paused_ ? ToolbarIcon::resume : ToolbarIcon::pause;
    draw_toolbar_icon(draw.hDC, icon, icon_rect, content_color, dpi_);
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
                                       std::uint64_t total) {
        fill_rect(dc, track, theme_.progress_track);
        frame_rect(dc, track, theme_.border);
        RECT inner = track;
        InflateRect(&inner, -scale(2), -scale(2));
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

    const std::uint64_t stage_completed = has_progress_
        ? (progress_.total_bytes > 0 ? progress_.completed_bytes
                                     : progress_.completed_items) : 0;
    const std::uint64_t stage_total = has_progress_
        ? (progress_.total_bytes > 0 ? progress_.total_bytes
                                     : progress_.total_items) : 0;
    draw_progress_bar({margin, scale(89), client.right - margin, scale(111)},
                      stage_completed, stage_total);
    const auto [file_completed, file_total] = displayed_file_progress();
    draw_progress_bar({margin, scale(177), client.right - margin, scale(199)},
                      file_completed, file_total);
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
            break;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED: apply_theme(); return 0;
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

} // namespace axiom::gui
