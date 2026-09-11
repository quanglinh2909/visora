#pragma once

// Runtime configuration, loaded once at startup.
//
// Plain structs with defaults that work: a fresh checkout starts and serves
// without a config file, using an in-memory store. That matters for a bring-up
// on new hardware, where PostgreSQL is one more thing to get wrong before you
// know whether the video path works at all.

#include <cstdint>
#include <string>

#include "core/Result.hpp"

namespace visora::api {

struct ServerConfig {
    std::string host = "0.0.0.0";
    std::uint16_t port = 8009;
};

struct DatabaseConfig {
    // Empty means "no database": the service runs against the in-memory store.
    std::string url;
    int poolMaxConnections = 10;
    int poolIdleSeconds = 60;

    bool enabled() const { return !url.empty(); }
};

struct StreamConfig {
    std::string rtspHost = "0.0.0.0";
    std::string publicRtspHost = "127.0.0.1";
    std::uint16_t rtspPort = 8554;
    std::uint32_t retryInitialMs = 1000;
    std::uint32_t retryMaxMs = 30000;
    int sourceLatencyMs = 300;

    // Keep the current group of pictures per camera, so a viewer opening a tile
    // sees one immediately instead of waiting for the next keyframe — 1 to 2
    // seconds on a typical camera. Off trades that first picture for the lowest
    // latency, which is the tradeoff SRS names in one line: "set to off for min
    // delay".
    bool gopCache = true;
    std::string recordingDir = "recordings";
    std::string motionSnapshotDir = "motion-snapshots";

    // Empty means "host candidates only", which is right for a LAN and wrong
    // for anything crossing a NAT. Left empty by default because a STUN server
    // that is unreachable costs every WHEP request a timeout.
    std::string stunServer;
    std::string turnServer;

    // Where the MoQ server listens. Empty disables the feature, which is the
    // default: most deployments do not run one, and endpoints should say so per
    // request rather than the service failing at startup.
    std::string moqSocketPath;
};

struct AiConfig {
    // Where model files live, for GET /ai-models. Jobs may still name a path
    // anywhere; this is only what the catalogue lists.
    std::string modelDir = "models";

    // How often a camera is decoded for analysis. Not every frame: a detector
    // at 5 fps sees everything that matters and leaves the accelerator for the
    // other cameras.
    int analyseFps = 5;
};

struct Config {
    ServerConfig server;
    DatabaseConfig database;
    StreamConfig stream;
    AiConfig ai;
    std::string swaggerTitle = "Visora API";
    std::string swaggerVersion = "1.0";
};

// Reads JSON from `path`. A missing file is not an error — the defaults are
// usable, and requiring a file to start is a bad first experience on a new
// board. A malformed file IS an error, because silently ignoring it would run
// with settings the operator believes they changed.
core::Result<Config> loadConfig(const std::string& path);

}  // namespace visora::api
