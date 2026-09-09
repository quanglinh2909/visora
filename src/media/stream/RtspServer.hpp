#pragma once

// The RTSP server every camera is restreamed through.
//
// One server with a mount point per camera, not one server per camera: a
// GstRTSPServer owns a listening socket and a main loop, and running dozens
// would mean dozens of ports and dozens of threads for no benefit.
//
// All GStreamer work happens on this object's own main loop thread. Public
// methods are callable from any thread and marshal onto it, because
// gst_rtsp_server calls from a request handler thread while the loop is
// running are a source of very hard-to-reproduce crashes.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/Result.hpp"
#include "media/source/EncodedSource.hpp"

namespace visora::media {

class RtspServer {
public:
    RtspServer(std::string host, std::uint16_t port);
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    core::Status start();
    void stop();

    // Publishes `launch` at `path`. Replaces any existing mount at that path,
    // so a camera whose source changed is republished rather than duplicated.
    // `launch` must be the parenthesised form.
    core::Status publish(const std::string& path, const std::string& launch);

    // Publishes a mount fed from the camera's SHARED source rather than from a
    // connection of its own.
    //
    // `launch` must contain an appsrc named `appsrcName`; every client of this
    // mount shares one media and therefore one attachment to the source, so a
    // camera watched by ten browsers is still pulled once.
    //
    // The provider is called when a client first asks for the stream, not now:
    // an on-demand mount that nobody watches must not hold a connection to the
    // camera open, which is the property the per-mount rtspsrc had and this
    // must not lose.
    using SourceProvider = std::function<std::shared_ptr<EncodedSource>()>;
    core::Status publishFromSource(const std::string& path, const std::string& launch,
                                   const std::string& appsrcName, SourceProvider provider);

    void unpublish(const std::string& path);

    // The port actually bound, which differs from the requested one when 0 was
    // asked for. Tests use that to avoid fighting over a fixed port.
    std::uint16_t boundPort() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace visora::media
