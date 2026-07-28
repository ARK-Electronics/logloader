#pragma once

#include <sstream>
#include <string>

// Leveled, thread-safe logging. The level is a runtime setting (log_level in
// config.toml) rather than a build flag, so a device in the field can be turned
// up to debug without reinstalling a differently-compiled binary.
namespace logging
{

enum class Level {
	Error = 0,
	Warn = 1,
	Info = 2,
	Debug = 3,
};

void set_level(Level level);
bool enabled(Level level);

// Returns false when the text does not name a level; out is left alone.
bool parse_level(const std::string& text, Level& out);

// One write() call emits one whole line, so lines from different threads never
// interleave the way chained operator<< on std::cout does.
void write(Level level, const std::string& message);

} // namespace logging

#define LOG_AT(level, expression)                    \
	do {                                         \
		if (logging::enabled(level)) {       \
			std::ostringstream _log_oss; \
			_log_oss << expression;      \
			logging::write(level, _log_oss.str()); \
		}                                    \
	} while (0)

#define LOG_ERROR(x) LOG_AT(logging::Level::Error, x)
#define LOG_WARN(x)  LOG_AT(logging::Level::Warn, x)
#define LOG(x)       LOG_AT(logging::Level::Info, x)
#define LOG_DEBUG(x) LOG_AT(logging::Level::Debug, x)
