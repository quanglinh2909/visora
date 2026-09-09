#include "media/gst/ElementAvailability.hpp"

#include <gst/gst.h>

#include <mutex>

namespace visora::media {
namespace {

bool askGStreamer(const std::string& factoryName) {
    // gst_init must have run. It is idempotent and cheap, and a provider probe
    // is the first thing that touches GStreamer in some processes (the
    // capability report, for one), so do it here rather than requiring every
    // caller to remember.
    static std::once_flag initialised;
    std::call_once(initialised, [] { gst_init(nullptr, nullptr); });

    GstElementFactory* factory = gst_element_factory_find(factoryName.c_str());
    if (factory == nullptr) return false;
    gst_object_unref(factory);
    return true;
}

std::mutex& probeMutex() {
    static std::mutex mutex;
    return mutex;
}

ElementProbe& probeSlot() {
    static ElementProbe probe = &askGStreamer;
    return probe;
}

unsigned long& generationSlot() {
    static unsigned long generation = 1;
    return generation;
}

}  // namespace

void setElementProbe(ElementProbe probe) {
    std::lock_guard<std::mutex> lock(probeMutex());
    probeSlot() = probe ? std::move(probe) : ElementProbe(&askGStreamer);
    ++generationSlot();
}

unsigned long elementProbeGeneration() {
    std::lock_guard<std::mutex> lock(probeMutex());
    return generationSlot();
}

bool elementExists(const std::string& factoryName) {
    ElementProbe probe;
    {
        std::lock_guard<std::mutex> lock(probeMutex());
        probe = probeSlot();
    }
    return probe && probe(factoryName);
}

}  // namespace visora::media
