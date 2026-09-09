#pragma once

// Pure geometry for fitting a picture into a model input and mapping
// coordinates back out again.
//
// This is arithmetic, not pixel pushing: it has no dependency on any backend,
// so every backend agrees on the same content rectangle and detections map back
// identically whether the blit ran on an NPU-adjacent 2D engine or on the CPU.
// It is also the part that was previously untestable, buried inside a header
// that pulled in librga.

#include "core/Geometry.hpp"

namespace visora::core {

// How a source picture is fitted into a fixed model input.
enum class FitMode {
    // Preserve aspect ratio, centre the image, pad the remainder. What object
    // detectors are trained on.
    Letterbox,
    // Fill the input exactly, distorting the aspect ratio. What a text
    // recogniser trained on lines squashed into a fixed size expects.
    Stretch,
    // Match the destination height, keep the true aspect ratio, anchor left;
    // whatever is left over on the right stays padding.
    FitHeight,
};

const char* toString(FitMode mode);

// Rounds down / up to an even number. Chroma-subsampled formats address pixels
// in 2x2 blocks, so odd offsets and sizes are not representable.
inline int alignDown2(int v) { return v & ~1; }
inline int alignUp2(int v) { return (v + 1) & ~1; }

// Where the real (non-padding) content lands inside a `dst`-sized destination.
// Returns a zero rect when either size is degenerate.
Rect fitContentRect(FitMode mode, Size src, Size dst);

// Maps a point in destination (model input) space back to source (full
// resolution) space, given the content rect `fitContentRect` produced. Points
// falling in the padding clamp to the source bounds.
void mapToSource(Rect content, Size src, Size dst,
                 float dstX, float dstY, float* srcX, float* srcY);

// Maps a whole box back to source space.
Rect mapToSource(Rect content, Size src, Size dst, Rect dstBox);

// Grows a crop around its own centre to at least `minSize` on each side, keeps
// it inside the frame, and rounds to even coordinates.
//
// Small crops are why this exists: a licence plate 30 px wide upscaled straight
// to a 512 px model input exceeds what a fixed-function 2D scaler will do, and
// the surrounding context is usually what makes the crop legible anyway.
Rect expandToMin(Rect crop, Size frame, int minSize = 128);

}  // namespace visora::core
