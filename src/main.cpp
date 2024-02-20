#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>
#include <functional>
#include <future>
#include <regex>
#include <set>
#include <toml.hpp>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/log_files/log_files.h>
#include <mavsdk/log_callback.h>

using namespace mavsdk;
using std::chrono::seconds;
using std::this_thread::sleep_for;
namespace fs = std::filesystem;

bool download_log(const LogFiles::Entry& entry, const std::string& filepath);
bool send_log_to_server(const std::string& email, const std::string& filepath);
std::string find_most_recent_log(const std::string& directory);
void mark_log_as_uploaded(const std::string& filepath);
bool has_log_been_uploaded(const std::string& filepath);

std::shared_ptr<LogFiles> _log_files;

static std::string LOG_DIRECTORY = "logs/";

int main(int argc, char* argv[])
{
	// We want to disable the mavsdk logging which spams stdout
	mavsdk::log::subscribe([](...) {
		// https://mavsdk.mavlink.io/main/en/cpp/guide/logging.html
		return true;
	});

	std::string name;
	std::string version;
	std::string connection_url;
	std::string logdir;
	std::string email;

	try {
		auto config = toml::parse_file("config.toml");
		name = config["name"].value_or("logloader");
		version = config["version"].value_or("0.0.0");
		connection_url = config["connection_url"].value_or("0.0.0");
		logdir = config["logdir"].value_or("logs/");
		email = config["email"].value_or("");

	} catch (const toml::parse_error& err) {
		std::cerr << "Parsing failed:\n" << err << "\n";
		return 1;

	} catch (const std::exception& err) {
		std::cerr << "Error: " << err.what() << "\n";
		return 1;
	}

	std::cout << "Name: " << name << std::endl;
	std::cout << "Version: " << version << std::endl;
	std::cout << "connection_url: " << connection_url << std::endl;
	std::cout << "logdir: " << logdir << std::endl;
	std::cout << "email: " << email << std::endl;

	// MAVSDK stuff
	Mavsdk mavsdk { Mavsdk::Configuration{ Mavsdk::ComponentType::GroundStation } };
	auto result = mavsdk.add_any_connection(connection_url);

	if (result != ConnectionResult::Success) {
		std::cerr << "Connection failed: " << result << std::endl;
		return 1;
	}

	auto system = mavsdk.first_autopilot(3);

	if (!system) {
		std::cerr << "Timed out waiting for system" << std::endl;
		return 1;
	}

	std::cout << "Connected to autopilot" << std::endl;

	// Only fetch entires while disarmed
	// TODO: if (armed) --> spin
	// TODO: else () --> download logs we don't have
	// TODO: if (!armed && network) --> upload logs

	std::cout << "Fetching logs..." << std::endl;
	_log_files = std::make_shared<LogFiles>(system.value());
	auto entries_result = _log_files->get_entries();

	if (entries_result.first != LogFiles::Result::Success) {
		std::cerr << "Couldn't get logs" << std::endl;
		return 1;
	}

	std::vector<LogFiles::Entry> entries = entries_result.second;

	// Ensure the logs directory exists
	fs::create_directories(logdir);

	if (logdir.back() != '/') {
		logdir += '/';
	}

	std::string most_recent_log = find_most_recent_log(logdir);

	std::cout << "Most recent local log: " << most_recent_log << std::endl;

	// TODO: mode:
	// -- Download/upload all logs
	// -- Download/upload logs after a predefined date
	// -- Ignore logs with date greater than X
	// -- Logs without a datetime in name? e.g session1.ulg

	if (most_recent_log.empty()) {
		std::cout << "No local logs found, downloading latest" << std::endl;
		LogFiles::Entry entry = entries.back();
		auto log_path = logdir + entry.date + ".ulg";
		download_log(entry, log_path);

	} else {
		// Check which logs need to be downloaded
		for (auto& entry : entries) {
			auto log_path = logdir + entry.date + ".ulg";

			std::cout << entry.date << std::endl;

			if (fs::exists(log_path) && fs::file_size(log_path) < entry.size_bytes) {
				std::cout << "File exists but size don't match!" << std::endl;
				fs::remove(log_path);
				download_log(entry, log_path);

			} else if (!fs::exists(log_path) && entry.date > most_recent_log) {
				download_log(entry, log_path);
			}
		}
	}

	// Store list of logs to upload
	std::cout << "Logs to upload:" << std::endl;
	std::vector<std::string> logs_to_upload;

	for (const auto& it : fs::directory_iterator(logdir)) {
		std::string logpath = it.path();

		if (!has_log_been_uploaded(logpath)) {
			logs_to_upload.push_back(logpath);
			std::cout << logpath << std::endl << std::flush;
		}
	}

	std::cout << "Uploading " << logs_to_upload.size() << " logs" << std::endl;

	for (const auto& logpath : logs_to_upload) {
		if (send_log_to_server(email, logpath)) {
			mark_log_as_uploaded(logpath);
		}
	}

	std::cout << "exiting" << std::endl;

	return 0;
}

bool download_log(const LogFiles::Entry& entry, const std::string& log_path)
{
	auto prom = std::promise<LogFiles::Result> {};
	auto future_result = prom.get_future();

	std::cout << "Downloading " << entry.size_bytes << " bytes -- " << entry.date + ".ulg" << std::endl;

	_log_files->download_log_file_async(
		entry,
		log_path,
	[&prom](LogFiles::Result result, LogFiles::ProgressData progress) {
		if (result != LogFiles::Result::Next) {
			prom.set_value(result);
		}

		std::cout << "\rDownloading log: " << int(progress.progress * 100) << "%" << std::flush;
	});

	auto result = future_result.get();

	return result == LogFiles::Result::Success;
}

std::string find_most_recent_log(const std::string& directory)
{
	// Updated regex pattern to match "yyyy-mm-ddThh:mm:ssZ.ulg" format
	std::regex log_pattern("^(\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z)\\.ulg$");
	std::string latest_date_time;

	for (const auto& entry : fs::directory_iterator(directory)) {
		std::string filename = entry.path().filename().string();
		std::smatch matches;

		if (std::regex_search(filename, matches, log_pattern) && matches.size() > 1) {
			std::string log_date_time = matches[1].str();

			if (log_date_time > latest_date_time) {
				latest_date_time = log_date_time;
			}
		}
	}

	return latest_date_time;
}

bool send_log_to_server(const std::string& email, const std::string& filepath)
{
	std::ifstream file(filepath, std::ios::binary);

	if (!file) {
		std::cerr << "Could not open file " << filepath << std::endl;
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
	items.push_back({"email", email, "", ""});
	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	items.push_back({"filearg", content, filepath, "application/octet-stream"});

	// Post multi-part form
	std::cout << "Uploading: " << filepath << std::endl;
	httplib::SSLClient cli("logs.px4.io");

	auto res = cli.Post("/upload", items);

	if (res && res->status == 302) {
		std::cout << "Upload success: " << res->get_header_value("Location") << std::endl;
		return true;
	}

	else {
		std::cerr << "Failed to upload " << filepath << ". Status: " << (res ? std::to_string(res->status) : "No response") << std::endl;
		return false;
	}
}

bool has_log_been_uploaded(const std::string& filepath)
{
	std::ifstream file("uploaded_logs.txt");
	std::string line;

	while (std::getline(file, line)) {
		if (line == filepath) {
			return true;
		}
	}

	return false;
}

void mark_log_as_uploaded(const std::string& filepath)
{
	std::ofstream file("uploaded_logs.txt", std::ios::app);

	if (file) {
		file << filepath << std::endl;
	}
}