#include "Log.hpp"

#include <atomic>
#include <iostream>
#include <mutex>

namespace logging
{

namespace
{

std::atomic<Level> g_level {Level::Info};
std::mutex g_mutex;

const char* prefix(Level level)
{
	switch (level) {
	case Level::Error: return "ERROR: ";

	case Level::Warn: return "WARN: ";

	case Level::Debug: return "DEBUG: ";

	case Level::Info: break;
	}

	return "";
}

} // namespace

void set_level(Level level)
{
	g_level.store(level, std::memory_order_relaxed);
}

bool enabled(Level level)
{
	return level <= g_level.load(std::memory_order_relaxed);
}

bool parse_level(const std::string& text, Level& out)
{
	if (text == "error") {
		out = Level::Error;

	} else if (text == "warn" || text == "warning") {
		out = Level::Warn;

	} else if (text == "info") {
		out = Level::Info;

	} else if (text == "debug") {
		out = Level::Debug;

	} else {
		return false;
	}

	return true;
}

void write(Level level, const std::string& message)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	std::ostream& stream = level == Level::Error ? std::cerr : std::cout;
	stream << prefix(level) << message << std::endl;
}

} // namespace logging
