#pragma once

#include <algorithm>

namespace visora::core {

struct Size {
    int width = 0;
    int height = 0;

    bool valid() const { return width > 0 && height > 0; }
    friend bool operator==(const Size& a, const Size& b) {
        return a.width == b.width && a.height == b.height;
    }
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool valid() const { return width > 0 && height > 0; }
    int right() const { return x + width; }
    int bottom() const { return y + height; }
    Size size() const { return {width, height}; }

    friend bool operator==(const Rect& a, const Rect& b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }
};

// Clips `r` to [0,0,bounds.width,bounds.height]. An entirely outside rect comes
// back with zero extent rather than negative sizes.
Rect clamp(Rect r, Size bounds);

}  // namespace visora::core
