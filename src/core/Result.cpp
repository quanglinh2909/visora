#include "core/Result.hpp"

namespace visora::core {

const char* toString(ErrorCode code) {
    switch (code) {
        case ErrorCode::Unsupported:     return "unsupported";
        case ErrorCode::InvalidArgument: return "invalid-argument";
        case ErrorCode::NotFound:        return "not-found";
        case ErrorCode::HardwareFailure: return "hardware-failure";
        case ErrorCode::Internal:        return "internal";
    }
    return "unknown";
}

std::string Error::str() const {
    std::string out = toString(code);
    if (!message.empty()) {
        out += ": ";
        out += message;
    }
    return out;
}

}  // namespace visora::core
