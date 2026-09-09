#include "core/Image.hpp"

#include <cstring>

namespace visora::core {

const char* toString(PixelFormat format) {
    switch (format) {
        case PixelFormat::NV12:    return "NV12";
        case PixelFormat::RGB888:  return "RGB888";
        case PixelFormat::BGR888:  return "BGR888";
        case PixelFormat::GRAY8:   return "GRAY8";
        case PixelFormat::Unknown: return "Unknown";
    }
    return "Unknown";
}

int planeCount(PixelFormat format) {
    switch (format) {
        case PixelFormat::NV12:    return 2;
        case PixelFormat::RGB888:
        case PixelFormat::BGR888:
        case PixelFormat::GRAY8:   return 1;
        case PixelFormat::Unknown: return 0;
    }
    return 0;
}

int packedStride(PixelFormat format, int width, int plane) {
    if (width <= 0 || plane < 0 || plane >= planeCount(format)) return 0;
    switch (format) {
        case PixelFormat::NV12:    return width;   // Y and interleaved UV are both `width` wide
        case PixelFormat::RGB888:
        case PixelFormat::BGR888:  return width * 3;
        case PixelFormat::GRAY8:   return width;
        case PixelFormat::Unknown: return 0;
    }
    return 0;
}

std::size_t packedSize(PixelFormat format, Size size) {
    if (!size.valid()) return 0;
    const auto w = static_cast<std::size_t>(size.width);
    const auto h = static_cast<std::size_t>(size.height);
    switch (format) {
        case PixelFormat::NV12:    return w * h * 3 / 2;
        case PixelFormat::RGB888:
        case PixelFormat::BGR888:  return w * h * 3;
        case PixelFormat::GRAY8:   return w * h;
        case PixelFormat::Unknown: return 0;
    }
    return 0;
}

Planes Planes::packed(PixelFormat format, Size size) {
    Planes p;
    const int count = planeCount(format);
    if (count <= 0 || !size.valid()) return p;

    p[0].stride = packedStride(format, size.width, 0);
    p[0].offset = 0;
    if (count > 1) {
        // NV12 is the only two-plane format here: UV follows the full Y plane.
        p[1].stride = packedStride(format, size.width, 1);
        p[1].offset = static_cast<std::size_t>(p[0].stride) *
                      static_cast<std::size_t>(size.height);
    }
    return p;
}

ImageView ImageView::packed(PixelFormat format, Size size, const std::uint8_t* data) {
    ImageView v;
    v.format = format;
    v.size = size;
    v.planes = Planes::packed(format, size);
    v.data = data;
    return v;
}

MutableImageView MutableImageView::packed(PixelFormat format, Size size, std::uint8_t* data) {
    MutableImageView v;
    v.format = format;
    v.size = size;
    v.planes = Planes::packed(format, size);
    v.data = data;
    return v;
}

ImageView MutableImageView::readable() const {
    ImageView v;
    v.format = format;
    v.size = size;
    v.planes = planes;
    v.data = data;
    v.dmaFd = dmaFd;
    v.nativeHandle = nativeHandle;
    return v;
}

OwnedImage::OwnedImage(PixelFormat format, Size size) { reset(format, size); }

void OwnedImage::reset(PixelFormat format, Size size) {
    m_format = format;
    m_size = size;
    m_bytes.assign(packedSize(format, size), 0);
}

void OwnedImage::fill(std::uint8_t value) {
    // NV12 is not filled with a single byte value in any meaningful way (the
    // chroma plane would tint the whole image), so callers wanting a padded
    // NV12 destination set the planes themselves. For the single-plane formats
    // this is exactly "paint the background".
    m_bytes.assign(m_bytes.size(), value);
}

MutableImageView OwnedImage::view() {
    MutableImageView v;
    v.format = m_format;
    v.size = m_size;
    v.planes = Planes::packed(m_format, m_size);
    v.data = m_bytes.data();
    return v;
}

ImageView OwnedImage::view() const {
    ImageView v;
    v.format = m_format;
    v.size = m_size;
    v.planes = Planes::packed(m_format, m_size);
    v.data = m_bytes.data();
    return v;
}

OwnedImage copyOf(const ImageView& image) {
    OwnedImage out;
    if (!image.valid() || !image.hasCpu()) return out;

    out.reset(image.format, image.size);
    const int planes = planeCount(image.format);
    const Planes packedPlanes = Planes::packed(image.format, image.size);
    for (int plane = 0; plane < planes; ++plane) {
        const int width = packedStride(image.format, image.size.width, plane);
        // The chroma plane of a 4:2:0 format is half as tall as the luma one.
        const int height = (image.format == PixelFormat::NV12 && plane == 1)
                               ? image.size.height / 2
                               : image.size.height;
        const int sourceStride =
            image.planes[plane].stride > 0 ? image.planes[plane].stride : width;
        const std::uint8_t* from = image.data + image.planes[plane].offset;
        std::uint8_t* to = out.data() + packedPlanes[plane].offset;
        for (int row = 0; row < height; ++row) {
            std::memcpy(to + static_cast<std::size_t>(row) * width,
                        from + static_cast<std::size_t>(row) * sourceStride,
                        static_cast<std::size_t>(width));
        }
    }
    return out;
}

std::string describe(const ImageView& image) {
    std::string out = toString(image.format);
    out += ' ';
    out += std::to_string(image.size.width);
    out += 'x';
    out += std::to_string(image.size.height);
    if (image.hasDmaBuf()) {
        out += " dmabuf:";
        out += std::to_string(image.dmaFd);
    }
    if (image.hasCpu()) out += " cpu";
    return out;
}

}  // namespace visora::core
