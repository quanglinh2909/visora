#pragma once

// A camera, as the business understands one.
//
// Plain C++. No oatpp, no database, no GStreamer. That is what lets the service
// around it be tested in milliseconds against an in-memory repository, and what
// keeps a change to the HTTP shape or the storage engine from reaching in here.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "media/gst/Codec.hpp"

namespace visora::media {

// What an operator sees. Deliberately coarse: the detailed runtime states the
// predecessor once exposed leaked implementation into the UI, and were
// collapsed to these three.
enum class CameraState {
    Offline,
    Online,
    Error,
};

const char* toString(CameraState state);
CameraState cameraStateFromString(std::string_view text);

enum class RecordingMode {
    Off,
    Continuous,
    Motion,
};

const char* toString(RecordingMode mode);
RecordingMode recordingModeFromString(std::string_view text);

struct Camera {
    std::string id;            // UUID, assigned by the store
    std::string name;
    std::string rtsp;          // what the operator typed
    std::string inputRtsp;     // resolved source, usually the same
    std::string outputRtsp;    // where we restream it
    CameraState state = CameraState::Offline;
    Codec codec = Codec::Unknown;

    // Which acceleration to use: "auto", "software", "vaapi", "nvdec", "v4l2",
    // "mpp". Kept as free text because it names a CodecProvider id, and adding
    // hardware must not require a schema change.
    std::string hardware = "auto";

    bool recordingEnabled = false;
    RecordingMode recordingMode = RecordingMode::Off;

    bool motionEnabled = false;
    double motionSensitivity = 0.5;
    double motionThreshold = 0.01;
    int preMotionSeconds = 10;
    int postMotionSeconds = 20;
    int segmentSeconds = 10;

    // Analyse only keyframes in the motion branch. Cuts decode cost enormously
    // on a busy board, at the price of coarser timing.
    bool motionKeyframeOnly = false;

    // Motion grid and zones.
    //
    // The grid is how many cells motioncells divides the frame into; the zones
    // are regions of it with their own trigger level. Zones are stored as JSON
    // text rather than a child table: they are edited as a whole by a drawing
    // UI, never queried individually, and a table would buy joins nobody wants.
    int motionGridX = 32;
    int motionGridY = 32;
    std::string motionCellLevels;  // one digit per cell, row-major
    std::string motionZones;       // JSON [{r1,c1,r2,c2,level}]

    // Whether motion events are written to the database or only pushed over the
    // websocket. A busy outdoor camera produces thousands a day, and an
    // installation that only wants live alerts should not pay to store them.
    bool motionSaveEvents = true;

    // Days of recordings to keep. 0 means keep everything and let the operator
    // manage disk themselves.
    int retentionDays = 0;

    int retryCount = 0;
    std::string lastError;
    std::string lastChangedAt;
};

// What a caller may set when creating or updating. Optionals distinguish "leave
// it alone" from "set it to the default", which a plain Camera cannot express
// and which the predecessor's update path got wrong for booleans.
struct CameraChanges {
    std::optional<std::string> name;
    std::optional<std::string> rtsp;
    std::optional<std::string> hardware;
    std::optional<bool> recordingEnabled;
    std::optional<RecordingMode> recordingMode;
    std::optional<bool> motionEnabled;
    std::optional<double> motionSensitivity;
    std::optional<double> motionThreshold;
    std::optional<int> preMotionSeconds;
    std::optional<int> postMotionSeconds;
    std::optional<int> segmentSeconds;
    std::optional<bool> motionKeyframeOnly;
    std::optional<int> motionGridX;
    std::optional<int> motionGridY;
    std::optional<std::string> motionCellLevels;
    std::optional<std::string> motionZones;
    std::optional<bool> motionSaveEvents;
    std::optional<int> retentionDays;
};

// Applies the changes in place. Returns what actually differed, so a caller can
// decide whether a change is worth tearing a live pipeline down for.
struct CameraDiff {
    bool sourceChanged = false;     // rtsp or hardware: needs a pipeline rebuild
    bool recordingChanged = false;
    bool motionChanged = false;
    bool cosmeticOnly() const { return !sourceChanged && !recordingChanged && !motionChanged; }
};

CameraDiff apply(const CameraChanges& changes, Camera& camera);

}  // namespace visora::media
