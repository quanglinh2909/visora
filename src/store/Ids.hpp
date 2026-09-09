#pragma once

// Identifiers for the in-memory repositories.
//
// UUID-shaped so an id from an in-memory store is interchangeable with one from
// PostgreSQL — including in a URL, where a differently shaped id would only
// break once someone switched backends.
//
// Shared between the repositories: two counters producing ids in the same shape
// is fine, two DIFFERENT shapes is a bug waiting for the first time a test
// passes in memory and fails against a database.

#include <cstdint>
#include <cstdio>
#include <string>

namespace visora::store {

// `prefix` distinguishes one repository's ids from another's, so a segment id
// and a camera id are never accidentally the same string in a test.
inline std::string makeUuidLikeId(unsigned prefix, std::uint64_t counter) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%08x-0000-4000-8000-%012llx", prefix,
                  static_cast<unsigned long long>(counter));
    return buffer;
}

}  // namespace visora::store
