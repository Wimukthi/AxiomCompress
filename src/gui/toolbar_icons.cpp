#define NOMINMAX
#include "gui/toolbar_icons.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
        case ToolbarIcon::none: return nullptr;
    }
    return nullptr;
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

} // namespace

void draw_toolbar_icon(HDC dc,
                       ToolbarIcon icon,
                       const RECT& bounds,
                       COLORREF color,
                       UINT dpi,
                       int logical_size) {
    const int size = scale_for_dpi(logical_size, dpi);
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
