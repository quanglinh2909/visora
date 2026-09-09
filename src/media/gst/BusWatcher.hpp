#pragma once

// Delivers a pipeline's bus messages, without a GLib main loop.
//
// WHY THIS EXISTS, because the alternative looks correct and does nothing:
// gst_bus_add_watch() attaches the callback to GLib's DEFAULT main context, and
// this program never iterates it — the RTSP server runs its own private
// context, and the main thread runs an HTTP server. A watch added that way is
// installed, returns a valid id, and is never called.
//
// The symptom is silence rather than an error. Recording wrote every segment to
// disk and indexed none of them, because the fragment-opened and
// fragment-closed messages were sitting unread on a bus. The shared camera
// source never noticed a camera dropping, because its error and EOS handling
// was on the same dead watch.
//
// So: one thread per pipeline, popping the bus with a timeout. The handler runs
// on that thread — not on a streaming thread — so it may do slow work such as a
// database write without stalling video.

#include <atomic>
#include <functional>
#include <string>
#include <thread>

typedef struct _GstElement GstElement;
typedef struct _GstMessage GstMessage;

namespace visora::media {

class BusWatcher {
public:
    // Called for every message on the bus, on the watcher's own thread.
    using Handler = std::function<void(GstMessage*)>;

    BusWatcher() = default;
    ~BusWatcher();

    BusWatcher(const BusWatcher&) = delete;
    BusWatcher& operator=(const BusWatcher&) = delete;

    // Starts watching `pipeline`. The pipeline must outlive the watcher — call
    // stop() before tearing it down.
    void start(GstElement* pipeline, std::string label, Handler handler);
    void stop();

private:
    void loop();

    GstElement* m_pipeline = nullptr;
    std::string m_label;
    Handler m_handler;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

// The message text of a GST_MESSAGE_ERROR, debug string included. Written once
// here because every pipeline in the program wants the same thing and the
// parse/free dance is easy to leak.
std::string errorTextOf(GstMessage* message);

}  // namespace visora::media
