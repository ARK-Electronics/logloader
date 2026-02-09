#include "LogLoader.hpp"
#include "Log.hpp"
#include <iostream>
#include <filesystem>
#include <future>
#include <fstream>
#include <functional>

namespace fs = std::filesystem;

LogLoader::LogLoader(const LogLoader::Settings& settings)
	: _settings(settings)
{
	// Disable mavsdk noise
	mavsdk::log::subscribe([](...) {
		// https://mavsdk.mavlink.io/main/en/cpp/guide/logging.html
		return true;
	});

	_logs_directory = _settings.application_directory + "logs/";

	// Initialize downloads database
	if (!init_downloads_db()) {
		std::cerr << "Failed to initialize downloads database" << std::endl;
	}

	// Setup local flight review backend
	FlightReviewBackend::Settings local_server_settings = {
		.server_url = settings.local_server,
		.user_email = "",
		.db_path = _settings.application_directory + "local_server.db",
		.upload_enabled = true, // Always upload to local server
		.public_logs = true, // Public required true for searching using Web UI
	};

	// Setup remote flight review backend
	FlightReviewBackend::Settings remote_server_settings = {
		.server_url = settings.remote_server,
		.user_email = settings.email,
		.db_path = _settings.application_directory + "remote_server.db",
		.upload_enabled = settings.upload_enabled,
		.public_logs = settings.public_logs,
	};

	_local_server = std::make_shared<FlightReviewBackend>(local_server_settings);
	_remote_server = std::make_shared<FlightReviewBackend>(remote_server_settings);

	// Setup Roboto backend
	if (!settings.roboto_api_url.empty() && !settings.roboto_api_token.empty()) {
		RobotoBackend::Settings roboto_settings = {
			.api_url = settings.roboto_api_url,
			.api_token = settings.roboto_api_token,
			.device_id = settings.roboto_device_id,
			.db_path = _settings.application_directory + "roboto.db",
			.upload_enabled = settings.roboto_upload_enabled,
		};

		_roboto_backend = std::make_shared<RobotoBackend>(roboto_settings);
	}

	std::cout << std::fixed << std::setprecision(8);

	fs::create_directories(_logs_directory);
}

LogLoader::~LogLoader()
{
	close_downloads_db();
}

void LogLoader::stop()
{
	{
		std::lock_guard<std::mutex> lock(_exit_cv_mutex);
		_should_exit = true;
	}
	_exit_cv.notify_all();
}

// --- Download tracking database ---

bool LogLoader::init_downloads_db()
{
	std::string db_path = _settings.application_directory + "downloads.db";
	int rc = sqlite3_open(db_path.c_str(), &_downloads_db);

	if (rc != SQLITE_OK) {
		std::cerr << "Cannot open downloads database: " << sqlite3_errmsg(_downloads_db) << std::endl;
		sqlite3_close(_downloads_db);
		_downloads_db = nullptr;
		return false;
	}

	const char* create_table =
		"CREATE TABLE IF NOT EXISTS logs ("
		"  uuid TEXT PRIMARY KEY,"
		"  id INTEGER,"
		"  date TEXT,"
		"  size_bytes INTEGER,"
		"  downloaded INTEGER DEFAULT 0"
		");";

	char* error_msg = nullptr;
	rc = sqlite3_exec(_downloads_db, create_table, nullptr, nullptr, &error_msg);

	if (rc != SQLITE_OK) {
		std::cerr << "SQL error creating downloads table: " << error_msg << std::endl;
		sqlite3_free(error_msg);
		return false;
	}

	return true;
}

void LogLoader::close_downloads_db()
{
	if (_downloads_db) {
		sqlite3_close(_downloads_db);
		_downloads_db = nullptr;
	}
}

std::string LogLoader::generate_uuid(const mavsdk::LogFiles::Entry& entry)
{
	// Create a unique identifier based on date and size
	std::stringstream ss;
	ss << entry.date << "_" << entry.size_bytes;

	// Use a simple hash for the UUID
	std::hash<std::string> hasher;
	size_t hash = hasher(ss.str());

	ss.str("");
	ss << std::hex << std::setw(16) << std::setfill('0') << hash;
	return ss.str();
}

bool LogLoader::add_log_entry(const mavsdk::LogFiles::Entry& entry)
{
	std::string uuid = generate_uuid(entry);

	// Check if already exists
	sqlite3_stmt* stmt;
	std::string check_query = "SELECT COUNT(*) FROM logs WHERE uuid = ?";

	if (sqlite3_prepare_v2(_downloads_db, check_query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing add_log_entry check: " << sqlite3_errmsg(_downloads_db) << std::endl;
		return false;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

	bool exists = false;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		exists = sqlite3_column_int(stmt, 0) > 0;
	}

	sqlite3_finalize(stmt);

	if (exists) {
		return true;
	}

	// Insert
	std::string insert_query =
		"INSERT INTO logs (uuid, id, date, size_bytes, downloaded) "
		"VALUES (?, ?, ?, ?, 0)";

	if (sqlite3_prepare_v2(_downloads_db, insert_query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing add_log_entry insert: " << sqlite3_errmsg(_downloads_db) << std::endl;
		return false;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 2, entry.id);
	sqlite3_bind_text(stmt, 3, entry.date.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 4, entry.size_bytes);

	bool success = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);

	return success;
}

bool LogLoader::update_download_status(const std::string& uuid, bool downloaded)
{
	std::string query = "UPDATE logs SET downloaded = ? WHERE uuid = ?";
	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_downloads_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing update_download_status: " << sqlite3_errmsg(_downloads_db) << std::endl;
		return false;
	}

	sqlite3_bind_int(stmt, 1, downloaded ? 1 : 0);
	sqlite3_bind_text(stmt, 2, uuid.c_str(), -1, SQLITE_STATIC);

	bool success = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);

	return success;
}

uint32_t LogLoader::num_logs_to_download()
{
	sqlite3_stmt* stmt;
	std::string query = "SELECT COUNT(*) FROM logs WHERE downloaded = 0";

	if (sqlite3_prepare_v2(_downloads_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing num_logs_to_download: " << sqlite3_errmsg(_downloads_db) << std::endl;
		return 0;
	}

	uint32_t count = 0;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		count = sqlite3_column_int(stmt, 0);
	}

	sqlite3_finalize(stmt);
	return count;
}

LogLoader::DownloadEntry LogLoader::get_next_log_to_download()
{
	DownloadEntry empty{};

	sqlite3_stmt* stmt;
	std::string query =
		"SELECT uuid, id, date, size_bytes FROM logs "
		"WHERE downloaded = 0 "
		"ORDER BY date DESC, size_bytes DESC LIMIT 1";

	if (sqlite3_prepare_v2(_downloads_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing get_next_log_to_download: " << sqlite3_errmsg(_downloads_db) << std::endl;
		return empty;
	}

	DownloadEntry entry{};

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char* uuid_text = sqlite3_column_text(stmt, 0);

		if (uuid_text != nullptr) {
			entry.uuid = reinterpret_cast<const char*>(uuid_text);
		}

		entry.id = sqlite3_column_int(stmt, 1);

		const unsigned char* date_text = sqlite3_column_text(stmt, 2);

		if (date_text != nullptr) {
			entry.date = reinterpret_cast<const char*>(date_text);
		}

		entry.size_bytes = sqlite3_column_int(stmt, 3);
	}

	sqlite3_finalize(stmt);
	return entry;
}

std::string LogLoader::filepath_from_entry(const mavsdk::LogFiles::Entry& entry) const
{
	std::ostringstream ss;
	ss << _logs_directory << "LOG" << std::setfill('0') << std::setw(4) << entry.id << "_" << entry.date << ".ulg";
	return ss.str();
}

std::string LogLoader::filepath_from_uuid(const std::string& uuid) const
{
	sqlite3_stmt* stmt;
	std::string query = "SELECT id, date FROM logs WHERE uuid = ?";

	if (sqlite3_prepare_v2(_downloads_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing filepath_from_uuid: " << sqlite3_errmsg(_downloads_db) << std::endl;
		return "";
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

	std::string filepath;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		int id = sqlite3_column_int(stmt, 0);
		const unsigned char* date_text = sqlite3_column_text(stmt, 1);

		if (date_text != nullptr) {
			std::string date = reinterpret_cast<const char*>(date_text);
			std::ostringstream ss;
			ss << _logs_directory << "LOG" << std::setfill('0') << std::setw(4) << id << "_" << date << ".ulg";
			filepath = ss.str();
		}
	}

	sqlite3_finalize(stmt);
	return filepath;
}

void LogLoader::register_log_with_backends(const std::string& uuid)
{
	_local_server->register_log(uuid);
	_remote_server->register_log(uuid);

	if (_roboto_backend) _roboto_backend->register_log(uuid);
}

// --- MAVSDK connection ---

bool LogLoader::wait_for_mavsdk_connection(double timeout_ms)
{
	LOG("Connecting to " << _settings.mavsdk_connection_url);
	_mavsdk = std::make_shared<mavsdk::Mavsdk>(mavsdk::Mavsdk::Configuration(1, MAV_COMP_ID_ONBOARD_COMPUTER,
			true)); // Emit heartbeats (Client)
	auto result = _mavsdk->add_any_connection(_settings.mavsdk_connection_url);

	if (result != mavsdk::ConnectionResult::Success) {
		LOG("Connection failed: " << result);
		return false;
	}

	auto system = _mavsdk->first_autopilot(timeout_ms);

	if (!system) {
		LOG("Timed out waiting for system");
		return false;
	}

	LOG("Connected.");

	// MAVSDK plugins
	_log_files = std::make_shared<mavsdk::LogFiles>(system.value());
	_telemetry = std::make_shared<mavsdk::Telemetry>(system.value());

	return true;
}

// --- Main loop ---

void LogLoader::run()
{
	auto upload_thread = std::thread(&LogLoader::upload_logs_thread, this);

	while (!_should_exit) {
		// Check if vehicle is armed or if the logger is running
		// TODO: use SYS_STATUS flags to check logger status -- needs MAVSDK impl
		// bool logger_running = _telemetry->sys_status_sensors().enabled & MAV_SYS_STATUS_LOGGING;
		bool logger_running = false;
		bool vehicle_armed = _telemetry->armed();

		if (logger_running || vehicle_armed) {
			_loop_disabled = true;
			_remote_server->stop();
			_local_server->stop();

			if (_roboto_backend) _roboto_backend->stop();

			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;

		} else if (_loop_disabled) {
			_loop_disabled = false;
			_remote_server->start();
			_local_server->start();

			if (_roboto_backend) _roboto_backend->start();

			// Stall for a few seconds to allow logger to finish writing
			std::this_thread::sleep_for(std::chrono::seconds(3));
		}

		// TODO:
		// - request log entries at boot after connecting only
		// - during runtime gate the log entry request on logger on/off events
		if (!request_log_entries()) {
			LOG_DEBUG("Failed to get logs");
			std::this_thread::sleep_for(std::chrono::seconds(5));
			continue;
		}

		uint32_t total_to_download = num_logs_to_download();
		uint32_t num_remaining = total_to_download;

		while (!_should_exit && num_remaining) {
			// Download logs until we should exit or there are none left to download
			LOG("Downloading log " << total_to_download - num_remaining + 1 << "/" << total_to_download);
			download_next_log();
			num_remaining = num_logs_to_download();
		}

		// Periodically request log list
		if (!_should_exit) {
			std::unique_lock<std::mutex> lock(_exit_cv_mutex);
			_exit_cv.wait_for(lock, std::chrono::seconds(30), [this] { return _should_exit.load(); });
		}
	}

	LOG_DEBUG("Waiting for upload thread");
	upload_thread.join();
}

// --- Download ---

bool LogLoader::request_log_entries()
{
	LOG_DEBUG("Requesting log entries...");

	// Debug profiling code. We need to check how this performs with 100+ logs
	auto request_start = std::chrono::high_resolution_clock::now();
	auto entries_result = _log_files->get_entries();

	//  Store log entries
	_log_entries = entries_result.second;

	auto request_end = std::chrono::high_resolution_clock::now();

	std::chrono::duration<double> request_duration = request_end - request_start;
	LOG_DEBUG("Received " << _log_entries.size() << " log entries in " << request_duration.count() << " seconds");

	if (entries_result.first != mavsdk::LogFiles::Result::Success) {
		LOG("Error getting log entries");
		return false;
	}

	// Time the database addition
	auto db_start = std::chrono::high_resolution_clock::now();

	for (const auto& entry : _log_entries) {
		add_log_entry(entry);
	}

	auto db_end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> db_duration = db_end - db_start;

	LOG_DEBUG("Added log entries to database in " << db_duration.count() << " seconds");
	LOG_DEBUG("Total processing time: " << (request_duration + db_duration).count() << " seconds");

	return true;
}

void LogLoader::download_next_log()
{
	DownloadEntry db_entry = get_next_log_to_download();

	if (db_entry.uuid.empty()) {
		return;
	}

	// Find the corresponding log entry in the list from the vehicle
	for (const auto& entry : _log_entries) {
		std::string uuid = generate_uuid(entry);

		if (uuid == db_entry.uuid) {
			if (download_log(entry)) {
				update_download_status(uuid, true);
				register_log_with_backends(uuid);
			}

			return;
		}
	}

	// Couldn't find matching entry in _log_entries
	// This could happen if the log is no longer available on the vehicle
	// Mark it as downloaded to avoid trying again
	update_download_status(db_entry.uuid, true);
}

bool LogLoader::download_log(const mavsdk::LogFiles::Entry& entry)
{
	auto prom = std::promise<mavsdk::LogFiles::Result> {};
	auto future_result = prom.get_future();
	auto download_path = filepath_from_entry(entry);

	// Check and delete file if it already exists. This can occur due to partial download.
	if (fs::exists(download_path)) {
		LOG("Found existing file, removing: " << download_path);

		try {
			fs::remove(download_path);

		} catch (const fs::filesystem_error& e) {
			LOG("Error removing existing file: " << e.what());
			return false;
		}
	}

	LOG("Downloading " << download_path);

	auto time_start = std::chrono::steady_clock::now();

	_log_files->download_log_file_async(
		entry,
		download_path,
	[&prom, &entry, &time_start, this](mavsdk::LogFiles::Result result, mavsdk::LogFiles::ProgressData progress) {

		if (_download_cancelled) return;

		auto now = std::chrono::steady_clock::now();

		if (_should_exit) {
			_download_cancelled = true;
			prom.set_value(mavsdk::LogFiles::Result::Timeout);
			std::cout << std::endl << "Download cancelled.. exiting" << std::endl;
			return;
		}

#ifdef DEBUG_BUILD
		// Calculate data rate in Kbps
		double rate_kbps = ((progress.progress * entry.size_bytes * 8.0)) / std::chrono::duration_cast<std::chrono::milliseconds>(now -
				   time_start).count(); // Convert bytes to bits and then to Kbps

		LOG_DEBUG("Downloading: "
			  << std::setw(24) << std::left << entry.date
			  << std::setw(8) << std::fixed << std::setprecision(2) << entry.size_bytes / 1e6 << "MB"
			  << std::setw(6) << std::right << int(progress.progress * 100.0f) << "%"
			  << std::setw(12) << std::fixed << std::setprecision(2) << rate_kbps << " Kbps"
			  << std::flush);
#else
		(void)progress;
#endif

		if (result != mavsdk::LogFiles::Result::Next) {
			double seconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - time_start).count() / 1000.;
			LOG("Finished in " << std::setprecision(2) << seconds << " seconds");
			prom.set_value(result);
		}
	});

	auto result = future_result.get();

	std::cout << std::endl;

	bool success = result == mavsdk::LogFiles::Result::Success;

	if (!success) {
		LOG("Download failed");
	}

	return success;
}

// --- Upload ---

void LogLoader::upload_logs_thread()
{
	while (!_should_exit) {
		if (_loop_disabled) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}

		// Query the number of pending log uploads for all backends
		uint32_t num_logs_local = _local_server->num_logs_to_upload();
		uint32_t num_logs_remote = _remote_server->num_logs_to_upload();

		// Process uploads for local server
		if (!_should_exit && !_settings.local_server.empty() && num_logs_local) {
			LOG_DEBUG("Uploading " << num_logs_local << " logs to LOCAL server");
			upload_pending_logs(_local_server);
		}

		// Process uploads for remote server
		if (!_should_exit && !_settings.remote_server.empty() && _settings.upload_enabled && num_logs_remote) {
			LOG_DEBUG("Uploading " << num_logs_remote << " logs to REMOTE server");
			upload_pending_logs(_remote_server);
		}

		// Process uploads for Roboto
		if (!_should_exit && _roboto_backend) {
			uint32_t num_logs_roboto = _roboto_backend->num_logs_to_upload();

			if (num_logs_roboto) {
				LOG_DEBUG("Uploading " << num_logs_roboto << " logs to ROBOTO");
				upload_pending_logs(_roboto_backend);
			}
		}

		if (!_should_exit) {
			std::unique_lock<std::mutex> lock(_exit_cv_mutex);
			_exit_cv.wait_for(lock, std::chrono::seconds(10), [this] { return _should_exit.load(); });
		}
	}

	LOG_DEBUG("upload_logs_thread exiting");
}

void LogLoader::upload_pending_logs(std::shared_ptr<UploadBackend> backend)
{
	// Upload all pending logs for this backend
	while (!_should_exit && backend->num_logs_to_upload()) {

		// Get one log at a time to upload
		std::string uuid = backend->get_next_log_to_upload();

		if (uuid.empty()) {
			LOG("Log with empty uuid!");
			break;
		}

		std::string filepath = filepath_from_uuid(uuid);

		if (filepath.empty()) {
			LOG("Could not determine file path for UUID: " << uuid);
			// Blacklist to prevent infinite retry
			backend->add_to_blacklist(uuid, "Could not determine file path");
			continue;
		}

		UploadBackend::UploadResult result = backend->upload_log(filepath, uuid);

		if (result.success) {
			LOG("Log upload SUCCESS: " << result.message);

		} else if (result.status_code == 400) {
			LOG("Log upload failed (" << result.status_code << "): " << result.message);

		} else {
			LOG("Log upload TEMPORARILY FAILED (" << result.status_code << "): "
			    << result.message << " - Will retry later");
		}
	}
}
