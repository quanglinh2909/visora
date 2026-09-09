#include "hal/Capabilities.hpp"

#include <sys/utsname.h>

#include <sstream>

#include "hal/ImageOps.hpp"
#include "hal/InferenceBackend.hpp"

namespace visora::hal {
namespace {

void appendJsonString(std::ostringstream& out, const std::string& text) {
    out << '"';
    for (const char c : text) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n";  break;
            case '\r': out << "\\r";  break;
            case '\t': out << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out << "\\u00" << std::hex << (static_cast<int>(c) >> 4)
                        << (static_cast<int>(c) & 0xF) << std::dec;
                } else {
                    out << c;
                }
        }
    }
    out << '"';
}

}  // namespace

Capabilities detectCapabilities() {
    Capabilities caps;

    utsname info{};
    if (::uname(&info) == 0) {
        caps.osName = info.sysname;
        caps.architecture = info.machine;
    }

    auto image = imageOps();
    if (image) {
        caps.imageOpsSelected = std::string(image.value()->id());
    } else {
        caps.imageOpsProblem = image.error().message;
    }

    auto infer = inference();
    if (infer) {
        caps.inferenceSelected = std::string(infer.value()->id());
    } else {
        caps.inferenceProblem = infer.error().message;
    }

    for (auto& row : imageOpsRegistry().status("image-ops", caps.imageOpsSelected)) {
        caps.backends.push_back(std::move(row));
    }
    for (auto& row : inferenceRegistry().status("inference", caps.inferenceSelected)) {
        caps.backends.push_back(std::move(row));
    }

    return caps;
}

std::string toText(const Capabilities& caps) {
    std::ostringstream out;
    out << "Visora capabilities\n";
    out << "  platform      : " << caps.osName << ' ' << caps.architecture << '\n';
    out << "  image ops     : "
        << (caps.imageOpsSelected.empty() ? "NONE (" + caps.imageOpsProblem + ")"
                                          : caps.imageOpsSelected)
        << '\n';
    out << "  inference     : "
        << (caps.inferenceSelected.empty() ? "NONE (" + caps.inferenceProblem + ")"
                                           : caps.inferenceSelected)
        << '\n';
    out << "  AI            : " << (caps.aiEnabled() ? "enabled" : "disabled") << "\n\n";

    out << "  backends (highest priority first)\n";
    if (caps.backends.empty()) {
        out << "    (none compiled in)\n";
        return out.str();
    }

    std::string kind;
    for (const BackendStatus& row : caps.backends) {
        if (row.kind != kind) {
            kind = row.kind;
            out << "    " << kind << ":\n";
        }
        out << "      " << (row.selected ? '*' : ' ') << ' '
            << (row.available ? "[ok]   " : "[skip] ") << row.id
            << " (priority " << row.priority << ") - " << row.detail << '\n';
    }
    return out.str();
}

std::string toJson(const Capabilities& caps) {
    std::ostringstream out;
    out << "{\"platform\":{\"os\":";
    appendJsonString(out, caps.osName);
    out << ",\"arch\":";
    appendJsonString(out, caps.architecture);
    out << "},\"imageOps\":{\"selected\":";
    if (caps.imageOpsSelected.empty()) {
        out << "null,\"problem\":";
        appendJsonString(out, caps.imageOpsProblem);
    } else {
        appendJsonString(out, caps.imageOpsSelected);
        out << ",\"problem\":null";
    }
    out << "},\"inference\":{\"selected\":";
    if (caps.inferenceSelected.empty()) {
        out << "null,\"problem\":";
        appendJsonString(out, caps.inferenceProblem);
    } else {
        appendJsonString(out, caps.inferenceSelected);
        out << ",\"problem\":null";
    }
    out << "},\"ai\":{\"enabled\":" << (caps.aiEnabled() ? "true" : "false")
        << "},\"backends\":[";

    bool first = true;
    for (const BackendStatus& row : caps.backends) {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":";
        appendJsonString(out, row.id);
        out << ",\"kind\":";
        appendJsonString(out, row.kind);
        out << ",\"priority\":" << row.priority
            << ",\"available\":" << (row.available ? "true" : "false")
            << ",\"selected\":" << (row.selected ? "true" : "false")
            << ",\"detail\":";
        appendJsonString(out, row.detail);
        out << '}';
    }
    out << "]}";
    return out.str();
}

}  // namespace visora::hal
