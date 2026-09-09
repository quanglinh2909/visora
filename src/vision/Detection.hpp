#pragma once

// What one inference produced, in the shape consumers already expect.
//
// These structures are a published contract, not an internal convenience: the
// Python consumer parses them off a socket. Fields may be added, but the
// meaning and the coordinate spaces of the existing ones are fixed.

#include <cstdint>
#include <string>
#include <vector>

namespace visora::vision {

struct Detection {
    // Box in the coordinate space THIS stage saw. For a stage-0 detection that
    // is the full frame; for a child it is the crop its parent stage produced.
    // The two are not comparable, which is why frame-space coordinates exist
    // separately below.
    float x1 = 0;
    float y1 = 0;
    float x2 = 0;
    float y2 = 0;

    float score = 0;
    int classId = 0;

    // Pose or face keypoints, flattened as (x, y, score) triples.
    std::vector<float> keypoints;

    // Face embedding. Empty for every non-face model.
    std::vector<float> embedding;

    // Readable name of classId when the model carries its own label table —
    // today only OCR, where it is the character this box holds.
    std::string text;

    // Segmentation mask for this detection: a kMaskGrid x kMaskGrid bitmap
    // covering exactly its box, row-major, bit set = pixel belongs to the
    // object.
    //
    // Coarse on purpose. The raw mask is a 640x640 byte image per frame — 410
    // KB — and shipping that per detection at 12 fps would dwarf everything
    // else on the wire and in the database, to show detail an overlay on a
    // video tile cannot render anyway. 32x32 bits is 128 bytes per object and
    // is plenty for a silhouette.
    static constexpr int kMaskGrid = 32;
    std::vector<std::uint8_t> maskBits;  // kMaskGrid * kMaskGrid / 8 bytes

    // Sub-detections from a later stage that is itself a detector: OCR
    // characters inside a plate crop, a plate inside a vehicle crop.
    std::vector<Detection> children;

    // The same box in ORIGINAL FRAME coordinates.
    //
    // A child's x1..y2 live in its parent's crop space, so two children from
    // different branches cannot be compared with each other — and reading a
    // two-line licence plate means exactly that: ordering characters from
    // several crops in one coordinate system. Only children carry it; a
    // stage-0 detection is already in frame space.
    float fx1 = 0;
    float fy1 = 0;
    float fx2 = 0;
    float fy2 = 0;
    bool hasFrameBox = false;

    // Which stage produced this detection (its index in the job's stage list).
    //
    // Depth in the tree cannot substitute for it: a branching job has two
    // sibling stages at the same depth, and from the result alone there would
    // be no way to tell which branch a box came from — "character read by the
    // OCR stage" versus "vehicle detail classified by another stage".
    int stage = 0;
};

// One inference result for one frame of one job.
struct Result {
    std::string cameraId;
    std::string jobId;
    std::uint64_t seq = 0;
    std::int64_t tsUs = 0;
    int origWidth = 0;
    int origHeight = 0;

    std::vector<Detection> detections;

    // Full-frame JPEG, present only when there is at least one detection.
    std::vector<std::uint8_t> fullJpeg;
};

}  // namespace visora::vision
