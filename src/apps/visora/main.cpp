// Visora — Intelligent Video Management Platform.
//
// The composition root: the one place that knows the whole dependency graph.
// Every service is built here and handed its collaborators through a
// constructor, so reading this file tells you what the program is made of.
//
// The predecessor used oatpp's OATPP_COMPONENT registry instead — a service
// locator, where any class could reach out and take any dependency at any time.
// Nothing then stated what the program was made of, and a unit test of one
// service had to construct all of them.

#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "api/Config.hpp"
#include "api/controllers/CameraController.hpp"
#include "api/controllers/CameraStreamController.hpp"
#include "api/controllers/PlaybackController.hpp"
#include "api/controllers/AiController.hpp"
#include "api/controllers/MoqController.hpp"
#include "api/controllers/WebRtcController.hpp"
#include "api/controllers/WebSocketController.hpp"
#include "api/ws/CameraStateFeed.hpp"
#include "api/ws/MotionEventFeed.hpp"
#include "core/Log.hpp"
#include "core/Time.hpp"
#include "hal/Capabilities.hpp"
#include "media/camera/CameraRuntime.hpp"
#include "media/camera/CameraService.hpp"
#include "media/gst/CodecProvider.hpp"
#include "media/recording/PlaybackService.hpp"
#include "media/recording/RecordingManager.hpp"
#include "media/recording/ThumbnailExtractor.hpp"
#include "media/source/CameraSourceRegistry.hpp"
#include "media/ai/AiRuntime.hpp"
#include "media/ai/MotionEventRecorder.hpp"
#include "media/moq/MoqService.hpp"
#include "media/webrtc/WebRtcService.hpp"
#include "media/stream/RtspServer.hpp"
#include "media/stream/SnapshotGrabber.hpp"
#include "media/stream/StreamManager.hpp"
#include "store/InMemoryCameraRepository.hpp"
#include "store/InMemoryAiJobRepository.hpp"
#include "store/InMemoryRecordingRepository.hpp"
#include "store/PostgresCameraRepository.hpp"
#include "store/PostgresRecordingRepository.hpp"
#include "vision/AiJobService.hpp"
#include "vision/ResultSink.hpp"

#include "oatpp-swagger/Controller.hpp"
#include "oatpp-websocket/ConnectionHandler.hpp"
#include "oatpp-swagger/Model.hpp"
#include "oatpp-swagger/Resources.hpp"
#include "oatpp/network/Server.hpp"
#include "oatpp/network/tcp/server/ConnectionProvider.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/HttpConnectionHandler.hpp"
#include "oatpp/web/server/HttpRouter.hpp"

namespace {

constexpr const char* kCategory = "app";

std::shared_ptr<oatpp::network::Server> g_server;

void onSignal(int) {
    // Only async-signal-safe work here: ask the server to stop and let the main
    // thread unwind normally. Tearing pipelines down from a signal handler is
    // how a shutdown turns into a crash.
    if (g_server) g_server->stop();
}

// Where cameras and recordings live.
//
// Both come from one place because they share one connection pool: two pools
// against the same database would double the connections for no benefit, and a
// deployment where cameras persist but recordings do not is not a configuration
// anyone wants.
//
// An empty database URL is a supported configuration, not a misconfiguration:
// it is how a bring-up on new hardware starts, before PostgreSQL is one more
// thing that can be wrong.
struct Repositories {
    std::shared_ptr<visora::media::CameraRepository> cameras;
    std::shared_ptr<visora::media::RecordingRepository> recordings;
    std::shared_ptr<visora::vision::AiJobRepository> aiJobs;
    bool ok() const {
        return cameras != nullptr && recordings != nullptr && aiJobs != nullptr;
    }
};

Repositories makeRepositories(const visora::api::DatabaseConfig& config) {
    using namespace visora;

    if (!config.enabled()) {
        VS_WARN(kCategory) << "no database configured; cameras and the recording index "
                              "are kept in memory and will not survive a restart "
                              "(recorded FILES do survive, but nothing will index them)";
        return {std::make_shared<store::InMemoryCameraRepository>(),
                std::make_shared<store::InMemoryRecordingRepository>(),
                std::make_shared<store::InMemoryAiJobRepository>()};
    }

    auto executor = store::makePostgresExecutor(config.url, config.poolMaxConnections,
                                                config.poolIdleSeconds);
    if (!executor) {
        // Deliberately fatal. Falling back to memory would look like it worked
        // and quietly lose every camera the operator adds.
        VS_ERROR(kCategory) << "database unavailable: " << executor.error().str();
        return {};
    }
    return {std::make_shared<store::PostgresCameraRepository>(executor.value()),
            std::make_shared<store::PostgresRecordingRepository>(executor.value()),
            // AI jobs stay in memory until the PostgreSQL adapter for them
            // exists; they are re-created from the API rather than lost
            // silently, and the warning below says so.
            std::make_shared<store::InMemoryAiJobRepository>()};
}

}  // namespace

int main(int argc, char** argv) {
    using namespace visora;

    std::string configPath = "config/config.json";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            configPath = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "usage: visora [--config PATH]\n\n"
                "Environment:\n"
                "  VISORA_LOG_LEVEL=trace|debug|info|warn|error|off\n"
                "  VISORA_LOG_CATEGORIES=app,camera,codec,hal\n"
                "  VISORA_CODEC_PROVIDER=<id>      force a codec provider\n"
                "  VISORA_IMAGE_BACKEND=<id>       force an image-ops backend\n"
                "  VISORA_RESULT_SOCKET=<path>     where AI results are published\n");
            return 0;
        }
    }

    auto config = api::loadConfig(configPath);
    if (!config) {
        VS_ERROR(kCategory) << config.error().str();
        return 1;
    }

    // What this machine can do, before anything tries to use it. On a board this
    // is the difference between "AI is off" and knowing why.
    hal::Capabilities capabilities = hal::detectCapabilities();
    for (auto& row : media::codecBackendStatus()) capabilities.backends.push_back(std::move(row));
    std::fputs(hal::toText(capabilities).c_str(), stderr);

    oatpp::base::Environment::init();
    int exitCode = 0;
    {
        const Repositories repositories = makeRepositories(config.value().database);
        if (!repositories.ok()) {
            oatpp::base::Environment::destroy();
            return 1;
        }
        auto repository = repositories.cameras;

        auto rtspServer = std::make_shared<media::RtspServer>(
            config.value().stream.rtspHost, config.value().stream.rtspPort);
        const core::Status rtspStarted = rtspServer->start();
        if (!rtspStarted.ok()) {
            VS_ERROR(kCategory) << rtspStarted.error().str();
            oatpp::base::Environment::destroy();
            return 1;
        }

        media::StreamManagerConfig streamConfig;
        streamConfig.publicHost = config.value().stream.publicRtspHost;
        streamConfig.rtspPort = rtspServer->boundPort();
        streamConfig.sourceLatencyMs = config.value().stream.sourceLatencyMs;
        streamConfig.retry.initialMs = config.value().stream.retryInitialMs;
        streamConfig.retry.maxMs = config.value().stream.retryMaxMs;

        // Connected browsers, told about every state change as it happens.
        // Built before the stream manager because the manager's status callback
        // pushes into it.
        auto cameraStateFeed = std::make_shared<api::CameraStateFeed>();

        // One connection per camera, shared by recording and — from step 7 —
        // by WebRTC and the AI pipeline. A camera permits only a handful of
        // simultaneous streams, so this is a constraint rather than a saving.
        media::RtspSourceOptions sourceOptions;
        sourceOptions.latencyMs = config.value().stream.sourceLatencyMs;
        auto sources = std::make_shared<media::CameraSourceRegistry>(sourceOptions);

        auto recordingRepository = repositories.recordings;

        media::RecordingManagerConfig recordingConfig;
        recordingConfig.recordingDir = config.value().stream.recordingDir;
        auto recordings = std::make_shared<media::RecordingManager>(
            recordingConfig, recordingRepository, sources);

        // Where results go. Registered plug-ins, several at once — the Unix
        // socket the Python consumer reads is simply the first one.
        auto resultSinks = std::make_shared<vision::ResultSinkSet>();
        resultSinks->startAll();

        // Motion pushed to browsers as it happens, and written to the index
        // as one row per event — two different shapes of the same signal.
        auto motionFeed = std::make_shared<api::MotionEventFeed>();
        auto motionEvents =
            std::make_shared<media::MotionEventRecorder>(recordingRepository);

        media::AiRuntimeConfig aiConfig;
        aiConfig.analyseFps = config.value().ai.analyseFps;
        auto aiRuntime = std::make_shared<media::AiRuntime>(aiConfig, sources, resultSinks);
        // A detection is worth keeping footage of, in a camera set to record
        // only around events. Recording decides what to do with it.
        aiRuntime->setEventSink(
            [recordings](const std::string& cameraId) { recordings->noteEvent(cameraId); });
        aiRuntime->setMotionSink(
            [motionFeed, motionEvents](const media::MotionNotice& notice) {
                motionFeed->publish(notice);
                motionEvents->observe(notice);
            });


        // The service is captured weakly on purpose: the manager's worker
        // thread outlives nothing here, but a strong reference would make the
        // two own each other and neither would ever be destroyed.
        std::weak_ptr<media::CameraService> cameraServiceWeak;
        auto streams = std::make_shared<media::StreamManager>(
            streamConfig, rtspServer,
            [&cameraServiceWeak, cameraStateFeed, recordings, aiRuntime, motionEvents](
                const std::string& cameraId, const media::StreamStatus& status) {
                media::CameraRuntimeFields fields;
                fields.state = status.state;
                fields.codec = status.codec;
                fields.outputRtsp = status.outputRtsp;
                fields.retryCount = status.retryCount;
                fields.lastError = status.lastError;
                fields.lastChangedAt = core::nowIso8601();

                std::string name;
                if (auto service = cameraServiceWeak.lock()) {
                    // Runtime state goes back to the row, so a restart resumes
                    // with what was true rather than with 'offline' for every
                    // camera.
                    service->reportRuntime(cameraId, fields);
                    if (auto camera = service->get(cameraId)) name = camera.value().name;
                }

                // The push happens after the write, so a client that reacts by
                // fetching the camera sees the state it was just told about —
                // and carries the same timestamp that was stored, rather than a
                // second one taken a moment later.
                media::CameraRuntimeStatus push;
                push.id = cameraId;
                push.name = std::move(name);
                push.state = fields.state;
                push.codec = fields.codec;
                push.outputRtsp = fields.outputRtsp;
                push.retryCount = fields.retryCount;
                push.lastError = fields.lastError;
                push.lastChangedAt = fields.lastChangedAt;
                push.streaming = status.desired;
                cameraStateFeed->broadcast(push);

                // Recording follows the stream rather than the row: the codec
                // is only known once the camera has been probed, and a
                // recording pipeline built for the wrong codec fails in a way
                // that looks like a broken camera.
                if (auto service = cameraServiceWeak.lock()) {
                    if (auto camera = service->get(cameraId)) {
                        const media::Codec live = status.state == media::CameraState::Online
                                                      ? status.codec
                                                      : media::Codec::Unknown;
                        recordings->apply(camera.value(), live);
                        aiRuntime->applyCamera(camera.value(), live);
                        motionEvents->setSaving(cameraId, camera.value().motionSaveEvents);
                    }
                }
            });

        // Events are callbacks so the dependency points one way: the camera
        // domain does not know a streaming layer exists.
        media::CameraEvents events;
        events.added = [streams](const media::Camera& camera) { streams->apply(camera); };
        events.changed = [streams, recordings](const media::Camera& camera,
                                              const media::CameraDiff& diff) {
            // Recording hears about every change and decides for itself: only a
            // source, mode or segment-length change rebuilds its pipeline, so a
            // rename does not discard the segment being written.
            if (diff.recordingChanged || diff.sourceChanged) {
                recordings->apply(camera, media::Codec::Unknown);
            }
            // Only a change the pipeline cares about reaches the manager, and
            // the manager itself ignores one whose source did not move — so a
            // rename never drops a viewer.
            if (diff.sourceChanged || diff.recordingChanged) streams->apply(camera);
        };
        events.removed = [streams, recordings, cameraStateFeed](const std::string& id) {
            streams->remove(id);
            recordings->remove(id);
            cameraStateFeed->broadcastRemoved(id);
        };

        auto cameras = std::make_shared<media::CameraService>(repository, std::move(events));
        cameraServiceWeak = cameras;

        media::SnapshotOptions snapshotOptions;
        snapshotOptions.latencyMs = config.value().stream.sourceLatencyMs;
        auto runtime = std::make_shared<media::CameraRuntime>(
            cameras, streams, media::makeGstSnapshotGrabber(), snapshotOptions);

        media::PlaybackConfig playbackConfig;
        playbackConfig.recordingDir = config.value().stream.recordingDir;
        playbackConfig.motionSnapshotDir = config.value().stream.motionSnapshotDir;
        auto playback = std::make_shared<media::PlaybackService>(
            playbackConfig, cameras, recordingRepository, media::makeGstThumbnailExtractor());

        media::WhepConfig whepConfig;
        whepConfig.stunServer = config.value().stream.stunServer;
        whepConfig.turnServer = config.value().stream.turnServer;
        auto webrtc = std::make_shared<media::WebRtcService>(whepConfig, cameras, sources,
                                                            recordingRepository);

        media::MoqConfig moqConfig;
        moqConfig.socketPath = config.value().stream.moqSocketPath;
        auto moq = std::make_shared<media::MoqService>(moqConfig, cameras, sources,
                                                       recordingRepository);

        vision::AiJobEvents aiEvents;
        aiEvents.added = [aiRuntime](const vision::AiJob& job) { aiRuntime->applyJob(job); };
        aiEvents.changed = [aiRuntime](const vision::AiJob& job, const vision::AiJobDiff&) {
            aiRuntime->applyJob(job);
        };
        aiEvents.removed = [aiRuntime](const std::string& id) { aiRuntime->removeJob(id); };
        auto aiJobs = std::make_shared<vision::AiJobService>(repositories.aiJobs,
                                                             std::move(aiEvents));

        streams->start();
        recordings->start();
        webrtc->start();
        moq->start();
        aiRuntime->start();

        // Jobs already stored start with the service, the same way cameras do.
        if (auto existing = aiJobs->list()) {
            for (const vision::AiJob& job : existing.value()) aiRuntime->applyJob(job);
            VS_INFO(kCategory) << "restored " << existing.value().size() << " AI job(s)";
        }

        // Everything already in the database starts streaming without waiting
        // for someone to touch the API.
        if (auto existing = cameras->list()) {
            for (const media::Camera& camera : existing.value()) {
                streams->apply(camera);
                motionEvents->setSaving(camera.id, camera.motionSaveEvents);
            }
            VS_INFO(kCategory) << "restored " << existing.value().size() << " camera(s)";
        }

        auto objectMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
        // Absent fields must stay absent, not become nulls: a partial update
        // depends on telling "not supplied" from "set to null".
        objectMapper->getSerializer()->getConfig()->includeNullFields = false;

        auto router = oatpp::web::server::HttpRouter::createShared();
        auto cameraController = api::CameraController::createShared(objectMapper, cameras);
        router->addController(cameraController);
        auto streamController =
            api::CameraStreamController::createShared(objectMapper, runtime, recordings);
        router->addController(streamController);
        auto playbackController =
            api::PlaybackController::createShared(objectMapper, playback);
        router->addController(playbackController);
        auto webrtcController = api::WebRtcController::createShared(objectMapper, webrtc);
        router->addController(webrtcController);
        auto moqController = api::MoqController::createShared(objectMapper, moq);
        router->addController(moqController);
        auto aiController = api::AiController::createShared(objectMapper, aiJobs, aiRuntime,
                                                            config.value().ai.modelDir);
        router->addController(aiController);

        auto cameraStateHandler = oatpp::websocket::ConnectionHandler::createShared();
        cameraStateHandler->setSocketInstanceListener(
            std::make_shared<api::CameraStateInstanceListener>(cameraStateFeed));
        auto motionHandler = oatpp::websocket::ConnectionHandler::createShared();
        motionHandler->setSocketInstanceListener(
            std::make_shared<api::MotionInstanceListener>(motionFeed));
        router->addController(api::WebSocketController::createShared(
            objectMapper, cameraStateHandler, motionHandler));

        auto documentInfo =
            oatpp::swagger::DocumentInfo::Builder()
                .setTitle(config.value().swaggerTitle.c_str())
                .setVersion(config.value().swaggerVersion.c_str())
                .setDescription("Intelligent Video Management Platform")
                .build();
        oatpp::web::server::api::Endpoints endpoints;
        endpoints.append(cameraController->getEndpoints());
        endpoints.append(streamController->getEndpoints());
        endpoints.append(playbackController->getEndpoints());
        endpoints.append(webrtcController->getEndpoints());
        endpoints.append(moqController->getEndpoints());
        endpoints.append(aiController->getEndpoints());
        // The websocket controller is deliberately absent: OpenAPI cannot
        // describe an upgrade handshake, and listing it as a GET that returns
        // 101 misleads whoever reads the docs.

        // createShared takes documentInfo and resources as OATPP_COMPONENT
        // DEFAULT arguments, which means they can simply be passed. Doing so
        // keeps the last piece of the graph explicit instead of registering two
        // globals for the library to find behind our back.
        // Named, because createShared takes the resources by non-const
        // reference and will not bind a temporary.
        auto swaggerResources =
            oatpp::swagger::Resources::loadResources(OATPP_SWAGGER_RES_PATH);
        auto swagger = oatpp::swagger::Controller::createShared(endpoints, documentInfo,
                                                                swaggerResources);
        router->addController(swagger);

        auto connectionHandler =
            oatpp::web::server::HttpConnectionHandler::createShared(router);
        auto connectionProvider = oatpp::network::tcp::server::ConnectionProvider::createShared(
            {config.value().server.host.c_str(),
             static_cast<v_uint16>(config.value().server.port),
             oatpp::network::Address::IP_4});

        g_server = std::make_shared<oatpp::network::Server>(connectionProvider,
                                                            connectionHandler);
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        VS_INFO(kCategory) << "listening on http://" << config.value().server.host << ':'
                           << config.value().server.port << "  (docs at /swagger/ui)";
        g_server->run();

        VS_INFO(kCategory) << "shutting down";
        g_server.reset();
        connectionHandler->stop();
        // Websocket connections are held by their own handler and would keep
        // the process alive after the HTTP server stopped.
        cameraStateHandler->stop();
        motionHandler->stop();
        // Recording first: it holds the shared sources and has to finalise
        // whatever segment it is writing. Then streams, whose worker touches
        // the camera service, which the repository outlives only until this
        // scope ends.
        // Viewers first: each holds a shared source, and a source that is still
        // referenced cannot be closed.
        // AI first: its workers hold frame taps, which hold shared sources.
        aiRuntime->stop();
        // An event left open reads as one that never ended, which is worse than
        // one that ended when the program did.
        motionEvents->closeAll();
        resultSinks->stopAll();
        moq->stop();
        webrtc->stop();
        recordings->stop();
        streams->stop();
        rtspServer->stop();
    }
    oatpp::base::Environment::destroy();
    return exitCode;
}
