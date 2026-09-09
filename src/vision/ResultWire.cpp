#include "vision/ResultWire.hpp"

#include <arpa/inet.h>

#include <ios>
#include <sstream>

#include "core/Json.hpp"

namespace visora::vision::wire {
namespace {

void escape(std::ostringstream& out, const std::string& text) {
    out << core::jsonString(text);
}

void writeFloatArray(std::ostringstream& out, const std::vector<float>& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out << ',';
        out << values[i];
    }
    out << ']';
}

void writeDetection(std::ostringstream& out, const Detection& d) {
    out << '{';
    out << "\"x1\":" << d.x1 << ",\"y1\":" << d.y1 << ",\"x2\":" << d.x2 << ",\"y2\":" << d.y2
        << ',';
    out << "\"score\":" << d.score << ',';
    out << "\"classId\":" << d.classId << ',';

    if (d.hasFrameBox) {
        out << "\"fx1\":" << d.fx1 << ",\"fy1\":" << d.fy1 << ",\"fx2\":" << d.fx2
            << ",\"fy2\":" << d.fy2 << ',';
    }
    if (d.stage > 0) {
        out << "\"stage\":" << d.stage << ',';
    }
    if (!d.text.empty()) {
        out << "\"text\":";
        escape(out, d.text);
        out << ',';
    }

    out << "\"keypoints\":";
    writeFloatArray(out, d.keypoints);
    out << ',';

    if (!d.maskBits.empty()) {
        static const char* kHex = "0123456789abcdef";
        out << "\"maskGrid\":" << Detection::kMaskGrid << ',';
        out << "\"mask\":\"";
        for (const unsigned char byte : d.maskBits) {
            out << kHex[byte >> 4] << kHex[byte & 0x0F];
        }
        out << "\",";
    }

    out << "\"embedding\":";
    writeFloatArray(out, d.embedding);
    out << ',';

    out << "\"children\":[";
    for (std::size_t i = 0; i < d.children.size(); ++i) {
        if (i != 0) out << ',';
        writeDetection(out, d.children[i]);
    }
    out << ']';
    out << '}';
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    const std::uint32_t be = ::htonl(value);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&be);
    out.insert(out.end(), bytes, bytes + 4);
}

}  // namespace

std::string toJson(const Result& result) {
    std::ostringstream out;
    // Part of the contract, not a formatting preference — see ResultWire.hpp.
    out.setf(std::ios::fixed);
    out.precision(4);

    out << '{';
    out << "\"cameraId\":";
    escape(out, result.cameraId);
    out << ',';
    out << "\"jobId\":";
    escape(out, result.jobId);
    out << ',';
    out << "\"seq\":" << result.seq << ',';
    out << "\"tsUs\":" << result.tsUs << ',';
    out << "\"origWidth\":" << result.origWidth << ',';
    out << "\"origHeight\":" << result.origHeight << ',';
    out << "\"fullJpegSize\":" << result.fullJpeg.size() << ',';
    out << "\"detections\":[";
    for (std::size_t i = 0; i < result.detections.size(); ++i) {
        if (i != 0) out << ',';
        writeDetection(out, result.detections[i]);
    }
    out << "]}";
    return out.str();
}

std::vector<std::uint8_t> encode(const Result& result) {
    const std::string json = toJson(result);
    const std::size_t bodyLength = 4 + json.size() + result.fullJpeg.size();

    std::vector<std::uint8_t> message;
    message.reserve(4 + bodyLength);
    appendU32(message, static_cast<std::uint32_t>(bodyLength));
    appendU32(message, static_cast<std::uint32_t>(json.size()));
    message.insert(message.end(), json.begin(), json.end());
    message.insert(message.end(), result.fullJpeg.begin(), result.fullJpeg.end());
    return message;
}

}  // namespace visora::vision::wire
