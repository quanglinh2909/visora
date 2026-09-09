#include "media/webrtc/Sdp.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <string_view>

namespace visora::media {
namespace {

// SDP lines are CRLF by spec and LF in practice. Splitting on '\n' and trimming
// a trailing '\r' handles both without caring which one arrived.
std::vector<std::string> lines(const std::string& sdp) {
    std::vector<std::string> out;
    std::istringstream in(sdp);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

bool startsWith(const std::string& text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool equalsIgnoringCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// "109 H264/90000" -> {109, "H264"}
bool parseRtpmap(const std::string& value, int& payloadType, std::string& codec) {
    const auto space = value.find(' ');
    if (space == std::string::npos) return false;

    payloadType = 0;
    for (std::size_t i = 0; i < space; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
        payloadType = payloadType * 10 + (value[i] - '0');
    }

    const auto slash = value.find('/', space + 1);
    codec = value.substr(space + 1, slash == std::string::npos ? std::string::npos
                                                              : slash - space - 1);
    return !codec.empty();
}

}  // namespace

bool hasVideoMedia(const std::string& offerSdp) {
    for (const std::string& line : lines(offerSdp)) {
        if (startsWith(line, "m=video")) return true;
    }
    return false;
}

int pickPayloadType(const std::string& offerSdp, const std::string& codec) {
    const auto all = lines(offerSdp);

    // Only the first video m-line. A browser offering several is offering
    // alternatives, and answering across two of them is not a thing.
    std::size_t start = all.size();
    std::size_t end = all.size();
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (startsWith(all[i], "m=")) {
            if (start != all.size()) {
                end = i;
                break;
            }
            if (startsWith(all[i], "m=video")) start = i;
        }
    }
    if (start == all.size()) return -1;

    const bool preferPacketizationMode1 = equalsIgnoringCase(codec, "H264");

    int inRange = -1;
    int outOfRange = -1;
    for (std::size_t i = start; i < end; ++i) {
        if (!startsWith(all[i], "a=rtpmap:")) continue;

        int payloadType = 0;
        std::string offered;
        if (!parseRtpmap(all[i].substr(9), payloadType, offered)) continue;
        if (!equalsIgnoringCase(offered, codec)) continue;

        if (!payloadTypeIsEmittable(payloadType)) {
            if (outOfRange < 0) outOfRange = payloadType;
            continue;
        }
        if (inRange < 0) inRange = payloadType;
        if (!preferPacketizationMode1) return payloadType;

        // Look for this payload type's fmtp and prefer packetization-mode=1.
        const std::string prefix = "a=fmtp:" + std::to_string(payloadType) + " ";
        for (std::size_t j = start; j < end; ++j) {
            if (!startsWith(all[j], prefix)) continue;
            if (all[j].find("packetization-mode=1") != std::string::npos) return payloadType;
            break;
        }
    }

    // In-range first: that path needs no packet rewriting.
    if (inRange >= 0) return inRange;
    return outOfRange;
}

std::string forceRemoteDtlsActive(const std::string& offerSdp) {
    const std::string from = "a=setup:actpass";
    const std::string to = "a=setup:active";

    std::string out;
    out.reserve(offerSdp.size());
    std::size_t at = 0;
    while (true) {
        const auto found = offerSdp.find(from, at);
        if (found == std::string::npos) {
            out.append(offerSdp, at, std::string::npos);
            break;
        }
        out.append(offerSdp, at, found - at);
        out += to;
        at = found + from.size();
    }
    return out;
}

std::vector<RemoteCandidate> remoteCandidates(const std::string& offerSdp) {
    std::vector<RemoteCandidate> out;
    int mlineIndex = -1;

    for (const std::string& line : lines(offerSdp)) {
        if (startsWith(line, "m=")) {
            ++mlineIndex;
            continue;
        }
        if (mlineIndex < 0) continue;  // a session-level candidate has no m-line
        if (!startsWith(line, "a=candidate:")) continue;

        RemoteCandidate candidate;
        candidate.mlineIndex = mlineIndex;
        // webrtcbin wants the value WITHOUT the "a=" but WITH "candidate:".
        candidate.candidate = line.substr(2);
        out.push_back(std::move(candidate));
    }
    return out;
}

}  // namespace visora::media
