#include "LogLoader.hpp"
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
	// mavsdk::log::subscribe([](...) {
	//  // https://mavsdk.mavlink.io/main/en/cpp/guide/logging.html
	//  return true;
	// });

	_logs_directory = _settings.application_directory + "logs/";

	// Initialize the database
	std::string db_path = _settings.application_directory + "logs.db";
	_log_db = std::make_shared<LogDatabase>(db_path);

	if (!_log_db->init()) {
		std::cerr << "Failed to initialize log database" << std::endl;
	}

	ServerUploadManager::Settings local_server_settings = {
		.server_url = settings.local_server,
		.user_email = "",
		.logs_directory = _logs_directory,
		.uploaded_logs_file = _settings.application_directory + "local_uploaded_logs.txt",
		.upload_enabled = true, // Always upload to local server
		.public_logs = true, // Public required true for searching using Web UI
	};

	ServerUploadManager::Settings remote_server_settings = {
		.server_url = settings.remote_server,
		.user_email = settings.email,
		.logs_directory = _logs_directory,
		.uploaded_logs_file = _settings.application_directory + "uploaded_logs.txt",
		.upload_enabled = settings.upload_enabled,
		.public_logs = settings.public_logs,
	};

	_local_server = std::make_shared<ServerUploadManager>(local_server_settings);
	_remote_server = std::make_shared<ServerUploadManager>(remote_server_settings);

	std::cout << std::fixed << std::setprecision(8);

	fs::create_directories(_logs_directory);
}

void LogLoader::stop()
{
	{
		std::lock_guard<std::mutex> lock(_exit_cv_mutex);
		_should_exit = true;
	}
	_exit_cv.notify_one();
}

bool LogLoader::wait_for_mavsdk_connection(double timeout_ms)
{
	std::cout << "Connecting to " << _settings.mavsdk_connection_url << std::endl;
	_mavsdk = std::make_shared<mavsdk::Mavsdk>(mavsdk::Mavsdk::Configuration(1, MAV_COMP_ID_ONBOARD_COMPUTER,
			true)); // Emit heartbeats (Client)
	auto result = _mavsdk->add_any_connection(_settings.mavsdk_connection_url);

	if (result != mavsdk::ConnectionResult::Success) {
		std::cout << "Connection failed: " << result << std::endl;
		return false;
	}

	auto system = _mavsdk->first_autopilot(timeout_ms);

	if (!system) {
		std::cout << "Timed out waiting for system" << std::endl;
		return false;
	}

	std::cout << "Connected to autopilot" << std::endl;

	// MAVSDK plugins
	_log_files = std::make_shared<mavsdk::LogFiles>(system.value());
	_telemetry = std::make_shared<mavsdk::Telemetry>(system.value());

	return true;
}

void LogLoader::run()
{
	auto upload_thread = std::thread(&LogLoader::upload_logs_thread, this);

	while (!_should_exit) {
		// Check if vehicle is armed or if the logger is running
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

		if (!request_log_entries()) {
			std::cout << "Failed to get logs" << std::endl;
			std::this_thread::sleep_for(std::chrono::seconds(5));
			continue;
		}

		// Process one log at a time
		while (process_next_log() && !_should_exit) {
			// This will process one log at a time
			// and check _should_exit after each download
		}

		// Periodically request log list
		if (!_should_exit) {
			std::unique_lock<std::mutex> lock(_exit_cv_mutex);
			_exit_cv.wait_for(lock, std::chrono::seconds(10), [this] { return _should_exit.load(); });
		}
	}

	upload_thread.join();
}

bool LogLoader::request_log_entries()
{
	auto entries_result = _log_files->get_entries();

	if (entries_result.first != mavsdk::LogFiles::Result::Success) {
		return false;
	}

	_log_entries = entries_result.second;

	// Add all entries to the database
	for (const auto& entry : _log_entries) {
		_log_db->add_log(entry);
	}

	return true;
}

bool LogLoader::process_next_log()
{
	// Get one undownloaded log from the database
	auto logs_to_download = _log_db->get_logs_to_download(1, 0);

	if (logs_to_download.empty()) {
		// No more logs to download
		return false;
	}

	auto& log_record = logs_to_download[0];

	// Find the corresponding log entry
	for (const auto& entry : _log_entries) {
		// Match by UUID (which is based on date and size)
		std::string uuid = _log_db->generate_uuid(entry);

		if (uuid == log_record.uuid) {
			bool success = download_log(entry);

			if (success) {
				_log_db->update_download_status(uuid, true);
			}

			return true;  // Successfully processed one log
		}
	}

	// Couldn't find matching entry in _log_entries
	// This could happen if the log is no longer available on the vehicle
	// Mark it as processed to avoid trying again
	_log_db->update_download_status(log_record.uuid, true);

	return true;  // Continue processing more logs
}

bool LogLoader::download_log(const mavsdk::LogFiles::Entry& entry)
{
	auto prom = std::promise<mavsdk::LogFiles::Result> {};
	auto future_result = prom.get_future();

	auto download_path = filepath_from_entry(entry);

	std::cout << "Downloading  " << download_path << std::endl;

	// Mark the file as currently being downloaded
	std::ofstream lock_file(download_path + ".lock");
	lock_file.close();

	auto time_start = std::chrono::steady_clock::now();

	_log_files->download_log_file_async(
		entry,
		download_path,
	[&prom, &entry, &time_start, this](mavsdk::LogFiles::Result result, mavsdk::LogFiles::ProgressData progress) {

		if (_download_cancelled) return;

		auto now = std::chrono::steady_clock::now();

		// Calculate data rate in Kbps
		double rate_kbps = ((progress.progress * entry.size_bytes * 8.0)) / std::chrono::duration_cast<std::chrono::milliseconds>(now -
				   time_start).count(); // Convert bytes to bits and then to Kbps

		if (_should_exit) {
			_download_cancelled = true;
			prom.set_value(mavsdk::LogFiles::Result::Timeout);
			std::cout << std::endl << "Download cancelled.. exiting" << std::endl;
			return;
		}

		std::cout << "Downloading:  "
			  << std::setw(24) << std::left << entry.date
			  << std::setw(8) << std::fixed << std::setprecision(2) << entry.size_bytes / 1e6 << "MB"
			  << std::setw(6) << std::right << int(progress.progress * 100.0f) << "%"
			  << std::setw(12) << std::fixed << std::setprecision(2) << rate_kbps << " Kbps"
			  << std::flush << std::endl;

		if (result != mavsdk::LogFiles::Result::Next) {
			double seconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - time_start).count() / 1000.;
			std::cout << "Finished in " << std::setprecision(2) << seconds << " seconds" << std::endl;
			prom.set_value(result);
		}
	});

	auto result = future_result.get();

	std::cout << std::endl;

	bool success = result == mavsdk::LogFiles::Result::Success;

	if (!success) {
		std::cout << "Download failed" << std::endl;
	}

	// Remove lock_file indicating download is complete
	std::filesystem::remove(download_path + ".lock");

	return success;
}

void LogLoader::upload_logs_thread()
{
	// Short startup delay to allow the download thread to start re-downloading a
	// potentially imcomplete log if the download was interrupted last time. We
	// need to wait so that we don't race to check the _current_download.second
	// status before the downloader marks the file as in-progress.
	std::this_thread::sleep_for(std::chrono::seconds(5));

	while (!_should_exit) {
		if (_loop_disabled) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}

		if (!_settings.remote_server.empty()) {
			// Process one log at a time for remote server
			auto logs_to_upload = _log_db->get_logs_to_upload_remote(1);

			if (!logs_to_upload.empty()) {
				auto& log = logs_to_upload[0];
				std::string log_path = filepath_from_uuid(log.uuid);

				if (_remote_server->upload_log(log_path)) {
					_log_db->update_upload_status(log.uuid, log.local_uploaded, true);
				}
			}
		}

		if (!_settings.local_server.empty()) {
			// Process one log at a time for local server
			auto logs_to_upload = _log_db->get_logs_to_upload_local(1);

			if (!logs_to_upload.empty()) {
				auto& log = logs_to_upload[0];
				std::string log_path = filepath_from_uuid(log.uuid);

				if (_local_server->upload_log(log_path)) {
					_log_db->update_upload_status(log.uuid, true, log.remote_uploaded);
				}
			}
		}

		std::this_thread::sleep_for(std::chrono::seconds(5));
	}
}

std::string LogLoader::filepath_from_entry(const mavsdk::LogFiles::Entry entry)
{
	std::ostringstream ss;
	ss << _logs_directory << "LOG" << std::setfill('0') << std::setw(4) << entry.id << "_" << entry.date << ".ulg";
	return ss.str();
}

std::string LogLoader::filepath_from_uuid(const std::string& uuid)
{
	// Look up the log entry by UUID
	auto record = _log_db->get_log_by_uuid(uuid);

	if (record.uuid.empty()) {
		return "";
	}

	// Construct the file path using the log record
	std::ostringstream ss;
	ss << _logs_directory << "LOG" << std::setfill('0') << std::setw(4) << record.id << "_" << record.date << ".ulg";
	return ss.str();
}
