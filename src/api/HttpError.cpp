#include "api/HttpError.hpp"

#include "core/Log.hpp"

namespace visora::api {

void abortWith(const core::Error& error) {
    // Logged at the boundary, once, with the code — so a 400 caused by a client
    // and a 500 caused by us are distinguishable in the log without correlating
    // request ids.
    if (error.code == core::ErrorCode::Internal ||
        error.code == core::ErrorCode::HardwareFailure) {
        VS_ERROR("http") << "request failed: " << error.str();
    } else {
        VS_DEBUG("http") << "request rejected: " << error.str();
    }

    throw oatpp::web::protocol::http::HttpError(httpStatusFor(error.code),
                                                error.message.c_str());
}

}  // namespace visora::api
