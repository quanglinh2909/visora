#pragma once

// One place that turns a domain error into an HTTP status.
//
// The predecessor scattered OATPP_ASSERT_HTTP through its controllers, so the
// status for "camera not found" was decided independently in each endpoint that
// could produce it — and a service that returned a bare false surfaced a
// constraint violation as 500.
//
// Here the domain says what went wrong in its own vocabulary and this is the
// only code that knows what that means over HTTP.

#include <string>

#include "core/Result.hpp"
#include "oatpp/web/protocol/http/Http.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

namespace visora::api {

using Status = oatpp::web::protocol::http::Status;

inline const Status& httpStatusFor(core::ErrorCode code) {
    switch (code) {
        // The caller sent something we cannot act on.
        case core::ErrorCode::InvalidArgument: return Status::CODE_400;
        case core::ErrorCode::NotFound:        return Status::CODE_404;
        // The request is reasonable; this deployment cannot serve it. 503
        // rather than 501, because it may well work after a restart on a
        // machine with the right hardware — which is exactly the situation on a
        // board with no NPU.
        case core::ErrorCode::Unsupported:     return Status::CODE_503;
        case core::ErrorCode::HardwareFailure: return Status::CODE_503;
        case core::ErrorCode::Internal:        return Status::CODE_500;
    }
    return Status::CODE_500;
}

// Aborts the request with the domain's own message. Used by controllers so the
// reason a service gave reaches the client instead of being replaced by a
// generic string.
[[noreturn]] void abortWith(const core::Error& error);

// Unwraps a Result or aborts. Keeps an endpoint down to the one line that
// actually does something.
template <class T>
T valueOrAbort(core::Result<T> result) {
    if (!result.ok()) abortWith(result.error());
    return std::move(result.value());
}

inline void okOrAbort(const core::Status& status) {
    if (!status.ok()) abortWith(status.error());
}

}  // namespace visora::api
