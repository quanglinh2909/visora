#include "core/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace visora::core::log {
namespace {

Level parseLevel(std::string_view text, Level fallback) {
    if (text == "trace") return Level::Trace;
    if (text == "debug") return Level::Debug;
    if (text == "info")  return Level::Info;
    if (text == "warn")  return Level::Warn;
    if (text == "error") return Level::Error;
    if (text == "off")   return Level::Off;
    return fallback;
}

const char* levelTag(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
        case Level::Off:   return "OFF  ";
    }
    return "?????";
}

std::vector<std::string> splitCsv(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ',' || c == ' ') {
            if (!current.empty()) out.push_back(std::exchange(current, std::string{}));
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(std::move(current));
    return out;
}

// One mutable state object, initialised from the environment on first use.
struct State {
    std::mutex mutex;
    Level level = Level::Info;
    std::unordered_set<std::string> categories;  // empty = allow all

    State() {
        if (const char* env = std::getenv("VISORA_LOG_LEVEL")) {
            level = parseLevel(env, Level::Info);
        }
        if (const char* env = std::getenv("VISORA_LOG_CATEGORIES")) {
            for (auto& name : splitCsv(env)) categories.insert(std::move(name));
        }
    }
};

State& state() {
    static State s;
    return s;
}

}  // namespace

Level level() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.level;
}

void setLevel(Level level) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.level = level;
}

void setCategories(std::string_view commaSeparated) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.categories.clear();
    for (auto& name : splitCsv(commaSeparated)) s.categories.insert(std::move(name));
}

bool enabled(Level level, std::string_view category) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (level < s.level || s.level == Level::Off) return false;
    if (s.categories.empty()) return true;
    return s.categories.find(std::string(category)) != s.categories.end();
}

Line::Line(Level level, std::string_view category)
    : m_level(level), m_category(category) {}

Line::~Line() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()) % 1000;

    std::tm tm{};
    ::localtime_r(&seconds, &tm);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(millis.count()));

    // One fwrite so concurrent threads cannot interleave halves of a line.
    const std::string body = m_stream.str();
    std::string line;
    line.reserve(body.size() + 48);
    line += stamp;
    line += ' ';
    line += levelTag(m_level);
    line += " [";
    line.append(m_category.data(), m_category.size());
    line += "] ";
    line += body;
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), stderr);
}

}  // namespace visora::core::log
