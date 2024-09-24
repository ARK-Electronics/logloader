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

		// Pretty print the log names
		// std::cout << "Found " << _log_entries.size() << " logs" << std::endl;
		// int indexWidth = std::to_string(_log_entries.size() - 1).length();
		// for (const auto& e : _log_entries) {
		// 	std::cout << std::setw(indexWidth) << std::right << e.id << "\t"  // Right-align the index
		// 		  << e.date << "\t" << std::fixed << std::setprecision(2) << e.size_bytes / 1e6 << "MB" << std::endl;
		// }

		// If we have no logs, just download the latest
		auto most_recent_log = find_most_recent_log();

		if (most_recent_log.date.empty()) {
			download_first_log();

		} else {
			// Otherwise download all logs more recent than the latest log we have locally
			download_logs_greater_than(most_recent_log);
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

	return true;
}

void LogLoader::download_first_log()
{
	std::cout << "No local logs found, downloading latest" << std::endl;
	auto entry = _log_entries.back();
	download_log(entry);
}

void LogLoader::download_logs_greater_than(const mavsdk::LogFiles::Entry& most_recent)
{
	// Check which logs need to be downloaded
	for (auto& entry : _log_entries) {

		if (_should_exit) {
			return;
		}

		bool new_log = entry.id > most_recent.id;
		bool partial_log = (entry.id == most_recent.id) && (entry.size_bytes > most_recent.size_bytes);

		if (new_log || partial_log) {
			if (partial_log) {
				std::cout << "Incomplete log, re-downloading..." << std::endl;
				std::cout << "size actual/downloaded: " << entry.size_bytes << "/" << most_recent.size_bytes << std::endl;

				auto log_path = filepath_from_entry(entry);

				if (fs::exists(log_path)) {
					fs::remove(log_path);
				}
			}

			download_log(entry);
		}
	}
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
			_remote_server->upload_logs();
		}

		if (!_settings.local_server.empty()) {
			_local_server->upload_logs();
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

mavsdk::LogFiles::Entry LogLoader::find_most_recent_log()
{
	mavsdk::LogFiles::Entry entry = {};
	// Regex pattern to match "LOG<index>_<datetime>.ulg" format
	std::regex log_pattern("LOG(\\d+)_(\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z)\\.ulg");
	int max_index = -1; // Start with -1 to ensure any found index will be greater
	std::string latest_datetime; // To keep track of the latest datetime for the highest index

	for (const auto& dir_iter : fs::directory_iterator(_logs_directory)) {
		std::string filename = dir_iter.path().filename().string();

		std::smatch matches;

		if (std::regex_search(filename, matches, log_pattern) && matches.size() > 2) {
			int index = std::stoi(matches[1].str()); // Index is in the first capture group
			std::string datetime = matches[2].str(); // Datetime is in the second capture group

			// Check if this log has a higher index or same index with a later timestamp
			if (index > max_index || (index == max_index && datetime > latest_datetime)) {
				max_index = index;
				latest_datetime = datetime;
				// construct log Entry
				entry.id = index;
				entry.date = datetime;
				entry.size_bytes = dir_iter.file_size();
			}
		}
	}

	return entry;
}
