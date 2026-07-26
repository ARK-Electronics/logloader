#include "LogLoader.hpp"
#include "Log.hpp"
#include <iostream>
#include <filesystem>
#include <future>
#include <regex>
#include <fstream>

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

	// Setup local server interface
	ServerInterface::Settings local_server_settings = {
		.server_url = settings.local_server,
		.user_email = "",
		.logs_directory = _logs_directory,
		.db_path = _settings.application_directory + "local_server.db",
		.upload_enabled = true, // Always upload to local server
		.public_logs = true, // Public required true for searching using Web UI
	};

	// Setup remote server interface
	ServerInterface::Settings remote_server_settings = {
		.server_url = settings.remote_server,
		.user_email = settings.email,
		.logs_directory = _logs_directory,
		.db_path = _settings.application_directory + "remote_server.db",
		.upload_enabled = settings.upload_enabled,
		.public_logs = settings.public_logs,
	};

	_local_server = std::make_shared<ServerInterface>(local_server_settings);
	_remote_server = std::make_shared<ServerInterface>(remote_server_settings);

	std::cout << std::fixed << std::setprecision(8);

	fs::create_directories(_logs_directory);
}

void LogLoader::stop()
{
	{
		std::lock_guard<std::mutex> lock(_exit_cv_mutex);
		_should_exit = true;
	}
	_exit_cv.notify_all();

	if (_ftp_fetcher) {
		_ftp_fetcher->stop();
	}

	if (_log_entry_lister) {
		_log_entry_lister->stop();
	}
}

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
	_log_entry_lister = std::make_shared<LogEntryLister>(system.value());

	setup_ftp_fetcher(system.value());

	return true;
}

void LogLoader::setup_ftp_fetcher(std::shared_ptr<mavsdk::System> system)
{
	_ftp_fetcher.reset();

	if (_settings.download_protocol == "mavlink") {
		LOG("Using the MAVLink log protocol (LOG_DATA) as configured");

	} else {
		FtpLogFetcher::Settings ftp_settings = {
			.temp_directory = _settings.application_directory + "tmp/",
			.remote_directory = _settings.remote_log_directory,
			.use_burst = _settings.ftp_use_burst,
		};

		auto fetcher = std::make_shared<FtpLogFetcher>(system, ftp_settings);

		if (fetcher->detect()) {
			_ftp_fetcher = fetcher;

		} else if (_settings.download_protocol == "ftp") {
			// Pinned to FTP, keep the fetcher around and let it retry as we run
			LOG("download_protocol is \"ftp\" but the vehicle did not answer, will keep trying");
			_ftp_fetcher = fetcher;
		}
	}

	apply_log_extension();
}

void LogLoader::apply_log_extension()
{
	// The log protocol gives us no way to tell PX4 and ArduPilot apart, so an operator
	// running ArduPilot without FTP has to name the extension in the config file.
	std::string extension = _settings.log_extension;

	if (extension.empty() && _ftp_fetcher && _ftp_fetcher->available()) {
		extension = _ftp_fetcher->log_extension();
	}

	if (extension.empty()) {
		extension = ".ulg";
	}

	_local_server->set_log_extension(extension);
	_remote_server->set_log_extension(extension);
}

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
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;

		} else if (_loop_disabled) {
			_loop_disabled = false;
			_remote_server->start();
			_local_server->start();
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

		uint32_t total_to_download = _local_server->num_logs_to_download();
		uint32_t num_remaining = total_to_download;

		while (!_should_exit && num_remaining) {
			// Download logs until we should exit or there are none left to download
			LOG("Downloading log " << total_to_download - num_remaining + 1 << "/" << total_to_download);

			if (!download_next_log()) {
				// Nothing was retired, so retrying right away would just spin
				break;
			}

			num_remaining = _local_server->num_logs_to_download();
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

bool LogLoader::request_log_entries()
{
	LOG_DEBUG("Requesting log entries...");

	// Debug profiling code. We need to check how this performs with 100+ logs
	auto request_start = std::chrono::high_resolution_clock::now();

	bool success = false;

	if (_ftp_fetcher && _ftp_fetcher->flavor() == FtpLogFetcher::Flavor::ArduPilot) {
		// MAVSDK's LogFiles plugin cannot enumerate ArduPilot logs, see LogEntryLister
		success = _log_entry_lister->get_entries(_log_entries);

	} else {
		auto entries_result = _log_files->get_entries();
		_log_entries = entries_result.second;
		success = entries_result.first == mavsdk::LogFiles::Result::Success;
	}

	auto request_end = std::chrono::high_resolution_clock::now();

	std::chrono::duration<double> request_duration = request_end - request_start;
	LOG_DEBUG("Received " << _log_entries.size() << "log entries in " << request_duration.count() << " seconds");

	if (!success) {
		LOG("Error getting log entries");
		return false;
	}

	// Time the database addition
	auto db_start = std::chrono::high_resolution_clock::now();

	for (const auto& entry : _log_entries) {
		_local_server->add_log_entry(entry);
		_remote_server->add_log_entry(entry);
	}

	auto db_end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> db_duration = db_end - db_start;

	LOG_DEBUG("Added log entries to databases in " << db_duration.count() << " seconds");
	LOG_DEBUG("Total processing time: " << (request_duration + db_duration).count() << " seconds");

	// Listing the remote directory costs a round trip per subdirectory, so only do it
	// when there is actually something to fetch.
	if (_ftp_fetcher && _local_server->num_logs_to_download()) {
		if (_ftp_fetcher->refresh()) {
			apply_log_extension();

		} else {
			LOG_DEBUG("Could not index the vehicle log directory over FTP");
		}
	}

	return true;
}

// Returns true when an entry was retired from the download queue, false when the caller
// should back off instead of immediately trying again.
bool LogLoader::download_next_log()
{
	// Get one undownloaded log, use the local server for query
	ServerInterface::DatabaseEntry db_entry = _local_server->get_next_log_to_download();

	if (db_entry.uuid.empty()) {
		return false;
	}

	// Find the corresponding log entry in the list from the vehicle
	for (const auto& entry : _log_entries) {
		// Match by UUID (which is based on date and size)
		std::string uuid = ServerInterface::generate_uuid(entry);

		if (uuid == db_entry.uuid) {
			if (!download_log(entry)) {
				return false;
			}

			// Update downloaded status in both databases
			_local_server->update_download_status(uuid, true);
			_remote_server->update_download_status(uuid, true);

			return true;
		}
	}

	// Couldn't find matching entry in _log_entries
	// This could happen if the log is no longer available on the vehicle
	// Mark it as processed to avoid trying again in both databases
	_local_server->update_download_status(db_entry.uuid, true);
	_remote_server->update_download_status(db_entry.uuid, true);

	return true;
}

bool LogLoader::download_log(const mavsdk::LogFiles::Entry& entry)
{
	auto download_path = _local_server->filepath_from_entry(entry);

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

	// Prefer MAVLink FTP. Unlike LOG_DATA it is addressed to us, so a router in between
	// unicasts the transfer instead of copying every chunk to every endpoint it serves.
	if (_ftp_fetcher && _ftp_fetcher->available()) {
		if (download_log_ftp(entry, download_path)) {
			return true;
		}
	}

	if (_settings.download_protocol == "ftp") {
		LOG("Could not fetch " << download_path << " over FTP and download_protocol is \"ftp\", not falling back");
		return false;
	}

	return download_log_mavlink(entry, download_path);
}

bool LogLoader::download_log_ftp(const mavsdk::LogFiles::Entry& entry, const std::string& download_path)
{
	const std::string remote_path = _ftp_fetcher->resolve(entry);

	if (remote_path.empty()) {
		LOG_DEBUG("No remote path matches log " << entry.id << " (" << entry.date << ")");
		return false;
	}

	LOG("Downloading " << download_path << " from " << remote_path);

	auto time_start = std::chrono::steady_clock::now();

	// Everything the callback touches is captured by value, MAVSDK may still hold it
	// after we have stopped waiting on the transfer.
	bool success = _ftp_fetcher->download(remote_path, download_path, entry.size_bytes,
	[date = entry.date, size_bytes = entry.size_bytes, time_start](uint32_t bytes_transferred, uint32_t total_bytes) {
#ifdef DEBUG_BUILD
		auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					  std::chrono::steady_clock::now() - time_start).count();
		double rate_kbps = elapsed_ms > 0 ? (bytes_transferred * 8.0) / elapsed_ms : 0.;
		int percent = total_bytes > 0 ? int((100.0 * bytes_transferred) / total_bytes) : 0;

		LOG_DEBUG("Downloading: "
			  << std::setw(24) << std::left << date
			  << std::setw(8) << std::fixed << std::setprecision(2) << size_bytes / 1e6 << "MB"
			  << std::setw(6) << std::right << percent << "%"
			  << std::setw(12) << std::fixed << std::setprecision(2) << rate_kbps << " Kbps"
			  << std::flush);
#else
		(void)date;
		(void)size_bytes;
		(void)time_start;
		(void)bytes_transferred;
		(void)total_bytes;
#endif
	});

	if (!success) {
		// Let a later pass try this file again rather than stranding it
		_ftp_fetcher->release(remote_path);
		LOG("FTP download failed");
		return false;
	}

	double seconds = std::chrono::duration_cast<std::chrono::milliseconds>(
				 std::chrono::steady_clock::now() - time_start).count() / 1000.;
	LOG("Finished in " << std::setprecision(2) << seconds << " seconds");

	return true;
}

bool LogLoader::download_log_mavlink(const mavsdk::LogFiles::Entry& entry, const std::string& download_path)
{
	auto prom = std::promise<mavsdk::LogFiles::Result> {};
	auto future_result = prom.get_future();

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

void LogLoader::upload_logs_thread()
{
	while (!_should_exit) {
		if (_loop_disabled) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}

		// Query the number of pending log uploads for both servers
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

		if (!_should_exit) {
			std::unique_lock<std::mutex> lock(_exit_cv_mutex);
			_exit_cv.wait_for(lock, std::chrono::seconds(10), [this] { return _should_exit.load(); });
		}
	}

	LOG_DEBUG("upload_logs_thread exiting");
}

void LogLoader::upload_pending_logs(std::shared_ptr<ServerInterface> server)
{
	// Upload all pending logs for this server
	while (!_should_exit && server->num_logs_to_upload()) {

		// Get one log at a time to upload
		ServerInterface::DatabaseEntry log_entry = server->get_next_log_to_upload();

		if (log_entry.uuid.empty()) {
			LOG("Log with empty uuid!");
			return;
		}

		std::string filepath = server->filepath_from_uuid(log_entry.uuid);

		if (filepath.empty()) {
			LOG("Could not determine file path for UUID: " << log_entry.uuid);
			return;
		}

		ServerInterface::UploadResult result = server->upload_log(filepath);

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
