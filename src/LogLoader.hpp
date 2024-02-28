#pragma once

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <mavsdk/plugins/log_files/log_files.h>
#include <mavsdk/log_callback.h>
#include <condition_variable>

class LogLoader
{
public:
	struct Settings {
		std::string email;
		std::string logging_directory;
		std::string uploaded_logs_file;
		std::string mavsdk_connection_url;
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
	void download_all_logs(const std::string& most_recent_log = std::string("2023-10-05T15:06:42Z"));
	bool download_log(const mavsdk::LogFiles::Entry& entry, const std::string& download_path);
	bool log_download_complete(const std::string& log_path);

	// Upload
	std::vector<std::string> get_logs_to_upload();
	void upload_logs_thread();
	bool send_log_to_server(const std::string& file_path);
	bool server_reachable();
	bool log_has_been_uploaded(const std::string& file_path);
	void mark_log_as_uploaded(const std::string& file_path);

	std::string find_most_recent_log();

	Settings _settings;
	std::shared_ptr<mavsdk::Mavsdk> _mavsdk;
	std::shared_ptr<mavsdk::Telemetry> _telemetry;
	std::shared_ptr<mavsdk::LogFiles> _log_files;
	std::vector<mavsdk::LogFiles::Entry> _log_entries;

	std::mutex _current_download_mutex;
	std::pair<std::string, bool> _current_download;

	bool _should_exit = false;
};
