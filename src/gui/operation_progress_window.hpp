#pragma once

#include "axiom/axiom.hpp"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

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
                CancelHandler cancel_handler);

    void set_theme(const OperationWindowTheme& theme);
    void set_progress(const OperationProgress& progress);
    void set_cancelling();
    void close();

    [[nodiscard]] HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    bool register_class() const;
    void create_controls();
    void rebuild_font();
    void apply_theme();
    void layout();
    void paint();
    void draw_button(const DRAWITEMSTRUCT& draw) const;
    void toggle_pause();
    void request_cancel();
    int scale(int value) const;

    HWND owner_{};
    HWND hwnd_{};
    HWND pause_button_{};
    HWND cancel_button_{};
    HINSTANCE instance_{};
    HFONT font_{};
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};
    OperationWindowTheme theme_{};
    PauseHandler pause_handler_;
    CancelHandler cancel_handler_;
    std::wstring title_;
    std::filesystem::path output_path_;
    OperationProgress progress_{};
    bool has_progress_{false};
    bool paused_{false};
    bool cancelling_{false};
    int pulse_{};
    std::chrono::steady_clock::time_point started_{};
};

} // namespace axiom::gui
