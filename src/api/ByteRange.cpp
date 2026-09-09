#include "api/ByteRange.hpp"

namespace visora::api {
namespace {

constexpr const char* kPrefix = "bytes=";

bool isDecimal(const std::string& text) {
    // 18 digits fits int64 with room to spare, and a longer run of digits is
    // not a length any file has — rejecting it beats overflowing on it.
    if (text.empty() || text.size() > 18) return false;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::int64_t toInt64(const std::string& text) {
    std::int64_t value = 0;
    for (const char c : text) value = value * 10 + (c - '0');
    return value;
}

}  // namespace

ByteRange parseByteRange(const std::string& headerValue, std::int64_t fileSize) {
    ByteRange range;

    const std::string prefix(kPrefix);
    if (headerValue.size() <= prefix.size() ||
        headerValue.compare(0, prefix.size(), prefix) != 0) {
        return range;  // absent, or a unit we do not speak — ignore it
    }

    const std::string spec = headerValue.substr(prefix.size());
    if (spec.find(',') != std::string::npos) return range;  // multi-range — ignore

    const auto dash = spec.find('-');
    if (dash == std::string::npos) return range;  // malformed — ignore

    const std::string startText = spec.substr(0, dash);
    const std::string endText = spec.substr(dash + 1);

    if (startText.empty()) {
        // "bytes=-N": the LAST n bytes. A player uses this to read the index at
        // the end of a container without fetching the whole file.
        if (!isDecimal(endText)) return range;
        range.present = true;
        const std::int64_t suffix = toInt64(endText);
        if (suffix <= 0 || fileSize <= 0) return range;  // understood, unsatisfiable
        range.start = suffix >= fileSize ? 0 : fileSize - suffix;
        range.end = fileSize - 1;
        range.satisfiable = true;
        return range;
    }

    if (!isDecimal(startText)) return range;
    if (!endText.empty() && !isDecimal(endText)) return range;

    range.present = true;
    range.start = toInt64(startText);
    range.end = endText.empty() ? fileSize - 1 : toInt64(endText);
    // Clamp rather than reject: asking for more than the file holds is what
    // every player does on its last request.
    if (range.end > fileSize - 1) range.end = fileSize - 1;
    range.satisfiable = fileSize > 0 && range.start < fileSize && range.start <= range.end;
    return range;
}

std::string contentRangeHeader(const ByteRange& range, std::int64_t fileSize) {
    if (!range.satisfiable) return "bytes */" + std::to_string(fileSize);
    return "bytes " + std::to_string(range.start) + '-' + std::to_string(range.end) + '/' +
           std::to_string(fileSize);
}

}  // namespace visora::api
