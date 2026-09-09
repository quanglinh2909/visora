#include "media/webrtc/WhepSession.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <vector>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/sdp/sdp.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#undef GST_USE_UNSTABLE_API

#include "core/Log.hpp"
#include "core/Time.hpp"
#include "media/gst/BusWatcher.hpp"
#include "media/webrtc/Sdp.hpp"

namespace visora::media {
namespace {

constexpr const char* kCategory = "webrtc";

// libnice's own defaults ask STUN three times with a 500 ms timeout, which adds
// 2.3 seconds to every WHEP request on a LAN where the answer is already known.
constexpr guint kStunInitialTimeoutMs = 200;
constexpr guint kStunMaxRetransmissions = 2;

std::uint32_t makeSsrc() {
    // Any non-zero 32-bit value. It only has to be stable within the session
    // and unique enough not to collide with another stream on the same
    // transport, and bundle-policy=max-bundle means there is only one.
    static std::atomic<std::uint32_t> counter{0x5A000000u};
    return counter.fetch_add(1u) | 1u;
}

// Replaces an mDNS host name in an ICE candidate with an address that can be
// reached.
//
// Chrome hides the local IP behind "<uuid>.local" for privacy, and libnice does
// not resolve mDNS. The address the HTTP request came from is the same machine,
// so it is the right substitution — and where it is wrong, the candidate was
// unusable anyway.
std::string rewriteMdnsCandidate(const std::string& candidate, const std::string& hint) {
    if (hint.empty()) return candidate;

    std::istringstream in(candidate);
    std::vector<std::string> tokens;
    for (std::string token; in >> token;) tokens.push_back(token);
    // "candidate:<foundation> <component> <transport> <priority> <address> ..."
    if (tokens.size() < 6) return candidate;

    const std::string& address = tokens[4];
    const std::string suffix = ".local";
    if (address.size() <= suffix.size() ||
        address.compare(address.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return candidate;
    }

    tokens[4] = hint;
    std::string out;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i != 0) out += ' ';
        out += tokens[i];
    }
    return out;
}

}  // namespace

struct WhepSession::Impl {
    WhepSession* owner = nullptr;
    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstElement* webrtc = nullptr;
    BusWatcher bus;
    std::uint64_t sinkId = 0;

    std::mutex mutex;
    bool enabled = false;
    bool capsSet = false;

    // ICE gathering, signalled from webrtcbin's notify callback.
    std::mutex gatherMutex;
    std::condition_variable gathered;
    bool gatheredFlag = false;

    std::atomic<bool> alive{false};
    std::atomic<std::uint64_t> rtpPackets{0};
    // Whether the browser ever answered, and whether it has since gone. Taken
    // from webrtcbin rather than inferred: outgoing packets say nothing about
    // whether anyone is still receiving them.
    std::atomic<bool> everConnected{false};
    std::atomic<bool> peerGone{false};
    std::int64_t startedAtMs = 0;

    bool transcoded = false;
    Codec sendCodec = Codec::Unknown;
    int browserPayloadType = 96;
    int payloaderPayloadType = 96;

    void push(GstBuffer* buffer, GstCaps* caps);
};

void WhepSession::Impl::push(GstBuffer* buffer, GstCaps* caps) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!enabled || !appsrc) return;

    if (!capsSet && caps) {
        gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
        capsSet = true;
    }

    // A shallow copy, because the buffer is shared with every other consumer of
    // this source and appsrc is about to write a timestamp onto it.
    GstBuffer* out = gst_buffer_make_writable(gst_buffer_ref(buffer));
    // Cleared so do-timestamp=true applies THIS pipeline's clock. The source's
    // PTS comes from a different clock; left in place, the payloader computes
    // RTP timestamps the browser cannot use — connected, and black.
    GST_BUFFER_PTS(out) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(out) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(out) = GST_CLOCK_TIME_NONE;
    gst_app_src_push_buffer(GST_APP_SRC(appsrc), out);
}

namespace {

void onConnectionStateChanged(GstElement* webrtc, GParamSpec*, gpointer user) {
    auto* impl = static_cast<WhepSession::Impl*>(user);
    GstWebRTCPeerConnectionState state = GST_WEBRTC_PEER_CONNECTION_STATE_NEW;
    g_object_get(webrtc, "connection-state", &state, nullptr);

    switch (state) {
        case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED:
            impl->everConnected.store(true);
            break;
        case GST_WEBRTC_PEER_CONNECTION_STATE_FAILED:
        case GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED:
            // A closed tab reaches us as this and nothing else — there is no
            // DELETE and no error.
            impl->peerGone.store(true);
            break;
        default:
            // "disconnected" is recoverable: a phone changing network comes
            // back within seconds, and reaping there would drop a viewer who
            // was merely walking between access points.
            break;
    }
}

void onIceGatheringChanged(GstElement* webrtc, GParamSpec*, gpointer user) {
    auto* impl = static_cast<WhepSession::Impl*>(user);
    GstWebRTCICEGatheringState state = GST_WEBRTC_ICE_GATHERING_STATE_NEW;
    g_object_get(webrtc, "ice-gathering-state", &state, nullptr);
    if (state != GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) return;

    std::lock_guard<std::mutex> lock(impl->gatherMutex);
    impl->gatheredFlag = true;
    impl->gathered.notify_all();
}

// Counts RTP packets actually reaching webrtcbin, and rewrites the payload type
// byte when the browser asked for one the payloader cannot emit.
//
// The caps rewrite alone is NOT enough: the packets still carry the payloader's
// own number, and a browser drops every packet whose payload type is not the
// one in the answer. Black screen, everything "connected".
GstPadProbeReturn onRtpOut(GstPad*, GstPadProbeInfo* info, gpointer user) {
    auto* impl = static_cast<WhepSession::Impl*>(user);
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    impl->rtpPackets.fetch_add(1);

    if (impl->browserPayloadType == impl->payloaderPayloadType) return GST_PAD_PROBE_OK;

    GstBuffer* writable = gst_buffer_make_writable(buffer);
    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (gst_rtp_buffer_map(writable, GST_MAP_READWRITE, &rtp)) {
        gst_rtp_buffer_set_payload_type(&rtp, static_cast<guint8>(impl->browserPayloadType));
        gst_rtp_buffer_unmap(&rtp);
    }
    GST_PAD_PROBE_INFO_DATA(info) = writable;
    return GST_PAD_PROBE_OK;
}

void handleBusMessage(WhepSession::Impl* impl, GstMessage* message) {
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR:
            VS_WARN(kCategory) << impl->owner->id() << ": " << errorTextOf(message);
            impl->alive.store(false);
            break;
        case GST_MESSAGE_EOS:
            impl->alive.store(false);
            break;
        default:
            break;
    }
}

}  // namespace

WhepSession::WhepSession(std::string sessionId, std::string cameraId,
                         std::shared_ptr<EncodedSource> source, WhepConfig config)
    : m_sessionId(std::move(sessionId)),
      m_cameraId(std::move(cameraId)),
      m_source(std::move(source)),
      m_config(std::move(config)),
      m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;
}

WhepSession::~WhepSession() { stop(); }

core::Result<std::string> WhepSession::start(const std::string& offerSdp,
                                             const std::string& clientAddressHint) {
    if (!m_source) return core::invalidArgument("no source for this camera");
    if (!hasVideoMedia(offerSdp)) return core::invalidArgument("the offer has no video");

    // Pin the browser to the DTLS "active" role BEFORE handing webrtcbin the
    // offer, so webrtcbin chooses "passive" for itself.
    //
    // Why passive matters: Chrome hides its local IP behind an mDNS name that
    // libnice cannot resolve. As the DTLS client we would have to send the
    // first ClientHello to an address we do not know — into nothing — and the
    // handshake hangs for ever even though ICE reports completed, because ICE
    // succeeded via a peer-reflexive candidate learned from the browser's own
    // packets. Passive means the browser speaks first and we simply reply to
    // wherever that packet came from.
    //
    // It has to be the OFFER. webrtcbin derives its role from the remote
    // description, so editing "a=setup:" in the answer changes the text and not
    // the behaviour. "actpass" means the browser accepts either role, so
    // choosing one for it is a legitimate reading, not a trick.
    const std::string patchedOffer = forceRemoteDtlsActive(offerSdp);

    const Codec sourceCodec = m_source->codec();
    WhepPipelineOptions options;
    options.stunServer = m_config.stunServer;
    options.turnServer = m_config.turnServer;
    options.ssrc = makeSsrc();

    // As the ANSWERING side we must send with the payload type the offer
    // assigned. Pinning our own number leaves our caps unable to intersect the
    // offer: webrtcbin quietly drops our video, creates an empty recvonly
    // transceiver for the m-line, and leaves the data probe in place — so the
    // browser's DTLS ClientHello sits in a queue and the session hangs at
    // "connecting". The symptom is a protocol layer away from the cause.
    if (sourceCodec == Codec::H265) {
        options.payloadType = pickPayloadType(patchedOffer, "H265");
        if (options.payloadType < 0) {
            // The browser will not take H.265. Re-encode to H.264.
            options.payloadType = pickPayloadType(patchedOffer, "H264");
            options.transcode = true;
        }
    } else {
        options.payloadType = pickPayloadType(patchedOffer, "H264");
    }
    if (options.payloadType < 0) {
        return core::unsupported("the offer has no H264 or H265 video this camera can send");
    }

    const std::string launch = whepLaunch(sourceCodec, options);
    if (launch.empty()) {
        return core::unsupported("no WebRTC pipeline for codec " +
                                 std::string(toString(sourceCodec)));
    }
    VS_DEBUG(kCategory) << m_sessionId << ": " << launch;

    m_impl->transcoded = options.transcode;
    m_impl->sendCodec = options.transcode ? Codec::H264 : sourceCodec;
    m_impl->browserPayloadType = options.payloadType;
    m_impl->payloaderPayloadType =
        needsPayloadTypeRewrite(options) ? kPayloaderInternalPayloadType : options.payloadType;

    GError* error = nullptr;
    m_impl->pipeline = gst_parse_launch(launch.c_str(), &error);
    if (!m_impl->pipeline || error) {
        const std::string message =
            error && error->message ? error->message : "could not build the WebRTC pipeline";
        if (error) g_error_free(error);
        stop();
        return core::internalError(message);
    }

    m_impl->appsrc = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "src");
    m_impl->webrtc = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "webrtc");
    if (!m_impl->appsrc || !m_impl->webrtc) {
        stop();
        return core::internalError("the WebRTC pipeline is missing elements");
    }

    g_signal_connect(m_impl->webrtc, "notify::ice-gathering-state",
                     G_CALLBACK(&onIceGatheringChanged), m_impl.get());
    g_signal_connect(m_impl->webrtc, "notify::connection-state",
                     G_CALLBACK(&onConnectionStateChanged), m_impl.get());
    tuneIceAgent();
    installRtpProbe();

    m_impl->bus.start(m_impl->pipeline, m_sessionId,
                      [this](GstMessage* message) { handleBusMessage(m_impl.get(), message); });

    // PLAYING before negotiating: webrtcbin needs the pipeline running to
    // produce an answer. It can do so before any buffer has arrived, because
    // the caps just before it are fixed — see whepLaunch.
    if (gst_element_set_state(m_impl->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        stop();
        return core::hardwareFailure("could not start the WebRTC pipeline");
    }

    auto answer = negotiate(patchedOffer, clientAddressHint);
    if (!answer) {
        stop();
        return answer.error();
    }

    m_impl->startedAtMs = core::nowEpochMs();
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->enabled = true;
    }
    m_impl->alive.store(true);
    // Attached last, once the pipeline is running and negotiated.
    m_impl->sinkId = m_source->addSink(
        [impl = m_impl.get()](GstBuffer* buffer, GstCaps* caps) { impl->push(buffer, caps); });

    VS_INFO(kCategory) << m_sessionId << ": " << m_cameraId << " -> browser ("
                       << toString(m_impl->sendCodec) << ", pt " << options.payloadType
                       << (options.transcode ? ", transcoded" : ", passthrough") << ')';
    return answer.value();
}

void WhepSession::stop() {
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->enabled = false;
        m_impl->capsSet = false;
    }
    if (m_source && m_impl->sinkId != 0) {
        m_source->removeSink(m_impl->sinkId);
        m_impl->sinkId = 0;
    }
    m_impl->alive.store(false);
    m_impl->bus.stop();

    if (m_impl->pipeline) gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);
    if (m_impl->appsrc) {
        gst_object_unref(m_impl->appsrc);
        m_impl->appsrc = nullptr;
    }
    if (m_impl->webrtc) {
        gst_object_unref(m_impl->webrtc);
        m_impl->webrtc = nullptr;
    }
    if (m_impl->pipeline) {
        gst_object_unref(m_impl->pipeline);
        m_impl->pipeline = nullptr;
    }
}

void WhepSession::tuneIceAgent() {
    // libnice defaults to three STUN attempts with a 500 ms timeout, which adds
    // over two seconds to every WHEP request — on a LAN, where the host
    // candidate was available immediately and no STUN answer is coming.
    // Both fetched as plain GObjects: GstWebRTCICE only became a public type in
    // GStreamer 1.22, and this has to build against 1.20 as well. The property
    // names are stable; the C type is not.
    GObject* ice = nullptr;
    g_object_get(m_impl->webrtc, "ice-agent", &ice, nullptr);
    if (!ice) return;

    GObject* agent = nullptr;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(ice), "agent")) {
        g_object_get(ice, "agent", &agent, nullptr);
    }
    if (agent) {
        // Set through the property names rather than the C API, because the
        // agent reaches us as a plain GObject.
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(agent), "stun-initial-timeout")) {
            g_object_set(agent, "stun-initial-timeout", kStunInitialTimeoutMs, nullptr);
        }
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(agent),
                                         "stun-max-retransmissions")) {
            g_object_set(agent, "stun-max-retransmissions", kStunMaxRetransmissions, nullptr);
        }
        g_object_unref(agent);
    }
    g_object_unref(ice);
}

void WhepSession::installRtpProbe() {
    GstElement* pay = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "pay");
    if (!pay) return;
    GstPad* src = gst_element_get_static_pad(pay, "src");
    if (src) {
        gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, &onRtpOut, m_impl.get(), nullptr);
        gst_object_unref(src);
    }
    gst_object_unref(pay);
}

core::Result<std::string> WhepSession::negotiate(const std::string& offerSdp,
                                                 const std::string& clientAddressHint) {
    GstSDPMessage* offerMessage = nullptr;
    if (gst_sdp_message_new_from_text(offerSdp.c_str(), &offerMessage) != GST_SDP_OK ||
        !offerMessage) {
        if (offerMessage) gst_sdp_message_free(offerMessage);
        return core::invalidArgument("malformed SDP offer");
    }

    // Takes ownership of offerMessage.
    GstWebRTCSessionDescription* offer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, offerMessage);

    GstPromise* promise = gst_promise_new();
    g_signal_emit_by_name(m_impl->webrtc, "set-remote-description", offer, promise);
    gst_promise_wait(promise);
    gst_promise_unref(promise);
    gst_webrtc_session_description_free(offer);

    // REQUIRED: webrtcbin does NOT read the "a=candidate:" lines out of the
    // remote description. It only accepts candidates through add-ice-candidate.
    // A non-trickle client puts them all in the offer, so without this webrtcbin
    // knows no address for the browser at all: signalling completes, ICE sits at
    // checking for ever, and the watchdog eventually reaps the session — which
    // looks to a user like "it spun for a while then said disconnected".
    int added = 0;
    for (const RemoteCandidate& candidate : remoteCandidates(offerSdp)) {
        const std::string value = rewriteMdnsCandidate(candidate.candidate, clientAddressHint);
        g_signal_emit_by_name(m_impl->webrtc, "add-ice-candidate", candidate.mlineIndex,
                              value.c_str());
        ++added;
    }
    VS_DEBUG(kCategory) << m_sessionId << ": loaded " << added << " ICE candidate(s)";

    promise = gst_promise_new();
    g_signal_emit_by_name(m_impl->webrtc, "create-answer", nullptr, promise);
    gst_promise_wait(promise);
    const GstStructure* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* answer = nullptr;
    if (reply) {
        gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer,
                          nullptr);
    }
    gst_promise_unref(promise);
    if (!answer) return core::internalError("webrtcbin produced no SDP answer");

    // The answer already says a=setup:passive, because the offer was pinned to
    // active — see start().
    promise = gst_promise_new();
    g_signal_emit_by_name(m_impl->webrtc, "set-local-description", answer, promise);
    gst_promise_wait(promise);
    gst_promise_unref(promise);
    gst_webrtc_session_description_free(answer);

    // Non-trickle: wait for gathering so the answer carries every candidate.
    // The browser then needs exactly one round trip.
    {
        std::unique_lock<std::mutex> lock(m_impl->gatherMutex);
        m_impl->gathered.wait_for(lock, std::chrono::milliseconds(m_config.iceGatherTimeoutMs),
                                  [this] { return m_impl->gatheredFlag; });
    }

    // Re-read the local description AFTER gathering: the one from create-answer
    // has no candidates in it yet.
    GstWebRTCSessionDescription* local = nullptr;
    g_object_get(m_impl->webrtc, "local-description", &local, nullptr);
    if (!local || !local->sdp) {
        if (local) gst_webrtc_session_description_free(local);
        return core::internalError("webrtcbin has no local description");
    }
    gchar* text = gst_sdp_message_as_text(local->sdp);
    std::string answerSdp = text ? text : "";
    if (text) g_free(text);
    gst_webrtc_session_description_free(local);

    if (answerSdp.empty()) return core::internalError("the SDP answer was empty");
    return answerSdp;
}

bool WhepSession::alive() const {
    if (!m_impl->alive.load()) return false;
    // A live camera source that has failed will produce nothing more, and the
    // browser should reconnect rather than watch a frozen frame. A PLAYBACK
    // source stays alive at the end of a recording, because seeking elsewhere
    // is exactly what the open session is for.
    if (m_source && !m_source->alive()) return false;

    // A tab closed without a DELETE reaches us as a connection-state change and
    // nothing else.
    if (m_impl->peerGone.load()) return false;

    // Never connected at all: give up eventually rather than hold a pipeline
    // for an offer nobody followed through on.
    if (!m_impl->everConnected.load()) {
        return core::nowEpochMs() - m_impl->startedAtMs < m_config.connectTimeoutMs;
    }
    return true;
}

ViewerInfo WhepSession::info() const {
    ViewerInfo out;
    out.sessionId = m_sessionId;
    out.cameraId = m_cameraId;
    out.codec = toString(m_impl->sendCodec);
    out.transcoded = m_impl->transcoded;
    out.rtpPackets = m_impl->rtpPackets.load();
    out.startedAtMs = m_impl->startedAtMs;
    return out;
}

}  // namespace visora::media
