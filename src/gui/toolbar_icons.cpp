#define NOMINMAX
#include "gui/toolbar_icons.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace axiom::gui {

namespace {

constexpr int kSourceSize = 24;

#include "gui/toolbar_icon_masks.inc"

const std::uint8_t* mask_for_icon(ToolbarIcon icon) {
    switch (icon) {
        case ToolbarIcon::archive: return kArchiveMask.data();
        case ToolbarIcon::extract: return kExtractMask.data();
        case ToolbarIcon::test: return kTestMask.data();
        case ToolbarIcon::view: return kViewMask.data();
        case ToolbarIcon::delete_item: return kDeleteMask.data();
        case ToolbarIcon::info: return kInfoMask.data();
        case ToolbarIcon::settings: return kSettingsMask.data();
        case ToolbarIcon::open: return kOpenMask.data();
        case ToolbarIcon::back: return kBackMask.data();
        case ToolbarIcon::forward: return kForwardMask.data();
        case ToolbarIcon::up: return kUpMask.data();
        case ToolbarIcon::refresh: return kRefreshMask.data();
        case ToolbarIcon::pause: return kPauseMask.data();
        case ToolbarIcon::resume: return kResumeMask.data();
        case ToolbarIcon::cancel: return kCancelMask.data();
        case ToolbarIcon::update_archive:
        case ToolbarIcon::freshen_archive:
        case ToolbarIcon::synchronize_archive:
        case ToolbarIcon::repack:
        case ToolbarIcon::split:
        case ToolbarIcon::join:
        case ToolbarIcon::select_all:
        case ToolbarIcon::compress_stream:
        case ToolbarIcon::decompress_stream:
        case ToolbarIcon::find:
        case ToolbarIcon::benchmark:
        case ToolbarIcon::comment:
        case ToolbarIcon::lock:
        case ToolbarIcon::repair:
        case ToolbarIcon::recovery:
        case ToolbarIcon::key:
        case ToolbarIcon::sign:
        case ToolbarIcon::verify_signature:
        case ToolbarIcon::sfx:
        case ToolbarIcon::tree:
        case ToolbarIcon::favorite:
        case ToolbarIcon::unfavorite:
        case ToolbarIcon::copy_path:
        case ToolbarIcon::copy_crc:
        case ToolbarIcon::none: return nullptr;
    }
    return nullptr;
}

bool generated_icon(ToolbarIcon icon) {
    switch (icon) {
        case ToolbarIcon::update_archive:
        case ToolbarIcon::freshen_archive:
        case ToolbarIcon::synchronize_archive:
        case ToolbarIcon::repack:
        case ToolbarIcon::split:
        case ToolbarIcon::join:
        case ToolbarIcon::select_all:
        case ToolbarIcon::compress_stream:
        case ToolbarIcon::decompress_stream:
        case ToolbarIcon::find:
        case ToolbarIcon::benchmark:
        case ToolbarIcon::comment:
        case ToolbarIcon::lock:
        case ToolbarIcon::repair:
        case ToolbarIcon::recovery:
        case ToolbarIcon::key:
        case ToolbarIcon::sign:
        case ToolbarIcon::verify_signature:
        case ToolbarIcon::sfx:
        case ToolbarIcon::tree:
        case ToolbarIcon::favorite:
        case ToolbarIcon::unfavorite:
        case ToolbarIcon::copy_path:
        case ToolbarIcon::copy_crc:
            return true;
        default:
            return false;
    }
}

COLORREF color_for_icon(ToolbarIcon icon, COLORREF fallback, ToolbarIconStyle style) {
    if (style != ToolbarIconStyle::colorful) return fallback;
    switch (icon) {
        case ToolbarIcon::archive: return RGB(255, 185, 60);
        case ToolbarIcon::open: return RGB(245, 169, 64);
        case ToolbarIcon::extract: return RGB(83, 174, 255);
        case ToolbarIcon::test: return RGB(96, 205, 112);
        case ToolbarIcon::view: return RGB(124, 185, 255);
        case ToolbarIcon::delete_item: return RGB(255, 99, 99);
        case ToolbarIcon::info: return RGB(92, 170, 255);
        case ToolbarIcon::settings: return RGB(180, 143, 255);
        case ToolbarIcon::back:
        case ToolbarIcon::forward:
        case ToolbarIcon::up:
        case ToolbarIcon::refresh:
            return RGB(116, 192, 255);
        case ToolbarIcon::pause: return RGB(255, 193, 84);
        case ToolbarIcon::resume: return RGB(96, 205, 112);
        case ToolbarIcon::cancel: return RGB(255, 99, 99);
        case ToolbarIcon::update_archive: return RGB(83, 174, 255);
        case ToolbarIcon::freshen_archive: return RGB(96, 205, 112);
        case ToolbarIcon::synchronize_archive: return RGB(72, 214, 198);
        case ToolbarIcon::repack: return RGB(255, 185, 60);
        case ToolbarIcon::split: return RGB(255, 193, 84);
        case ToolbarIcon::join: return RGB(72, 214, 198);
        case ToolbarIcon::select_all: return RGB(124, 185, 255);
        case ToolbarIcon::compress_stream: return RGB(255, 185, 60);
        case ToolbarIcon::decompress_stream: return RGB(83, 174, 255);
        case ToolbarIcon::find: return RGB(124, 185, 255);
        case ToolbarIcon::benchmark: return RGB(255, 193, 84);
        case ToolbarIcon::comment: return RGB(255, 185, 60);
        case ToolbarIcon::lock: return RGB(255, 193, 84);
        case ToolbarIcon::repair: return RGB(255, 153, 87);
        case ToolbarIcon::recovery: return RGB(96, 205, 112);
        case ToolbarIcon::key: return RGB(255, 211, 92);
        case ToolbarIcon::sign: return RGB(96, 205, 112);
        case ToolbarIcon::verify_signature: return RGB(96, 205, 112);
        case ToolbarIcon::sfx: return RGB(180, 143, 255);
        case ToolbarIcon::tree: return RGB(83, 174, 255);
        case ToolbarIcon::favorite: return RGB(255, 211, 92);
        case ToolbarIcon::unfavorite: return RGB(255, 142, 142);
        case ToolbarIcon::copy_path: return RGB(124, 185, 255);
        case ToolbarIcon::copy_crc: return RGB(180, 143, 255);
        case ToolbarIcon::none: return fallback;
    }
    return fallback;
}

int scale_for_dpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi),
                  USER_DEFAULT_SCREEN_DPI);
}

std::uint8_t sample_mask(const std::uint8_t* mask, double source_x, double source_y) {
    const int x0 = std::clamp(static_cast<int>(std::floor(source_x)), 0, kSourceSize - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(source_y)), 0, kSourceSize - 1);
    const int x1 = std::min(x0 + 1, kSourceSize - 1);
    const int y1 = std::min(y0 + 1, kSourceSize - 1);
    const double fx = std::clamp(source_x - std::floor(source_x), 0.0, 1.0);
    const double fy = std::clamp(source_y - std::floor(source_y), 0.0, 1.0);
    const auto at = [mask](int x, int y) {
        return static_cast<double>(mask[y * kSourceSize + x]);
    };
    const double top = at(x0, y0) * (1.0 - fx) + at(x1, y0) * fx;
    const double bottom = at(x0, y1) * (1.0 - fx) + at(x1, y1) * fx;
    return static_cast<std::uint8_t>(std::lround(top * (1.0 - fy) + bottom * fy));
}

struct CachedBitmap {
    ToolbarIcon icon{ToolbarIcon::none};
    COLORREF color{};
    int size{};
    HBITMAP bitmap{};
};

class IconBitmapCache {
public:
    ~IconBitmapCache() {
        for (const auto& entry : entries_) {
            if (entry.bitmap != nullptr) DeleteObject(entry.bitmap);
        }
    }

    HBITMAP get(ToolbarIcon icon, COLORREF color, int size) {
        for (const auto& entry : entries_) {
            if (entry.icon == icon && entry.color == color && entry.size == size) {
                return entry.bitmap;
            }
        }
        HBITMAP bitmap = create(icon, color, size);
        if (bitmap != nullptr) entries_.push_back({icon, color, size, bitmap});
        return bitmap;
    }

private:
    static HBITMAP create(ToolbarIcon icon, COLORREF color, int size) {
        const std::uint8_t* mask = mask_for_icon(icon);
        if (mask == nullptr || size <= 0) return nullptr;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size;
        info.bmiHeader.biHeight = -size;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* raw_pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                          &raw_pixels, nullptr, 0);
        if (bitmap == nullptr || raw_pixels == nullptr) {
            if (bitmap != nullptr) DeleteObject(bitmap);
            return nullptr;
        }

        auto* pixels = static_cast<std::uint32_t*>(raw_pixels);
        const std::uint8_t red = GetRValue(color);
        const std::uint8_t green = GetGValue(color);
        const std::uint8_t blue = GetBValue(color);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const double source_x = (static_cast<double>(x) + 0.5) * kSourceSize / size - 0.5;
                const double source_y = (static_cast<double>(y) + 0.5) * kSourceSize / size - 0.5;
                const std::uint8_t alpha = sample_mask(mask, source_x, source_y);
                const std::uint8_t premultiplied_red = static_cast<std::uint8_t>(red * alpha / 255);
                const std::uint8_t premultiplied_green = static_cast<std::uint8_t>(green * alpha / 255);
                const std::uint8_t premultiplied_blue = static_cast<std::uint8_t>(blue * alpha / 255);
                pixels[y * size + x] =
                    (static_cast<std::uint32_t>(alpha) << 24) |
                    (static_cast<std::uint32_t>(premultiplied_red) << 16) |
                    (static_cast<std::uint32_t>(premultiplied_green) << 8) |
                    premultiplied_blue;
            }
        }
        return bitmap;
    }

    std::vector<CachedBitmap> entries_;
};

IconBitmapCache& icon_cache() {
    static IconBitmapCache cache;
    return cache;
}

class GeneratedIconPainter {
public:
    GeneratedIconPainter(HDC dc, const RECT& bounds, COLORREF color, UINT dpi,
                         int logical_size)
        : dc_(dc) {
        size_ = scale_for_dpi(logical_size, dpi);
        left_ = bounds.left + (bounds.right - bounds.left - size_) / 2;
        top_ = bounds.top + (bounds.bottom - bounds.top - size_) / 2;
        pen_ = CreatePen(PS_SOLID, std::max(1, scale_for_dpi(2, dpi)), color);
        old_pen_ = SelectObject(dc_, pen_);
        old_brush_ = SelectObject(dc_, GetStockObject(HOLLOW_BRUSH));
        SetBkMode(dc_, TRANSPARENT);
    }

    ~GeneratedIconPainter() {
        if (old_pen_ != nullptr) SelectObject(dc_, old_pen_);
        if (old_brush_ != nullptr) SelectObject(dc_, old_brush_);
        if (pen_ != nullptr) DeleteObject(pen_);
    }

    int x(int value) const { return left_ + MulDiv(value, size_, kSourceSize); }
    int y(int value) const { return top_ + MulDiv(value, size_, kSourceSize); }
    RECT r(int left, int top, int right, int bottom) const {
        return {x(left), y(top), x(right), y(bottom)};
    }
    POINT p(int x_value, int y_value) const { return {x(x_value), y(y_value)}; }

    void line(int x1, int y1, int x2, int y2) const {
        MoveToEx(dc_, x(x1), y(y1), nullptr);
        LineTo(dc_, x(x2), y(y2));
    }
    void rect(int left, int top, int right, int bottom) const {
        const RECT bounds = r(left, top, right, bottom);
        Rectangle(dc_, bounds.left, bounds.top, bounds.right, bounds.bottom);
    }
    void ellipse(int left, int top, int right, int bottom) const {
        const RECT bounds = r(left, top, right, bottom);
        Ellipse(dc_, bounds.left, bounds.top, bounds.right, bounds.bottom);
    }
    void polyline(std::initializer_list<POINT> points) const {
        std::vector<POINT> copy(points.begin(), points.end());
        if (!copy.empty()) Polyline(dc_, copy.data(), static_cast<int>(copy.size()));
    }

private:
    HDC dc_{};
    int left_{};
    int top_{};
    int size_{};
    HPEN pen_{};
    HGDIOBJ old_pen_{};
    HGDIOBJ old_brush_{};
};

void draw_generated_toolbar_icon(HDC dc, ToolbarIcon icon, const RECT& bounds,
                                 COLORREF color, UINT dpi, int logical_size) {
    GeneratedIconPainter icon_dc(dc, bounds, color, dpi, logical_size);
    switch (icon) {
        case ToolbarIcon::update_archive:
            icon_dc.rect(4, 5, 20, 19);
            icon_dc.line(4, 8, 20, 8);
            icon_dc.line(12, 6, 12, 15);
            icon_dc.line(8, 11, 12, 15);
            icon_dc.line(16, 11, 12, 15);
            break;
        case ToolbarIcon::freshen_archive:
            icon_dc.ellipse(5, 5, 19, 19);
            icon_dc.line(12, 12, 12, 7);
            icon_dc.line(12, 12, 16, 12);
            icon_dc.line(18, 5, 18, 9);
            icon_dc.line(16, 7, 20, 7);
            break;
        case ToolbarIcon::synchronize_archive:
            Arc(dc, icon_dc.x(4), icon_dc.y(5), icon_dc.x(20), icon_dc.y(19),
                icon_dc.x(18), icon_dc.y(7), icon_dc.x(7), icon_dc.y(7));
            icon_dc.line(7, 7, 10, 5);
            icon_dc.line(7, 7, 10, 10);
            Arc(dc, icon_dc.x(4), icon_dc.y(5), icon_dc.x(20), icon_dc.y(19),
                icon_dc.x(6), icon_dc.y(17), icon_dc.x(17), icon_dc.y(17));
            icon_dc.line(17, 17, 14, 14);
            icon_dc.line(17, 17, 14, 20);
            break;
        case ToolbarIcon::repack:
            icon_dc.rect(5, 8, 16, 19);
            icon_dc.rect(8, 5, 19, 16);
            icon_dc.line(9, 18, 13, 18);
            icon_dc.line(13, 18, 11, 16);
            break;
        case ToolbarIcon::split:
            icon_dc.rect(4, 5, 20, 19);
            icon_dc.line(12, 5, 12, 19);
            icon_dc.line(8, 12, 4, 8);
            icon_dc.line(8, 12, 4, 16);
            icon_dc.line(16, 12, 20, 8);
            icon_dc.line(16, 12, 20, 16);
            break;
        case ToolbarIcon::join:
            icon_dc.rect(4, 5, 20, 19);
            icon_dc.line(12, 5, 12, 19);
            icon_dc.line(4, 12, 9, 12);
            icon_dc.line(9, 12, 7, 10);
            icon_dc.line(9, 12, 7, 14);
            icon_dc.line(20, 12, 15, 12);
            icon_dc.line(15, 12, 17, 10);
            icon_dc.line(15, 12, 17, 14);
            break;
        case ToolbarIcon::select_all:
            icon_dc.rect(5, 5, 19, 19);
            icon_dc.line(8, 12, 11, 15);
            icon_dc.line(11, 15, 17, 9);
            break;
        case ToolbarIcon::compress_stream:
            icon_dc.rect(5, 4, 19, 20);
            icon_dc.line(8, 8, 16, 8);
            icon_dc.line(8, 12, 16, 12);
            icon_dc.line(8, 16, 16, 16);
            icon_dc.line(12, 5, 12, 19);
            icon_dc.line(9, 9, 12, 12);
            icon_dc.line(15, 9, 12, 12);
            break;
        case ToolbarIcon::decompress_stream:
            icon_dc.rect(5, 4, 19, 20);
            icon_dc.line(8, 8, 16, 8);
            icon_dc.line(8, 12, 16, 12);
            icon_dc.line(8, 16, 16, 16);
            icon_dc.line(12, 5, 12, 19);
            icon_dc.line(9, 15, 12, 18);
            icon_dc.line(15, 15, 12, 18);
            break;
        case ToolbarIcon::find:
            icon_dc.ellipse(5, 5, 15, 15);
            icon_dc.line(14, 14, 20, 20);
            break;
        case ToolbarIcon::benchmark:
            icon_dc.line(5, 19, 20, 19);
            icon_dc.line(5, 19, 5, 5);
            icon_dc.rect(8, 13, 10, 19);
            icon_dc.rect(12, 9, 14, 19);
            icon_dc.rect(16, 6, 18, 19);
            break;
        case ToolbarIcon::comment:
            icon_dc.rect(4, 6, 20, 17);
            icon_dc.line(8, 17, 6, 21);
            icon_dc.line(6, 21, 12, 17);
            break;
        case ToolbarIcon::lock:
            icon_dc.rect(6, 11, 18, 20);
            icon_dc.line(8, 11, 8, 9);
            icon_dc.line(8, 9, 12, 5);
            icon_dc.line(12, 5, 16, 9);
            icon_dc.line(16, 9, 16, 11);
            break;
        case ToolbarIcon::repair:
            icon_dc.line(7, 17, 17, 7);
            icon_dc.line(6, 18, 9, 21);
            icon_dc.line(15, 5, 19, 9);
            icon_dc.line(17, 7, 20, 4);
            break;
        case ToolbarIcon::recovery:
            icon_dc.ellipse(4, 4, 20, 20);
            icon_dc.ellipse(9, 9, 15, 15);
            icon_dc.line(12, 4, 12, 9);
            icon_dc.line(12, 15, 12, 20);
            icon_dc.line(4, 12, 9, 12);
            icon_dc.line(15, 12, 20, 12);
            break;
        case ToolbarIcon::key:
            icon_dc.ellipse(4, 9, 11, 16);
            icon_dc.line(11, 12, 21, 12);
            icon_dc.line(17, 12, 17, 16);
            icon_dc.line(20, 12, 20, 15);
            break;
        case ToolbarIcon::sign:
            icon_dc.rect(5, 4, 19, 20);
            icon_dc.line(8, 15, 11, 18);
            icon_dc.line(11, 18, 17, 11);
            icon_dc.line(8, 8, 16, 8);
            break;
        case ToolbarIcon::verify_signature:
            icon_dc.rect(5, 4, 19, 20);
            icon_dc.line(8, 13, 11, 16);
            icon_dc.line(11, 16, 17, 9);
            break;
        case ToolbarIcon::sfx:
            icon_dc.rect(5, 4, 19, 20);
            icon_dc.line(10, 7, 15, 7);
            icon_dc.line(9, 12, 15, 12);
            icon_dc.line(9, 16, 15, 16);
            icon_dc.line(12, 8, 10, 13);
            icon_dc.line(10, 13, 14, 13);
            icon_dc.line(14, 13, 12, 18);
            break;
        case ToolbarIcon::tree:
            icon_dc.ellipse(4, 5, 8, 9);
            icon_dc.ellipse(16, 5, 20, 9);
            icon_dc.ellipse(10, 16, 14, 20);
            icon_dc.line(8, 7, 16, 7);
            icon_dc.line(12, 7, 12, 16);
            break;
        case ToolbarIcon::favorite:
        case ToolbarIcon::unfavorite:
            icon_dc.polyline({
                icon_dc.p(12, 4), icon_dc.p(14, 9), icon_dc.p(20, 9),
                icon_dc.p(15, 13), icon_dc.p(17, 20), icon_dc.p(12, 16),
                icon_dc.p(7, 20), icon_dc.p(9, 13), icon_dc.p(4, 9),
                icon_dc.p(10, 9), icon_dc.p(12, 4)
            });
            if (icon == ToolbarIcon::unfavorite) icon_dc.line(5, 20, 20, 5);
            break;
        case ToolbarIcon::copy_path:
            icon_dc.rect(7, 5, 18, 16);
            icon_dc.rect(4, 8, 15, 19);
            icon_dc.line(7, 15, 12, 15);
            break;
        case ToolbarIcon::copy_crc:
            icon_dc.rect(5, 4, 19, 20);
            icon_dc.line(8, 9, 16, 9);
            icon_dc.line(8, 13, 16, 13);
            icon_dc.line(8, 17, 13, 17);
            break;
        default:
            break;
    }
}

} // namespace

void draw_toolbar_icon(HDC dc,
                       ToolbarIcon icon,
                       const RECT& bounds,
                       COLORREF color,
                       UINT dpi,
                       int logical_size,
                       ToolbarIconStyle style) {
    const int size = scale_for_dpi(logical_size, dpi);
    color = color_for_icon(icon, color, style);
    if (dc != nullptr && generated_icon(icon)) {
        draw_generated_toolbar_icon(dc, icon, bounds, color, dpi, logical_size);
        return;
    }
    HBITMAP bitmap = icon_cache().get(icon, color, size);
    if (dc == nullptr || bitmap == nullptr) return;

    HDC memory_dc = CreateCompatibleDC(dc);
    if (memory_dc == nullptr) return;
    HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
    const int x = bounds.left + (bounds.right - bounds.left - size) / 2;
    const int y = bounds.top + (bounds.bottom - bounds.top - size) / 2;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, x, y, size, size, memory_dc, 0, 0, size, size, blend);
    SelectObject(memory_dc, old_bitmap);
    DeleteDC(memory_dc);
}

} // namespace axiom::gui
