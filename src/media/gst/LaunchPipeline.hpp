#pragma once

// A GStreamer pipeline description, assembled from elements rather than
// concatenated as text.
//
// Two rendering modes, and the difference matters:
//
//   * unwrapped, for gst_parse_launch — which returns a real GstPipeline only
//     for a description with no surrounding parentheses;
//   * wrapped in "( ... )", for gst_rtsp_media_factory_set_launch.
//
// Getting that backwards produces a GstBin where a GstPipeline was expected,
// and the failure appears far from the cause.

#include <string>
#include <vector>

#include "media/gst/ElementSpec.hpp"

namespace visora::media {

// One linear run of elements joined by " ! ".
class LaunchChain {
public:
    LaunchChain& add(const ElementSpec& element);
    LaunchChain& add(const std::string& factory);

    // A caps filter, e.g. "video/x-raw,width=320". Written as-is between the
    // surrounding elements.
    LaunchChain& caps(const std::string& capsString);

    // An escape hatch for a fragment this class has no vocabulary for. Prefer
    // add(): a raw fragment cannot be inspected or substituted.
    LaunchChain& raw(const std::string& fragment);

    bool empty() const { return m_parts.empty(); }
    std::string toLaunch() const;

private:
    std::vector<std::string> m_parts;
};

class LaunchPipeline {
public:
    // The main chain. Called repeatedly, it returns the same chain.
    LaunchChain& chain();

    // A branch off a named tee, rendered as "teeName. ! ...".
    LaunchChain& branch(const std::string& teeName);

    // `wrapped` puts the whole description in parentheses, which is what
    // gst_rtsp_media_factory_set_launch wants and what gst_parse_launch must
    // NOT be given.
    std::string toLaunch(bool wrapped = false) const;

private:
    struct Branch {
        std::string teeName;  // empty for the main chain
        LaunchChain chain;
    };
    std::vector<Branch> m_branches;
};

}  // namespace visora::media
