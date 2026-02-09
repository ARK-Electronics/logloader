#include "FlightReviewBackend.hpp"
#include "Log.hpp"

#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

namespace fs = std::filesystem;

FlightReviewBackend::FlightReviewBackend(const FlightReviewBackend::Settings& settings)
	: UploadBackend(settings.db_path, settings.upload_enabled)
	, _settings(settings)
{
	sanitize_url_and_determine_protocol();
}

void FlightReviewBackend::sanitize_url_and_determine_protocol()
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

FlightReviewBackend::UploadResult FlightReviewBackend::upload(const std::string& filepath)
{
	// Skip files that are in progress (have a .lock file)
	if (fs::exists(filepath + ".lock")) {
		return {false, 0, "File is locked (currently being downloaded)"};
	}

	// Skip files that don't exist
	if (!fs::exists(filepath)) {
		return {false, 404, "Log file does not exist: " + filepath};
	}

	// Skip files with size zero
	if (fs::file_size(filepath) == 0) {
		return {false, 0, "Skipping zero-size log file: " + filepath};
	}

	if (!server_reachable()) {
		return {false, 0, "Server unreachable: " + _settings.server_url};
	}

	std::ifstream file(filepath, std::ios::binary);

	if (!file) {
		return {false, 0, "Could not open file: " + filepath};
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

	LOG("Uploading " << fs::path(filepath).filename().string() << " to " << _settings.server_url);

	// Post multi-part form
	httplib::Result res;

	if (_protocol == Protocol::Https) {
		httplib::SSLClient cli(_settings.server_url);
		cli.set_connection_timeout(10);
		cli.set_read_timeout(30);
		res = cli.Post("/upload", items);

	} else {
		httplib::Client cli(_settings.server_url);
		cli.set_connection_timeout(10);
		cli.set_read_timeout(30);
		res = cli.Post("/upload", items);
	}

	if (res && res->status == 302) {
		return {true, 302, "Success: " + _settings.server_url + res->get_header_value("Location")};

	} else if (res && res->status == 400) {
		return {false, 400, "Bad Request - Will not retry"};

	} else {
		return {false, res ? res->status : 0, "Will retry later"};
	}
}

bool FlightReviewBackend::server_reachable()
{
	httplib::Result res;

	if (_protocol == Protocol::Https) {
		httplib::SSLClient cli(_settings.server_url);
		cli.set_connection_timeout(10);
		cli.set_read_timeout(30);
		res = cli.Get("/");

	} else {
		httplib::Client cli(_settings.server_url);
		cli.set_connection_timeout(10);
		cli.set_read_timeout(30);
		res = cli.Get("/");
	}

	bool success = res && res->status == 200;

	if (!success) {
		LOG("Connection to " << _settings.server_url << " failed: " << (res ? std::to_string(res->status) : "No response"));
	}

	return success;
}
