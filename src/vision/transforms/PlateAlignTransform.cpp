// Plate alignment before OCR: cut the plate out TIGHT and fit it to the
// reader's input.
//
// Deliberately not the plain crop. `crop` pads a detection with a quarter of
// its own size in context, which is right for a detector — one is trained on
// boxes that include surroundings — and wrong here. The model input is a fixed
// size, so every pixel spent on padding is a pixel not spent on a character: a
// reader given a glyph 12 px tall reads worse than the same reader given it at
// 16 px, and that difference is the whole margin on a distant plate.
//
// A plate seen at an angle would read better still if its quadrilateral were
// rectified rather than boxed. That needs the four corners, which means the
// segmentation mask, and the mask this system carries is 32x32 for the whole
// box — about three pixels of corner precision on a typical plate, enough to
// tilt a correction the wrong way. Boxing is what the predecessor shipped and
// what its accuracy was measured on, so it is what this does; corner
// rectification is a change to make with real plates in front of you.

#include "vision/Transform.hpp"

namespace visora::vision {
namespace {

class PlateAlignTransform final : public Transform {
public:
    std::string_view id() const override { return "align_plate"; }
    std::string_view label() const override { return "Plate alignment"; }
    std::string_view description() const override {
        return "Cuts the plate out tight, with no context padding, before the reader.";
    }

    core::Status apply(TransformContext& context,
                       core::MutableImageView& out) const override {
        Transform* crop = transform(kDefaultTransformId);
        if (crop == nullptr) return core::internalError("the crop transform is not registered");
        // The one thing this changes about a crop — no context padding. Asking
        // for it this way rather than copying the crop means a later fix to how
        // boxes are cut reaches plates too.
        context.tightCrop = true;
        return crop->apply(context, out);
    }
};

const core::Register<Transform> registration({
    "align_plate",
    /*priority=*/0,
    [] { return core::Probe::yes("crop the plate tight for the reader"); },
    [] { return std::unique_ptr<Transform>(new PlateAlignTransform()); },
});

}  // namespace
}  // namespace visora::vision
