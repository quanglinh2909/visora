#include "media/stream/RtspServer.hpp"

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
