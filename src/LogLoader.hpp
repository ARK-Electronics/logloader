#pragma once

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <mavsdk/plugins/log_files/log_files.h>
#include <mavsdk/log_callback.h>
#include <condition_variable>
#include <sqlite3.h>

#include "FlightReviewBackend.hpp"
#include "RobotoBackend.hpp"

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
		// Roboto settings
		std::string roboto_api_url;
		std::string roboto_api_token;
		std::string roboto_device_id;
		bool roboto_upload_enabled;
	};

	LogLoader(const Settings& settings);
	~LogLoader();

	void run();
	void stop();
	bool wait_for_mavsdk_connection(double timeout_ms);

private:
	// Download tracking (single database for all backends)
	bool init_downloads_db();
	void close_downloads_db();
	static std::string generate_uuid(const mavsdk::LogFiles::Entry& entry);
	bool add_log_entry(const mavsdk::LogFiles::Entry& entry);
	bool update_download_status(const std::string& uuid, bool downloaded);
	uint32_t num_logs_to_download();

	struct DownloadEntry {
		std::string uuid;
		uint32_t id;
		std::string date;
		uint32_t size_bytes;
	};

	DownloadEntry get_next_log_to_download();
	std::string filepath_from_entry(const mavsdk::LogFiles::Entry& entry) const;
	std::string filepath_from_uuid(const std::string& uuid) const;

	// Download
	bool request_log_entries();
	void download_next_log();
	bool download_log(const mavsdk::LogFiles::Entry& entry);

	// Upload
	void upload_logs_thread();
	void upload_pending_logs(std::shared_ptr<UploadBackend> backend);

	// Register a downloaded log with all upload backends
	void register_log_with_backends(const std::string& uuid);

	Settings _settings;
	std::string _logs_directory;

	// Downloads database
	sqlite3* _downloads_db = nullptr;

	// Upload backends (each with its own database)
	std::shared_ptr<FlightReviewBackend> _local_server;
	std::shared_ptr<FlightReviewBackend> _remote_server;
	std::shared_ptr<RobotoBackend> _roboto_backend;

	std::shared_ptr<mavsdk::Mavsdk> _mavsdk;
	std::shared_ptr<mavsdk::Telemetry> _telemetry;
	std::shared_ptr<mavsdk::LogFiles> _log_files;
	std::vector<mavsdk::LogFiles::Entry> _log_entries;

	std::atomic<bool> _should_exit = false;
	std::atomic<bool> _download_cancelled = false;

	std::condition_variable _exit_cv;
	std::mutex _exit_cv_mutex;

	std::atomic<bool> _loop_disabled{false};
};
