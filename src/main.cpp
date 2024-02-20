#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>
#include <regex>
#include <set>
#include <argparse/argparse.hpp>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/log_files/log_files.h>
#include <future>

using namespace mavsdk;
using std::chrono::seconds;
using std::this_thread::sleep_for;
namespace fs = std::filesystem;

bool download_log(const LogFiles::Entry& entry, const std::string& filepath);
bool send_log_to_server(const std::string& email, const std::string& filepath);
std::string find_most_recent_log(const std::string& directory);
void mark_log_as_uploaded(const std::string& log);
bool has_log_been_uploaded(const std::string& log);

std::shared_ptr<LogFiles> _log_files;

static std::string LOG_DIRECTORY = "logs/";

int main(int argc, char* argv[])
{
	argparse::ArgumentParser parser("logloader");
	parser.add_argument("--url").help("Connection URL, eg: udp://192.168.1.34:14550").default_value(std::string("udp://192.168.1.34:14550"));
	parser.add_argument("--email").help("Your e-mail to send the upload link").default_value(std::string("dahl.jakejacob@gmail.com"));
	parser.add_argument("--logdir").help("Directory to store and look for logs").default_value(LOG_DIRECTORY);

	try {
		parser.parse_args(argc, argv);
	}

	catch (const std::runtime_error& err) {
		std::cerr << err.what() << std::endl;
		std::cerr << parser;
		return 1;
	}

	std::string email = parser.get("--email");
	std::string url = parser.get("--url");
	std::string logdir = parser.get("--logdir");

	// MAVSDK stuff
	Mavsdk mavsdk { Mavsdk::Configuration{ Mavsdk::ComponentType::GroundStation } };
	auto result = mavsdk.add_any_connection(url);

	if (result != ConnectionResult::Success) {
		std::cerr << "Connection failed: " << result << std::endl;
		return 1;
	}

	auto system = mavsdk.first_autopilot(3);

	if (!system) {
		std::cerr << "Timed out waiting for system\n";
		return 1;
	}

	_log_files = std::make_shared<LogFiles>(system.value());
	auto entries_result = _log_files->get_entries();

	if (entries_result.first != LogFiles::Result::Success) {
		std::cerr << "Couldn't get logs" << std::endl;
		return 1;
	}

	std::vector<LogFiles::Entry> entries = entries_result.second;

	// Ensure the logs directory exists
	fs::create_directories(logdir);

	std::string most_recent_log = find_most_recent_log(logdir);

	std::cout << "Most recent: " << most_recent_log << std::endl;

	// MAVSDK setup and log retrieval omitted for brevity, assume setup is successful and entries contains log entries

	if (most_recent_log.empty()) {
		// No valid logs locally, just download the latest from the FC
		std::cout << "No local logs found, downloading latest" << std::endl;
		LogFiles::Entry entry = entries.back();
		auto log_path = logdir + entry.date + ".ulg";
		download_log(entry, log_path);

	} else {
		// Check which logs need to be downloaded
		for (auto& entry : entries) {
			auto log_path = logdir + entry.date + ".ulg";

			if (!fs::exists(log_path) && entry.date > most_recent_log) {
				download_log(entry, log_path);
			}
		}
	}

	std::vector<std::string> logs_to_upload;

	for (const auto& it : fs::directory_iterator(logdir)) {
		std::string log_file = it.path();

		if (!has_log_been_uploaded(log_file)) {
			logs_to_upload.push_back(log_file);
		}
	}

	std::cout << "Uploading " << logs_to_upload.size() << " logs" << std::endl;

	for (const auto& log : logs_to_upload) {
		std::cout << log << std::endl;
	}

	for (const auto& log : logs_to_upload) {
		if (send_log_to_server(email, log)) {
			std::cout << "Uploaded success: -- " << log << std::endl;
			mark_log_as_uploaded(log);
		}
	}

	return 0;
}

bool download_log(const LogFiles::Entry& entry, const std::string& log_path)
{
	auto prom = std::promise<LogFiles::Result> {};
	auto future_result = prom.get_future();

	std::cout << "Downloading " << entry.size_bytes << " bytes -- " << entry.date + ".ulg";

	_log_files->download_log_file_async(
		entry,
		log_path,
	[&prom](LogFiles::Result result, LogFiles::ProgressData progress) {
		if (result != LogFiles::Result::Next) {
			prom.set_value(result);
		}
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
		std::cout << "Uploaded successfully. URL: " << res->get_header_value("Location") << std::endl;
		return true;
	}

	else {
		std::cerr << "Failed to upload " << filepath << ". Status: " << (res ? std::to_string(res->status) : "No response") << std::endl;
		return false;
	}
}

bool has_log_been_uploaded(const std::string& log)
{
	std::ifstream file("uploaded_logs.txt");
	std::string line;

	while (std::getline(file, line)) {
		if (line == log) {
			return true;
		}
	}

	return false;
}

void mark_log_as_uploaded(const std::string& log)
{
	std::ofstream file("uploaded_logs.txt", std::ios::app);

	if (file) {
		file << log << std::endl;
	}
}