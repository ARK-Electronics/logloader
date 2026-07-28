#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

class LogLoader;

namespace httplib
{
class Server;
}

// A small read/write HTTP surface on localhost, so ARK-OS can show the vehicle's
// logs and ask for particular ones instead of logloader guessing.
//
// The routes are listed in README.md rather than here, so there is one place to
// keep correct.
class ApiServer
{
public:
	struct Settings {
		std::string bind;
		uint16_t port {3005};
	};

	ApiServer(const Settings& settings, LogLoader& loader);
	~ApiServer();

	ApiServer(const ApiServer&) = delete;
	ApiServer& operator=(const ApiServer&) = delete;

	// Binds and serves on a thread of its own. False when the port is taken.
	bool start();
	void stop();

private:
	void install_routes();

	Settings _settings;
	LogLoader& _loader;
	std::unique_ptr<httplib::Server> _server;
	std::thread _thread;
	std::atomic<bool> _stopping {false};
	std::atomic<size_t> _streams {0};
};
