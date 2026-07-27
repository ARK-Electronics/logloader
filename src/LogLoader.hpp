#pragma once

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <mavsdk/log_callback.h>
#include <condition_variable>
#include <map>

#include "FtpLogFetcher.hpp"
#include "ServerInterface.hpp"

class LogLoader
{
public:
	struct Settings {
		std::string email;
		std::string local_server;
		std::string remote_server;
		// API key for remote_server (ARK Flight Review account key). Empty = none.
		std::string remote_api_key;
		std::string mavsdk_connection_url;
		std::string application_directory;
		bool upload_enabled;
		bool public_logs;
		std::string remote_log_directory; // Empty probes the known locations
		bool ftp_use_burst;
	};

	LogLoader(const Settings& settings);

	void run();
	void stop();
	bool wait_for_mavsdk_connection(double timeout_ms);

private:
	// Download
	bool refresh_log_index();
	void download_pending_logs();
	bool download_log(const ServerInterface::DatabaseEntry& db_entry, const FtpLogFetcher::RemoteLog& log);
	const FtpLogFetcher::RemoteLog* find_remote_log(const ServerInterface::DatabaseEntry& db_entry) const;
	std::string local_path_for(const ServerInterface::DatabaseEntry& db_entry);

	// Upload
	void upload_logs_thread();
	void upload_pending_logs(std::shared_ptr<ServerInterface> server);

	// Returns true if we should exit
	bool wait_for(std::chrono::seconds duration);

	Settings _settings;
	std::string _logs_directory;

	// Server objects (each with its own database)
	std::shared_ptr<ServerInterface> _local_server;
	std::shared_ptr<ServerInterface> _remote_server;

	std::shared_ptr<mavsdk::Mavsdk> _mavsdk;
	std::shared_ptr<mavsdk::Telemetry> _telemetry;
	std::shared_ptr<FtpLogFetcher> _ftp_fetcher;

	// Consecutive download failures per log. A file the vehicle will not part
	// with (a very large one that keeps timing out, say) is retried last so it
	// cannot hold up everything behind it.
	std::map<std::string, int> _download_failures;

	std::atomic<bool> _should_exit = false;

	std::condition_variable _exit_cv;
	std::mutex _exit_cv_mutex;

	bool _loop_disabled = false;
};
