#pragma once

// Wall-clock timestamps, in the one format this system writes.
//
// ISO-8601, UTC, second resolution: "2026-09-09T12:59:07Z". Everything a client
// sees uses it, so it is defined once rather than formatted at each call site
// with a slightly different pattern.
//
// UTC on purpose. A recording index and an event log written in local time are
// unreadable across a daylight-saving change and unmergeable across machines.

#include <chrono>
#include <string>

namespace visora::core {

std::string toIso8601(std::chrono::system_clock::time_point when);

inline std::string nowIso8601() { return toIso8601(std::chrono::system_clock::now()); }

}  // namespace visora::core
