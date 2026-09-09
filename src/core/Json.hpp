#pragma once

// The one JSON string escaper.
//
// Small, and deliberately shared: two hand-written escapers drift, and the one
// that forgets control characters emits JSON a strict parser rejects — for a
// camera someone named with a stray tab, or an error message carrying a NUL
// from a driver.
//
// This is not a JSON library. Structured output that a client consumes goes
// through oatpp's mapper; this is for the few places that write a small
// fixed-shape message by hand because the shape IS the contract.

#include <string>
#include <string_view>

namespace visora::core {

// Appends `text` to `out` as a quoted, escaped JSON string, quotes included.
void appendJsonString(std::string& out, std::string_view text);

// The same, as a value.
std::string jsonString(std::string_view text);

}  // namespace visora::core
