#include "core/Time.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace visora::core {
namespace {

// The _r forms throughout: the shared-buffer gmtime()/localtime() would be a
// data race between the recording thread and a request thread formatting at the
// same moment.
std::tm utcParts(std::time_t seconds) {
    std::tm out{};
    gmtime_r(&seconds, &out);
    return out;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Reads `count` digits at `at`, or returns false. Deliberately strict: a
// timestamp that is nearly right is worse than one that is rejected, because it
// silently places a recording an hour or a century away.
bool readInt(const std::string& text, std::size_t at, std::size_t count, int& out) {
    if (at + count > text.size()) return false;
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!isDigit(text[at + i])) return false;
        value = value * 10 + (text[at + i] - '0');
    }
    out = value;
    return true;
}

}  // namespace

std::string toIso8601(std::chrono::system_clock::time_point when) {
    const std::tm utc = utcParts(std::chrono::system_clock::to_time_t(when));
    char buffer[32];
    const std::size_t written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buffer, written);
}

std::int64_t nowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string toIso8601(std::int64_t epochMs) {
    return toIso8601(std::chrono::system_clock::time_point(std::chrono::milliseconds(epochMs)));
}

std::string toIso8601Millis(std::int64_t epochMs) {
    // Floor division, so an instant before 1970 does not round the wrong way
    // and produce a negative millisecond field.
    std::int64_t seconds = epochMs / 1000;
    std::int64_t millis = epochMs % 1000;
    if (millis < 0) {
        millis += 1000;
        --seconds;
    }
    const std::tm utc = utcParts(static_cast<std::time_t>(seconds));
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                  utc.tm_sec, static_cast<int>(millis));
    return buffer;
}

std::int64_t parseEpochMs(const std::string& text) {
    // YYYY-MM-DD, then 'T' or ' ', then HH:MM:SS — the shortest accepted form.
    if (text.size() < 19) return -1;

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!readInt(text, 0, 4, year) || text[4] != '-') return -1;
    if (!readInt(text, 5, 2, month) || text[7] != '-') return -1;
    if (!readInt(text, 8, 2, day)) return -1;
    if (text[10] != 'T' && text[10] != 't' && text[10] != ' ') return -1;
    if (!readInt(text, 11, 2, hour) || text[13] != ':') return -1;
    if (!readInt(text, 14, 2, minute) || text[16] != ':') return -1;
    if (!readInt(text, 17, 2, second)) return -1;

    std::size_t at = 19;
    std::int64_t millis = 0;
    if (at < text.size() && text[at] == '.') {
        ++at;
        // Read every fractional digit but keep three. A database that returns
        // microseconds must not be truncated to a whole second, and one that
        // returns a single digit means tenths, not thousandths.
        int scale = 100;
        while (at < text.size() && isDigit(text[at])) {
            if (scale > 0) {
                millis += (text[at] - '0') * scale;
                scale /= 10;
            }
            ++at;
        }
    }

    // Zone. Absent means UTC: everything this program writes is UTC, and
    // guessing local time for a value that omits the zone is how a timeline
    // shifts by the machine's offset.
    std::int64_t offsetSeconds = 0;
    if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
        const int sign = text[at] == '-' ? -1 : 1;
        ++at;
        int offsetHour = 0;
        int offsetMinute = 0;
        if (!readInt(text, at, 2, offsetHour)) return -1;
        at += 2;
        if (at < text.size() && text[at] == ':') ++at;
        if (at < text.size() && isDigit(text[at])) {
            if (!readInt(text, at, 2, offsetMinute)) return -1;
        }
        offsetSeconds = sign * (offsetHour * 3600LL + offsetMinute * 60LL);
    }

    std::tm parts{};
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_sec = second;
    // timegm, never mktime: mktime reads the machine's timezone, so the same
    // string would parse to a different instant on two machines.
    const std::time_t utcSeconds = timegm(&parts);
    if (utcSeconds == static_cast<std::time_t>(-1)) return -1;

    return (static_cast<std::int64_t>(utcSeconds) - offsetSeconds) * 1000 + millis;
}

std::string nowLocalFileTimestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buffer[32];
    const std::size_t written =
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return std::string(buffer, written);
}

std::string localDateStamp(std::int64_t epochMs) {
    const std::time_t seconds = static_cast<std::time_t>(epochMs / 1000);
    std::tm local{};
    localtime_r(&seconds, &local);
    char buffer[16];
    const std::size_t written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
    return std::string(buffer, written);
}

}  // namespace visora::core
