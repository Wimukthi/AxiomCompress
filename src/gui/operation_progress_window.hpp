#pragma once

#include "axiom/axiom.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <deque>
#include <string>
#include <utility>

namespace axiom::gui {

struct OperationWindowTheme {
    bool dark{};
    COLORREF background{};
    COLORREF panel{};
    COLORREF text{};
    COLORREF muted_text{};
    COLORREF border{};
    COLORREF button{};
    COLORREF button_hot{};
    COLORREF button_pressed{};
    COLORREF progress_track{};
    COLORREF progress_fill{};
};

class OperationProgressWindow {
public:
    using PauseHandler = std::function<void(bool)>;
    using CancelHandler = std::function<void()>;

    OperationProgressWindow() = default;
    ~OperationProgressWindow();

    OperationProgressWindow(const OperationProgressWindow&) = delete;
    OperationProgressWindow& operator=(const OperationProgressWindow&) = delete;

    bool create(HWND owner,
                HINSTANCE instance,
                std::wstring title,
                std::filesystem::path output_path,
                const OperationWindowTheme& theme,
                PauseHandler pause_handler,
                CancelHandler cancel_handler,
                bool pause_available = true);

    void set_theme(const OperationWindowTheme& theme);
    void set_progress(const OperationProgress& progress);
    void set_progress_source(std::shared_ptr<OperationControl> source);
    void set_cancelling();
    void close();

    [[nodiscard]] HWND hwnd() const { return hwnd_; }

private:
    enum class TelemetryField : std::size_t {
        stage,
        current_path,
        output_path,
        overall_percent,
        overall_completed,
        overall_total,
        items_completed,
        items_total,
        speed,
        file_percent,
        file_completed,
        file_total,
        eta,
        elapsed,
        checkpoint_age,
        activity,
        count,
    };

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    bool register_class() const;
    void create_controls();
    void rebuild_font();
    void apply_theme();
    void layout();
    void update_telemetry_fields();
    void set_field_text(TelemetryField field, std::wstring text);
    [[nodiscard]] HWND field(TelemetryField field) const;
    [[nodiscard]] bool muted_field(HWND control) const;
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t>
        displayed_file_progress() const;
    void invalidate_progress_area();
    void paint();
    bool ensure_back_buffer(HDC reference, int width, int height);
    void release_back_buffer();
    void draw_button(const DRAWITEMSTRUCT& draw) const;
    void toggle_pause();
    void request_cancel();
    int scale(int value) const;

    HWND owner_{};
    HWND hwnd_{};
    HWND pause_button_{};
    HWND cancel_button_{};
    std::array<HWND, static_cast<std::size_t>(TelemetryField::count)> telemetry_fields_{};
    std::array<std::wstring, static_cast<std::size_t>(TelemetryField::count)> telemetry_text_{};
    HINSTANCE instance_{};
    HFONT font_{};
    HBRUSH background_brush_{};
    HDC back_buffer_dc_{};
    HBITMAP back_buffer_bitmap_{};
    HGDIOBJ back_buffer_old_bitmap_{};
    SIZE back_buffer_size_{};
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};
    OperationWindowTheme theme_{};
    PauseHandler pause_handler_;
    CancelHandler cancel_handler_;
    std::wstring title_;
    std::filesystem::path output_path_;
    OperationProgress progress_{};
    std::shared_ptr<OperationControl> progress_source_;
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>>
        rate_samples_;
    std::uint64_t last_progress_sequence_{};
    double current_rate_{};
    bool has_progress_{false};
    bool progress_dirty_{false};
    bool telemetry_dirty_{false};
    bool paused_{false};
    bool cancelling_{false};
    bool pause_available_{true};
    int pulse_{};
    std::chrono::steady_clock::time_point started_{};
    std::chrono::steady_clock::time_point last_progress_time_{};
    std::chrono::steady_clock::time_point last_heartbeat_paint_{};
};

} // namespace axiom::gui
