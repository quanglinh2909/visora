#include "core/Json.hpp"

#include <cstdio>

namespace visora::core {

void appendJsonString(std::string& out, std::string_view text) {
    out += '"';
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    // \u00XX is the only legal way to carry a control character
                    // in JSON. Bytes >= 0x20 pass through untouched, which
                    // keeps UTF-8 intact without decoding it.
                    char buffer[7];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out += raw;
                }
        }
    }
    out += '"';
}

std::string jsonString(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    appendJsonString(out, text);
    return out;
}

}  // namespace visora::core
