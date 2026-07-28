#pragma once

#include <memory>
#include <thread>

#include "Config.hpp"

class LogLoader;

namespace httplib
{
class Server;
}

// A small read/write HTTP surface on localhost, so ARK-OS can show the vehicle's
// logs and ask for particular ones instead of logloader guessing.
//
//   GET    /status              what logloader is doing right now
//   GET    /logs                every known log with its download/upload state
//   GET    /events              the same, as server-sent events
//   POST   /logs/download       {"ids": [..]} or {"all": true}
//   POST   /logs/upload         {"ids": [..]} or {"all": true}, optional "targets"
//   POST   /logs/cancel         {"ids": [..]}
//   DELETE /logs/{id}/file      remove the downloaded copy
class ApiServer
{
public:
	ApiServer(const Config& config, LogLoader& loader);
	~ApiServer();

	ApiServer(const ApiServer&) = delete;
	ApiServer& operator=(const ApiServer&) = delete;

	// Binds and serves on a thread of its own. False when the port is taken.
	bool start();
	void stop();

private:
	void install_routes();

	Config _config;
	LogLoader& _loader;
	std::unique_ptr<httplib::Server> _server;
	std::thread _thread;
};
