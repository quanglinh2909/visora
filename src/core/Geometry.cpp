#include "core/Geometry.hpp"

namespace visora::core {

Rect clamp(Rect r, Size bounds) {
    const int left   = std::max(0, r.x);
    const int top    = std::max(0, r.y);
    const int right  = std::min(bounds.width, r.right());
    const int bottom = std::min(bounds.height, r.bottom());
    if (right <= left || bottom <= top) return {left, top, 0, 0};
    return {left, top, right - left, bottom - top};
}

}  // namespace visora::core
