#include "ServerUploadManager.hpp"

#include <iostream>
#include <filesystem>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

namespace fs = std::filesystem;

ServerUploadManager::ServerUploadManager(const ServerUploadManager::Settings& settings)
	: _settings(settings)
{
	// We must sanitize the URL to strip off the prefix. The prefix informs us which protocl to use.
	// If there is no prefix in the URL string, we assume https.
	sanitize_url_and_determine_protocol();
}

void ServerUploadManager::sanitize_url_and_determine_protocol()
{
	std::string url = _settings.server_url;
	std::string sanitized_url;
	Protocol protocol;

	std::string http_prefix = "http://";
	std::string https_prefix = "https://";

	size_t pos = std::string::npos;

	if ((pos = url.find(https_prefix)) != std::string::npos) {
		sanitized_url = url.substr(pos + https_prefix.length());
		protocol = Protocol::Https;

	} else if ((pos = url.find(http_prefix)) != std::string::npos) {
		sanitized_url = url.substr(pos + http_prefix.length());
		protocol = Protocol::Http;

	} else {
		sanitized_url = url;
		protocol = Protocol::Https;
	}

	_settings.server_url = sanitized_url;
	_protocol = protocol;
}

void ServerUploadManager::start()
{
	_should_exit = false;
}

void ServerUploadManager::stop()
{
	_should_exit = true;
}

void ServerUploadManager::upload_logs()
{
	if (!_settings.upload_enabled || _should_exit) {
		return;
	}

	for (const auto& log_path : upload_logs_list()) {

		if (_should_exit) {
			return;
		}

		// TODO: investigate root cause, we can remove this logic eventually
		if (fs::file_size(log_path) == 0) {
			// Skip and delete erroneous logs of size zero
			std::cout << "Deleting erroneous zero length log file" << std::endl;
			fs::remove(log_path);
			continue;
		}

		if (!upload_log(log_path)) {
			std::cout << "Upload failed" << std::endl;
			return;
		}

		std::cout << "Server upload success: " << _settings.server_url << std::endl;
		set_uploaded(log_path);
	}
}

bool ServerUploadManager::upload_log(const std::string& log_path)
{
	if (!server_reachable()) {
		std::cout << "Server unreachable" << std::endl;
		return false;
	}

	if (!send_to_server(log_path)) {
		std::cout << "Sending log to server failed" << std::endl;
		return false;
	}

	return true;
}

bool ServerUploadManager::server_reachable()
{
	httplib::Result res;

	if (_protocol == Protocol::Https) {
		httplib::SSLClient cli(_settings.server_url);
		res = cli.Get("/");

	} else {
		httplib::Client cli(_settings.server_url);
		res = cli.Get("/");
	}

	bool success = res && res->status == 200;

	if (!success) {
		std::cout << "Connection to " << _settings.server_url << " failed: " << (res ? std::to_string(res->status) : "No response") << std::endl;
	}

	return success;
}


bool ServerUploadManager::send_to_server(const std::string& filepath)
{
	std::ifstream file(filepath, std::ios::binary);

	if (!file) {
		std::cout << "Could not open file " << filepath << std::endl;
		return false;
	}

	// Build multi-part form data
	httplib::MultipartFormDataItems items = {
		{"type", _settings.public_logs ? "flightreport" : "personal", "", ""}, // NOTE: backend logic is funky
		{"description", "Uploaded by logloader", "", ""},
		{"feedback", "", "", ""},
		{"email", _settings.user_email, "", ""},
		{"source", "auto", "", ""},
		{"videoUrl", "", "", ""},
		{"rating", "", "", ""},
		{"windSpeed", "", "", ""},
		{"public", _settings.public_logs ? "true" : "false", "", ""},
	};

	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	items.push_back({"filearg", content, filepath, "application/octet-stream"});

	// Post multi-part form
	std::cout << "Uploading:  "
		  << std::setw(24) << std::left << fs::path(filepath).filename().string()
		  << std::setw(8) << std::fixed << std::setprecision(2) << fs::file_size(filepath) / 1e6 << "MB"
		  << std::flush << std::endl;

	httplib::Result res;

	if (_protocol == Protocol::Https) {
		httplib::SSLClient cli(_settings.server_url);
		res = cli.Post("/upload", items);

	} else {
		httplib::Client cli(_settings.server_url);
		res = cli.Post("/upload", items);
	}

	if (res && res->status == 302) {
		std::string url = _settings.server_url + res->get_header_value("Location");
		std::cout << std::endl << "Upload success:" << std::endl << url << std::endl;
		return true;
	}

	else {
		std::cout << "Failed to upload " << filepath << " to " << _settings.server_url << " Status: " << (res ? std::to_string(
					res->status) : "No response") << std::endl;
		return false;
	}
}

std::vector<std::string> ServerUploadManager::upload_logs_list()
{
	std::vector<std::string> logs;

	for (const auto& it : fs::directory_iterator(_settings.logs_directory)) {
		std::string filepath = it.path().string();
		std::string filename = it.path().filename().string();
		bool should_upload = !is_uploaded(filename) && download_complete(filepath);

		if (should_upload) {
			logs.push_back(it.path());
		}
	}

	return logs;
}

bool ServerUploadManager::download_complete(const std::string& filepath)
{
	return !fs::exists(filepath + ".lock");
}

void ServerUploadManager::set_uploaded(const std::string& filepath)
{
	std::ofstream file(_settings.uploaded_logs_file, std::ios::app);
	file << std::filesystem::path(filepath).filename().string() << std::endl;
}

bool ServerUploadManager::is_uploaded(const std::string& filename)
{
	std::ifstream file(_settings.uploaded_logs_file);
	std::string line;

	while (std::getline(file, line)) {
		if (line == filename) {
			return true;
		}
	}

	return false;
}
