#include "api/QueryParam.hpp"

#include <cerrno>
#include <cstdlib>

#include "core/Time.hpp"

namespace visora::api {
namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string percentDecoded(const oatpp::String& value) {
    if (!value) return {};
    const std::string& in = *value;

    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int high = hexDigit(in[i + 1]);
            const int low = hexDigit(in[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>(high * 16 + low));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

std::int64_t parseInstant(const oatpp::String& value) {
    const std::string text = percentDecoded(value);
    if (text.empty()) return -1;

    const bool digitsOnly =
        text.find_first_not_of("0123456789") == std::string::npos;
    if (digitsOnly) {
        errno = 0;
        char* end = nullptr;
        const long long parsed = std::strtoll(text.c_str(), &end, 10);
        // A value that overflowed or did not consume the whole string is not a
        // timestamp, however digit-like it looked.
        if (errno != 0 || end == nullptr || *end != '\0' || parsed <= 0) return -1;
        return static_cast<std::int64_t>(parsed);
    }
    return core::parseEpochMs(text);
}

}  // namespace visora::api
