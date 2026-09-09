#pragma once

// Levelled, categorised logging.
//
// Usage:
//   VS_INFO("hal") << "selected image backend " << id;
//   VS_WARN("rga") << "dma-heap unavailable, falling back";
//
// Runtime control (read once, on first use):
//   VISORA_LOG_LEVEL=trace|debug|info|warn|error|off      default: info
//   VISORA_LOG_CATEGORIES=hal,rga                         default: all
//
// The message is only formatted when the level and category are enabled, so a
// disabled VS_TRACE costs one comparison rather than building a string.

#include <sstream>
#include <string>
#include <string_view>

namespace visora::core::log {

enum class Level { Trace, Debug, Info, Warn, Error, Off };

Level level();
void setLevel(Level level);

// Restricts output to these categories. Empty = every category.
void setCategories(std::string_view commaSeparated);

bool enabled(Level level, std::string_view category);

// Collects one line and emits it on destruction.
class Line {
public:
    Line(Level level, std::string_view category);
    ~Line();

    Line(const Line&) = delete;
    Line& operator=(const Line&) = delete;

    std::ostringstream& stream() { return m_stream; }

private:
    Level m_level;
    std::string_view m_category;
    std::ostringstream m_stream;
};

}  // namespace visora::core::log

// The `if/else` shape keeps the macro safe inside an unbraced if-statement and
// keeps the right-hand side unevaluated when the level is off.
#define VS_LOG_AT(lvl, cat)                                            \
    if (!::visora::core::log::enabled((lvl), (cat))) {                 \
    } else                                                             \
        ::visora::core::log::Line((lvl), (cat)).stream()

#define VS_TRACE(cat) VS_LOG_AT(::visora::core::log::Level::Trace, cat)
#define VS_DEBUG(cat) VS_LOG_AT(::visora::core::log::Level::Debug, cat)
#define VS_INFO(cat)  VS_LOG_AT(::visora::core::log::Level::Info,  cat)
#define VS_WARN(cat)  VS_LOG_AT(::visora::core::log::Level::Warn,  cat)
#define VS_ERROR(cat) VS_LOG_AT(::visora::core::log::Level::Error, cat)
