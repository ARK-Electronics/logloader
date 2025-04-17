#include "ServerInterface.hpp"
#include "Log.hpp"

#include <iostream>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <functional>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

namespace fs = std::filesystem;

ServerInterface::ServerInterface(const ServerInterface::Settings& settings)
	: _settings(settings)
{
	// Sanitize the URL to strip off the prefix
	sanitize_url_and_determine_protocol();

	// Initialize the database
	if (!init_database()) {
		std::cerr << "Failed to initialize database for server: " << _settings.server_url << std::endl;
	}
}

ServerInterface::~ServerInterface()
{
	close_database();
}

void ServerInterface::sanitize_url_and_determine_protocol()
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

void ServerInterface::start()
{
	_should_exit = false;
}

void ServerInterface::stop()
{
	_should_exit = true;
}

std::string ServerInterface::generate_uuid(const mavsdk::LogFiles::Entry& entry)
{
	// Create a unique identifier based on date and size
	std::stringstream ss;
	ss << entry.date << "_" << entry.size_bytes;

	// Use a simple hash for the UUID
	std::hash<std::string> hasher;
	size_t hash = hasher(ss.str());

	ss.str("");
	ss << std::hex << std::setw(16) << std::setfill('0') << hash;
	return ss.str();
}

bool ServerInterface::add_log_entry(const mavsdk::LogFiles::Entry& entry)
{
	std::string uuid = generate_uuid(entry);

	// Check if the log already exists
	sqlite3_stmt* stmt;
	std::string check_query = "SELECT COUNT(*) FROM logs WHERE uuid = ?";

	if (sqlite3_prepare_v2(_db, check_query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing add_log_entry check: " << sqlite3_errmsg(_db) << std::endl;
		return false;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

	bool exists = false;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		exists = sqlite3_column_int(stmt, 0) > 0;
	}

	sqlite3_finalize(stmt);

	if (exists) {
		return true; // Already exists, no need to add
	}

	// Insert the log
	std::string insert_query =
		"INSERT INTO logs (uuid, id, date, size_bytes, downloaded, uploaded) "
		"VALUES (?, ?, ?, ?, 0, 0)";

	if (sqlite3_prepare_v2(_db, insert_query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing add_log_entry insert: " << sqlite3_errmsg(_db) << std::endl;
		return false;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 2, entry.id);
	sqlite3_bind_text(stmt, 3, entry.date.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 4, entry.size_bytes);

	bool success = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);

	return success;
}

bool ServerInterface::update_download_status(const std::string& uuid, bool downloaded)
{
	std::string query = "UPDATE logs SET downloaded = ? WHERE uuid = ?";
	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing update_download_status: " << sqlite3_errmsg(_db) << std::endl;
		return false;
	}

	sqlite3_bind_int(stmt, 1, downloaded ? 1 : 0);
	sqlite3_bind_text(stmt, 2, uuid.c_str(), -1, SQLITE_STATIC);

	bool success = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);

	return success;
}

uint32_t ServerInterface::num_logs_to_upload()
{
	if (!_settings.upload_enabled || _should_exit) {
		return false;
	}

	sqlite3_stmt* stmt;
	std::string query =
		"SELECT COUNT(*) FROM logs "
		"WHERE downloaded = 1 AND uploaded = 0 "
		"AND uuid NOT IN (SELECT uuid FROM blacklist)";

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing has_logs_to_upload: " << sqlite3_errmsg(_db) << std::endl;
		return false;
	}

	uint32_t log_count = 0;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		log_count = sqlite3_column_int(stmt, 0);
	}

	sqlite3_finalize(stmt);
	return log_count;
}

ServerInterface::DatabaseEntry ServerInterface::get_next_log_to_upload()
{
	DatabaseEntry empty_entry;
	empty_entry.uuid = ""; // Empty UUID indicates not found

	if (!_settings.upload_enabled || _should_exit) {
		return empty_entry;
	}

	sqlite3_stmt* stmt;
	std::string query =
		"SELECT uuid, id, date, size_bytes, downloaded, uploaded FROM logs "
		"WHERE downloaded = 1 AND uploaded = 0 "
		"AND uuid NOT IN (SELECT uuid FROM blacklist) "
		"ORDER BY date DESC, size_bytes DESC LIMIT 1";

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing get_next_log_to_upload: " << sqlite3_errmsg(_db) << std::endl;
		return empty_entry;
	}

	DatabaseEntry entry = empty_entry;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		entry = row_to_db_entry(stmt);
	}

	sqlite3_finalize(stmt);
	return entry;
}

ServerInterface::UploadResult ServerInterface::upload_log(const std::string& filepath)
{
	if (!_settings.upload_enabled || _should_exit) {
		return {false, 0, "Upload disabled or shutting down"};
	}

	// Extract UUID from filename
	std::string filename = fs::path(filepath).filename().string();
	std::string uuid;

	// Parse the ID and date from filename (assuming format like LOG0001_2023-04-15T12:34:56Z.ulg)
	size_t underscore_pos = filename.find('_');
	size_t dot_pos = filename.find_last_of('.');

	if (underscore_pos != std::string::npos && dot_pos != std::string::npos) {
		std::string id_part = filename.substr(3, underscore_pos - 3); // Skip "LOG" prefix
		std::string date_part = filename.substr(underscore_pos + 1, dot_pos - underscore_pos - 1);

		uint32_t id = std::stoi(id_part);
		uint32_t size = fs::exists(filepath) ? fs::file_size(filepath) : 0;

		// Create a log entry and generate UUID
		mavsdk::LogFiles::Entry entry;
		entry.id = id;
		entry.date = date_part;
		entry.size_bytes = size;

		uuid = generate_uuid(entry);

		// Add to database if not already there
		sqlite3_stmt* stmt;
		std::string check_query = "SELECT COUNT(*) FROM logs WHERE uuid = ?";

		if (sqlite3_prepare_v2(_db, check_query.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

			if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 0) {
				// Log doesn't exist, add it
				add_log_entry(entry);
				update_download_status(uuid, true); // Mark as downloaded since we have the file
			}

			sqlite3_finalize(stmt);
		}
	}

	if (uuid.empty()) {
		return {false, 0, "Could not determine UUID from filename"};
	}

	// Check if already blacklisted
	if (is_blacklisted(uuid)) {
		return {false, 400, "Log is blacklisted"};
	}

	// Perform the upload
	UploadResult result = upload(filepath);

	// Update database with result
	if (result.success) {
		std::string query = "UPDATE logs SET uploaded = 1 WHERE uuid = ?";
		sqlite3_stmt* stmt;

		if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
		}

	} else if (result.status_code == 400) {
		// Permanent failure - add to blacklist
		add_to_blacklist(uuid, "HTTP 400: Bad Request");
	}

	return result;
}

bool ServerInterface::is_blacklisted(const std::string& uuid)
{
	std::string query = "SELECT COUNT(*) FROM blacklist WHERE uuid = ?";
	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing is_blacklisted: " << sqlite3_errmsg(_db) << std::endl;
		return false;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

	bool blacklisted = false;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		blacklisted = sqlite3_column_int(stmt, 0) > 0;
	}

	sqlite3_finalize(stmt);
	return blacklisted;
}

uint32_t ServerInterface::num_logs_to_download()
{
	sqlite3_stmt* stmt;
	std::string query =
		"SELECT COUNT(*) FROM logs "
		"WHERE downloaded = 0";

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing num_logs_to_download: " << sqlite3_errmsg(_db) << std::endl;
		return 0;
	}

	uint32_t log_count = 0;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		log_count = sqlite3_column_int(stmt, 0);
	}

	sqlite3_finalize(stmt);
	return log_count;
}

ServerInterface::DatabaseEntry ServerInterface::get_next_log_to_download()
{
	DatabaseEntry empty_entry;
	empty_entry.uuid = ""; // Empty UUID indicates not found

	sqlite3_stmt* stmt;
	std::string query =
		"SELECT uuid, id, date, size_bytes, downloaded, uploaded "
		"FROM logs WHERE downloaded = 0 "
		"ORDER BY date DESC, size_bytes DESC LIMIT 1";

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing get_next_log_to_download: " << sqlite3_errmsg(_db) << std::endl;
		return empty_entry;
	}

	DatabaseEntry entry = empty_entry;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		entry = row_to_db_entry(stmt);
	}

	sqlite3_finalize(stmt);
	return entry;
}

std::string ServerInterface::filepath_from_entry(const mavsdk::LogFiles::Entry& entry) const
{
	std::ostringstream ss;
	ss << _settings.logs_directory << "LOG" << std::setfill('0') << std::setw(4) << entry.id << "_" << entry.date << ".ulg";
	return ss.str();
}

std::string ServerInterface::filepath_from_uuid(const std::string& uuid) const
{
	// Look up the log entry by UUID
	sqlite3_stmt* stmt;
	std::string query =
		"SELECT id, date FROM logs WHERE uuid = ?";

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing filepath_from_uuid: " << sqlite3_errmsg(_db) << std::endl;
		return "";
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

	std::string filepath;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		int id = sqlite3_column_int(stmt, 0);
		const unsigned char* date_text = sqlite3_column_text(stmt, 1);

		if (date_text != nullptr) {
			std::string date = reinterpret_cast<const char*>(date_text);
			std::ostringstream ss;
			ss << _settings.logs_directory << "LOG" << std::setfill('0') << std::setw(4) << id << "_" << date << ".ulg";
			filepath = ss.str();
		}
	}

	sqlite3_finalize(stmt);
	return filepath;
}

ServerInterface::UploadResult ServerInterface::upload(const std::string& filepath)
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
		res = cli.Post("/upload", items);

	} else {
		httplib::Client cli(_settings.server_url);
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

bool ServerInterface::server_reachable()
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
		LOG("Connection to " << _settings.server_url << " failed: " << (res ? std::to_string(res->status) : "No response"));
	}

	return success;
}

bool ServerInterface::init_database()
{
	int rc = sqlite3_open(_settings.db_path.c_str(), &_db);

	if (rc != SQLITE_OK) {
		std::cerr << "Cannot open database: " << sqlite3_errmsg(_db) << std::endl;
		sqlite3_close(_db);
		_db = nullptr;
		return false;
	}

	// Create logs table
	const char* create_logs_table =
		"CREATE TABLE IF NOT EXISTS logs ("
		"  uuid TEXT PRIMARY KEY,"  // UUID of the log
		"  id INTEGER,"             // Original log ID
		"  date TEXT,"              // ISO8601 date from log
		"  size_bytes INTEGER,"     // Size in bytes
		"  downloaded INTEGER DEFAULT 0," // Has it been downloaded
		"  uploaded INTEGER DEFAULT 0"   // Has it been uploaded
		");";

	// Create blacklist table
	const char* create_blacklist_table =
		"CREATE TABLE IF NOT EXISTS blacklist ("
		"  uuid TEXT PRIMARY KEY,"  // UUID of the log
		"  reason TEXT,"            // Reason for blacklisting
		"  timestamp TEXT"          // When the log was blacklisted
		");";

	bool success = execute_query(create_logs_table) && execute_query(create_blacklist_table);
	return success;
}

void ServerInterface::close_database()
{
	if (_db) {
		sqlite3_close(_db);
		_db = nullptr;
	}
}

bool ServerInterface::add_to_blacklist(const std::string& uuid, const std::string& reason)
{
	// Get current timestamp
	auto now = std::chrono::system_clock::now();
	auto now_c = std::chrono::system_clock::to_time_t(now);
	std::stringstream ss;
	ss << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S");
	std::string timestamp = ss.str();

	// Add to blacklist
	std::string query = "INSERT OR REPLACE INTO blacklist (uuid, reason, timestamp) VALUES (?, ?, ?)";
	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing add_to_blacklist: " << sqlite3_errmsg(_db) << std::endl;
		return false;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, reason.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, timestamp.c_str(), -1, SQLITE_STATIC);

	bool success = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);

	return success;
}

bool ServerInterface::execute_query(const std::string& query)
{
	char* error_msg = nullptr;
	int rc = sqlite3_exec(_db, query.c_str(), nullptr, nullptr, &error_msg);

	if (rc != SQLITE_OK) {
		std::cerr << "SQL error: " << error_msg << std::endl;
		sqlite3_free(error_msg);
		return false;
	}

	return true;
}

ServerInterface::DatabaseEntry ServerInterface::row_to_db_entry(sqlite3_stmt* stmt)
{
	DatabaseEntry entry;

	const unsigned char* uuid_text = sqlite3_column_text(stmt, 0);

	if (uuid_text != nullptr) {
		entry.uuid = reinterpret_cast<const char*>(uuid_text);

	} else {
		entry.uuid = "";
	}

	entry.id = sqlite3_column_int(stmt, 1);

	const unsigned char* date_text = sqlite3_column_text(stmt, 2);

	if (date_text != nullptr) {
		entry.date = reinterpret_cast<const char*>(date_text);

	} else {
		entry.date = "";
	}

	entry.size_bytes = sqlite3_column_int(stmt, 3);
	entry.downloaded = sqlite3_column_int(stmt, 4) != 0;

	return entry;
}
