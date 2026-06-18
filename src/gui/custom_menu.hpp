#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace axiom::gui {

struct CustomMenuTheme {
    COLORREF background{};
    COLORREF hot{};
    COLORREF pressed{};
    COLORREF text{};
    COLORREF disabled_text{};
    COLORREF border{};
    COLORREF separator{};
};

struct CustomMenuItem {
    UINT command{};
    std::wstring label;
    std::wstring shortcut;
    bool enabled{true};
    bool separator{false};
    bool checked{false};
};

class CustomMenuBar {
public:
    using ItemProvider = std::function<std::vector<CustomMenuItem>(UINT)>;
    using CommandHandler = std::function<void(UINT)>;

    CustomMenuBar() = default;
    CustomMenuBar(const CustomMenuBar&) = delete;
    CustomMenuBar& operator=(const CustomMenuBar&) = delete;

    bool create(HWND parent,
                HINSTANCE instance,
                std::vector<std::pair<UINT, std::wstring>> entries,
                ItemProvider item_provider,
                CommandHandler command_handler);

    [[nodiscard]] HWND hwnd() const { return hwnd_; }
    [[nodiscard]] int preferred_height() const;

    void set_dpi(UINT dpi);
    void set_font(HFONT font);
    void set_theme(const CustomMenuTheme& theme);
    void move(int x, int y, int width, int height);

    // Called before the application's accelerator routing so Alt/F10 and menu
    // mnemonics retain standard menu-bar behavior while focus stays elsewhere.
    bool translate_message(const MSG& message);
    UINT show_context_menu(std::vector<CustomMenuItem> items, POINT screen_point);

private:
    struct Entry {
        UINT id{};
        std::wstring text;
        RECT rect{};
    };

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    void paint();
    void layout_entries(HDC dc, int width);
    void track_mouse_leave();
    void set_keyboard_index(int index);
    void enter_keyboard_mode(int index = 0);
    void exit_keyboard_mode(bool restore_focus);
    void show_menu(int index);
    int hit_test(POINT point) const;
    int mnemonic_index(WPARAM key) const;
    bool handle_menu_key(WPARAM key);

    HWND parent_{};
    HWND hwnd_{};
    HINSTANCE instance_{};
    HFONT font_{};
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};
    CustomMenuTheme theme_{};
    std::vector<Entry> entries_;
    ItemProvider item_provider_;
    CommandHandler command_handler_;
    int hot_index_{-1};
    int active_index_{-1};
    int keyboard_index_{-1};
    bool mouse_tracking_{false};
    bool keyboard_cues_{false};
    HWND previous_focus_{};
};

} // namespace axiom::gui
