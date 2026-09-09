// One set of expectations, run against EVERY image-ops backend compiled in.
//
// The software path is the definition of correct. An accelerated backend either
// produces the same picture within tolerance, or declines the case outright —
// declining is legitimate and the chain covers it. What is not acceptable is
// producing a different picture and staying silent, which is exactly what a
// pixel-vs-byte stride mistake did on real hardware while every x86 test passed.
//
// Run it on each machine you care about. On a board it is the difference between
// "the accelerator is wired up" and "the accelerator is correct".

#include "TestHarness.hpp"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/Log.hpp"
#include "hal/ImageOps.hpp"

using namespace visora;
using visora::core::PixelFormat;
using visora::core::Rect;
using visora::core::Size;

namespace {

struct Backend {
    std::string id;
    std::unique_ptr<hal::ImageOps> ops;
};

// Every backend that says it can run here, tried one at a time with no chain
// underneath — otherwise a broken accelerator hides behind the software path.
std::vector<Backend>& backends() {
    static std::vector<Backend> list = [] {
        std::vector<Backend> built;
        for (const hal::BackendStatus& row : hal::imageOpsRegistry().status("image-ops")) {
            if (!row.available) continue;
            auto selected = hal::imageOpsRegistry().select("image-ops", row.id);
            if (selected.ok()) built.push_back({row.id, std::move(selected.value())});
        }
        return built;
    }();
    return list;
}

bool near(int a, int b, int tolerance) { return (a - b <= tolerance) && (b - a <= tolerance); }

const std::uint8_t* rgbPixel(const core::OwnedImage& image, int x, int y) {
    return image.data() + (static_cast<std::size_t>(y) * image.size().width + x) * 3;
}

core::OwnedImage solidRgb(Size size, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    core::OwnedImage image(PixelFormat::RGB888, size);
    for (std::size_t i = 0; i < image.byteCount(); i += 3) {
        image.data()[i] = r;
        image.data()[i + 1] = g;
        image.data()[i + 2] = b;
    }
    return image;
}

// A solid-colour NV12 image with the given luma and neutral chroma, optionally
// with a padded row stride the way a real decoder hands one over.
core::OwnedImage solidNv12(Size size, std::uint8_t luma) {
    core::OwnedImage image(PixelFormat::NV12, size);
    const std::size_t lumaBytes =
        static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height);
    std::memset(image.data(), luma, lumaBytes);
    std::memset(image.data() + lumaBytes, 128, image.byteCount() - lumaBytes);
    return image;
}

// Reports a failure with the backend id attached, so a conformance run names
// which backend is wrong rather than only which assertion tripped.
void fail(const std::string& id, const char* file, int line, const std::string& what) {
    ::visora::test::reportFailure(file, line, "[" + id + "] " + what);
}

#define CONFORM_CHECK(id, cond)                                                \
    do {                                                                       \
        if (!(cond)) fail((id), __FILE__, __LINE__, #cond);                    \
    } while (false)

#define CONFORM_NEAR(id, got, want, tol)                                       \
    do {                                                                       \
        const int g = static_cast<int>(got);                                   \
        if (!near(g, (want), (tol))) {                                         \
            fail((id), __FILE__, __LINE__,                                     \
                 std::string(#got " = ") + std::to_string(g) + ", expected ~" + \
                     std::to_string(want));                                    \
        }                                                                      \
    } while (false)

// Declining is a valid answer. Anything else must be right.
bool declined(const core::Error& error) {
    return error.code == core::ErrorCode::Unsupported ||
           error.code == core::ErrorCode::HardwareFailure;
}

}  // namespace

VS_TEST(at_least_one_backend_is_available) {
    VS_CHECK(!backends().empty());
    for (const Backend& backend : backends()) {
        std::fprintf(stderr, "    backend under test: %s\n", backend.id.c_str());
    }
}

VS_TEST(letterbox_rgb_to_rgb) {
    const core::OwnedImage src = solidRgb({320, 240}, 60, 120, 180);
    for (const Backend& backend : backends()) {
        core::OwnedImage dst(PixelFormat::RGB888, {128, 128});
        auto content = backend.ops->fit(src.view(), dst.view(), core::FitMode::Letterbox, 0);
        if (!content.ok()) {
            CONFORM_CHECK(backend.id, declined(content.error()));
            continue;
        }
        CONFORM_CHECK(backend.id, content.value().width == 128);
        CONFORM_CHECK(backend.id, content.value().height == 96);
        CONFORM_CHECK(backend.id, content.value().y == 16);

        const std::uint8_t* centre = rgbPixel(dst, 64, 64);
        CONFORM_NEAR(backend.id, centre[0], 60, 4);
        CONFORM_NEAR(backend.id, centre[1], 120, 4);
        CONFORM_NEAR(backend.id, centre[2], 180, 4);

        const std::uint8_t* bar = rgbPixel(dst, 64, 4);  // inside the top bar
        CONFORM_NEAR(backend.id, bar[0], 0, 1);
        CONFORM_NEAR(backend.id, bar[1], 0, 1);
        CONFORM_NEAR(backend.id, bar[2], 0, 1);
    }
}

VS_TEST(letterbox_nv12_to_rgb_is_the_real_pipeline_path) {
    // What every camera frame does: full-resolution NV12 from the decoder into a
    // square model input.
    const core::OwnedImage src = solidNv12({1920, 1080}, 128);
    for (const Backend& backend : backends()) {
        core::OwnedImage dst(PixelFormat::RGB888, {640, 640});
        auto content = backend.ops->fit(src.view(), dst.view(), core::FitMode::Letterbox, 114);
        if (!content.ok()) {
            CONFORM_CHECK(backend.id, declined(content.error()));
            continue;
        }
        CONFORM_CHECK(backend.id, content.value().width == 640);
        CONFORM_CHECK(backend.id, content.value().height == 360);
        CONFORM_CHECK(backend.id, content.value().y == 140);

        // Y=128 with neutral chroma is mid grey. A stride mistake shows up here
        // as streaks or garbage rather than a uniform value.
        const std::uint8_t* centre = rgbPixel(dst, 320, 320);
        CONFORM_NEAR(backend.id, centre[0], 128, 8);
        CONFORM_NEAR(backend.id, centre[1], 128, 8);
        CONFORM_NEAR(backend.id, centre[2], 128, 8);

        const std::uint8_t* bar = rgbPixel(dst, 320, 20);
        CONFORM_NEAR(backend.id, bar[0], 114, 2);
    }
}

VS_TEST(crop_extracts_the_requested_region) {
    // Left half red, right half blue, so a crop from the right must be pure blue.
    core::OwnedImage src(PixelFormat::RGB888, {256, 64});
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 256; ++x) {
            std::uint8_t* p = src.data() + (static_cast<std::size_t>(y) * 256 + x) * 3;
            p[0] = x < 128 ? 255 : 0;
            p[1] = 0;
            p[2] = x < 128 ? 0 : 255;
        }
    }

    for (const Backend& backend : backends()) {
        core::OwnedImage dst(PixelFormat::RGB888, {32, 32});
        const core::Status ok = backend.ops->crop(src.view(), Rect{160, 8, 64, 48}, dst.view());
        if (!ok.ok()) {
            CONFORM_CHECK(backend.id, declined(ok.error()));
            continue;
        }
        const std::uint8_t* centre = rgbPixel(dst, 16, 16);
        CONFORM_NEAR(backend.id, centre[0], 0, 6);
        CONFORM_NEAR(backend.id, centre[2], 255, 6);
    }
}

VS_TEST(nv12_with_a_padded_row_stride) {
    // Decoders pad rows and place chroma at their own offset. Assuming
    // stride == width is the single most common way to get this wrong.
    const int stride = 96;
    const std::size_t uvOffset = static_cast<std::size_t>(stride) * 64;
    std::vector<std::uint8_t> buffer(uvOffset + static_cast<std::size_t>(stride) * 32, 0);
    for (int y = 0; y < 64; ++y) {
        std::memset(buffer.data() + static_cast<std::size_t>(y) * stride, 128, 64);
    }
    std::memset(buffer.data() + uvOffset, 128, static_cast<std::size_t>(stride) * 32);

    core::ImageView src;
    src.format = PixelFormat::NV12;
    src.size = {64, 64};
    src.planes[0] = {stride, 0};
    src.planes[1] = {stride, uvOffset};
    src.data = buffer.data();

    for (const Backend& backend : backends()) {
        core::OwnedImage dst(PixelFormat::RGB888, {32, 32});
        const core::Status ok = backend.ops->convert(src, dst.view());
        if (!ok.ok()) {
            CONFORM_CHECK(backend.id, declined(ok.error()));
            continue;
        }
        const std::uint8_t* centre = rgbPixel(dst, 16, 16);
        CONFORM_NEAR(backend.id, centre[0], 128, 8);
        CONFORM_NEAR(backend.id, centre[1], 128, 8);
        CONFORM_NEAR(backend.id, centre[2], 128, 8);
    }
}

VS_TEST(large_crop_upscaled_past_the_single_pass_scale_limit) {
    // 128x128 (the smallest RGA takes) into 512x512 is only 4x, so this is the
    // multi-pass path's sibling: big enough for hardware, and a check that a
    // large upscale is correct on whichever backend runs it.
    core::OwnedImage src(PixelFormat::RGB888, {1920, 1080});
    std::memset(src.data(), 0, src.byteCount());
    for (int y = 500; y < 628; ++y) {
        for (int x = 900; x < 1028; ++x) {
            std::uint8_t* p = src.data() + (static_cast<std::size_t>(y) * 1920 + x) * 3;
            p[0] = 20;
            p[1] = 200;
            p[2] = 90;
        }
    }
    for (const Backend& backend : backends()) {
        core::OwnedImage dst(PixelFormat::RGB888, {512, 512});
        const core::Status ok =
            backend.ops->crop(src.view(), Rect{900, 500, 128, 128}, dst.view());
        if (!ok.ok()) {
            CONFORM_CHECK(backend.id, declined(ok.error()));
            continue;
        }
        const std::uint8_t* centre = rgbPixel(dst, 256, 256);
        CONFORM_NEAR(backend.id, centre[0], 20, 10);
        CONFORM_NEAR(backend.id, centre[1], 200, 10);
        CONFORM_NEAR(backend.id, centre[2], 90, 10);
    }
}

VS_TEST(tight_crop_below_what_the_accelerator_handles) {
    // A licence plate about 30 px wide going into a 512 px model input: a 17x
    // upscale, past what one RGA pass does, and below the source size RGA
    // handles at all. Whichever backend takes it, the crop must stay tight —
    // growing it changes what a tight-crop model sees — and the colour must
    // survive. RGA declines this one and the software path finishes it, which
    // is a correct outcome, not a failure.
    core::OwnedImage src(PixelFormat::RGB888, {1920, 1080});
    std::memset(src.data(), 0, src.byteCount());
    for (int y = 500; y < 530; ++y) {
        for (int x = 900; x < 930; ++x) {
            std::uint8_t* p = src.data() + (static_cast<std::size_t>(y) * 1920 + x) * 3;
            p[0] = 240;
            p[1] = 30;
            p[2] = 20;
        }
    }

    for (const Backend& backend : backends()) {
        core::OwnedImage dst(PixelFormat::RGB888, {512, 512});
        const core::Status ok =
            backend.ops->crop(src.view(), Rect{900, 500, 30, 30}, dst.view());
        if (!ok.ok()) {
            CONFORM_CHECK(backend.id, declined(ok.error()));
            continue;
        }
        // The centre of a 30x30 patch upscaled to 512x512 is well inside it.
        const std::uint8_t* centre = rgbPixel(dst, 256, 256);
        CONFORM_NEAR(backend.id, centre[0], 240, 10);
        CONFORM_NEAR(backend.id, centre[1], 30, 10);
        CONFORM_NEAR(backend.id, centre[2], 20, 10);
    }
}

VS_TEST(destination_width_that_is_not_16_aligned) {
    // RGB destinations need a 16-aligned pixel stride on some hardware. 100 is
    // not, so this exercises the stride-scratch path rather than the fast one.
    const core::OwnedImage src = solidRgb({640, 480}, 200, 100, 50);
    for (const Backend& backend : backends()) {
        core::OwnedImage dst(PixelFormat::RGB888, {100, 100});
        auto content = backend.ops->fit(src.view(), dst.view(), core::FitMode::Stretch, 0);
        if (!content.ok()) {
            CONFORM_CHECK(backend.id, declined(content.error()));
            continue;
        }
        const std::uint8_t* centre = rgbPixel(dst, 50, 50);
        CONFORM_NEAR(backend.id, centre[0], 200, 5);
        CONFORM_NEAR(backend.id, centre[1], 100, 5);
        CONFORM_NEAR(backend.id, centre[2], 50, 5);

        // The last pixel of a row: a stride mistake in the copy-back shows here
        // first, because that is where a wrong stride runs off the end.
        const std::uint8_t* edge = rgbPixel(dst, 99, 50);
        CONFORM_NEAR(backend.id, edge[0], 200, 5);
    }
}

VS_TEST(rgb_to_nv12_and_back) {
    const core::OwnedImage src = solidRgb({64, 64}, 180, 90, 40);
    for (const Backend& backend : backends()) {
        core::OwnedImage nv12(PixelFormat::NV12, {64, 64});
        const core::Status forward = backend.ops->convert(src.view(), nv12.view());
        if (!forward.ok()) {
            CONFORM_CHECK(backend.id, declined(forward.error()));
            continue;
        }
        core::OwnedImage back(PixelFormat::RGB888, {64, 64});
        const core::Status reverse = backend.ops->convert(nv12.view(), back.view());
        if (!reverse.ok()) {
            CONFORM_CHECK(backend.id, declined(reverse.error()));
            continue;
        }
        // 4:2:0 is lossy, but a swapped U/V or a wrong channel order is far
        // outside this tolerance.
        const std::uint8_t* p = rgbPixel(back, 32, 32);
        CONFORM_NEAR(backend.id, p[0], 180, 14);
        CONFORM_NEAR(backend.id, p[1], 90, 14);
        CONFORM_NEAR(backend.id, p[2], 40, 14);
    }
}

VS_MAIN()
