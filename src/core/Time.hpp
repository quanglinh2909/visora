#pragma once

// Wall-clock timestamps, in the one format this system writes.
//
// ISO-8601, UTC: "2026-09-09T12:59:07Z", or with milliseconds where a
// recording timeline needs them. Everything a client sees uses it, so it is
// defined once rather than formatted at each call site with a slightly
// different pattern.
//
// UTC on purpose. A recording index and an event log written in local time are
// unreadable across a daylight-saving change and unmergeable across machines.
//
// Milliseconds since the epoch is the internal currency: a timeline compares,
// sorts and subtracts instants far more than it prints them, and doing that on
// formatted strings is how off-by-one-hour bugs get in.

#include <chrono>
#include <cstdint>
#include <string>

namespace visora::core {

std::string toIso8601(std::chrono::system_clock::time_point when);

inline std::string nowIso8601() { return toIso8601(std::chrono::system_clock::now()); }

// Milliseconds since the Unix epoch, UTC.
std::int64_t nowEpochMs();

// "2026-09-09T12:59:07.512Z". Used where a sub-second difference is visible —
// a playback cursor lands on the wrong frame otherwise.
std::string toIso8601Millis(std::int64_t epochMs);

std::string toIso8601(std::int64_t epochMs);

// Parses what a database or a client sends. Accepts "2026-09-09T12:59:07Z",
// "2026-09-09 12:59:07.512+00", "2026-09-09T12:59:07+07:00" and the same
// without a zone (read as UTC).
//
// Returns -1 rather than throwing: this is called from GStreamer threads, where
// an exception escaping ends the process.
std::int64_t parseEpochMs(const std::string& text);

// Local time, "2026-09-09 19:59:07" — for a recording FILE NAME only.
//
// Deliberately local and deliberately not the format above: an operator
// browsing the recordings directory with `ls` reads these, and a name in UTC
// sends them to the wrong hour. Nothing parses it back; the database holds the
// authoritative instant.
std::string nowLocalFileTimestamp();

// "2026-09-09", local. Recordings and snapshots are foldered by day so one
// directory does not reach tens of thousands of files.
std::string localDateStamp(std::int64_t epochMs);

}  // namespace visora::core
