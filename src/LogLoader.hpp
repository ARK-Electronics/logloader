#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include "ServerInterface.hpp"

class LogLoader
{
public:
	struct Settings {
		std::string email;
		std::string local_server;
		std::string remote_server;
		std::string application_directory;
		bool upload_enabled;
		bool public_logs;
	};

	LogLoader(const Settings& settings);

	void run();
	void stop();

private:
	// Scan the logs directory for .bin files written by the pymavlink downloader and
	// register any new ones in both server databases so the upload loop will send them.
	void register_new_logs();

	// Upload
	void upload_logs_thread();
	void upload_pending_logs(std::shared_ptr<ServerInterface> server);

	Settings _settings;
	std::string _logs_directory;

	// Server objects (each with its own database)
	std::shared_ptr<ServerInterface> _local_server;
	std::shared_ptr<ServerInterface> _remote_server;

	std::atomic<bool> _should_exit = false;

	std::condition_variable _exit_cv;
	std::mutex _exit_cv_mutex;
};
