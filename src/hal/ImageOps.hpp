#pragma once

// Every pixel operation the system performs, as three verbs.
//
// The old code had six ad-hoc functions named after the exact conversion each
// one did (letterboxNv12ToRgb, cropNv12ToRgb, cropNv12ToNv12, rgbToNv12, ...),
// which meant a new format pairing needed a new function and a new call site.
// These three are format-general: the formats live in the ImageView, so adding
// GRAY8 or I420 is a backend detail, not an interface change.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/Image.hpp"
#include "core/ImageMath.hpp"
#include "core/Result.hpp"
#include "hal/NativeHandle.hpp"
#include "hal/Registry.hpp"

namespace visora::hal {

class ImageOps {
public:
    virtual ~ImageOps() = default;

    virtual std::string_view id() const = 0;

    // Fits `src` into `dst`, converting format and scaling as needed. Padding
    // is painted with `padValue` first, so the caller does not have to.
    // Returns the content rect inside `dst` — what maps detections back out.
    virtual core::Result<core::Rect> fit(const core::ImageView& src,
                                         const core::MutableImageView& dst,
                                         core::FitMode mode,
                                         std::uint8_t padValue) = 0;

    // Crops `roi` from `src` and scales/converts it to fill `dst` exactly.
    virtual core::Status crop(const core::ImageView& src,
                              core::Rect roi,
                              const core::MutableImageView& dst) = 0;

    // Whole-image convert and scale. Defaults to cropping the full frame, so a
    // backend only implements it when it has a cheaper path.
    virtual core::Status convert(const core::ImageView& src,
                                 const core::MutableImageView& dst) {
        return crop(src, core::Rect{0, 0, src.size.width, src.size.height}, dst);
    }

    // Imports a dmabuf-backed image so subsequent operations avoid a copy.
    // Backends without a zero-copy path say so instead of pretending.
    virtual core::Result<NativeHandle> import(const core::ImageView& /*image*/) {
        return core::unsupported(std::string(id()) + " has no zero-copy import");
    }

    virtual bool supportsZeroCopy() const { return false; }
};

Registry<ImageOps>& imageOpsRegistry();

// Selects once and caches. `VISORA_IMAGE_BACKEND` forces a specific id, which
// is how you compare hardware against software on the same machine.
core::Result<ImageOps*> imageOps();

}  // namespace visora::hal
