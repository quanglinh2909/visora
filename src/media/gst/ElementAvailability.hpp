#pragma once

// Does this machine have that GStreamer element?
//
// A seam on purpose. The real implementation asks the GStreamer registry, but
// provider selection — "prefer MPP, fall back to VA-API, then software" — is
// pure logic, and it is worth being able to test that logic for hardware this
// machine does not have. Without the seam those paths would only ever be
// exercised on the one board that happens to be plugged in.

#include <functional>
#include <string>

namespace visora::media {

using ElementProbe = std::function<bool(const std::string& factoryName)>;

// Replaces the probe. Pass nullptr to restore the GStreamer-backed default.
void setElementProbe(ElementProbe probe);

bool elementExists(const std::string& factoryName);

// Bumped every time the probe changes.
//
// Anything that caches a decision derived from element availability must
// rebuild when this moves. A cache derived from the probe that outlives a probe
// change is simply wrong — it reports what some earlier machine looked like.
unsigned long elementProbeGeneration();

}  // namespace visora::media
