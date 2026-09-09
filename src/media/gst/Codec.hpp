#pragma once

// Video codec vocabulary, and the elements that are the same on every machine.
//
// Depayloaders, parsers and payloaders are not hardware choices — rtph264depay
// is rtph264depay everywhere. Keeping them out of CodecProvider means a new
// hardware provider only implements the three things that actually differ:
// decode, encode, and JPEG encode.

#include <string>
#include <string_view>

namespace visora::media {

enum class Codec {
    Unknown,
    H264,
    H265,
};

Codec codecFromString(std::string_view text);
const char* toString(Codec codec);

// RTP caps encoding-name, e.g. "H264".
const char* encodingName(Codec codec);

const char* depayloader(Codec codec);   // rtph264depay
const char* parser(Codec codec);        // h264parse
const char* payloader(Codec codec);     // rtph264pay

// Caps of the parsed elementary stream, as recording and muxing want it.
const char* parsedCaps(Codec codec);

}  // namespace visora::media
