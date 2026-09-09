#pragma once

// HTTP Range parsing, for serving a recorded segment.
//
// Pure and separate from the controller, because the rules are fiddly and every
// one of them is an off-by-one that shows up as a video that will not seek.
// RFC 7233 in one line: a Range header that cannot be understood is IGNORED —
// the server answers 200 with the whole file — while one that is understood but
// impossible to satisfy is 416.

#include <cstdint>
#include <string>

namespace visora::api {

struct ByteRange {
    // A single "bytes=" range was supplied and parsed. False means the caller
    // should serve the whole file with 200.
    bool present = false;
    // The range is valid for this file. False WITH present=true is 416.
    bool satisfiable = false;
    std::int64_t start = 0;  // inclusive
    std::int64_t end = 0;    // inclusive

    std::int64_t length() const { return satisfiable ? end - start + 1 : 0; }
};

// Parses a Range header value against a known file size. Only a single range is
// supported: multi-range responses need multipart/byteranges, no player asks
// for one, and answering the first range instead would be wrong rather than
// merely incomplete.
ByteRange parseByteRange(const std::string& headerValue, std::int64_t fileSize);

// "bytes 0-1023/4096", for the Content-Range header of a 206.
std::string contentRangeHeader(const ByteRange& range, std::int64_t fileSize);

}  // namespace visora::api
