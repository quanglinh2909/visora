#include "media/stream/RtspServer.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <thread>

#include "core/Log.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "rtsp";

void ensureGstInit() {
    static std::once_flag once;
    std::call_once(once, [] { gst_init(nullptr, nullptr); });
}

// One media's attachment to a camera's shared source.
//
// Lives as long as the GstRTSPMedia it feeds — it is hung on the media as
// object data, so a media the server tears down takes its attachment with it
// and nothing has to remember to detach.
//
// Held by shared_ptr and captured by the sink lambda: removeSink does not wait
// for a callback already in flight, so the state has to outlive the removal.
// `enabled` under the mutex is what makes that safe rather than merely
// unlikely — a push either completes before the teardown or sees the flag and
// does nothing.
struct SourceFeed {
    std::shared_ptr<EncodedSource> source;
    std::mutex mutex;
    GstElement* appsrc = nullptr;
    std::uint64_t sinkId = 0;
    bool enabled = false;
    bool capsSet = false;
    // The first timestamp this media saw, so its timeline starts at zero.
    GstClockTime basePts = GST_CLOCK_TIME_NONE;

    void push(GstBuffer* buffer, GstCaps* caps) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!enabled || appsrc == nullptr) return;

        if (!capsSet && caps != nullptr) {
            gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
            capsSet = true;
        }

        // A shallow copy: the buffer is shared with every other consumer of
        // this source and appsrc is about to write a timestamp onto it. The
        // payload is shared, so this is cheap.
        GstBuffer* out = gst_buffer_make_writable(gst_buffer_ref(buffer));

        // REBASED, not restamped.
        //
        // The source's timestamps come from another pipeline's clock with
        // another base, so they cannot be passed through — but the SPACING
        // between them is the camera's real frame timing, and that is worth
        // keeping. Subtracting the first one this media saw starts its timeline
        // at zero while preserving that spacing exactly.
        //
        // Letting do-timestamp stamp them with the local clock instead was
        // measured to give two frames the same timestamp when a burst arrives
        // together — the keyframe and the frames the jitter buffer released
        // behind it — and ffmpeg rejects the duplicate DTS.
        const GstClockTime pts = GST_BUFFER_PTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(pts)) {
            if (!GST_CLOCK_TIME_IS_VALID(basePts) || pts < basePts) basePts = pts;
            GST_BUFFER_PTS(out) = pts - basePts;
            const GstClockTime dts = GST_BUFFER_DTS(buffer);
            // H.264 and H.265 from a camera have no B-frames in practice, so
            // DTS equals PTS; taken from the buffer when it has one anyway.
            GST_BUFFER_DTS(out) =
                GST_CLOCK_TIME_IS_VALID(dts) && dts >= basePts ? dts - basePts : GST_BUFFER_PTS(out);
        } else {
            // No timestamp at all: cleared so appsrc's do-timestamp applies
            // this pipeline's clock, which only stamps a buffer that has none.
            GST_BUFFER_PTS(out) = GST_CLOCK_TIME_NONE;
            GST_BUFFER_DTS(out) = GST_CLOCK_TIME_NONE;
        }
        GST_BUFFER_DURATION(out) = GST_CLOCK_TIME_NONE;
        gst_app_src_push_buffer(GST_APP_SRC(appsrc), out);
    }

    void detach() {
        std::shared_ptr<EncodedSource> held;
        std::uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            enabled = false;
            basePts = GST_CLOCK_TIME_NONE;
            held = source;
            id = sinkId;
            sinkId = 0;
            if (appsrc != nullptr) {
                gst_object_unref(appsrc);
                appsrc = nullptr;
            }
        }
        if (held && id != 0) held->removeSink(id);
    }
};

// What a mount needs in order to build a feed when a client arrives.
struct MountSource {
    std::string appsrcName;
    RtspServer::SourceProvider provider;
};

}  // namespace

struct RtspServer::Impl {
    std::string host;
    std::uint16_t requestedPort = 0;
    std::uint16_t boundPort = 0;

    GMainContext* context = nullptr;
    GMainLoop* loop = nullptr;
    GstRTSPServer* server = nullptr;
    guint sourceId = 0;
    std::thread loopThread;

    std::mutex mutex;
    std::condition_variable ready;
    bool running = false;
    std::map<std::string, bool> mounts;

    // Runs `work` on the loop thread and waits for it.
    //
    // Every gst_rtsp_server call goes through here. Calling from a request
    // handler thread while the loop is running corrupts internal state in ways
    // that surface much later, as a crash in an unrelated camera.
    void invoke(std::function<void()> work) {
        if (!running) {
            work();
            return;
        }
        std::mutex done;
        std::condition_variable finished;
        bool complete = false;

        struct Call {
            std::function<void()> work;
            std::mutex* done;
            std::condition_variable* finished;
            bool* complete;
        };
        auto* call = new Call{std::move(work), &done, &finished, &complete};

        GSource* source = g_idle_source_new();
        g_source_set_callback(
            source,
            [](gpointer data) -> gboolean {
                auto* c = static_cast<Call*>(data);
                c->work();
                {
                    std::lock_guard<std::mutex> lock(*c->done);
                    *c->complete = true;
                }
                c->finished->notify_all();
                delete c;
                return G_SOURCE_REMOVE;
            },
            call, nullptr);
        g_source_attach(source, context);
        g_source_unref(source);

        std::unique_lock<std::mutex> lock(done);
        finished.wait(lock, [&] { return complete; });
    }
};

RtspServer::RtspServer(std::string host, std::uint16_t port) : m_impl(new Impl) {
    m_impl->host = std::move(host);
    m_impl->requestedPort = port;
}

RtspServer::~RtspServer() { stop(); }

core::Status RtspServer::start() {
    ensureGstInit();
    if (m_impl->running) return {};

    m_impl->context = g_main_context_new();
    m_impl->loop = g_main_loop_new(m_impl->context, FALSE);

    // The context must be the thread default while the server is created, or it
    // attaches its sources to the global context and never sees them again.
    g_main_context_push_thread_default(m_impl->context);
    m_impl->server = gst_rtsp_server_new();
    gst_rtsp_server_set_address(m_impl->server, m_impl->host.c_str());
    gst_rtsp_server_set_service(m_impl->server,
                                std::to_string(m_impl->requestedPort).c_str());
    m_impl->sourceId = gst_rtsp_server_attach(m_impl->server, m_impl->context);
    g_main_context_pop_thread_default(m_impl->context);

    if (m_impl->sourceId == 0) {
        g_object_unref(m_impl->server);
        m_impl->server = nullptr;
        g_main_loop_unref(m_impl->loop);
        m_impl->loop = nullptr;
        g_main_context_unref(m_impl->context);
        m_impl->context = nullptr;
        return core::internalError("could not bind RTSP server to " + m_impl->host + ':' +
                                   std::to_string(m_impl->requestedPort));
    }

    m_impl->boundPort = static_cast<std::uint16_t>(gst_rtsp_server_get_bound_port(m_impl->server));
    m_impl->running = true;
    m_impl->loopThread = std::thread([impl = m_impl.get()] {
        g_main_context_push_thread_default(impl->context);
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
        }
        impl->ready.notify_all();
        g_main_loop_run(impl->loop);
        g_main_context_pop_thread_default(impl->context);
    });

    VS_INFO(kCategory) << "RTSP server on " << m_impl->host << ':' << m_impl->boundPort;
    return {};
}

void RtspServer::stop() {
    if (!m_impl->running) return;
    m_impl->running = false;

    if (m_impl->loop != nullptr) g_main_loop_quit(m_impl->loop);
    if (m_impl->loopThread.joinable()) m_impl->loopThread.join();

    if (m_impl->sourceId != 0) {
        GSource* source = g_main_context_find_source_by_id(m_impl->context, m_impl->sourceId);
        if (source != nullptr) g_source_destroy(source);
        m_impl->sourceId = 0;
    }
    if (m_impl->server != nullptr) {
        g_object_unref(m_impl->server);
        m_impl->server = nullptr;
    }
    if (m_impl->loop != nullptr) {
        g_main_loop_unref(m_impl->loop);
        m_impl->loop = nullptr;
    }
    if (m_impl->context != nullptr) {
        g_main_context_unref(m_impl->context);
        m_impl->context = nullptr;
    }
    m_impl->mounts.clear();
    VS_INFO(kCategory) << "RTSP server stopped";
}

core::Status RtspServer::publish(const std::string& path, const std::string& launch) {
    if (m_impl->server == nullptr) return core::internalError("RTSP server is not running");
    if (launch.empty()) return core::invalidArgument("empty launch description for " + path);

    core::Status result;
    m_impl->invoke([&] {
        GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(m_impl->server);
        // Removing first makes republishing idempotent: a camera whose source
        // changed gets one mount, not two.
        gst_rtsp_mount_points_remove_factory(mounts, path.c_str());

        GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_launch(factory, launch.c_str());
        // Shared: several viewers of one camera pull the source once. Without
        // this every browser tab opens its own connection to the camera, and
        // cameras have a hard limit on concurrent sessions.
        gst_rtsp_media_factory_set_shared(factory, TRUE);
        gst_rtsp_mount_points_add_factory(mounts, path.c_str(), factory);
        g_object_unref(mounts);

        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->mounts[path] = true;
    });

    VS_INFO(kCategory) << "published " << path;
    return result;
}

namespace {

// Builds the attachment when a client first asks for this mount.
//
// gst_rtsp_media_factory "media-configure" fires once per media, and a SHARED
// factory makes one media for every client of that mount — so one camera
// watched by ten browsers attaches to the shared source exactly once.
void onMediaConfigure(GstRTSPMediaFactory*, GstRTSPMedia* media, gpointer user) {
    const auto* mount = static_cast<const MountSource*>(user);
    if (mount == nullptr || !mount->provider) return;

    GstElement* element = gst_rtsp_media_get_element(media);
    if (element == nullptr) return;
    // Down into the child bins, not up into the parents: _recurse_up would keep
    // looking outside this media, and there is one of these per mount.
    GstElement* appsrc = gst_bin_get_by_name(GST_BIN(element), mount->appsrcName.c_str());
    gst_object_unref(element);
    if (appsrc == nullptr) {
        VS_WARN(kCategory) << "no appsrc named " << mount->appsrcName
                           << " in the mount's pipeline; nothing will be restreamed";
        return;
    }

    auto source = mount->provider();
    if (!source) {
        // The camera is not reachable, or its codec is not known yet. The
        // client gets a mount that produces nothing rather than a crash, and
        // the next attempt builds a new media with a fresh look at the source.
        VS_WARN(kCategory) << "no shared source available for this mount yet";
        gst_object_unref(appsrc);
        return;
    }

    auto feed = std::make_shared<SourceFeed>();
    feed->source = std::move(source);
    feed->appsrc = appsrc;  // the ref from get_by_name is handed over
    feed->enabled = true;
    feed->sinkId = feed->source->addSink(
        [feed](GstBuffer* buffer, GstCaps* caps) { feed->push(buffer, caps); });

    // Hung on the media, so the media being torn down detaches the feed. A
    // separate registry would be a second place that has to be told about
    // every client that goes away.
    g_object_set_data_full(G_OBJECT(media), "visora-source-feed",
                           new std::shared_ptr<SourceFeed>(feed), [](gpointer data) {
                               auto* held = static_cast<std::shared_ptr<SourceFeed>*>(data);
                               (*held)->detach();
                               delete held;
                           });
}

}  // namespace

core::Status RtspServer::publishFromSource(const std::string& path, const std::string& launch,
                                           const std::string& appsrcName,
                                           SourceProvider provider) {
    if (m_impl->server == nullptr) return core::internalError("RTSP server is not running");
    if (launch.empty()) return core::invalidArgument("empty launch description for " + path);
    if (!provider) return core::invalidArgument("no source provider for " + path);

    m_impl->invoke([&] {
        GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(m_impl->server);
        gst_rtsp_mount_points_remove_factory(mounts, path.c_str());

        GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_launch(factory, launch.c_str());
        // Shared: every client of this mount gets one media and therefore one
        // attachment to the camera's source.
        gst_rtsp_media_factory_set_shared(factory, TRUE);

        // Owned by the factory, so it outlives every media the factory builds
        // and dies with the mount.
        auto* mount = new MountSource{appsrcName, provider};
        g_signal_connect_data(
            factory, "media-configure", G_CALLBACK(onMediaConfigure), mount,
            [](gpointer data, GClosure*) { delete static_cast<MountSource*>(data); },
            static_cast<GConnectFlags>(0));

        gst_rtsp_mount_points_add_factory(mounts, path.c_str(), factory);
        g_object_unref(mounts);

        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->mounts[path] = true;
    });

    VS_INFO(kCategory) << "published " << path << " from the shared source";
    return {};
}

void RtspServer::unpublish(const std::string& path) {
    if (m_impl->server == nullptr) return;
    m_impl->invoke([&] {
        GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(m_impl->server);
        gst_rtsp_mount_points_remove_factory(mounts, path.c_str());
        g_object_unref(mounts);

        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->mounts.erase(path);
    });
    VS_INFO(kCategory) << "unpublished " << path;
}

std::uint16_t RtspServer::boundPort() const { return m_impl->boundPort; }

}  // namespace visora::media
