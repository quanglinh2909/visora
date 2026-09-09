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
#include "media/recording/RecordingRepository.hpp"

namespace visora::media {

class MotionEventRecorder {
public:
    explicit MotionEventRecorder(std::shared_ptr<RecordingRepository> recordings);

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
    std::mutex m_mutex;
    std::map<std::string, Open> m_open;      // camera id -> open event
    std::map<std::string, bool> m_saving;    // camera id -> store events?
};

}  // namespace visora::media
