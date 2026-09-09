// Software implementation of every pixel operation, on OpenCV.
//
// This is the floor the whole system stands on: it is always compiled, always
// available, and every accelerated backend is measured against it. A machine
// with no 2D engine and no NPU still runs the full pipeline through here, just
// slower — which is what makes the project developable on a laptop.
//
// Priority 0 on purpose: any hardware backend that probes successfully wins.

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/Log.hpp"
#include "hal/ImageOps.hpp"

namespace visora::hal {
namespace {

using core::ImageView;
using core::MutableImageView;
using core::PixelFormat;
using core::Rect;
using core::Size;

constexpr const char* kCategory = "cpu-imageops";

// Copies the ROI of an NV12 image into a packed NV12 buffer laid out the way
// OpenCV wants it: a single (h * 3/2) x w single-channel image.
//
// Arbitrary strides are the normal case, not the exception — decoders pad rows
// and put the chroma plane at whatever offset suits their hardware.
bool packNv12Roi(const ImageView& src, Rect roi, std::vector<std::uint8_t>& out) {
    const int yStride = src.planes[0].stride > 0 ? src.planes[0].stride : src.size.width;
    const int uvStride = src.planes[1].stride > 0 ? src.planes[1].stride : src.size.width;
    const std::size_t uvOffset =
        src.planes[1].offset > 0
            ? src.planes[1].offset
            : static_cast<std::size_t>(yStride) * static_cast<std::size_t>(src.size.height);

    const int width = roi.width;
    const int height = roi.height;
    out.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3 / 2);

    const std::uint8_t* base = src.data;
    std::uint8_t* dst = out.data();

    for (int row = 0; row < height; ++row) {
        const std::uint8_t* srcRow =
            base + static_cast<std::size_t>(roi.y + row) * static_cast<std::size_t>(yStride) +
            static_cast<std::size_t>(roi.x);
        std::memcpy(dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(width),
                    srcRow, static_cast<std::size_t>(width));
    }

    std::uint8_t* dstUv = dst + static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (int row = 0; row < height / 2; ++row) {
        const std::uint8_t* srcRow =
            base + uvOffset +
            static_cast<std::size_t>(roi.y / 2 + row) * static_cast<std::size_t>(uvStride) +
            static_cast<std::size_t>(roi.x);  // UV is interleaved: x maps 1:1
        std::memcpy(dstUv + static_cast<std::size_t>(row) * static_cast<std::size_t>(width),
                    srcRow, static_cast<std::size_t>(width));
    }
    return true;
}

// Reads `roi` out of `src` as BGR — the single intermediate every conversion
// goes through, so N source formats and M destination formats cost N + M paths
// rather than N * M.
core::Result<cv::Mat> readBgr(const ImageView& src, Rect roi) {
    if (!src.hasCpu()) {
        return core::unsupported("cpu image ops need a mapped pointer; got " +
                                 core::describe(src));
    }

    switch (src.format) {
        case PixelFormat::NV12: {
            // Chroma is subsampled 2x2, so an odd origin or extent is not
            // representable. Round rather than reject: callers derive these
            // from detection boxes and cannot be expected to align them.
            Rect even{core::alignDown2(roi.x), core::alignDown2(roi.y),
                      core::alignDown2(roi.width), core::alignDown2(roi.height)};
            even = core::clamp(even, src.size);
            even.width = core::alignDown2(even.width);
            even.height = core::alignDown2(even.height);
            if (!even.valid()) {
                return core::invalidArgument("empty NV12 crop after even-alignment");
            }

            std::vector<std::uint8_t> packed;
            packNv12Roi(src, even, packed);

            const cv::Mat nv12(even.height * 3 / 2, even.width, CV_8UC1, packed.data());
            cv::Mat bgr;
            cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);
            return bgr;  // owns its pixels; `packed` may die
        }

        case PixelFormat::RGB888:
        case PixelFormat::BGR888: {
            const Rect clamped = core::clamp(roi, src.size);
            if (!clamped.valid()) return core::invalidArgument("empty crop");
            const int stride = src.planes[0].stride > 0 ? src.planes[0].stride
                                                        : src.size.width * 3;
            const cv::Mat whole(src.size.height, src.size.width, CV_8UC3,
                                const_cast<std::uint8_t*>(src.data),
                                static_cast<std::size_t>(stride));
            cv::Mat bgr = whole(cv::Rect(clamped.x, clamped.y, clamped.width, clamped.height)).clone();
            if (src.format == PixelFormat::RGB888) cv::cvtColor(bgr, bgr, cv::COLOR_RGB2BGR);
            return bgr;
        }

        case PixelFormat::GRAY8: {
            const Rect clamped = core::clamp(roi, src.size);
            if (!clamped.valid()) return core::invalidArgument("empty crop");
            const int stride = src.planes[0].stride > 0 ? src.planes[0].stride : src.size.width;
            const cv::Mat whole(src.size.height, src.size.width, CV_8UC1,
                                const_cast<std::uint8_t*>(src.data),
                                static_cast<std::size_t>(stride));
            cv::Mat bgr;
            cv::cvtColor(whole(cv::Rect(clamped.x, clamped.y, clamped.width, clamped.height)),
                         bgr, cv::COLOR_GRAY2BGR);
            return bgr;
        }

        case PixelFormat::Unknown:
            break;
    }
    return core::unsupported(std::string("cpu image ops cannot read ") +
                             core::toString(src.format));
}

// Writes a BGR image into `dst` at `where`, converting to the destination
// format. `where` must already fit inside dst.
core::Status writeBgr(const cv::Mat& bgr, const MutableImageView& dst, Rect where) {
    if (!dst.hasCpu()) return core::unsupported("cpu image ops need a writable pointer");

    switch (dst.format) {
        case PixelFormat::RGB888:
        case PixelFormat::BGR888: {
            const int stride = dst.planes[0].stride > 0 ? dst.planes[0].stride
                                                        : dst.size.width * 3;
            cv::Mat whole(dst.size.height, dst.size.width, CV_8UC3, dst.data,
                          static_cast<std::size_t>(stride));
            cv::Mat target = whole(cv::Rect(where.x, where.y, where.width, where.height));
            if (dst.format == PixelFormat::RGB888) {
                cv::cvtColor(bgr, target, cv::COLOR_BGR2RGB);
            } else {
                bgr.copyTo(target);
            }
            return {};
        }

        case PixelFormat::GRAY8: {
            const int stride = dst.planes[0].stride > 0 ? dst.planes[0].stride : dst.size.width;
            cv::Mat whole(dst.size.height, dst.size.width, CV_8UC1, dst.data,
                          static_cast<std::size_t>(stride));
            cv::Mat target = whole(cv::Rect(where.x, where.y, where.width, where.height));
            cv::cvtColor(bgr, target, cv::COLOR_BGR2GRAY);
            return {};
        }

        case PixelFormat::NV12: {
            // OpenCV converts to planar I420; NV12 wants the two chroma planes
            // interleaved, so that last step is ours.
            if ((where.x | where.y | where.width | where.height) & 1) {
                return core::invalidArgument("NV12 destination rect must be even-aligned");
            }
            cv::Mat i420;
            cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);

            const int w = where.width;
            const int h = where.height;
            const int yStride = dst.planes[0].stride > 0 ? dst.planes[0].stride : dst.size.width;
            const int uvStride = dst.planes[1].stride > 0 ? dst.planes[1].stride : dst.size.width;
            const std::size_t uvOffset =
                dst.planes[1].offset > 0
                    ? dst.planes[1].offset
                    : static_cast<std::size_t>(yStride) *
                          static_cast<std::size_t>(dst.size.height);

            for (int row = 0; row < h; ++row) {
                std::memcpy(dst.data +
                                static_cast<std::size_t>(where.y + row) *
                                    static_cast<std::size_t>(yStride) +
                                static_cast<std::size_t>(where.x),
                            i420.ptr<std::uint8_t>(row), static_cast<std::size_t>(w));
            }

            const std::uint8_t* uPlane = i420.ptr<std::uint8_t>(h);
            const std::uint8_t* vPlane = uPlane + static_cast<std::size_t>(w / 2) *
                                                      static_cast<std::size_t>(h / 2);
            for (int row = 0; row < h / 2; ++row) {
                std::uint8_t* out = dst.data + uvOffset +
                                    static_cast<std::size_t>(where.y / 2 + row) *
                                        static_cast<std::size_t>(uvStride) +
                                    static_cast<std::size_t>(where.x);
                for (int col = 0; col < w / 2; ++col) {
                    out[col * 2] = uPlane[static_cast<std::size_t>(row) *
                                              static_cast<std::size_t>(w / 2) + col];
                    out[col * 2 + 1] = vPlane[static_cast<std::size_t>(row) *
                                                  static_cast<std::size_t>(w / 2) + col];
                }
            }
            return {};
        }

        case PixelFormat::Unknown:
            break;
    }
    return core::unsupported(std::string("cpu image ops cannot write ") +
                             core::toString(dst.format));
}

// Paints the whole destination with the padding value, so `fit` only has to
// draw the content and the bars take care of themselves.
void paint(const MutableImageView& dst, std::uint8_t value) {
    const std::size_t bytes = core::packedSize(dst.format, dst.size);
    if (dst.format == PixelFormat::NV12) {
        const int yStride = dst.planes[0].stride > 0 ? dst.planes[0].stride : dst.size.width;
        const std::size_t uvOffset =
            dst.planes[1].offset > 0
                ? dst.planes[1].offset
                : static_cast<std::size_t>(yStride) * static_cast<std::size_t>(dst.size.height);
        // Luma gets the pad value; chroma gets neutral grey, or the bars come
        // out tinted.
        std::memset(dst.data, value, uvOffset);
        std::memset(dst.data + uvOffset, 128, bytes > uvOffset ? bytes - uvOffset : 0);
        return;
    }
    std::memset(dst.data, value, bytes);
}

class CpuImageOps final : public ImageOps {
public:
    std::string_view id() const override { return "cpu"; }

    core::Result<Rect> fit(const ImageView& src, const MutableImageView& dst,
                           core::FitMode mode, std::uint8_t padValue) override {
        if (!src.valid() || !dst.valid()) {
            return core::invalidArgument("fit: invalid source or destination");
        }

        const Rect content = core::fitContentRect(mode, src.size, dst.size);
        if (!content.valid()) {
            return core::invalidArgument("fit: degenerate content rect");
        }

        paint(dst, padValue);

        auto bgr = readBgr(src, Rect{0, 0, src.size.width, src.size.height});
        if (!bgr) return bgr.error();

        cv::Mat scaled;
        cv::resize(bgr.value(), scaled, cv::Size(content.width, content.height), 0, 0,
                   cv::INTER_LINEAR);

        const core::Status written = writeBgr(scaled, dst, content);
        if (!written) return written.error();

        VS_TRACE(kCategory) << "fit " << core::describe(src) << " -> "
                            << core::toString(dst.format) << ' ' << dst.size.width << 'x'
                            << dst.size.height << " (" << core::toString(mode) << ')';
        return content;
    }

    core::Status crop(const ImageView& src, Rect roi,
                      const MutableImageView& dst) override {
        if (!src.valid() || !dst.valid()) {
            return core::invalidArgument("crop: invalid source or destination");
        }

        auto bgr = readBgr(src, roi);
        if (!bgr) return bgr.error();

        cv::Mat scaled;
        cv::resize(bgr.value(), scaled, cv::Size(dst.size.width, dst.size.height), 0, 0,
                   cv::INTER_LINEAR);

        return writeBgr(scaled, dst, Rect{0, 0, dst.size.width, dst.size.height});
    }
};

Probe probeCpu() {
    return Probe::yes("OpenCV software path (always available)");
}

const Register<ImageOps> registration{{
    "cpu",
    0,
    &probeCpu,
    [] { return std::unique_ptr<ImageOps>(new CpuImageOps()); },
}};

}  // namespace
}  // namespace visora::hal
