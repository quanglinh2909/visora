#pragma once

// Turns one detection into the image the NEXT stage sees.
//
// The obvious case is a plain crop, and that is the default. The interesting
// ones are not: a face recogniser wants the face rotated upright by its eye
// landmarks, and a plate reader wants the plate deskewed by its corners. Both
// read far better on a warped crop than on a rectangular one, and neither is
// something a detector can do for itself.
//
// Registered like model types, so adding one is a new file rather than an edit
// to a list — it appears in GET /ai-transforms by itself.
//
// Implementations MUST be stateless: one shared instance is used by every job
// worker thread.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/Geometry.hpp"
#include "core/Image.hpp"
#include "core/Registry.hpp"
#include "core/Result.hpp"
#include "vision/Detection.hpp"
#include "vision/ModelType.hpp"

namespace visora::vision {

struct TransformContext {
    // The full-resolution frame. Crops are taken from here, never from the
    // letterboxed image the detector saw — that is the difference between
    // reading small text and guessing at it.
    core::ImageView source;

    const Detection* detection = nullptr;

    // The box to cut, ALWAYS in frame coordinates.
    //
    // Read from here, not from detection->x1: from the third stage on, a
    // parent's x1..y2 are in ITS parent's crop space, while the cut still
    // happens on the original frame.
    core::Rect box;

    // Flat (x, y, score) triples in frame coordinates, when the detector
    // produced them. A transform that needs landmarks and has none must decline
    // rather than invent them.
    const std::vector<float>* keypoints = nullptr;

    core::Size target;
    FramePrep prep = FramePrep::Letterbox;
    bool tightCrop = false;

    // --- OUTPUT ---
    // The region of the source frame the produced image covers. The runner maps
    // child boxes back through this, which is what lets a THIRD stage crop
    // correctly. A transform that is not a rectangular crop (a rotation, a
    // warp) leaves this empty and the runner falls back to the parent's box.
    core::Rect covered;

    // Where the content sits inside the produced image, when it was letterboxed
    // into it. Left empty when the transform filled the target exactly.
    core::Rect contentRect;
};

class Transform {
public:
    virtual ~Transform() = default;

    // The id stored in a job stage. The empty id is the plain crop.
    virtual std::string_view id() const = 0;
    virtual std::string_view label() const = 0;
    virtual std::string_view description() const = 0;

    // Produces the stage input. Returns Unsupported to SKIP this detection —
    // a face transform with no landmarks, say — which is not an error: the
    // other detections in the frame still go through.
    virtual core::Status apply(TransformContext& context,
                               core::MutableImageView& out) const = 0;
};

core::Registry<Transform>& transformRegistry();

std::vector<Transform*> transforms();
Transform* transform(std::string_view id);

}  // namespace visora::vision
