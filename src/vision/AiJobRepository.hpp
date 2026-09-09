#pragma once

// The persistence PORT for AI jobs.
//
// Declared in the domain and implemented in store/, like the camera and
// recording repositories — so the job rules are tested against an in-memory
// implementation in milliseconds.

#include <string>
#include <vector>

#include "core/Result.hpp"
#include "vision/AiJob.hpp"

namespace visora::vision {

class AiJobRepository {
public:
    virtual ~AiJobRepository() = default;

    virtual core::Result<std::vector<AiJob>> list() = 0;
    virtual core::Result<std::vector<AiJob>> listForCamera(const std::string& cameraId) = 0;
    virtual core::Result<AiJob> get(const std::string& id) = 0;
    virtual core::Result<AiJob> insert(const AiJob& job) = 0;
    virtual core::Result<AiJob> update(const AiJob& job) = 0;
    virtual core::Status remove(const std::string& id) = 0;
};

}  // namespace visora::vision
