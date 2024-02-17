#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <argparse/argparse.hpp>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/log_files/log_files.h>
#include <future>

using namespace mavsdk;
using std::chrono::seconds;
using std::this_thread::sleep_for;

bool send_log_to_server(const std::string& email, const std::string& filepath);

int main(int argc, char* argv[])
{
	argparse::ArgumentParser parser("logloader");
	parser.add_argument("--url").help("Connection URL, eg: udp://192.168.1.34:14550").default_value(std::string("udp://192.168.1.34:14550"));
	parser.add_argument("--email").help("Your e-mail to send the upload link").default_value(std::string("dahl.jakejacob@gmail.com"));

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

	auto log_files = LogFiles { system.value() };
	auto entries_result = log_files.get_entries();

	if (entries_result.first != LogFiles::Result::Success) {
		std::cerr << "Couldn't get logs" << std::endl;
		return 1;
	}


	std::vector<LogFiles::Entry> entries = entries_result.second;

	size_t total_size = 0;

	for (auto& e : entries) {
		// std::cout << "Got log file with ID " << e.id << " and date " << e.date << " and size " << e.size_bytes << std::endl;
		total_size += e.size_bytes;
	}

	std::cout << "Total size: " << total_size << std::endl;
	std::cout << "Time: " << total_size / 600000 << std::endl;

	// Look at list of already downloaded logs in the folder. If there are any logs in
	// the download folder, only download logs with a date greater than the most recent log.
	bool folder_has_logs = false;


	if (folder_has_logs) {
		// Download all logs from the drone that have a greater date than the most recent log
		std::cout << "We got some logs now" << std::endl;

	} else {
		// Download only the most recent log from the drone
		LogFiles::Entry entry = entries.back();

		auto prom = std::promise<LogFiles::Result> {};
		auto future_result = prom.get_future();

		auto log_path = std::string("logs/") + entry.date + ".ulg";

		std::cout << "Downloading " << entry.size_bytes << " bytes -- " << entry.date + ".ulg";

		log_files.download_log_file_async(
			entry,
			log_path,
		[&prom](LogFiles::Result result, LogFiles::ProgressData progress) {
			if (result != LogFiles::Result::Next) {
				prom.set_value(result);
			}
		});

		auto result = future_result.get();

		if (result == LogFiles::Result::Success) {
			bool success = send_log_to_server(email, log_path);
		}

		else {
			std::cerr << "LogFiles::download_log_file failed: " << result << std::endl;
		}
	}

	return 0;
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
	std::cout << "Uploading..." << std::endl;
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

