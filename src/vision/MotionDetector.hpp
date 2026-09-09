#pragma once

// Motion detection, as a consumer of the frames the AI pipeline already has.
//
// WHY IT LIVES HERE rather than as a GStreamer branch of its own. The
// predecessor measured its `mppvideodec ! videoscale ! videoconvert !
// motioncells` branch at about 25% of a core PER 1080p CAMERA:
//
//     decode                    1.3%   — a second time; AI had already decoded it
//     videoscale + videoconvert 15.1%  — scaling 1080p on the CPU
//     motioncells               8.8%
//
// All three vanish here. The frame has already been decoded once for the
// camera, and already scaled by the image layer — on an accelerator where there
// is one. What is left is subtracting two frames on a fixed sampling grid: a
// few hundred thousand points a second, an order of magnitude cheaper.
//
// motioncells is deliberately not reused: it takes RGB (another conversion),
// and its mask reads only the first 255 cells — a limit that made large grids
// go quietly wrong on the predecessor.

#include <cstdint>
#include <string>
#include <vector>

#include "core/Geometry.hpp"
#include "core/Image.hpp"

namespace visora::vision {

// The sampling grid, FIXED and independent of the frame size.
//
// Load-bearing: comparing two frames means comparing point with point, and a
// buffer sized from the frame would compare the wrong points the moment a
// camera changes resolution mid-run.
inline constexpr int kMotionSampleWidth = 160;
inline constexpr int kMotionSampleHeight = 120;

// How much a sampled point must change to count as moved. Below this is sensor
// noise, which every camera has and which would otherwise light up the whole
// grid at night.
inline constexpr int kMotionPixelDelta = 18;

// What fraction of a cell's points must have moved for the cell to count. A
// person crossing a cell only covers part of it.
inline constexpr unsigned kMotionCellPercent = 8;

struct MotionGrid {
    int columns = 32;
    int rows = 32;
};

class MotionDetector {
public:
    explicit MotionDetector(MotionGrid grid = {});

    // Analyses one frame against the previous one.
    //
    // `content` is where the real picture sits inside `frame`, so the grid is
    // laid over the IMAGE and not over the letterbox padding — otherwise the
    // edge cells never move and the zones an operator drew on the picture sit
    // over the wrong cells.
    //
    // NV12 and GRAY8 are read straight from the Y plane, which already IS
    // luminance — so the format a hardware decoder produces needs no conversion
    // at all. Packed RGB/BGR is accepted too, at the cost of a multiply per
    // sampled point.
    //
    // Returns the cells that moved as "row:col,row:col", the format the
    // predecessor's zone evaluation reads, EMPTY when nothing did. The empty
    // answer matters as much as a full one: it is what closes an open event.
    std::string analyse(const core::ImageView& frame, core::Rect content);

    // Whether a previous frame exists to compare against. The first frame after
    // a start or a resolution change can only be recorded, not judged.
    bool primed() const { return m_primed; }

    void reset();

private:
    MotionGrid m_grid;
    std::vector<std::uint8_t> m_previous;
    std::vector<std::uint8_t> m_current;
    std::vector<unsigned> m_moved;
    std::vector<unsigned> m_total;
    bool m_primed = false;
};

// A rectangular region of the grid with its own trigger level.
//
// Levels exist because one threshold cannot serve a whole scene: a doorway
// should fire on a person crossing it, while a tree at the edge of the frame
// should not fire on wind. Level N means "N tenths of THIS ZONE's cells moved
// in one frame".
struct MotionZone {
    int row1 = 0, col1 = 0, row2 = 0, col2 = 0;  // inclusive
    int level = 1;                                // 1..10
};

// Whether any zone fired, given the cells that moved.
//
// A camera with NO zones never fires. That is deliberate and was measured on
// the predecessor: 11 of 12 cameras had empty zones, and the whole motion
// branch ran on all of them to throw every result away.
bool zonesTriggered(const std::string& movedCells, const std::vector<MotionZone>& zones,
                    MotionGrid grid);

// Parses the zone JSON stored on a camera: [{"r1":..,"c1":..,"r2":..,"c2":..,"level":..}]
//
// Hand-parsed rather than through a JSON library because this lives in the
// domain, which has no JSON dependency, and the shape is fixed. Anything it
// cannot read yields no zones — which means the camera does not fire, rather
// than firing on everything.
std::vector<MotionZone> parseMotionZones(const std::string& json);

}  // namespace visora::vision
