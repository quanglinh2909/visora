// Rockchip RGA 2D engine as an ImageOps backend.
//
// Buffer hand-off rules, learned the hard way on RK3588 and carried over
// deliberately (see DmaHeapBuffer.hpp for the kernel-oops detail):
//
//   * a decoder frame that came as a dmabuf is given to RGA BY HANDLE, never by
//     mapped pointer — get_user_pages on a dmabuf mmap intermittently feeds the
//     driver a bogus page and oopses the kernel;
//   * a job runs either fully in handle mode or fully in raw virtual-address
//     mode; mixing one imported handle with one raw pointer is rejected;
//   * a CPU-read destination in handle mode must live in dma-heap memory and be
//     cache-invalidated after the blit, or the CPU reads stale lines;
//   * a synthetic image with no dmabuf keeps the virtual-address path on both
//     sides, which is what that path has always handled safely.
//
// Two hardware limits are worked around here rather than pushed onto callers:
//
//   * RGB destinations need a 16-aligned pixel stride. Handled with a
//     stride-aligned scratch and a row copy — and skipped entirely when the
//     destination width is already a multiple of 16, which most model inputs
//     are (640, 512, 320, 128).
//   * A single pass scales by at most 16x. A 30 px plate into a 512 px model
//     input is 17x and would fail. Rather than growing the crop — which changes
//     what a tight-crop model sees and costs accuracy — the blit is split into
//     several passes (30 -> 480 -> 512), each within the limit. The result is
//     the tight crop, upscaled, still entirely on hardware.
//
// Anything still refused is reported as Unsupported or HardwareFailure, and the
// chain in hal/ImageOps.cpp finishes the job in software.

#include <rga/im2d.h>
#include <rga/rga.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/Log.hpp"
#include "hal/ImageOps.hpp"
#include "hal/rockchip/DmaHeapBuffer.hpp"
#include "hal/rockchip/RgaLock.hpp"

namespace visora::hal::rockchip {
namespace {

using core::ImageView;
using core::MutableImageView;
using core::PixelFormat;
using core::Rect;
using core::Size;

constexpr const char* kCategory = "rga";

// A single RGA pass scales by at most this much in either direction. Kept a
// little under the documented 16 so a rounding error cannot land on the edge.
constexpr double kMaxScalePerPass = 15.5;

// RGB destinations need their pixel stride 16-aligned.
constexpr int kRgbStrideAlign = 16;

int alignUp(int value, int to) { return (value + to - 1) / to * to; }

// --- format mapping ----------------------------------------------------------

bool toRgaFormat(PixelFormat format, int* out) {
    switch (format) {
        case PixelFormat::NV12:   *out = RK_FORMAT_YCbCr_420_SP; return true;
        case PixelFormat::RGB888: *out = RK_FORMAT_RGB_888;      return true;
        case PixelFormat::BGR888: *out = RK_FORMAT_BGR_888;      return true;
        // GRAY8 has no clean single-plane equivalent that every RGA revision
        // agrees on. Declining is correct: the software path handles it, and a
        // wrong guess here produces silently mangled pixels.
        case PixelFormat::GRAY8:
        case PixelFormat::Unknown:
            break;
    }
    return false;
}

int yStrideOf(const ImageView& image) {
    return image.planes[0].stride > 0 ? image.planes[0].stride : image.size.width;
}

// RGA describes a buffer by width/height stride, so an NV12 frame whose chroma
// plane sits at a custom offset has to be expressed as a taller Y plane.
int hStrideOf(const ImageView& image) {
    if (image.format != PixelFormat::NV12) return image.size.height;
    const int ws = yStrideOf(image);
    const std::size_t uvOffset = image.planes[1].offset;
    if (uvOffset > 0 && ws > 0) return static_cast<int>(uvOffset / static_cast<std::size_t>(ws));
    return image.size.height;
}

int strideOfMutable(const MutableImageView& image) {
    if (image.planes[0].stride > 0) {
        return image.format == PixelFormat::NV12 ? image.planes[0].stride
                                                 : image.planes[0].stride / 3;
    }
    return image.size.width;
}

// --- low-level blit ----------------------------------------------------------

im_rect makeRect(Rect r) {
    im_rect out;
    out.x = r.x;
    out.y = r.y;
    out.width = r.width;
    out.height = r.height;
    return out;
}

rga_buffer_t emptyBuffer() {
    rga_buffer_t buffer;
    std::memset(&buffer, 0, sizeof(buffer));
    return buffer;
}

IM_STATUS runBlit(const rga_buffer_t& src, const rga_buffer_t& dst,
                  const im_rect& srect, const im_rect& drect) {
    rga_buffer_t pattern = emptyBuffer();
    im_rect prect = makeRect(Rect{0, 0, 0, 0});
    std::lock_guard<std::mutex> lock(rgaMutex());
    return improcess(src, dst, pattern, srect, drect, prect, 0, nullptr, nullptr, IM_SYNC);
}

// Describes the source for RGA, in whichever mode the image allows.
bool describeSource(const ImageView& image, int rgaFormat, rga_buffer_t* out) {
    if (image.hasNativeHandle()) {
        *out = wrapbuffer_handle(static_cast<rga_buffer_handle_t>(image.nativeHandle),
                                 image.size.width, image.size.height, rgaFormat,
                                 yStrideOf(image), hStrideOf(image));
        return true;
    }
    if (image.hasCpu()) {
        *out = wrapbuffer_virtualaddr(const_cast<std::uint8_t*>(image.data), image.size.width,
                                      image.size.height, rgaFormat, yStrideOf(image),
                                      hStrideOf(image));
        return true;
    }
    return false;
}

// How many passes it takes to get from `from` to `to` without exceeding the
// per-pass scale limit in either direction.
int passesNeeded(Size from, Size to) {
    const double ratioX = std::max(static_cast<double>(to.width) / from.width,
                                   static_cast<double>(from.width) / to.width);
    const double ratioY = std::max(static_cast<double>(to.height) / from.height,
                                   static_cast<double>(from.height) / to.height);
    const double worst = std::max(ratioX, ratioY);
    if (worst <= kMaxScalePerPass) return 1;
    // Three passes cover 3700x, far beyond anything a camera pipeline asks for.
    const int needed = static_cast<int>(std::ceil(std::log(worst) / std::log(kMaxScalePerPass)));
    return std::min(needed, 3);
}

// The size to aim for on pass `index` of `total`, geometrically interpolated so
// no single pass exceeds the limit.
Size intermediateSize(Size from, Size to, int index, int total) {
    if (index >= total - 1) return to;
    const double t = static_cast<double>(index + 1) / total;
    const auto scale = [t](int a, int b) {
        const double value = std::pow(static_cast<double>(b) / a, t) * a;
        return std::max(2, core::alignDown2(static_cast<int>(std::lround(value))));
    };
    return {scale(from.width, to.width), scale(from.height, to.height)};
}

// --- the backend -------------------------------------------------------------

class RgaImageOps final : public ImageOps {
public:
    std::string_view id() const override { return "rga"; }
    bool supportsZeroCopy() const override { return true; }

    core::Result<NativeHandle> import(const ImageView& image) override {
        if (!image.hasDmaBuf()) {
            return core::unsupported("rga import needs a dmabuf fd");
        }
        int rgaFormat = 0;
        if (!toRgaFormat(image.format, &rgaFormat)) {
            return core::unsupported(std::string("rga cannot import ") +
                                     core::toString(image.format));
        }

        im_handle_param_t param;
        param.width = static_cast<std::uint32_t>(yStrideOf(image));
        param.height = static_cast<std::uint32_t>(hStrideOf(image));
        param.format = static_cast<std::uint32_t>(rgaFormat);

        rga_buffer_handle_t handle = 0;
        {
            std::lock_guard<std::mutex> lock(rgaMutex());
            handle = importbuffer_fd(image.dmaFd, &param);
        }
        if (handle == 0) {
            return core::hardwareFailure("importbuffer_fd failed for fd " +
                                         std::to_string(image.dmaFd));
        }
        return NativeHandle(static_cast<std::uint64_t>(handle), &releaseHandle);
    }

    core::Result<Rect> fit(const ImageView& src, const MutableImageView& dst,
                           core::FitMode mode, std::uint8_t padValue) override {
        if (!src.valid() || !dst.valid()) {
            return core::invalidArgument("fit: invalid source or destination");
        }
        const Rect content = core::fitContentRect(mode, src.size, dst.size);
        if (!content.valid()) return core::invalidArgument("fit: degenerate content rect");

        // The bars are painted by the CPU because RGA would need a second fill
        // job for them; a memset of the destination is cheaper than that.
        paintPadding(dst, padValue);

        const core::Status blitted =
            scaleInto(src, Rect{0, 0, src.size.width, src.size.height}, dst, content);
        if (!blitted.ok()) return blitted.error();
        return content;
    }

    core::Status crop(const ImageView& src, Rect roi,
                      const MutableImageView& dst) override {
        if (!src.valid() || !dst.valid()) {
            return core::invalidArgument("crop: invalid source or destination");
        }

        Rect even{std::max(0, core::alignDown2(roi.x)), std::max(0, core::alignDown2(roi.y)),
                  roi.width, roi.height};
        even.width = core::alignUp2(std::min(roi.width, src.size.width - even.x));
        even.height = core::alignUp2(std::min(roi.height, src.size.height - even.y));
        even = core::clamp(even, src.size);
        even.width = core::alignDown2(even.width);
        even.height = core::alignDown2(even.height);
        if (!even.valid()) return core::invalidArgument("crop: empty region after alignment");

        return scaleInto(src, even, dst, Rect{0, 0, dst.size.width, dst.size.height});
    }

private:
    static void releaseHandle(std::uint64_t handle) {
        std::lock_guard<std::mutex> lock(rgaMutex());
        releasebuffer_handle(static_cast<rga_buffer_handle_t>(handle));
    }

    static void paintPadding(const MutableImageView& dst, std::uint8_t value) {
        const std::size_t bytes = core::packedSize(dst.format, dst.size);
        if (dst.format == PixelFormat::NV12) {
            const std::size_t uvOffset =
                dst.planes[1].offset > 0
                    ? dst.planes[1].offset
                    : static_cast<std::size_t>(dst.planes[0].stride) *
                          static_cast<std::size_t>(dst.size.height);
            std::memset(dst.data, value, uvOffset);
            // Neutral chroma, or the bars come out tinted.
            if (bytes > uvOffset) std::memset(dst.data + uvOffset, 128, bytes - uvOffset);
            return;
        }
        std::memset(dst.data, value, bytes);
    }

    // Scales `srcRect` of `src` into `dstRect` of `dst`, in as many passes as the
    // hardware scale limit requires.
    core::Status scaleInto(const ImageView& src, Rect srcRect,
                           const MutableImageView& dst, Rect dstRect) {
        int srcFormat = 0;
        int dstFormat = 0;
        if (!toRgaFormat(src.format, &srcFormat)) {
            return core::unsupported(std::string("rga cannot read ") +
                                     core::toString(src.format));
        }
        if (!toRgaFormat(dst.format, &dstFormat)) {
            return core::unsupported(std::string("rga cannot write ") +
                                     core::toString(dst.format));
        }
        if (!dst.hasCpu()) {
            return core::unsupported("rga destination needs a mapped pointer");
        }
        if (!src.hasNativeHandle() && !src.hasCpu()) {
            return core::unsupported("rga source needs a handle or a mapped pointer");
        }

        const int passes = passesNeeded(srcRect.size(), dstRect.size());
        if (passes == 1) {
            return blitOnce(src, srcFormat, srcRect, dst, dstFormat, dstRect);
        }

        VS_DEBUG(kCategory) << "scaling " << srcRect.width << 'x' << srcRect.height << " -> "
                            << dstRect.width << 'x' << dstRect.height << " in " << passes
                            << " passes (per-pass limit " << kMaxScalePerPass << "x)";

        // Intermediates carry the destination format so the colour conversion
        // happens once, on the first pass.
        ImageView current = src;
        Rect currentRect = srcRect;
        int currentFormat = srcFormat;

        for (int pass = 0; pass < passes - 1; ++pass) {
            const Size target = intermediateSize(srcRect.size(), dstRect.size(), pass, passes);
            DmaHeapBuffer& scratch = passScratch(pass);
            const core::Status step = blitToScratch(current, currentFormat, currentRect,
                                                    dstFormat, target, scratch);
            if (!step.ok()) return step;

            current = scratchView(scratch, dst.format, target);
            currentRect = Rect{0, 0, target.width, target.height};
            currentFormat = dstFormat;
        }

        return blitOnce(current, currentFormat, currentRect, dst, dstFormat, dstRect);
    }

    // A blit whose destination is the caller's buffer. Uses a stride-aligned
    // dma-heap scratch when the destination stride or the source mode requires
    // it, then copies the rows back.
    core::Status blitOnce(const ImageView& src, int srcFormat, Rect srcRect,
                          const MutableImageView& dst, int dstFormat, Rect dstRect) {
        const bool handleMode = src.hasNativeHandle();
        const int dstPixelStride = strideOfMutable(dst);
        const bool strideOk = dst.format == PixelFormat::NV12 ||
                              dstPixelStride % kRgbStrideAlign == 0;

        // A handle-mode source must not be paired with a raw pointer
        // destination, and a CPU-read destination in handle mode has to be
        // dma-heap memory to get its cache invalidated. Either condition sends
        // us through the scratch.
        if (handleMode || !strideOk) {
            const Size target = dstRect.size();
            DmaHeapBuffer& scratch = outputScratch();
            const core::Status blitted =
                blitToScratch(src, srcFormat, srcRect, dstFormat, target, scratch);
            if (!blitted.ok()) return blitted;
            copyOut(scratch, target, dst, dstRect);
            return {};
        }

        rga_buffer_t source;
        if (!describeSource(src, srcFormat, &source)) {
            return core::unsupported("rga source has neither handle nor pointer");
        }
        rga_buffer_t destination =
            wrapbuffer_virtualaddr(dst.data, dst.size.width, dst.size.height, dstFormat,
                                   dstPixelStride, dst.size.height);

        const IM_STATUS status = runBlit(source, destination, makeRect(srcRect), makeRect(dstRect));
        if (status != IM_STATUS_SUCCESS) {
            return core::hardwareFailure(std::string("improcess failed: ") + imStrError(status));
        }
        return {};
    }

    // A blit into a stride-aligned dma-heap scratch, which is the only kind of
    // CPU-readable destination that is safe in handle mode.
    core::Status blitToScratch(const ImageView& src, int srcFormat, Rect srcRect, int dstFormat,
                               Size target, DmaHeapBuffer& scratch) {
        const bool packed = dstFormat == RK_FORMAT_YCbCr_420_SP;
        const int wstride = packed ? target.width : alignUp(target.width, kRgbStrideAlign);
        const std::size_t bytes =
            packed ? static_cast<std::size_t>(wstride) * target.height * 3 / 2
                   : static_cast<std::size_t>(wstride) * target.height * 3;

        if (!scratch.ensure(wstride, target.height, dstFormat, bytes)) {
            return core::unsupported("dma-heap unavailable; refusing the raw-pointer path "
                                     "for a dmabuf source (known kernel-oops risk)");
        }

        rga_buffer_t source;
        if (!describeSource(src, srcFormat, &source)) {
            return core::unsupported("rga source has neither handle nor pointer");
        }
        rga_buffer_t destination =
            wrapbuffer_handle(static_cast<rga_buffer_handle_t>(scratch.handle()), target.width,
                              target.height, dstFormat, wstride, target.height);

        const IM_STATUS status = runBlit(source, destination, makeRect(srcRect),
                                         makeRect(Rect{0, 0, target.width, target.height}));
        if (status != IM_STATUS_SUCCESS) {
            return core::hardwareFailure(std::string("improcess failed: ") + imStrError(status));
        }
        scratch.syncForCpuRead();
        return {};
    }

    static ImageView scratchView(const DmaHeapBuffer& scratch, PixelFormat format, Size size) {
        ImageView view;
        view.format = format;
        view.size = size;
        view.data = scratch.data();
        if (format == PixelFormat::NV12) {
            view.planes[0] = {scratch.wstride(), 0};
            view.planes[1] = {scratch.wstride(),
                              static_cast<std::size_t>(scratch.wstride()) *
                                  static_cast<std::size_t>(size.height)};
        } else {
            view.planes[0] = {scratch.wstride() * 3, 0};
        }
        return view;
    }

    // Copies the scratch content into the caller's buffer, row by row because
    // the strides differ.
    static void copyOut(const DmaHeapBuffer& scratch, Size target,
                        const MutableImageView& dst, Rect dstRect) {
        if (dst.format == PixelFormat::NV12) {
            const int dstStride = dst.planes[0].stride > 0 ? dst.planes[0].stride : dst.size.width;
            const std::size_t dstUv =
                dst.planes[1].offset > 0
                    ? dst.planes[1].offset
                    : static_cast<std::size_t>(dstStride) * static_cast<std::size_t>(dst.size.height);
            for (int row = 0; row < target.height; ++row) {
                std::memcpy(dst.data +
                                static_cast<std::size_t>(dstRect.y + row) * dstStride + dstRect.x,
                            scratch.data() + static_cast<std::size_t>(row) * scratch.wstride(),
                            static_cast<std::size_t>(target.width));
            }
            const std::uint8_t* srcUv =
                scratch.data() + static_cast<std::size_t>(scratch.wstride()) * target.height;
            for (int row = 0; row < target.height / 2; ++row) {
                std::memcpy(dst.data + dstUv +
                                static_cast<std::size_t>(dstRect.y / 2 + row) * dstStride +
                                dstRect.x,
                            srcUv + static_cast<std::size_t>(row) * scratch.wstride(),
                            static_cast<std::size_t>(target.width));
            }
            return;
        }

        const int dstStride =
            dst.planes[0].stride > 0 ? dst.planes[0].stride : dst.size.width * 3;
        for (int row = 0; row < target.height; ++row) {
            std::memcpy(dst.data + static_cast<std::size_t>(dstRect.y + row) * dstStride +
                            static_cast<std::size_t>(dstRect.x) * 3,
                        scratch.data() +
                            static_cast<std::size_t>(row) * scratch.wstride() * 3,
                        static_cast<std::size_t>(target.width) * 3);
        }
    }

    // Scratch buffers are per-thread: several pipeline threads blit at once, and
    // sharing one would serialise them behind the RGA lock for the copy as well.
    static DmaHeapBuffer& outputScratch() {
        thread_local DmaHeapBuffer buffer;
        return buffer;
    }
    static DmaHeapBuffer& passScratch(int index) {
        thread_local DmaHeapBuffer first;
        thread_local DmaHeapBuffer second;
        return (index % 2 == 0) ? first : second;
    }
};

Probe probeRga() {
    // Ask the driver rather than trusting that the library loaded: a board can
    // have librga installed with no RGA node exposed to this container.
    const char* version = nullptr;
    {
        std::lock_guard<std::mutex> lock(rgaMutex());
        version = querystring(RGA_VERSION);
    }
    if (version == nullptr || *version == '\0') {
        return Probe::no("librga present but the RGA driver did not answer");
    }

    std::string detail(version);
    // querystring returns a multi-line block; the first line identifies it.
    const std::size_t newline = detail.find('\n');
    if (newline != std::string::npos) detail.resize(newline);
    while (!detail.empty() && (detail.back() == ' ' || detail.back() == '\r')) detail.pop_back();
    return Probe::yes("RGA 2D: " + detail);
}

const Register<ImageOps> registration{{
    "rga",
    100,
    &probeRga,
    [] { return std::unique_ptr<ImageOps>(new RgaImageOps()); },
}};

}  // namespace
}  // namespace visora::hal::rockchip
