#pragma once

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <mavsdk/plugins/log_files/log_files.h>
#include <mavsdk/log_callback.h>
#include <condition_variable>

#include "ServerInterface.hpp"

class LogLoader
{
public:
	struct Settings {
		std::string email;
		std::string local_server;
		std::string remote_server;
		std::string mavsdk_connection_url;
		std::string application_directory;
		bool upload_enabled;
		bool public_logs;
	};

	LogLoader(const Settings& settings);

	void run();
	void stop();
	bool wait_for_mavsdk_connection(double timeout_ms);

private:
	// Download
	bool request_log_entries();
	bool download_next_log();
	bool download_log(const mavsdk::LogFiles::Entry& entry);

	// Upload
	void upload_logs_thread();
	void upload_pending_logs(std::shared_ptr<ServerInterface> server);

	Settings _settings;
	std::string _logs_directory;

	// Server objects (each with its own database)
	std::shared_ptr<ServerInterface> _local_server;
	std::shared_ptr<ServerInterface> _remote_server;

	std::shared_ptr<mavsdk::Mavsdk> _mavsdk;
	std::shared_ptr<mavsdk::Telemetry> _telemetry;
	std::shared_ptr<mavsdk::LogFiles> _log_files;
	std::vector<mavsdk::LogFiles::Entry> _log_entries;

	std::atomic<bool> _should_exit = false;
	std::atomic<bool> _download_cancelled = false;

	std::condition_variable _exit_cv;
	std::mutex _exit_cv_mutex;

	bool _loop_disabled = false;
};
