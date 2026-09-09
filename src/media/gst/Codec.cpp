#include "media/gst/Codec.hpp"

#include <algorithm>
#include <cctype>

namespace visora::media {

Codec codecFromString(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char c : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lowered == "h264" || lowered == "avc") return Codec::H264;
    if (lowered == "h265" || lowered == "hevc") return Codec::H265;
    return Codec::Unknown;
}

const char* toString(Codec codec) {
    switch (codec) {
        case Codec::H264:    return "h264";
        case Codec::H265:    return "h265";
        case Codec::Unknown: return "unknown";
    }
    return "unknown";
}

const char* encodingName(Codec codec) {
    switch (codec) {
        case Codec::H264:    return "H264";
        case Codec::H265:    return "H265";
        case Codec::Unknown: return "";
    }
    return "";
}

const char* depayloader(Codec codec) {
    switch (codec) {
        case Codec::H264:    return "rtph264depay";
        case Codec::H265:    return "rtph265depay";
        case Codec::Unknown: return "";
    }
    return "";
}

const char* parser(Codec codec) {
    switch (codec) {
        case Codec::H264:    return "h264parse";
        case Codec::H265:    return "h265parse";
        case Codec::Unknown: return "";
    }
    return "";
}

const char* payloader(Codec codec) {
    switch (codec) {
        case Codec::H264:    return "rtph264pay";
        case Codec::H265:    return "rtph265pay";
        case Codec::Unknown: return "";
    }
    return "";
}

const char* parsedCaps(Codec codec) {
    switch (codec) {
        case Codec::H264:    return "video/x-h264,stream-format=byte-stream,alignment=au";
        case Codec::H265:    return "video/x-h265,stream-format=byte-stream,alignment=au";
        case Codec::Unknown: return "";
    }
    return "";
}

}  // namespace visora::media
