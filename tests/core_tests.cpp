// Tests for the pure layer. Links visora::core only — no GStreamer, no OpenCV,
// no accelerator. Runs anywhere, including CI containers with no devices.

#include "TestHarness.hpp"

#include "core/Geometry.hpp"
#include "core/Image.hpp"
#include "core/ImageMath.hpp"
#include "core/Log.hpp"
#include "core/Result.hpp"

using namespace visora::core;

VS_TEST(letterbox_keeps_aspect_ratio_and_centres) {
    // 1920x1080 into 640x640: the content is 640 wide, 360 high, centred.
    const Rect content = fitContentRect(FitMode::Letterbox, {1920, 1080}, {640, 640});
    VS_CHECK_EQ(content.width, 640);
    VS_CHECK_EQ(content.height, 360);
    VS_CHECK_EQ(content.x, 0);
    VS_CHECK_EQ(content.y, 140);
}

VS_TEST(letterbox_result_is_always_even_aligned) {
    // Chroma-subsampled destinations cannot represent odd offsets or extents.
    const Rect content = fitContentRect(FitMode::Letterbox, {1233, 977}, {641, 641});
    VS_CHECK_EQ(content.x % 2, 0);
    VS_CHECK_EQ(content.y % 2, 0);
    VS_CHECK_EQ(content.width % 2, 0);
    VS_CHECK_EQ(content.height % 2, 0);
}

VS_TEST(stretch_fills_the_whole_destination) {
    const Rect content = fitContentRect(FitMode::Stretch, {1920, 1080}, {320, 48});
    VS_CHECK_EQ(content.x, 0);
    VS_CHECK_EQ(content.y, 0);
    VS_CHECK_EQ(content.width, 320);
    VS_CHECK_EQ(content.height, 48);
}

VS_TEST(fit_height_anchors_left_and_keeps_ratio) {
    // 200x100 into 320x48: height fills, width follows the true ratio (96).
    const Rect content = fitContentRect(FitMode::FitHeight, {200, 100}, {320, 48});
    VS_CHECK_EQ(content.x, 0);
    VS_CHECK_EQ(content.y, 0);
    VS_CHECK_EQ(content.height, 48);
    VS_CHECK_EQ(content.width, 96);
}

VS_TEST(fit_height_never_overflows_the_destination) {
    // A very wide source would compute past the destination width; it clamps.
    const Rect content = fitContentRect(FitMode::FitHeight, {4000, 100}, {320, 48});
    VS_CHECK(content.width <= 320);
}

VS_TEST(degenerate_sizes_yield_an_empty_rect) {
    VS_CHECK(!fitContentRect(FitMode::Letterbox, {0, 0}, {640, 640}).valid());
    VS_CHECK(!fitContentRect(FitMode::Letterbox, {640, 480}, {0, 640}).valid());
}

VS_TEST(map_back_undoes_the_letterbox) {
    const Size src{1920, 1080};
    const Size dst{640, 640};
    const Rect content = fitContentRect(FitMode::Letterbox, src, dst);

    // The top-left of the content maps to the top-left of the source.
    float x = -1.0f;
    float y = -1.0f;
    mapToSource(content, src, dst, static_cast<float>(content.x),
                static_cast<float>(content.y), &x, &y);
    VS_CHECK_EQ(static_cast<int>(x), 0);
    VS_CHECK_EQ(static_cast<int>(y), 0);

    // The bottom-right of the content maps to the bottom-right of the source.
    mapToSource(content, src, dst, static_cast<float>(content.right()),
                static_cast<float>(content.bottom()), &x, &y);
    VS_CHECK_EQ(static_cast<int>(x), 1920);
    VS_CHECK_EQ(static_cast<int>(y), 1080);
}

VS_TEST(map_back_clamps_points_that_land_in_the_padding) {
    const Size src{1920, 1080};
    const Size dst{640, 640};
    const Rect content = fitContentRect(FitMode::Letterbox, src, dst);

    float x = 0.0f;
    float y = 0.0f;
    mapToSource(content, src, dst, 10.0f, 0.0f, &x, &y);  // y=0 is a padding bar
    VS_CHECK_EQ(static_cast<int>(y), 0);
    VS_CHECK(x >= 0.0f);
}

VS_TEST(expand_to_min_grows_a_tiny_crop_around_its_centre) {
    // A 30x12 plate at (500,400) grows to 128x128 keeping the same centre.
    const Rect grown = expandToMin(Rect{500, 400, 30, 12}, Size{1920, 1080}, 128);
    VS_CHECK_EQ(grown.width, 128);
    VS_CHECK_EQ(grown.height, 128);
    VS_CHECK(grown.x <= 500 && grown.right() >= 530);
    VS_CHECK(grown.y <= 400 && grown.bottom() >= 412);
}

VS_TEST(expand_to_min_stays_inside_the_frame) {
    // A crop in the corner cannot centre itself; it shifts in, never out.
    const Rect grown = expandToMin(Rect{0, 0, 10, 10}, Size{1920, 1080}, 128);
    VS_CHECK_EQ(grown.x, 0);
    VS_CHECK_EQ(grown.y, 0);
    VS_CHECK(grown.right() <= 1920);
    VS_CHECK(grown.bottom() <= 1080);

    const Rect corner = expandToMin(Rect{1910, 1070, 10, 10}, Size{1920, 1080}, 128);
    VS_CHECK(corner.right() <= 1920);
    VS_CHECK(corner.bottom() <= 1080);
}

VS_TEST(expand_to_min_never_exceeds_the_frame_itself) {
    // Asking for 128 on a 64x64 frame must not produce a rect bigger than it.
    const Rect grown = expandToMin(Rect{10, 10, 4, 4}, Size{64, 64}, 128);
    VS_CHECK(grown.width <= 64);
    VS_CHECK(grown.height <= 64);
}

VS_TEST(expand_to_min_is_even_aligned) {
    const Rect grown = expandToMin(Rect{501, 401, 33, 15}, Size{1920, 1080}, 127);
    VS_CHECK_EQ(grown.x % 2, 0);
    VS_CHECK_EQ(grown.y % 2, 0);
    VS_CHECK_EQ(grown.width % 2, 0);
    VS_CHECK_EQ(grown.height % 2, 0);
}

VS_TEST(clamp_clips_to_bounds_without_negative_extents) {
    const Rect clipped = clamp(Rect{-20, -20, 40, 40}, Size{100, 100});
    VS_CHECK_EQ(clipped.x, 0);
    VS_CHECK_EQ(clipped.y, 0);
    VS_CHECK_EQ(clipped.width, 20);
    VS_CHECK_EQ(clipped.height, 20);

    const Rect outside = clamp(Rect{200, 200, 40, 40}, Size{100, 100});
    VS_CHECK_EQ(outside.width, 0);
    VS_CHECK_EQ(outside.height, 0);
}

VS_TEST(packed_layouts_match_the_format) {
    VS_CHECK_EQ(packedSize(PixelFormat::NV12, Size{640, 480}),
                static_cast<std::size_t>(640 * 480 * 3 / 2));
    VS_CHECK_EQ(packedSize(PixelFormat::RGB888, Size{640, 480}),
                static_cast<std::size_t>(640 * 480 * 3));
    VS_CHECK_EQ(packedSize(PixelFormat::GRAY8, Size{640, 480}),
                static_cast<std::size_t>(640 * 480));

    const Planes nv12 = Planes::packed(PixelFormat::NV12, Size{640, 480});
    VS_CHECK_EQ(nv12[0].stride, 640);
    VS_CHECK_EQ(nv12[0].offset, static_cast<std::size_t>(0));
    VS_CHECK_EQ(nv12[1].stride, 640);
    VS_CHECK_EQ(nv12[1].offset, static_cast<std::size_t>(640 * 480));
}

VS_TEST(result_carries_the_failure_reason) {
    Result<int> good{42};
    VS_CHECK(good.ok());
    VS_CHECK_EQ(good.value(), 42);

    Result<int> bad{unsupported("no such thing")};
    VS_CHECK(!bad.ok());
    VS_CHECK(bad.error().code == ErrorCode::Unsupported);
    VS_CHECK(bad.error().str() == "unsupported: no such thing");
    VS_CHECK_EQ(bad.valueOr(7), 7);
}

VS_TEST(status_is_ok_by_default) {
    Status fine;
    VS_CHECK(fine.ok());

    Status broken{hardwareFailure("device busy")};
    VS_CHECK(!broken.ok());
    VS_CHECK(broken.error().code == ErrorCode::HardwareFailure);
}

VS_TEST(log_filters_by_level_and_category) {
    log::setLevel(log::Level::Warn);
    log::setCategories("");
    VS_CHECK(!log::enabled(log::Level::Info, "hal"));
    VS_CHECK(log::enabled(log::Level::Error, "hal"));

    log::setLevel(log::Level::Trace);
    log::setCategories("hal,rga");
    VS_CHECK(log::enabled(log::Level::Trace, "hal"));
    VS_CHECK(!log::enabled(log::Level::Trace, "gst"));

    log::setCategories("");  // empty = everything
    VS_CHECK(log::enabled(log::Level::Trace, "gst"));

    log::setLevel(log::Level::Off);
    VS_CHECK(!log::enabled(log::Level::Error, "hal"));
    log::setLevel(log::Level::Info);
}

VS_MAIN()
