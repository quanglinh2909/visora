#include "core/ImageMath.hpp"

#include <algorithm>

namespace visora::core {

const char* toString(FitMode mode) {
    switch (mode) {
        case FitMode::Letterbox: return "letterbox";
        case FitMode::Stretch:   return "stretch";
        case FitMode::FitHeight: return "fit-height";
    }
    return "unknown";
}

Rect fitContentRect(FitMode mode, Size src, Size dst) {
    if (!src.valid() || !dst.valid()) return {};

    switch (mode) {
        case FitMode::Stretch:
            return {0, 0, alignDown2(dst.width), alignDown2(dst.height)};

        case FitMode::FitHeight: {
            const float scale = static_cast<float>(dst.height) / static_cast<float>(src.height);
            const int height = alignDown2(dst.height);
            const int width = alignDown2(std::max(
                2, std::min(dst.width, static_cast<int>(static_cast<float>(src.width) * scale))));
            return {0, 0, width, height};
        }

        case FitMode::Letterbox: {
            const float scale = std::min(
                static_cast<float>(dst.width) / static_cast<float>(src.width),
                static_cast<float>(dst.height) / static_cast<float>(src.height));
            const int width = alignDown2(
                std::max(2, static_cast<int>(static_cast<float>(src.width) * scale)));
            const int height = alignDown2(
                std::max(2, static_cast<int>(static_cast<float>(src.height) * scale)));
            return {alignDown2((dst.width - width) / 2),
                    alignDown2((dst.height - height) / 2),
                    width, height};
        }
    }
    return {};
}

void mapToSource(Rect content, Size src, Size dst,
                 float dstX, float dstY, float* srcX, float* srcY) {
    // A zero content rect means nobody recorded the fit; treat the whole
    // destination as content so callers still get a sane mapping.
    const float contentW = content.width > 0 ? static_cast<float>(content.width)
                                             : static_cast<float>(dst.width);
    const float contentH = content.height > 0 ? static_cast<float>(content.height)
                                              : static_cast<float>(dst.height);

    const float scaleX = contentW > 1e-6f ? static_cast<float>(src.width) / contentW : 1.0f;
    const float scaleY = contentH > 1e-6f ? static_cast<float>(src.height) / contentH : 1.0f;

    float x = (dstX - static_cast<float>(content.x)) * scaleX;
    float y = (dstY - static_cast<float>(content.y)) * scaleY;

    x = std::clamp(x, 0.0f, static_cast<float>(src.width));
    y = std::clamp(y, 0.0f, static_cast<float>(src.height));

    if (srcX) *srcX = x;
    if (srcY) *srcY = y;
}

Rect mapToSource(Rect content, Size src, Size dst, Rect dstBox) {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    mapToSource(content, src, dst, static_cast<float>(dstBox.x),
                static_cast<float>(dstBox.y), &left, &top);
    mapToSource(content, src, dst, static_cast<float>(dstBox.right()),
                static_cast<float>(dstBox.bottom()), &right, &bottom);
    return {static_cast<int>(left), static_cast<int>(top),
            static_cast<int>(right - left), static_cast<int>(bottom - top)};
}

Rect expandToMin(Rect crop, Size frame, int minSize) {
    if (!frame.valid()) return {};

    const int width = std::min(std::max(crop.width, minSize), frame.width);
    const int height = std::min(std::max(crop.height, minSize), frame.height);

    int x = crop.x + crop.width / 2 - width / 2;
    int y = crop.y + crop.height / 2 - height / 2;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + width > frame.width) x = frame.width - width;
    if (y + height > frame.height) y = frame.height - height;

    return {alignDown2(std::max(0, x)), alignDown2(std::max(0, y)),
            alignDown2(width), alignDown2(height)};
}

}  // namespace visora::core
