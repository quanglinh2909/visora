// Five-point face alignment: rotate and scale the face upright before the
// embedding model sees it.
//
// A recogniser is trained on faces in ONE canonical pose — eyes on a fixed
// horizontal line, mouth at a fixed height — and feeding it a tilted face costs
// accuracy in a way more training data does not fix: the embedding is meant to
// encode identity, and any pose left in the crop leaks into it instead. So the
// eyes, nose and mouth corners the detector already found are used to solve the
// rotation, scale and shift that put them where the model expects, and the crop
// is resampled through that.
//
// A SIMILARITY, not a general affine: four free parameters (one rotation, one
// uniform scale, two translations) fitted to ten measurements. An affine has
// six and would fit the five landmarks more closely by stretching the face to
// do it — which changes the very proportions the recogniser identifies people
// by. Being unable to stretch is the point.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/Image.hpp"
#include "core/ImageMath.hpp"
#include "hal/ImageOps.hpp"
#include "vision/Transform.hpp"

namespace visora::vision {
namespace {

constexpr int kLandmarks = 5;

// ArcFace's canonical landmark positions — left eye, right eye, nose tip, left
// mouth corner, right mouth corner — quoted for a 96x112 crop. Every recogniser
// in this family (ArcFace, AdaFace, CosFace) was trained on crops built from
// these exact numbers, so they are part of the model's contract rather than
// something to tune.
constexpr float kReferenceX[kLandmarks] = {30.2946f, 65.5318f, 48.0252f, 33.5493f, 62.7299f};
constexpr float kReferenceY[kLandmarks] = {51.6963f, 51.5014f, 71.7366f, 92.3655f, 92.2041f};
constexpr float kReferenceWidth = 96.0f;
constexpr float kReferenceHeight = 112.0f;

// How much to grow the face box before cutting it. The canonical square reaches
// past a tight face box — forehead and chin especially — and any pixel it wants
// that was not cut comes out black. Margin costs one larger crop and buys real
// pixels there.
constexpr float kMargin = 0.35f;

// The transform, held as destination -> source.
//
// That is the direction resampling actually walks: for each output pixel, ask
// where it came from. The forward matrix would have to be inverted again before
// it could be used, and solving directly in this direction means there is no
// 3x3 inversion in the file at all.
struct InverseMap {
    float sc = 1.0f;
    float ss = 0.0f;
    float tx = 0.0f;
    float ty = 0.0f;

    float sourceX(float x, float y) const { return sc * x + ss * y + tx; }
    float sourceY(float x, float y) const { return -ss * x + sc * y + ty; }
};

// Gauss-Jordan with partial pivoting on a 4x5 augmented matrix. Four unknowns
// is well under the size where a matrix library would earn its dependency.
bool solve4(double m[4][5], double out[4]) {
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 4; ++row) {
            if (std::fabs(m[row][col]) > std::fabs(m[pivot][col])) pivot = row;
        }
        // Singular: the landmarks are coincident or collinear, which is what a
        // detector reports for a face it barely found.
        if (std::fabs(m[pivot][col]) < 1e-9) return false;
        if (pivot != col) {
            for (int j = col; j < 5; ++j) std::swap(m[pivot][j], m[col][j]);
        }
        const double diag = m[col][col];
        for (int j = col; j < 5; ++j) m[col][j] /= diag;
        for (int row = 0; row < 4; ++row) {
            if (row == col) continue;
            const double factor = m[row][col];
            if (factor == 0.0) continue;
            for (int j = col; j < 5; ++j) m[row][j] -= factor * m[col][j];
        }
    }
    for (int i = 0; i < 4; ++i) out[i] = m[i][4];
    return true;
}

// Least-squares fit of the similarity that carries the REFERENCE points onto
// the DETECTED ones — the destination-to-source direction, solved directly.
//
// Each landmark contributes two equations in (sc, ss, tx, ty):
//     srcX = sc*refX + ss*refY + tx
//     srcY = sc*refY - ss*refX + ty
// Ten equations, four unknowns, so this accumulates the normal equations and
// solves those.
bool fitSimilarity(const float srcX[kLandmarks], const float srcY[kLandmarks],
                   const float refX[kLandmarks], const float refY[kLandmarks],
                   InverseMap& out) {
    double normal[4][5] = {};  // A^T A, augmented with A^T b in the last column
    for (int i = 0; i < kLandmarks; ++i) {
        const double x = refX[i];
        const double y = refY[i];
        const double rowX[4] = {x, y, 1.0, 0.0};
        const double rowY[4] = {y, -x, 0.0, 1.0};
        for (int r = 0; r < 4; ++r) {
            normal[r][4] += rowX[r] * srcX[i] + rowY[r] * srcY[i];
            for (int c = 0; c < 4; ++c) {
                normal[r][c] += rowX[r] * rowX[c] + rowY[r] * rowY[c];
            }
        }
    }

    double solved[4];
    if (!solve4(normal, solved)) return false;
    // sc and ss together are the scale: both near zero means the fit collapsed
    // the face to a point, and warping through it would produce one colour.
    if (std::fabs(solved[0]) < 1e-6 && std::fabs(solved[1]) < 1e-6) return false;

    out.sc = static_cast<float>(solved[0]);
    out.ss = static_cast<float>(solved[1]);
    out.tx = static_cast<float>(solved[2]);
    out.ty = static_cast<float>(solved[3]);
    return true;
}

// Where the canonical landmarks sit inside THIS model's input.
//
// The published numbers describe a 96x112 crop. A square input — 112x112, what
// essentially every current export uses — centres them horizontally, which is
// the +8 the reference implementations all carry. Anything else is scaled from
// whichever of the two layouts matches its shape, so a 160x160 model gets a
// face filling its input instead of one parked in a corner.
void referencePoints(core::Size target, float outX[kLandmarks], float outY[kLandmarks]) {
    const bool square = target.width == target.height;
    const float canonicalWidth = square ? kReferenceHeight : kReferenceWidth;
    const float shift = square ? (kReferenceHeight - kReferenceWidth) * 0.5f : 0.0f;
    const float scaleX = static_cast<float>(target.width) / canonicalWidth;
    const float scaleY = static_cast<float>(target.height) / kReferenceHeight;
    for (int i = 0; i < kLandmarks; ++i) {
        outX[i] = (kReferenceX[i] + shift) * scaleX;
        outY[i] = kReferenceY[i] * scaleY;
    }
}

// Bilinear sample of a packed RGB image. Leaves `out` untouched outside the
// image, so the caller's zero shows through as black — the same edge behaviour
// as a constant-border warp.
void sample(const std::uint8_t* src, int stride, core::Size size, float x, float y,
            std::uint8_t out[3]) {
    if (x < 0.0f || y < 0.0f || x > static_cast<float>(size.width - 1) ||
        y > static_cast<float>(size.height - 1)) {
        return;
    }
    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, size.width - 1);
    const int y1 = std::min(y0 + 1, size.height - 1);
    const float dx = x - static_cast<float>(x0);
    const float dy = y - static_cast<float>(y0);

    const std::uint8_t* row0 = src + static_cast<std::size_t>(y0) * stride;
    const std::uint8_t* row1 = src + static_cast<std::size_t>(y1) * stride;
    for (int c = 0; c < 3; ++c) {
        const float a = static_cast<float>(row0[x0 * 3 + c]);
        const float b = static_cast<float>(row0[x1 * 3 + c]);
        const float d = static_cast<float>(row1[x0 * 3 + c]);
        const float e = static_cast<float>(row1[x1 * 3 + c]);
        const float top = a + (b - a) * dx;
        const float bottom = d + (e - d) * dx;
        out[c] = static_cast<std::uint8_t>(std::lround(top + (bottom - top) * dy));
    }
}

void warpInto(const core::ImageView& src, const InverseMap& map, std::uint8_t* dst,
              int dstStride, core::Size dstSize, bool swapRedBlue) {
    const int srcStride = src.planes[0].stride;
    for (int y = 0; y < dstSize.height; ++y) {
        std::uint8_t* row = dst + static_cast<std::size_t>(y) * dstStride;
        const float fy = static_cast<float>(y);
        for (int x = 0; x < dstSize.width; ++x) {
            const float fx = static_cast<float>(x);
            std::uint8_t rgb[3] = {0, 0, 0};
            sample(src.data, srcStride, src.size, map.sourceX(fx, fy), map.sourceY(fx, fy), rgb);
            std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 3;
            pixel[0] = swapRedBlue ? rgb[2] : rgb[0];
            pixel[1] = rgb[1];
            pixel[2] = swapRedBlue ? rgb[0] : rgb[2];
        }
    }
}

class FaceAlignTransform final : public Transform {
public:
    std::string_view id() const override { return "align_face"; }
    std::string_view label() const override { return "Face alignment"; }
    std::string_view description() const override {
        return "Rotates the face upright by its five landmarks before the embedding model.";
    }

    core::Status apply(TransformContext& context,
                       core::MutableImageView& out) const override {
        if (!context.source.valid()) return core::invalidArgument("no source frame");
        if (!out.valid()) return core::invalidArgument("no destination image");
        const core::Size target = out.size;

        // No landmarks is NOT an error. A detector that found a face but no
        // keypoints — a plain box detector used by mistake, or a face too small
        // to resolve joints on — simply gives this stage nothing to align, and
        // the other faces in the frame still go through.
        if (context.keypoints == nullptr ||
            context.keypoints->size() < static_cast<std::size_t>(kLandmarks) * 3) {
            return core::unsupported("the detection carries no facial landmarks");
        }

        core::Rect region = grow(context.box, kMargin);
        region = core::clamp(region, context.source.size);
        if (region.width <= 1 || region.height <= 1) {
            return core::unsupported("the face box has no area inside the frame");
        }
        // Small faces are the common case at any useful camera distance, and a
        // fixed-function scaler refuses crops below its minimum. Growing here
        // costs nothing: the warp reads by landmark position, so extra margin
        // only turns pixels that would have been black into real ones.
        region = core::expandToMin(region, context.source.size, kMinCropSide);

        auto ops = hal::imageOps();
        if (!ops) return ops.error();

        core::OwnedImage cut(core::PixelFormat::RGB888,
                             core::Size{region.width, region.height});
        core::MutableImageView cutView = cut.view();
        const core::Status cropped = ops.value()->crop(context.source, region, cutView);
        if (!cropped.ok()) return cropped;

        float srcX[kLandmarks];
        float srcY[kLandmarks];
        for (int i = 0; i < kLandmarks; ++i) {
            srcX[i] = (*context.keypoints)[static_cast<std::size_t>(i) * 3 + 0] -
                      static_cast<float>(region.x);
            srcY[i] = (*context.keypoints)[static_cast<std::size_t>(i) * 3 + 1] -
                      static_cast<float>(region.y);
        }

        float refX[kLandmarks];
        float refY[kLandmarks];
        referencePoints(target, refX, refY);

        InverseMap map;
        if (!fitSimilarity(srcX, srcY, refX, refY, map)) {
            return core::unsupported("the facial landmarks are degenerate");
        }

        // The warped face fills the output, so a later stage maps its boxes
        // through the whole of it. `covered` stays empty on purpose: this is a
        // rotation, not a rectangle of the frame, and the runner's documented
        // fallback (the parent's box) is the closest honest answer.
        context.contentRect = core::Rect{0, 0, target.width, target.height};

        if (out.hasCpu() && (out.format == core::PixelFormat::RGB888 ||
                             out.format == core::PixelFormat::BGR888)) {
            warpInto(cutView.readable(), map, out.data, out.planes[0].stride, target,
                     out.format == core::PixelFormat::BGR888);
            return {};
        }

        // Any other input format — a model wanting NV12, a dmabuf-only
        // destination — goes through a packed buffer the image layer can
        // convert from. Rare enough not to optimise, wrong to not support.
        core::OwnedImage warped(core::PixelFormat::RGB888, target);
        core::MutableImageView warpedView = warped.view();
        warpInto(cutView.readable(), map, warpedView.data, warpedView.planes[0].stride, target,
                 false);
        return ops.value()->convert(warped.view(), out);
    }

private:
    // The smallest crop side to hand the image layer. 128 is what core's own
    // default settled on across the scalers this runs against.
    static constexpr int kMinCropSide = 128;

    static core::Rect grow(const core::Rect& box, float fraction) {
        const int dx = static_cast<int>(static_cast<float>(box.width) * fraction * 0.5f);
        const int dy = static_cast<int>(static_cast<float>(box.height) * fraction * 0.5f);
        return core::Rect{box.x - dx, box.y - dy, box.width + 2 * dx, box.height + 2 * dy};
    }
};

const core::Register<Transform> registration({
    "align_face",
    /*priority=*/0,
    [] { return core::Probe::yes("align the face by its five landmarks"); },
    [] { return std::unique_ptr<Transform>(new FaceAlignTransform()); },
});

}  // namespace
}  // namespace visora::vision
