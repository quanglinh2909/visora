#include "vision/MotionDetector.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace visora::vision {
namespace {

// Approximate luminance, (2R + 5G + B) / 8. Integer, and quite good enough to
// compare two consecutive frames — the exact coefficients would cost a multiply
// per point for no change in what is detected.
inline std::uint8_t luma(const std::uint8_t* pixel) {
    return static_cast<std::uint8_t>((2 * pixel[0] + 5 * pixel[1] + pixel[2]) >> 3);
}

}  // namespace

MotionDetector::MotionDetector(MotionGrid grid) : m_grid(grid) {
    m_grid.columns = std::max(1, m_grid.columns);
    m_grid.rows = std::max(1, m_grid.rows);
}

void MotionDetector::reset() {
    m_primed = false;
    std::fill(m_previous.begin(), m_previous.end(), 0);
}

std::string MotionDetector::analyse(const core::ImageView& frame, core::Rect content) {
    if (!frame.valid() || !frame.hasCpu()) return {};

    // NV12 and GRAY8 are read STRAIGHT FROM THE Y PLANE, which already is
    // luminance — the only thing motion detection wants. So the format a
    // hardware decoder produces costs no conversion at all here, which is the
    // single largest saving available: the predecessor spent 15% of a core per
    // 1080p camera converting frames for its motion branch.
    const bool packedRgb = frame.format == core::PixelFormat::RGB888 ||
                           frame.format == core::PixelFormat::BGR888;
    const bool lumaPlane = frame.format == core::PixelFormat::NV12 ||
                           frame.format == core::PixelFormat::GRAY8;
    if (!packedRgb && !lumaPlane) return {};

    if (content.width <= 0 || content.height <= 0) {
        content = core::Rect{0, 0, frame.size.width, frame.size.height};
    }

    const std::size_t points =
        static_cast<std::size_t>(kMotionSampleWidth) * kMotionSampleHeight;
    if (m_previous.size() != points) {
        m_previous.assign(points, 0);
        m_primed = false;
    }
    m_current.resize(points);

    const int bytesPerPixel = packedRgb ? 3 : 1;
    const int stride = frame.planes[0].stride > 0 ? frame.planes[0].stride
                                                  : frame.size.width * bytesPerPixel;

    for (int sy = 0; sy < kMotionSampleHeight; ++sy) {
        const int y = std::min(content.y + content.height * sy / kMotionSampleHeight,
                               frame.size.height - 1);
        const std::uint8_t* line =
            frame.data + frame.planes[0].offset + static_cast<std::size_t>(y) * stride;
        std::uint8_t* out = m_current.data() + static_cast<std::size_t>(sy) * kMotionSampleWidth;
        for (int sx = 0; sx < kMotionSampleWidth; ++sx) {
            const int x = std::min(content.x + content.width * sx / kMotionSampleWidth,
                                   frame.size.width - 1);
            const std::uint8_t* pixel = line + static_cast<std::size_t>(x) * bytesPerPixel;
            out[sx] = packedRgb ? luma(pixel) : *pixel;
        }
    }

    std::string cells;
    if (m_primed) {
        const std::size_t cellCount =
            static_cast<std::size_t>(m_grid.columns) * m_grid.rows;
        m_moved.assign(cellCount, 0u);
        m_total.assign(cellCount, 0u);

        for (int sy = 0; sy < kMotionSampleHeight; ++sy) {
            const std::size_t row =
                static_cast<std::size_t>(m_grid.rows) * sy / kMotionSampleHeight;
            const std::uint8_t* current =
                m_current.data() + static_cast<std::size_t>(sy) * kMotionSampleWidth;
            const std::uint8_t* previous =
                m_previous.data() + static_cast<std::size_t>(sy) * kMotionSampleWidth;
            for (int sx = 0; sx < kMotionSampleWidth; ++sx) {
                const std::size_t column =
                    static_cast<std::size_t>(m_grid.columns) * sx / kMotionSampleWidth;
                const std::size_t index = row * m_grid.columns + column;
                ++m_total[index];
                const int delta = static_cast<int>(current[sx]) - static_cast<int>(previous[sx]);
                if (delta > kMotionPixelDelta || delta < -kMotionPixelDelta) ++m_moved[index];
            }
        }

        cells.reserve(256);
        for (std::size_t index = 0; index < cellCount; ++index) {
            if (m_total[index] == 0) continue;
            // Counting CHANGED POINTS rather than averaging the cell. A person
            // crossing covers only part of a cell, and an average dilutes that
            // change until the cell never fires.
            if (m_moved[index] * 100u < m_total[index] * kMotionCellPercent) continue;
            if (!cells.empty()) cells += ',';
            cells += std::to_string(index / m_grid.columns);
            cells += ':';
            cells += std::to_string(index % m_grid.columns);
        }
    }

    m_previous.swap(m_current);
    m_primed = true;
    return cells;
}

std::vector<MotionZone> parseMotionZones(const std::string& json) {
    std::vector<MotionZone> zones;
    if (json.empty()) return zones;

    // A deliberately small reader for a fixed shape. Anything it does not
    // understand yields no zones, so a malformed value makes the camera quiet
    // rather than making it fire on everything.
    std::size_t at = 0;
    while (at < json.size()) {
        const auto open = json.find('{', at);
        if (open == std::string::npos) break;
        const auto close = json.find('}', open);
        if (close == std::string::npos) break;

        const std::string object = json.substr(open + 1, close - open - 1);
        const auto field = [&object](const char* name, int fallback) {
            const std::string key = std::string("\"") + name + "\"";
            const auto keyAt = object.find(key);
            if (keyAt == std::string::npos) return fallback;
            auto valueAt = object.find(':', keyAt + key.size());
            if (valueAt == std::string::npos) return fallback;
            ++valueAt;
            while (valueAt < object.size() &&
                   std::isspace(static_cast<unsigned char>(object[valueAt]))) {
                ++valueAt;
            }
            if (valueAt >= object.size()) return fallback;
            return std::atoi(object.c_str() + valueAt);
        };

        MotionZone zone;
        zone.row1 = field("r1", 0);
        zone.col1 = field("c1", 0);
        zone.row2 = field("r2", 0);
        zone.col2 = field("c2", 0);
        zone.level = std::clamp(field("level", 1), 1, 10);
        if (zone.row2 < zone.row1) std::swap(zone.row1, zone.row2);
        if (zone.col2 < zone.col1) std::swap(zone.col1, zone.col2);
        zones.push_back(zone);

        at = close + 1;
    }
    return zones;
}

bool zonesTriggered(const std::string& movedCells, const std::vector<MotionZone>& zones,
                    MotionGrid grid) {
    // No zones, no events. Deliberate: the predecessor measured 11 of 12
    // cameras with empty zones, every one of them running the whole motion
    // branch and discarding every result.
    if (zones.empty() || movedCells.empty()) return false;

    std::vector<bool> moved(static_cast<std::size_t>(std::max(1, grid.columns)) *
                                std::max(1, grid.rows),
                            false);
    std::size_t at = 0;
    while (at < movedCells.size()) {
        const auto comma = movedCells.find(',', at);
        const std::string token =
            movedCells.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        const auto colon = token.find(':');
        if (colon != std::string::npos) {
            const int row = std::atoi(token.c_str());
            const int column = std::atoi(token.c_str() + colon + 1);
            if (row >= 0 && row < grid.rows && column >= 0 && column < grid.columns) {
                moved[static_cast<std::size_t>(row) * grid.columns + column] = true;
            }
        }
        if (comma == std::string::npos) break;
        at = comma + 1;
    }

    for (const MotionZone& zone : zones) {
        int cells = 0;
        int hits = 0;
        for (int row = zone.row1; row <= zone.row2; ++row) {
            for (int column = zone.col1; column <= zone.col2; ++column) {
                if (row < 0 || row >= grid.rows || column < 0 || column >= grid.columns) {
                    continue;
                }
                ++cells;
                if (moved[static_cast<std::size_t>(row) * grid.columns + column]) ++hits;
            }
        }
        if (cells == 0) continue;
        // Level N means N tenths of THIS ZONE's cells. Relative to the zone,
        // not to the frame, so a small zone stays as sensitive as a large one.
        if (hits * 10 >= cells * zone.level) return true;
    }
    return false;
}

}  // namespace visora::vision
