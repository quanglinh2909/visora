#pragma once

// The image vocabulary the whole system speaks.
//
// Deliberately knows nothing about GStreamer, librga or any accelerator: a
// picture is a format, a size, per-plane strides, and either a CPU pointer or a
// dmabuf file descriptor. That is what lets `core` compile and be tested on a
// machine with no camera and no NPU, and what stops vendor headers leaking
// into every file that merely wants to describe a frame.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Geometry.hpp"

namespace visora::core {

enum class PixelFormat {
    Unknown,
    NV12,     // Y plane + interleaved UV, 4:2:0
    RGB888,   // packed, 3 bytes per pixel
    BGR888,   // packed, 3 bytes per pixel
    GRAY8,    // single plane
};

const char* toString(PixelFormat format);

inline constexpr int kMaxPlanes = 2;

int planeCount(PixelFormat format);

// Bytes per row for a tightly packed image of this width.
int packedStride(PixelFormat format, int width, int plane);

// Total bytes for a tightly packed image.
std::size_t packedSize(PixelFormat format, Size size);

// Per-plane geometry. `offset` is measured from the start of the buffer, which
// is how a decoder hands over NV12 (Y at 0, UV at yStride * yHeight) and how
// dmabuf-backed frames describe themselves.
struct PlaneLayout {
    int stride = 0;
    std::size_t offset = 0;
};

struct Planes {
    std::array<PlaneLayout, kMaxPlanes> plane{};

    PlaneLayout& operator[](int i) { return plane[static_cast<std::size_t>(i)]; }
    const PlaneLayout& operator[](int i) const { return plane[static_cast<std::size_t>(i)]; }

    // Layout of a tightly packed image, which is what most callers have.
    static Planes packed(PixelFormat format, Size size);
};

// A non-owning description of an image someone else owns.
//
// `data` and `dmaFd` are independent: a decoder frame may expose only a dmabuf
// fd (nothing mapped into this process), a synthetic image only a CPU pointer,
// and an imported dmabuf both. A backend picks whichever it can use and reports
// Unsupported when it can use neither.
struct ImageView {
    PixelFormat format = PixelFormat::Unknown;
    Size size;
    Planes planes;
    const std::uint8_t* data = nullptr;
    int dmaFd = -1;

    bool hasCpu() const { return data != nullptr; }
    bool hasDmaBuf() const { return dmaFd >= 0; }
    bool valid() const { return format != PixelFormat::Unknown && size.valid(); }

    static ImageView packed(PixelFormat format, Size size, const std::uint8_t* data);
};

struct MutableImageView {
    PixelFormat format = PixelFormat::Unknown;
    Size size;
    Planes planes;
    std::uint8_t* data = nullptr;
    int dmaFd = -1;

    bool hasCpu() const { return data != nullptr; }
    bool hasDmaBuf() const { return dmaFd >= 0; }
    bool valid() const { return format != PixelFormat::Unknown && size.valid(); }

    ImageView readable() const;

    // A writable view is trivially readable, so the conversion is implicit:
    // callers pass a destination buffer straight into a read parameter without
    // a ceremonial .readable() at every site.
    operator ImageView() const { return readable(); }  // NOLINT(google-explicit-constructor)

    static MutableImageView packed(PixelFormat format, Size size, std::uint8_t* data);
};

// An image this object owns, for the many places that just need a destination
// buffer of the right size.
class OwnedImage {
public:
    OwnedImage() = default;
    OwnedImage(PixelFormat format, Size size);

    void reset(PixelFormat format, Size size);
    void fill(std::uint8_t value);

    bool empty() const { return m_bytes.empty(); }
    PixelFormat format() const { return m_format; }
    Size size() const { return m_size; }

    std::uint8_t* data() { return m_bytes.data(); }
    const std::uint8_t* data() const { return m_bytes.data(); }
    std::size_t byteCount() const { return m_bytes.size(); }

    std::vector<std::uint8_t>& bytes() { return m_bytes; }
    const std::vector<std::uint8_t>& bytes() const { return m_bytes; }

    MutableImageView view();
    ImageView view() const;

private:
    PixelFormat m_format = PixelFormat::Unknown;
    Size m_size;
    std::vector<std::uint8_t> m_bytes;
};

std::string describe(const ImageView& image);

}  // namespace visora::core
