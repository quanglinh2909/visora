#pragma once

// One GStreamer element and its properties, as data rather than as a string
// someone assembled by hand.
//
// The predecessor built pipelines with std::ostringstream in ten different
// places, each embedding element names directly. Supporting a new decoder meant
// finding and editing all ten, and not one line of that construction was
// covered by a test — the strings only existed at run time, on a machine with a
// camera attached.
//
// As data, an element can be produced by a CodecProvider, substituted, compared
// and asserted on. The rendering to launch syntax happens once, here.

#include <string>
#include <utility>
#include <vector>

namespace visora::media {

class ElementSpec {
public:
    ElementSpec() = default;
    explicit ElementSpec(std::string factory) : m_factory(std::move(factory)) {}

    // Names the instance, so other parts of the pipeline and the application can
    // refer to it (`gst_bin_get_by_name`, tee branches).
    ElementSpec& named(std::string instanceName);

    ElementSpec& set(std::string property, std::string value);
    ElementSpec& set(std::string property, const char* value);
    ElementSpec& set(std::string property, int value);
    ElementSpec& set(std::string property, unsigned value);
    ElementSpec& set(std::string property, long long value);
    ElementSpec& set(std::string property, double value);
    ElementSpec& set(std::string property, bool value);

    // A value that must be quoted and escaped — a URL with characters the
    // launch parser would otherwise treat as syntax.
    ElementSpec& setQuoted(std::string property, const std::string& value);

    bool valid() const { return !m_factory.empty(); }
    const std::string& factory() const { return m_factory; }
    const std::string& instanceName() const { return m_instanceName; }

    // Whether a property was set, and its rendered value. For tests and for a
    // provider that wants to inspect what another produced.
    bool has(const std::string& property) const;
    std::string valueOf(const std::string& property) const;

    // "mpph264enc name=enc bps=4000000 rc-mode=vbr"
    std::string toLaunch() const;

private:
    ElementSpec& setRaw(std::string property, std::string rendered);

    std::string m_factory;
    std::string m_instanceName;
    // A vector, not a map: property order is part of the rendered string, and a
    // stable order is what makes golden-string tests possible.
    std::vector<std::pair<std::string, std::string>> m_properties;
};

// Quotes and escapes a value for gst_parse_launch. Exposed because raw
// fragments occasionally need it too.
std::string quoteLaunchValue(const std::string& value);

}  // namespace visora::media
