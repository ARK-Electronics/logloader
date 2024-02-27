#include "LogLoader.hpp"
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <future>
#include <regex>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

namespace fs = std::filesystem;

static std::string UPLOADED_LOGS_TEXT_FILE = "uploaded_logs.txt";

LogLoader::LogLoader(const LogLoader::Settings& settings)
	: _settings(settings)
{
	// Disable mavsdk noise
	// mavsdk::log::subscribe([](...) {
	// 	// https://mavsdk.mavlink.io/main/en/cpp/guide/logging.html
	// 	return true;
	// });

	// Set fixed-point notation and 2 decimal places
	std::cout << std::fixed << std::setprecision(8);

	// Ensure the logs directory exists
	if (_settings.logging_dir.back() != '/') {
		_settings.logging_dir += '/';
	}

	fs::create_directories(_settings.logging_dir);
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

	_log_files = std::make_shared<mavsdk::LogFiles>(system.value());
	_telemetry = std::make_shared<mavsdk::Telemetry>(system.value());

	return true;
}

bool LogLoader::fetch_log_entries()
{
	std::cout << "Fetching logs..." << std::endl;
	auto entries_result = _log_files->get_entries();

	if (entries_result.first != mavsdk::LogFiles::Result::Success) {
		std::cout << "Couldn't get logs" << std::endl;
		return false;
	}

	_log_entries = entries_result.second;

	for (auto& e : _log_entries) {
		std::cout << e.id << "\t" << e.date << "\t" << e.size_bytes / 1e6 << "MB" << std::endl;
	}

	return true;
}

void LogLoader::run()
{
	while (!_should_exit) {
		// Check if vehicle is armed
		//  -- in the future we check if MAV_SYS_STATUS_LOGGING bit is high
		if (_telemetry->armed()) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}

		if (!fetch_log_entries()) {
			std::cout << "Failed to fetch logs" << std::endl;
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}

		// TODO: mode:
		//
		// DownloadMode:
		// 	- Download logs greater than most recent local log
		// 	- Download all logs
		//
		// UploadMode:
		// 	- Upload all logs
		//  - Upload most recent
		// 	- Disabled

		// If we have no logs, just download the latest
		std::string most_recent_log = find_most_recent_log();

		if (most_recent_log.empty()) {
			std::cout << "No local logs found, downloading latest" << std::endl;
			mavsdk::LogFiles::Entry entry = _log_entries.back();
			std::cout << entry.id << "\t" << entry.date << "\t" << entry.size_bytes / 1e6 << "MB" << std::endl;
			auto log_path = _settings.logging_dir + entry.date + ".ulg";
			download_log(entry, log_path);

		} else {
			// Check which logs need to be downloaded
			for (auto& entry : _log_entries) {

				if (_telemetry->armed() || _should_exit) {
					break;
				}

				auto log_path = _settings.logging_dir + entry.date + ".ulg";


				if (fs::exists(log_path) && fs::file_size(log_path) < entry.size_bytes) {
					std::cout << "File exists but size doesn't match -- incomplete log!" << std::endl;
					std::cout << "size actual / size downloaded: " << entry.size_bytes << "/" << fs::file_size(log_path) << std::endl;

					fs::remove(log_path);
					download_log(entry, log_path);

				} else if (!fs::exists(log_path) && entry.date > most_recent_log) {
					download_log(entry, log_path);
				}
			}
		}

		if (_settings.upload) {
			std::vector<std::string> logs_to_upload;

			for (const auto& it : fs::directory_iterator(_settings.logging_dir)) {
				std::string logpath = it.path();

				if (!log_has_been_uploaded(logpath)) {
					logs_to_upload.push_back(logpath);
				}
			}

			// TODO: interrupt upload for ARMED state change
			for (const auto& logpath : logs_to_upload) {

				if (_telemetry->armed() || _should_exit) {
					break;
				}

				if (send_log_to_server(logpath)) {
					mark_log_as_uploaded(logpath);
				}
			}
		}
	}
}

bool LogLoader::download_log(const mavsdk::LogFiles::Entry& entry, const std::string& dowload_path)
{
	auto prom = std::promise<mavsdk::LogFiles::Result> {};
	auto future_result = prom.get_future();

	// std::cout << std::endl << "DL" << "\t" << entry.date << "\t" << entry.size_bytes / 1e6 << "MB";

	_log_files->download_log_file_async(
		entry,
		dowload_path,
	[&prom, &entry](mavsdk::LogFiles::Result result, mavsdk::LogFiles::ProgressData progress) {
		if (result != mavsdk::LogFiles::Result::Next) {
			prom.set_value(result);
		}

		std::cout << "\rDownloading..."
			  << "\t" << entry.date << "\t"
			  << entry.size_bytes / 1e6 << "MB"
			  << "\t" << int(progress.progress * 100.f) << "%"
			  << "\t" << progress.kbps << "Kbps" << std::flush;

		// TODO: download rate
		// std::cout << "\rDownloading..." << "\t" << entry.date << "\t" << entry.size_bytes / 1e6 << "MB" << "\t" << int(
		// 		  progress.progress * 100) << "%" << std::flush;
	});

	auto result = future_result.get();

	std::cout << std::endl;

	return result == mavsdk::LogFiles::Result::Success;
}

bool LogLoader::send_log_to_server(const std::string& filepath)
{
	std::ifstream file(filepath, std::ios::binary);

	if (!file) {
		std::cout << "Could not open file " << filepath << std::endl;
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
		{"public", "false", "", ""}
	};

	// Add items to form
	items.push_back({"email", _settings.email, "", ""});
	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	items.push_back({"filearg", content, filepath, "application/octet-stream"});

	// Post multi-part form
	std::cout << "Uploading " << fs::file_size(filepath) / 1e6 << " MB -- " << fs::path(filepath).filename() << std::endl;

	httplib::SSLClient cli("logs.px4.io");

	auto res = cli.Post("/upload", items);

	if (res && res->status == 302) {
		std::cout << "Upload success: " << res->get_header_value("Location") << std::endl;
		return true;
	}

	else {
		std::cout << "Failed to upload " << filepath << ". Status: " << (res ? std::to_string(res->status) : "No response") << std::endl;
		return false;
	}
}

std::string LogLoader::find_most_recent_log()
{
	// Regex pattern to match "yyyy-mm-ddThh:mm:ssZ.ulg" format
	std::regex log_pattern("^(\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z)\\.ulg$");
	std::string latest_log;

	for (const auto& entry : fs::directory_iterator(_settings.logging_dir)) {
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

bool LogLoader::log_has_been_uploaded(const std::string& filepath)
{
	std::ifstream file(UPLOADED_LOGS_TEXT_FILE);
	std::string line;

	while (std::getline(file, line)) {
		if (line == filepath) {
			return true;
		}
	}

	return false;
}

void LogLoader::mark_log_as_uploaded(const std::string& filepath)
{
	std::ofstream file(UPLOADED_LOGS_TEXT_FILE, std::ios::app);

	if (file) {
		file << filepath << std::endl;
	}
}
