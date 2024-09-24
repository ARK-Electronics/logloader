#pragma once

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <mavsdk/plugins/log_files/log_files.h>
#include <mavsdk/log_callback.h>
#include <condition_variable>

#include "ServerUploadManager.hpp"

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
	void download_first_log();
	void download_logs_greater_than(const mavsdk::LogFiles::Entry& most_recent);
	bool download_log(const mavsdk::LogFiles::Entry& entry);


	void upload_logs_thread();
	std::string filepath_from_entry(const mavsdk::LogFiles::Entry entry);

	mavsdk::LogFiles::Entry find_most_recent_log();

	Settings _settings;
	std::string _logs_directory;
	std::string _uploaded_logs_file;
	std::string _local_uploaded_logs_file;

	// Server uploader objects
	std::shared_ptr<ServerUploadManager> _local_server;
	std::shared_ptr<ServerUploadManager> _remote_server;


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
