#pragma once

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <mavsdk/plugins/log_files/log_files.h>
#include <mavsdk/log_callback.h>

class LogLoader
{
public:
	struct Settings {
		std::string email;
		std::string logging_dir;
		std::string mavsdk_connection_url;
		bool upload;
	};

	LogLoader(const Settings& settings);

	void run();
	void stop();
	bool wait_for_mavsdk_connection(double timeout_ms);

private:
	bool fetch_log_entries();
	bool send_log_to_server(const std::string& filepath);

	bool download_log(const mavsdk::LogFiles::Entry& entry, const std::string& dowload_path);
	std::string find_most_recent_log();
	bool log_has_been_uploaded(const std::string& filepath);
	void mark_log_as_uploaded(const std::string& filepath);

	Settings _settings;
	std::shared_ptr<mavsdk::Mavsdk> _mavsdk;
	std::shared_ptr<mavsdk::Telemetry> _telemetry;
	std::shared_ptr<mavsdk::LogFiles> _log_files;
	std::vector<mavsdk::LogFiles::Entry> _log_entries;

	bool _should_exit = false;
};
