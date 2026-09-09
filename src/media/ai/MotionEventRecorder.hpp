#pragma once

// Writes motion events to the recording index.
//
// Separate from the websocket feed because they want different things: a
// browser wants every frame's cells so it can highlight movement live, while
// the index wants ONE row per event, opened when it starts and closed when it
// ends, with the cells accumulated over its whole life.
//
// A camera can turn this off (`motionSaveEvents`) and keep the live push. A
// busy outdoor camera produces thousands of events a day, and an installation
// that only wants alerts should not pay to store them.

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "media/ai/AiRuntime.hpp"
#include "media/ai/MotionSnapshotWriter.hpp"
#include "media/recording/RecordingRepository.hpp"

namespace visora::media {

struct MotionEventRecorderConfig {
    // Where snapshots are written. Empty turns them off, which is what a build
    // with no JPEG encoder ends up with anyway.
    std::string snapshotDir = "motion-snapshots";
    int jpegQuality = 85;
};

class MotionEventRecorder {
public:
    MotionEventRecorder(std::shared_ptr<RecordingRepository> recordings,
                        MotionEventRecorderConfig config = {});
    ~MotionEventRecorder();

    // Whether this camera's events are stored at all.
    void setSaving(const std::string& cameraId, bool saving);

    // Called for every analysed frame; opens and closes rows on the edges.
    void observe(const MotionNotice& notice);

    // Closes anything still open. An event left open reads as one that never
    // ended, which is worse than one that ended when the program did.
    void closeAll();

private:
    struct Open {
        std::string eventId;
        std::set<std::string> cells;   // accumulated over the whole event
        std::int64_t startMs = 0;
    };

    std::shared_ptr<RecordingRepository> m_recordings;
    // Owned here because this is what decides an event has started, and the
    // snapshot belongs to that decision. It writes on its own thread and
    // attaches the path to the row once the file exists.
    std::unique_ptr<MotionSnapshotWriter> m_snapshots;
    std::mutex m_mutex;
    std::map<std::string, Open> m_open;      // camera id -> open event
    std::map<std::string, bool> m_saving;    // camera id -> store events?
};

}  // namespace visora::media
