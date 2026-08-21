#pragma once

#include "axiom/axiom.hpp"
#include "core/progress_rate.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <deque>
#include <memory>
#include <string>
#include <thread>
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

    // Screen placement supplied by the caller instead of derived from an owner
    // window. A progress window hosted on its own UI thread must not take a
    // cross-thread owner, because owning a window on another thread attaches
    // the two input queues and reintroduces the stall the thread exists to
    // avoid. Placement lets such a window still centre on the main window.
    struct Placement {
        RECT anchor{};       // Screen rect to centre on; empty falls back to the work area.
        UINT dpi{};          // 0 derives the DPI from the owner, or the system.
        bool topmost{false}; // Keep an ownerless window from falling behind the drop target.
        // Live theme tracking reads and writes process-wide theme state, which
        // is only safe from the GUI thread. A window hosted on another thread
        // must decline the broadcast and keep the theme it was created with.
        bool follow_system_theme{true};
    };

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
                bool pause_available = true,
                const Placement* placement = nullptr);

    void set_theme(const OperationWindowTheme& theme);
    void set_progress(const OperationProgress& progress);
    void set_progress_source(std::shared_ptr<OperationControl> source);
    void set_cancelling();
    void close();

    [[nodiscard]] HWND hwnd() const { return hwnd_; }

private:
    // One row per thing a reader needs, not one row per struct field. Anything
    // that is diagnostic rather than actionable lives behind Details.
    enum class TelemetryField : std::size_t {
        stage,            // "Stage 3 of 5: Compressing"
        output_path,      // Static destination, muted
        overall_summary,  // Percent, bytes, items
        overall_rate,     // Speed and time remaining
        current_path,     // Item in flight, muted
        file_summary,     // Bytes for the item in flight
        result_summary,   // Compressed size, ratio, reuse
        plan_summary,     // Add/update/remove/unchanged, only when planned
        activity,         // Active / Paused / Cancelling, muted
        detail_timing,    // Details: elapsed and time since the last update
        detail_archive,   // Details: physical archive bytes read
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
    void toggle_details();
    void request_cancel();
    int scale(int value) const;
    void apply_window_size();
    void reserve_optional_rows(int needed);
    [[nodiscard]] int activity_row() const;
    [[nodiscard]] int collapsed_height() const;
    [[nodiscard]] int expanded_height() const;

    HWND owner_{};
    HWND hwnd_{};
    HWND pause_button_{};
    HWND cancel_button_{};
    HWND details_button_{};
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
    ProgressRateTracker rate_;
    std::uint64_t last_progress_sequence_{};
    bool has_progress_{false};
    bool progress_dirty_{false};
    bool telemetry_dirty_{false};
    bool paused_{false};
    bool cancelling_{false};
    bool pause_available_{true};
    bool follow_system_theme_{true};
    bool details_expanded_{false};
    // Sticky for the life of one operation: see the file-bar comment in paint().
    bool file_bar_active_{false};
    int reserved_optional_rows_{0};
    int pulse_{};
    std::chrono::steady_clock::time_point started_{};
    std::chrono::steady_clock::time_point last_progress_time_{};
    std::chrono::steady_clock::time_point last_heartbeat_paint_{};
};

// An OperationProgressWindow hosted on its own UI thread.
//
// Use this when the work being reported runs *on the GUI thread itself* and so
// cannot yield to a message loop. The clearest case is drag-and-drop out of an
// archive: the shell copies staged files by calling back into Axiom's
// IStream implementation, and those calls are dispatched on the source STA.
// While they run, that thread dispatches no WM_TIMER and no input, so a
// progress window it owns neither repaints nor accepts a Cancel click.
//
// Hosting the window on a separate thread decouples it from that. Progress is
// read from the shared OperationControl snapshot, which is already the single
// source of progress truth and is safe to read from any thread.
class ThreadedOperationProgressWindow {
public:
    using PauseHandler = OperationProgressWindow::PauseHandler;
    using CancelHandler = OperationProgressWindow::CancelHandler;

    ThreadedOperationProgressWindow() = default;
    ~ThreadedOperationProgressWindow();

    ThreadedOperationProgressWindow(const ThreadedOperationProgressWindow&) = delete;
    ThreadedOperationProgressWindow& operator=(const ThreadedOperationProgressWindow&) = delete;

    // Blocks until the window exists, so a caller can rely on it being visible
    // on return. `anchor` is read on the calling thread and never retained.
    // The handlers are invoked on the progress thread, so they must only touch
    // thread-safe state -- OperationControl is the intended target.
    bool start(HWND anchor,
               HINSTANCE instance,
               std::wstring title,
               std::filesystem::path output_path,
               const OperationWindowTheme& theme,
               PauseHandler pause_handler,
               CancelHandler cancel_handler,
               bool pause_available,
               std::shared_ptr<OperationControl> source);

    // Closes the window and joins its thread. Safe to call when not running.
    void stop();

    [[nodiscard]] bool running() const { return thread_.joinable(); }

private:
    std::thread thread_;
    DWORD thread_id_{};
};

} // namespace axiom::gui
