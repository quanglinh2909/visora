#pragma once

// Reading a query parameter that arrived percent-encoded.
//
// oatpp hands query values over EXACTLY as they appeared on the wire — it
// splits on '&' and '=' and stops there. That is fine until something between
// the browser and here re-encodes the URL, and something always does: the
// frontend proxies every engine call through a Next.js API route, which
// rebuilds the target URL and encodes ':' as "%3A" on the way. An ISO timestamp
// then arrives as "2026-09-10T04%3A49%3A31Z", the timestamp parser refuses it,
// and the endpoint answers 400 to a request the client formed correctly.
//
// The symptom is worse than the cause: a timeline whose fetch returns 400 shows
// an EMPTY day rather than an error, so a camera that is recording perfectly
// well looks like it is recording nothing.
//
// Applied to every string query parameter rather than only to the timestamps,
// because the encoding is a property of the transport and not of what the value
// happens to mean.

#include <cstdint>
#include <string>

#include "oatpp/core/Types.hpp"

namespace visora::api {

// Percent-decoding, and ONLY percent-decoding.
//
// '+' is deliberately left alone. It means a space in form-encoded bodies, and
// a query string is close enough to that convention for the substitution to
// look right — but the values that reach here are ISO timestamps, where '+' is
// the sign of a timezone offset. Turning "+07:00" into " 07:00" would break the
// exact parameter this function exists to rescue.
//
// A stray '%' that does not begin a valid escape is passed through unchanged:
// the caller's parser gives a better error about the whole value than this
// could about one character.
std::string percentDecoded(const oatpp::String& value);

// A moment in time from a query parameter, decoded first, in EITHER of the two
// forms clients actually send. Returns -1 for anything else, and for an empty
// value — the caller decides what "not given" means, which differs per endpoint
// ("now" for a seek, "the most recent" for a thumbnail).
//
// Two forms because the published API has two and always did: the timeline
// asks for recordings with ISO instants (`new Date(ms).toISOString()`) and for
// a hover thumbnail with epoch milliseconds (`Math.round(atMs)`). Accepting one
// and rejecting the other answers 400 to half the timeline, which is how a
// camera that is recording perfectly ends up looking like it never started.
//
// All digits means epoch milliseconds; anything else goes to the ISO parser.
// The two cannot be confused: an ISO instant is at least nineteen characters
// and none of the forms is bare digits.
std::int64_t parseInstant(const oatpp::String& value);

}  // namespace visora::api
