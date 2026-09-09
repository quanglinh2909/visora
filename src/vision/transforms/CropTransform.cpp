// The plain crop: cut the box out of the frame and fit it to the model input.
//
// The default, and what most stages want. Registered like every other
// transform, so it is not a special case in the runner — the runner asks for
// the transform named "" and gets this.

#include "core/ImageMath.hpp"
#include "hal/ImageOps.hpp"
#include "vision/Transform.hpp"

namespace visora::vision {
namespace {

class CropTransform final : public Transform {
public:
    std::string_view id() const override { return ""; }
    std::string_view label() const override { return "Crop"; }
    std::string_view description() const override {
        return "Cuts the detection out of the frame and fits it to the model input.";
    }

    core::Status apply(TransformContext& context,
                       core::MutableImageView& out) const override {
        if (!context.source.valid()) return core::invalidArgument("no source frame");

        core::Rect box = context.box;
        if (box.width <= 0 || box.height <= 0) {
            return core::unsupported("the detection has no area");
        }

        if (!context.tightCrop) {
            // A little context around the object. Detectors are trained on
            // boxes that include some surroundings, and a crop cut exactly to
            // the box reads worse than one with a margin — the object touches
            // every edge, which it never does in training.
            box = expandBox(box, kContextFraction);
        }
        box = core::clamp(box, context.source.size);
        if (box.width <= 0 || box.height <= 0) {
            return core::unsupported("the detection falls outside the frame");
        }

        auto ops = hal::imageOps();
        if (!ops) return ops.error();

        // Tell the runner what this covered, so a later stage's boxes map back
        // to the frame.
        context.covered = box;

        if (context.prep == FramePrep::Stretch) {
            // Fill the input, distortion and all. Some recognisers are trained
            // that way and read worse on a letterbox.
            return ops.value()->crop(context.source, box, out);
        }

        // Letterbox: scale to fit and pad. The content rect goes back to the
        // runner so child boxes are mapped out of the padding rather than
        // through it.
        core::OwnedImage cut(context.source.format, core::Size{box.width, box.height});
        core::MutableImageView cutView = cut.view();
        const core::Status cropped = ops.value()->crop(context.source, box, cutView);
        if (!cropped.ok()) return cropped;

        auto content = ops.value()->fit(cutView.readable(), out,
                                        context.prep == FramePrep::FitHeight
                                            ? core::FitMode::FitHeight
                                            : core::FitMode::Letterbox,
                                        114);
        if (!content) return content.error();
        context.contentRect = content.value();
        return {};
    }

private:
    // How much of the box's own size to add around it. A quarter is what the
    // predecessor settled on across detectors.
    static constexpr float kContextFraction = 0.25f;

    static core::Rect expandBox(const core::Rect& box, float fraction) {
        const int dx = static_cast<int>(static_cast<float>(box.width) * fraction * 0.5f);
        const int dy = static_cast<int>(static_cast<float>(box.height) * fraction * 0.5f);
        return core::Rect{box.x - dx, box.y - dy, box.width + 2 * dx, box.height + 2 * dy};
    }
};

const core::Register<Transform> registration({
    "",
    /*priority=*/0,
    [] { return core::Probe::yes("crop the detection out of the frame"); },
    [] { return std::unique_ptr<Transform>(new CropTransform()); },
});

}  // namespace
}  // namespace visora::vision
