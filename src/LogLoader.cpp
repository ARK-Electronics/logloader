#include "LogLoader.hpp"
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <future>
#include <regex>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

namespace fs = std::filesystem;

LogLoader::LogLoader(const LogLoader::Settings& settings)
	: _settings(settings)
{
	// Disable mavsdk noise
	// mavsdk::log::subscribe([](...) {
	// 	// https://mavsdk.mavlink.io/main/en/cpp/guide/logging.html
	// 	return true;
	// });

	std::cout << std::fixed << std::setprecision(8);

	// Ensure proper directory syntax
	if (_settings.logging_directory.back() != '/') {
		_settings.logging_directory += '/';
	}

	fs::create_directories(_settings.logging_directory);
}

void LogLoader::stop()
{
	_should_exit = true;
}

bool LogLoader::wait_for_mavsdk_connection(double timeout_ms)
{
	_mavsdk = std::make_shared<mavsdk::Mavsdk>(mavsdk::Mavsdk::Configuration(mavsdk::Mavsdk::ComponentType::GroundStation));
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
		// Check if vehicle is armed
		//  -- in the future we check if MAV_SYS_STATUS_LOGGING bit is high
		if (_telemetry->armed()) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}

		if (!request_log_entries()) {
			std::cout << "Failed to get logs" << std::endl;
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}

		// If we have no logs, just download the latest
		std::string most_recent_log = find_most_recent_log();

		if (most_recent_log.empty()) {
			download_first_log();

		} else {
			// Otherwise download all logs more recent than the latest log we have locally
			download_all_logs(most_recent_log);
		}

		// Periodically request log list
		std::this_thread::sleep_for(std::chrono::seconds(10));
	}

	upload_thread.join();
}

bool LogLoader::request_log_entries()
{
	std::cout << "Requesting log list..." << std::endl;
	auto entries_result = _log_files->get_entries();

	if (entries_result.first != mavsdk::LogFiles::Result::Success) {
		std::cout << "Failed to get log list" << std::endl;
		return false;
	}

	_log_entries = entries_result.second;

	std::cout << "Found " << _log_entries.size() << " logs" << std::endl;

	for (auto& e : _log_entries) {
		std::cout << e.id << "\t" << e.date << "\t" << e.size_bytes / 1e6 << "MB" << std::endl;
	}

	return true;
}

void LogLoader::download_first_log()
{
	std::cout << "No local logs found, downloading latest" << std::endl;
	auto entry = _log_entries.back();
	auto log_path = _settings.logging_directory + entry.date + ".ulg";
	download_log(entry, log_path);
}

void LogLoader::download_all_logs(const std::string& most_recent_log)
{
	// Check which logs need to be downloaded
	for (auto& entry : _log_entries) {

		if (_telemetry->armed() || _should_exit) {
			return;
		}

		auto log_path = _settings.logging_directory + entry.date + ".ulg";

		if (fs::exists(log_path) && fs::file_size(log_path) < entry.size_bytes) {
			std::cout << "Incomplete log, re-downloading..." << std::endl;
			std::cout << "size actual/downloaded: " << entry.size_bytes << "/" << fs::file_size(log_path) << std::endl;

			fs::remove(log_path);
			download_log(entry, log_path);

		} else if (!fs::exists(log_path) && entry.date > most_recent_log) {
			download_log(entry, log_path);
		}
	}
}

bool LogLoader::download_log(const mavsdk::LogFiles::Entry& entry, const std::string& download_path)
{
	auto prom = std::promise<mavsdk::LogFiles::Result> {};
	auto future_result = prom.get_future();

	// Mark the file as currently being downloaded
	{
		std::lock_guard<std::mutex> lock(_current_download_mutex);
		_current_download.second = false;
		_current_download.first = download_path;
	}

	auto time_start = std::chrono::steady_clock::now();

	_log_files->download_log_file_async(
		entry,
		download_path,
	[&prom, &entry, &time_start](mavsdk::LogFiles::Result result, mavsdk::LogFiles::ProgressData progress) {

		auto now = std::chrono::steady_clock::now();

		// Calculate data rate in Kbps
		double rate_kbps = ((progress.progress * entry.size_bytes * 8.0) / 1000.0) / std::chrono::duration_cast<std::chrono::seconds>(now -
				   time_start).count(); // Convert bytes to bits and then to Kbps

		if (result != mavsdk::LogFiles::Result::Next) {
			prom.set_value(result);
		}

		std::cout << "\rDownloading..."
			  << "\t" << entry.date << "\t"
			  << entry.size_bytes / 1e6 << "MB"
			  << "\t" << int(progress.progress * 100.f) << "%\t" << rate_kbps << " Kbps" << std::flush;
	});

	auto result = future_result.get();

	std::cout << std::endl;

	bool success = result == mavsdk::LogFiles::Result::Success;

	if (success) {
		std::lock_guard<std::mutex> lock(_current_download_mutex);
		_current_download.second = true;
	}

	return success;
}

void LogLoader::upload_logs_thread()
{
	if (!_settings.upload_enabled) {
		return;
	}

	// Short startup delay to allow the download thread to start re-downloading a
	// potentially imcomplete log if the download was interrupted last time. We
	// need to wait so that we don't race to check the _current_download.second
	// status before the downloader marks the file as in-progress.
	std::this_thread::sleep_for(std::chrono::seconds(5));

	while (!_should_exit) {

		if (_telemetry->armed()) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}

		// Get list of logs to upload
		auto logs_to_upload = get_logs_to_upload();

		for (const auto& log_path : logs_to_upload) {

			// Break if armed
			if (_telemetry->armed() || _should_exit) {
				continue;
			}

			// Upload the log
			if (server_reachable() && send_log_to_server(log_path)) {
				mark_log_as_uploaded(log_path);

			} else {
				std::cout << "Connection with server failed" << std::endl;
			}
		}

		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
}

std::vector<std::string> LogLoader::get_logs_to_upload()
{
	std::vector<std::string> logs;

	for (const auto& it : fs::directory_iterator(_settings.logging_directory)) {
		std::string log_path = it.path();

		bool should_upload = !log_has_been_uploaded(log_path) && log_download_complete(log_path);

		if (should_upload) {
			logs.push_back(log_path);
		}
	}

	return logs;
}

bool LogLoader::log_has_been_uploaded(const std::string& file_path)
{
	std::ifstream file(_settings.uploaded_logs_file);
	std::string line;

	while (std::getline(file, line)) {
		if (line == file_path) {
			return true;
		}
	}

	return false;
}

bool LogLoader::log_download_complete(const std::string& log_path)
{
	std::lock_guard<std::mutex> lock(_current_download_mutex);

	if (_current_download.first == log_path) {
		return _current_download.second;
	}

	return true;
}

void LogLoader::mark_log_as_uploaded(const std::string& file_path)
{
	std::ofstream file(_settings.uploaded_logs_file, std::ios::app);
	file << file_path << std::endl;
}

bool LogLoader::server_reachable()
{
	httplib::SSLClient cli(_settings.server);
	auto res = cli.Get("/");
	return res && res->status == 200;
}

// TODO: Add RobotoAI endpoint. Abstract away endpoint interface and just pass the data
bool LogLoader::send_log_to_server(const std::string& file_path)
{
	// std::this_thread::sleep_for(std::chrono::seconds(1));
	// std::cout << std::endl << "Upload success: " << std::endl;
	// return true;

	std::ifstream file(file_path, std::ios::binary);

	if (!file) {
		std::cout << "Could not open file " << file_path << std::endl;
		return false;
	}

	// Build multi-part form data
	httplib::MultipartFormDataItems items = {
		{"type", "personal", "", ""},
		{"description", "Auto Log Upload", "", ""},
		{"feedback", "hmmm", "", ""},
		{"source", "auto", "", ""},
		{"videoUrl", "", "", ""},
		{"rating", "", "", ""},
		{"windSpeed", "", "", ""},
		{"public", _settings.public_logs ? "true" : "false", "", ""}
	};

	// Add items to form
	items.push_back({"email", _settings.email, "", ""});
	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	items.push_back({"filearg", content, file_path, "application/octet-stream"});

	// Post multi-part form
	std::cout << std::endl << "Uploading: " << fs::path(file_path).filename() << "\t" << fs::file_size(file_path) / 1e6 << "MB" << std::endl;

	httplib::SSLClient cli(_settings.server);

	auto res = cli.Post("/upload", items);

	if (res && res->status == 302) {
		std::string url = "https://" + _settings.server + res->get_header_value("Location");
		std::cout << std::endl << "Upload success:\t" <<  url << std::endl;
		return true;
	}

	else {
		std::cout << "Failed to upload " << file_path << ". Status: " << (res ? std::to_string(res->status) : "No response") << std::endl;
		return false;
	}
}

std::string LogLoader::find_most_recent_log()
{
	// Regex pattern to match "yyyy-mm-ddThh:mm:ssZ.ulg" format
	std::regex log_pattern("^(\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z)\\.ulg$");
	std::string latest_log;

	for (const auto& entry : fs::directory_iterator(_settings.logging_directory)) {
		std::string filename = entry.path().filename().string();
		std::smatch matches;

		if (std::regex_search(filename, matches, log_pattern) && matches.size() > 1) {
			std::string log = matches[1].str();

			if (log > latest_log) {
				latest_log = log;
			}
		}
	}

	return latest_log;
}
