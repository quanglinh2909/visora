// Tests for the extension mechanism and the software image path.
//
// The first group is the executable form of this project's central promise:
// a backend registers itself from a file nothing else references, and selection
// picks it purely on priority and availability. If that ever stops holding,
// supporting new hardware stops being additive and these tests fail.

#include "TestHarness.hpp"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hal/Capabilities.hpp"
#include "hal/ImageOps.hpp"
#include "hal/NativeHandle.hpp"
#include "hal/Registry.hpp"

using namespace visora;
using visora::core::PixelFormat;
using visora::core::Rect;
using visora::core::Size;

// --- the extension mechanism -------------------------------------------------

namespace {

// A backend interface that exists only in this file, to exercise the registry
// without disturbing the real one.
class Widget {
public:
    virtual ~Widget() = default;
    virtual std::string name() const = 0;
};

struct FastWidget : Widget {
    std::string name() const override { return "fast"; }
};
struct SlowWidget : Widget {
    std::string name() const override { return "slow"; }
};

bool g_fastAvailable = true;

// Registered from this translation unit alone. Nothing in src/ mentions it.
const hal::Register<Widget> registerFast{{
    "fast", 100,
    [] {
        return g_fastAvailable ? hal::Probe::yes("simulated accelerator present")
                               : hal::Probe::no("simulated accelerator absent");
    },
    [] { return std::unique_ptr<Widget>(new FastWidget()); },
}};

const hal::Register<Widget> registerSlow{{
    "slow", 0,
    [] { return hal::Probe::yes("always available"); },
    [] { return std::unique_ptr<Widget>(new SlowWidget()); },
}};

}  // namespace

VS_TEST(registry_prefers_the_highest_priority_available_backend) {
    g_fastAvailable = true;
    auto chosen = hal::Registry<Widget>::instance().select("widget");
    VS_CHECK(chosen.ok());
    VS_CHECK(chosen.value()->name() == "fast");
}

VS_TEST(registry_falls_through_when_the_preferred_backend_is_unavailable) {
    g_fastAvailable = false;
    auto chosen = hal::Registry<Widget>::instance().select("widget");
    VS_CHECK(chosen.ok());
    VS_CHECK(chosen.value()->name() == "slow");
    g_fastAvailable = true;
}

VS_TEST(registry_honours_a_forced_backend) {
    g_fastAvailable = true;
    auto chosen = hal::Registry<Widget>::instance().select("widget", "slow");
    VS_CHECK(chosen.ok());
    VS_CHECK(chosen.value()->name() == "slow");
}

VS_TEST(forcing_an_unavailable_backend_explains_why) {
    g_fastAvailable = false;
    auto chosen = hal::Registry<Widget>::instance().select("widget", "fast");
    VS_CHECK(!chosen.ok());
    VS_CHECK(chosen.error().message.find("simulated accelerator absent") != std::string::npos);
    g_fastAvailable = true;
}

VS_TEST(forcing_an_unknown_backend_says_it_is_not_compiled_in) {
    auto chosen = hal::Registry<Widget>::instance().select("widget", "quantum");
    VS_CHECK(!chosen.ok());
    VS_CHECK(chosen.error().code == core::ErrorCode::NotFound);
}

VS_TEST(status_lists_every_backend_highest_priority_first) {
    g_fastAvailable = true;
    const auto rows = hal::Registry<Widget>::instance().status("widget", "fast");
    VS_CHECK_EQ(rows.size(), static_cast<std::size_t>(2));
    VS_CHECK(rows[0].id == "fast");
    VS_CHECK(rows[0].selected);
    VS_CHECK(rows[0].available);
    VS_CHECK(rows[1].id == "slow");
    VS_CHECK(!rows[1].selected);
}

// --- NativeHandle ------------------------------------------------------------

namespace {
int g_releaseCount = 0;
void countingRelease(std::uint64_t) { ++g_releaseCount; }
}  // namespace

VS_TEST(native_handle_releases_exactly_once) {
    g_releaseCount = 0;
    {
        hal::NativeHandle handle(42, &countingRelease);
        VS_CHECK(static_cast<bool>(handle));
        VS_CHECK_EQ(handle.get(), static_cast<std::uint64_t>(42));
    }
    VS_CHECK_EQ(g_releaseCount, 1);
}

VS_TEST(native_handle_move_does_not_double_release) {
    g_releaseCount = 0;
    {
        hal::NativeHandle first(7, &countingRelease);
        hal::NativeHandle second(std::move(first));
        VS_CHECK(!static_cast<bool>(first));
        VS_CHECK(static_cast<bool>(second));
    }
    VS_CHECK_EQ(g_releaseCount, 1);
}

VS_TEST(empty_native_handle_releases_nothing) {
    g_releaseCount = 0;
    { hal::NativeHandle handle; }
    VS_CHECK_EQ(g_releaseCount, 0);
}

// --- the software image path -------------------------------------------------

namespace {

std::unique_ptr<hal::ImageOps> cpuOps() {
    auto selected = hal::imageOpsRegistry().select("image-ops", "cpu");
    if (!selected) {
        ::visora::test::reportFailure(__FILE__, __LINE__,
                                      "cpu image ops unavailable: " + selected.error().str());
        return nullptr;
    }
    return std::move(selected.value());
}

core::OwnedImage solidRgb(Size size, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    core::OwnedImage image(PixelFormat::RGB888, size);
    std::uint8_t* p = image.data();
    for (std::size_t i = 0; i < image.byteCount(); i += 3) {
        p[i] = r;
        p[i + 1] = g;
        p[i + 2] = b;
    }
    return image;
}

const std::uint8_t* pixelAt(const core::OwnedImage& image, int x, int y) {
    const int stride = image.size().width * 3;
    return image.data() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 3;
}

bool near(int a, int b, int tolerance) { return (a - b <= tolerance) && (b - a <= tolerance); }

}  // namespace

VS_TEST(cpu_is_registered_and_always_available) {
    const auto rows = hal::imageOpsRegistry().status("image-ops");
    bool found = false;
    for (const hal::BackendStatus& row : rows) {
        if (row.id == "cpu") {
            found = true;
            VS_CHECK(row.available);
        }
    }
    VS_CHECK(found);
}

VS_TEST(cpu_letterbox_places_content_and_pads_the_bars) {
    auto ops = cpuOps();
    if (!ops) return;

    const core::OwnedImage src = solidRgb({1920, 1080}, 200, 100, 50);
    core::OwnedImage dst(PixelFormat::RGB888, {640, 640});

    auto content = ops->fit(src.view(), dst.view(), core::FitMode::Letterbox, 114);
    VS_CHECK(content.ok());
    if (!content.ok()) return;

    VS_CHECK_EQ(content.value().width, 640);
    VS_CHECK_EQ(content.value().height, 360);
    VS_CHECK_EQ(content.value().y, 140);

    // Centre of the content carries the source colour.
    const std::uint8_t* centre = pixelAt(dst, 320, 320);
    VS_CHECK(near(centre[0], 200, 2));
    VS_CHECK(near(centre[1], 100, 2));
    VS_CHECK(near(centre[2], 50, 2));

    // The bar above the content is the pad value, on every channel.
    const std::uint8_t* bar = pixelAt(dst, 320, 10);
    VS_CHECK_EQ(static_cast<int>(bar[0]), 114);
    VS_CHECK_EQ(static_cast<int>(bar[1]), 114);
    VS_CHECK_EQ(static_cast<int>(bar[2]), 114);
}

VS_TEST(cpu_stretch_leaves_no_padding) {
    auto ops = cpuOps();
    if (!ops) return;

    const core::OwnedImage src = solidRgb({200, 100}, 10, 220, 30);
    core::OwnedImage dst(PixelFormat::RGB888, {320, 48});

    auto content = ops->fit(src.view(), dst.view(), core::FitMode::Stretch, 0);
    VS_CHECK(content.ok());
    if (!content.ok()) return;
    VS_CHECK_EQ(content.value().width, 320);
    VS_CHECK_EQ(content.value().height, 48);

    // Corners are content, not padding.
    const std::uint8_t* corner = pixelAt(dst, 0, 0);
    VS_CHECK(near(corner[1], 220, 3));
}

VS_TEST(cpu_crop_extracts_the_requested_region) {
    auto ops = cpuOps();
    if (!ops) return;

    // Left half red, right half blue.
    core::OwnedImage src(PixelFormat::RGB888, {256, 64});
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 256; ++x) {
            std::uint8_t* p = src.data() + (static_cast<std::size_t>(y) * 256 + x) * 3;
            p[0] = x < 128 ? 255 : 0;
            p[1] = 0;
            p[2] = x < 128 ? 0 : 255;
        }
    }

    core::OwnedImage dst(PixelFormat::RGB888, {32, 32});
    const core::Status ok = ops->crop(src.view(), Rect{160, 8, 64, 48}, dst.view());
    VS_CHECK(ok.ok());
    if (!ok.ok()) return;

    const std::uint8_t* centre = pixelAt(dst, 16, 16);
    VS_CHECK(near(centre[0], 0, 3));    // no red
    VS_CHECK(near(centre[2], 255, 3));  // all blue
}

VS_TEST(cpu_converts_rgb_to_nv12_and_back) {
    auto ops = cpuOps();
    if (!ops) return;

    const core::OwnedImage src = solidRgb({64, 64}, 180, 90, 40);

    core::OwnedImage nv12(PixelFormat::NV12, {64, 64});
    const core::Status toNv12 = ops->convert(src.view(), nv12.view());
    VS_CHECK(toNv12.ok());
    if (!toNv12.ok()) return;

    core::OwnedImage back(PixelFormat::RGB888, {64, 64});
    const core::Status fromNv12 = ops->convert(nv12.view(), back.view());
    VS_CHECK(fromNv12.ok());
    if (!fromNv12.ok()) return;

    // YUV 4:2:0 is lossy; a wide tolerance still catches a wrong plane layout,
    // a swapped U/V, or a channel-order mistake, which is what this guards.
    const std::uint8_t* p = pixelAt(back, 32, 32);
    VS_CHECK(near(p[0], 180, 12));
    VS_CHECK(near(p[1], 90, 12));
    VS_CHECK(near(p[2], 40, 12));
}

VS_TEST(cpu_reads_nv12_with_padded_strides) {
    auto ops = cpuOps();
    if (!ops) return;

    // Decoders pad rows: build a 64x64 NV12 with a 96-byte stride and the
    // chroma plane at a non-default offset, the shape a real decoder hands over.
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

    core::OwnedImage dst(PixelFormat::RGB888, {32, 32});
    const core::Status ok = ops->convert(src, dst.view());
    VS_CHECK(ok.ok());
    if (!ok.ok()) return;

    // Y=128, U=V=128 is mid grey. Getting this wrong (reading with the wrong
    // stride) produces obvious streaking rather than a uniform value.
    const std::uint8_t* p = pixelAt(dst, 16, 16);
    VS_CHECK(near(p[0], 128, 6));
    VS_CHECK(near(p[1], 128, 6));
    VS_CHECK(near(p[2], 128, 6));
}

VS_TEST(cpu_rejects_an_image_it_cannot_read) {
    auto ops = cpuOps();
    if (!ops) return;

    core::ImageView dmabufOnly;
    dmabufOnly.format = PixelFormat::NV12;
    dmabufOnly.size = {64, 64};
    dmabufOnly.planes = core::Planes::packed(PixelFormat::NV12, {64, 64});
    dmabufOnly.dmaFd = 3;  // no CPU pointer

    core::OwnedImage dst(PixelFormat::RGB888, {32, 32});
    const core::Status result = ops->convert(dmabufOnly, dst.view());
    VS_CHECK(!result.ok());
    VS_CHECK(result.error().code == core::ErrorCode::Unsupported);
}

VS_TEST(cpu_has_no_zero_copy_import) {
    auto ops = cpuOps();
    if (!ops) return;
    VS_CHECK(!ops->supportsZeroCopy());

    core::ImageView image;
    image.format = PixelFormat::NV12;
    image.size = {64, 64};
    image.dmaFd = 3;
    auto imported = ops->import(image);
    VS_CHECK(!imported.ok());
}

// --- the capability report ---------------------------------------------------

VS_TEST(capability_report_names_the_platform_and_the_backends) {
    const hal::Capabilities caps = hal::detectCapabilities();
    VS_CHECK(!caps.osName.empty());
    VS_CHECK(!caps.architecture.empty());
    VS_CHECK(!caps.backends.empty());

    const std::string text = hal::toText(caps);
    VS_CHECK(text.find("image ops") != std::string::npos);
    VS_CHECK(text.find("cpu") != std::string::npos);

    const std::string json = hal::toJson(caps);
    VS_CHECK(json.find("\"imageOps\"") != std::string::npos);
    VS_CHECK(json.find("\"backends\"") != std::string::npos);
}

VS_TEST(ai_is_reported_disabled_with_a_reason_when_no_backend_exists) {
    const hal::Capabilities caps = hal::detectCapabilities();
    if (!caps.aiEnabled()) {
        // The whole point: never "disabled" with no explanation.
        VS_CHECK(!caps.inferenceProblem.empty());
    }
}

VS_MAIN()
