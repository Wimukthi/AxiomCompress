#pragma once

#include <windows.h>

namespace axiom::gui {

enum class ToolbarIcon {
    none,
    archive,
    extract,
    test,
    view,
    delete_item,
    info,
    settings,
    open,
    back,
    forward,
    up,
    refresh,
    pause,
    resume,
    cancel,
};

// Draws a theme-tinted Fluent icon centered in bounds. Rasterized bitmaps are
// cached by icon, DPI, and color so owner-draw hover/focus repaints stay cheap.
void draw_toolbar_icon(HDC dc,
                       ToolbarIcon icon,
                       const RECT& bounds,
                       COLORREF color,
                       UINT dpi,
                       int logical_size = 18);

} // namespace axiom::gui
