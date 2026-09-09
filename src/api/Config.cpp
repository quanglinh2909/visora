#include "api/Config.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/Log.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"

namespace visora::api {
namespace {

constexpr const char* kCategory = "config";

// A tiny reader over oatpp's JSON tree. The config is a handful of scalars, and
// a DTO per section would be more ceremony than the thing it describes.
// Structured bindings are avoided in these loops: gcc 11 treats the bound name
// as dependent and refuses `value.retrieve<T>()` without a `template` keyword,
// which is more confusing to read than a plain pair access.
const oatpp::Any* find(const oatpp::Fields<oatpp::Any>& fields, const char* name) {
    if (!fields) return nullptr;
    for (auto it = fields->begin(); it != fields->end(); ++it) {
        const oatpp::String& key = it->first;
        if (key && *key == name && it->second) return &it->second;
    }
    return nullptr;
}

// Any::retrieve() THROWS when the stored type differs from the requested one,
// and which numeric type a JSON number lands in is not something a config file
// author can be expected to control. Reading `"port": 8019` as Int32 aborted the
// process at startup before this was wrapped — a config file crashing the
// server is about the worst failure mode a config file can have.
template <class Wrapper>
bool tryRetrieve(const oatpp::Any& value, Wrapper& out) {
    try {
        out = value.retrieve<Wrapper>();
        return static_cast<bool>(out);
    } catch (const std::runtime_error&) {
        return false;
    }
}

const oatpp::Fields<oatpp::Any>* section(const oatpp::Fields<oatpp::Any>& root,
                                         const char* name,
                                         oatpp::Fields<oatpp::Any>& storage) {
    for (auto it = root->begin(); it != root->end(); ++it) {
        const oatpp::String& key = it->first;
        const oatpp::Any& value = it->second;
        if (key && *key == name && value) {
            if (tryRetrieve(value, storage)) return &storage;
        }
    }
    return nullptr;
}

void readString(const oatpp::Fields<oatpp::Any>& fields, const char* name, std::string& out) {
    const oatpp::Any* value = find(fields, name);
    if (value == nullptr) return;
    oatpp::String text;
    if (tryRetrieve(*value, text)) out = *text;
}

// Accepts whichever numeric type the parser chose.
template <class T>
void readInt(const oatpp::Fields<oatpp::Any>& fields, const char* name, T& out) {
    const oatpp::Any* value = find(fields, name);
    if (value == nullptr) return;

    oatpp::Int32 asInt32;
    if (tryRetrieve(*value, asInt32)) {
        out = static_cast<T>(*asInt32);
        return;
    }
    oatpp::Int64 asInt64;
    if (tryRetrieve(*value, asInt64)) {
        out = static_cast<T>(*asInt64);
        return;
    }
    // UInt64 first among the unsigned types: it is what oatpp's JSON parser
    // actually produces for a positive integer literal, which cost a startup
    // failure to discover.
    oatpp::UInt64 asUInt64;
    if (tryRetrieve(*value, asUInt64)) {
        out = static_cast<T>(*asUInt64);
        return;
    }
    oatpp::UInt32 asUInt32;
    if (tryRetrieve(*value, asUInt32)) {
        out = static_cast<T>(*asUInt32);
        return;
    }
    oatpp::Float64 asFloat;
    if (tryRetrieve(*value, asFloat)) {
        out = static_cast<T>(*asFloat);
        return;
    }
    const oatpp::Type* stored = value->getStoredType();
    VS_WARN(kCategory) << "config value '" << name << "' is not a number (stored as "
                       << (stored != nullptr ? stored->classId.name : "nothing")
                       << "); keeping the default";
}

}  // namespace

core::Result<Config> loadConfig(const std::string& path) {
    Config config;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        VS_INFO(kCategory) << "no config at " << path << "; using defaults"
                           << (config.database.enabled() ? "" : " with an in-memory store");
        return config;
    }

    std::ifstream file(path);
    if (!file) return core::notFound("cannot read " + path);
    std::ostringstream buffer;
    buffer << file.rdbuf();

    oatpp::parser::json::mapping::ObjectMapper mapper;
    oatpp::Fields<oatpp::Any> root;
    try {
        root = mapper.readFromString<oatpp::Fields<oatpp::Any>>(
            oatpp::String(buffer.str().c_str()));
    } catch (const std::exception& error) {
        // Deliberately fatal. Starting with settings the operator believes they
        // changed is worse than not starting.
        return core::invalidArgument(path + " is not valid JSON: " + error.what());
    }
    if (!root) return core::invalidArgument(path + " is empty or not a JSON object");

    oatpp::Fields<oatpp::Any> storage;
    if (const auto* server = section(root, "server", storage)) {
        readString(*server, "host", config.server.host);
        readInt(*server, "port", config.server.port);
    }
    if (const auto* database = section(root, "database", storage)) {
        readString(*database, "url", config.database.url);
        readInt(*database, "poolMaxConnections", config.database.poolMaxConnections);
        readInt(*database, "poolIdleSeconds", config.database.poolIdleSeconds);
    }
    if (const auto* stream = section(root, "gstreamer", storage)) {
        readString(*stream, "rtspHost", config.stream.rtspHost);
        readString(*stream, "publicRtspHost", config.stream.publicRtspHost);
        readInt(*stream, "rtspPort", config.stream.rtspPort);
        readInt(*stream, "retryInitialMs", config.stream.retryInitialMs);
        readInt(*stream, "retryMaxMs", config.stream.retryMaxMs);
        readInt(*stream, "sourceLatencyMs", config.stream.sourceLatencyMs);
        readString(*stream, "recordingDir", config.stream.recordingDir);
        readString(*stream, "motionSnapshotDir", config.stream.motionSnapshotDir);
        readString(*stream, "stunServer", config.stream.stunServer);
        readString(*stream, "turnServer", config.stream.turnServer);
    }
    if (const auto* swagger = section(root, "swagger", storage)) {
        readString(*swagger, "title", config.swaggerTitle);
        readString(*swagger, "version", config.swaggerVersion);
    }

    VS_INFO(kCategory) << "loaded " << path << "; database "
                       << (config.database.enabled() ? "configured" : "not configured");
    return config;
}

}  // namespace visora::api
