#pragma once

// The published wire format for inference results.
//
// This is an EXTERNAL CONTRACT. A Python consumer parses these bytes off a
// Unix socket, and it is deployed separately from this program — a change here
// breaks a component that is not in this repository and may not be redeployed
// at the same time.
//
// Message framing, all integers big-endian:
//
//   [u32 total_len][u32 json_len][json bytes][full jpeg bytes]
//
// `total_len` counts everything after itself. The JSON carries `fullJpegSize`
// so a consumer can slice the trailing blob without guessing.
//
// Floats are written fixed with 4 decimal places. That is part of the contract:
// it bounds message size for the long keypoint and embedding arrays, and
// consumers have been parsing it for as long as this format has existed.
//
// Keys that are absent when empty — `fx1`..`fy2`, `stage`, `text`, `maskGrid`
// and `mask` — stay absent. A single-stage detection job pays nothing for
// features it does not use, and that frugality is why the format is affordable
// at frame rate.
//
// tests/result_wire_tests.cpp pins the exact bytes.

#include <cstdint>
#include <string>
#include <vector>

#include "vision/Detection.hpp"

namespace visora::vision::wire {

// The JSON body alone. Exposed separately so a one-shot HTTP endpoint can
// return exactly what the socket would have carried.
std::string toJson(const Result& result);

// A complete framed message, ready to write to a socket.
std::vector<std::uint8_t> encode(const Result& result);

}  // namespace visora::vision::wire
