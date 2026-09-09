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
#include "core/Log.hpp"
#include "hal/Capabilities.hpp"
#include "media/camera/CameraService.hpp"
#include "media/gst/CodecProvider.hpp"
#include "media/stream/RtspServer.hpp"
#include "media/stream/StreamManager.hpp"
#include "store/InMemoryCameraRepository.hpp"
#include "store/PostgresCameraRepository.hpp"

#include "oatpp-swagger/Controller.hpp"
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

// Chooses where cameras live. An empty database URL is a supported
// configuration, not a misconfiguration: it is how a bring-up on new hardware
// starts, before PostgreSQL is one more thing that can be wrong.
std::shared_ptr<visora::media::CameraRepository> makeCameraRepository(
    const visora::api::DatabaseConfig& config) {
    using namespace visora;

    if (!config.enabled()) {
        VS_WARN(kCategory) << "no database configured; cameras are kept in memory "
                              "and will not survive a restart";
        return std::make_shared<store::InMemoryCameraRepository>();
    }

    auto executor = store::makePostgresExecutor(config.url, config.poolMaxConnections,
                                                config.poolIdleSeconds);
    if (!executor) {
        // Deliberately fatal. Falling back to memory would look like it worked
        // and quietly lose every camera the operator adds.
        VS_ERROR(kCategory) << "database unavailable: " << executor.error().str();
        return nullptr;
    }
    return std::make_shared<store::PostgresCameraRepository>(executor.value());
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
        auto repository = makeCameraRepository(config.value().database);
        if (!repository) {
            oatpp::base::Environment::destroy();
            return 1;
        }

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

        // The service is captured weakly on purpose: the manager's worker
        // thread outlives nothing here, but a strong reference would make the
        // two own each other and neither would ever be destroyed.
        std::weak_ptr<media::CameraService> cameraServiceWeak;
        auto streams = std::make_shared<media::StreamManager>(
            streamConfig, rtspServer,
            [&cameraServiceWeak](const std::string& cameraId,
                                 const media::StreamStatus& status) {
                if (auto service = cameraServiceWeak.lock()) {
                    // Runtime state goes back to the row, so a restart resumes
                    // with what was true rather than with 'offline' for every
                    // camera.
                    service->reportRuntime(cameraId, status.state, status.codec,
                                           status.outputRtsp, status.retryCount,
                                           status.lastError);
                }
            });

        // Events are callbacks so the dependency points one way: the camera
        // domain does not know a streaming layer exists.
        media::CameraEvents events;
        events.added = [streams](const media::Camera& camera) { streams->apply(camera); };
        events.changed = [streams](const media::Camera& camera, const media::CameraDiff& diff) {
            // Only a change the pipeline cares about reaches the manager, and
            // the manager itself ignores one whose source did not move — so a
            // rename never drops a viewer.
            if (diff.sourceChanged || diff.recordingChanged) streams->apply(camera);
        };
        events.removed = [streams](const std::string& id) { streams->remove(id); };

        auto cameras = std::make_shared<media::CameraService>(repository, std::move(events));
        cameraServiceWeak = cameras;

        streams->start();

        // Everything already in the database starts streaming without waiting
        // for someone to touch the API.
        if (auto existing = cameras->list()) {
            for (const media::Camera& camera : existing.value()) streams->apply(camera);
            VS_INFO(kCategory) << "restored " << existing.value().size() << " camera(s)";
        }

        auto objectMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
        // Absent fields must stay absent, not become nulls: a partial update
        // depends on telling "not supplied" from "set to null".
        objectMapper->getSerializer()->getConfig()->includeNullFields = false;

        auto router = oatpp::web::server::HttpRouter::createShared();
        auto cameraController = api::CameraController::createShared(objectMapper, cameras);
        router->addController(cameraController);

        auto documentInfo =
            oatpp::swagger::DocumentInfo::Builder()
                .setTitle(config.value().swaggerTitle.c_str())
                .setVersion(config.value().swaggerVersion.c_str())
                .setDescription("Intelligent Video Management Platform")
                .build();
        oatpp::web::server::api::Endpoints endpoints;
        endpoints.append(cameraController->getEndpoints());

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
        // Streams first: their worker touches the camera service, which the
        // repository outlives only until this scope ends.
        streams->stop();
        rtspServer->stop();
    }
    oatpp::base::Environment::destroy();
    return exitCode;
}
