#include "ServerInterface.hpp"
#include "Log.hpp"

#include <iostream>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <functional>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

namespace fs = std::filesystem;

namespace
{

std::optional<int64_t> parse_iso8601_utc(const std::string& text)
{
	std::tm tm {};

	if (sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ",
		   &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
		return std::nullopt;
	}

	tm.tm_year -= 1900;
	tm.tm_mon -= 1;

	return static_cast<int64_t>(timegm(&tm));
}

} // namespace

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

std::string ServerInterface::generate_uuid(const std::string& remote_path, uint32_t size_bytes)
{
	// Create a unique identifier based on the remote path and size
	std::stringstream ss;
	ss << remote_path << "_" << size_bytes;

	// Use a simple hash for the UUID
	std::hash<std::string> hasher;
	size_t hash = hasher(ss.str());

	ss.str("");
	ss << std::hex << std::setw(16) << std::setfill('0') << hash;
	return ss.str();
}

bool ServerInterface::sync_log(const LogEntry& entry)
{
	// Check if the log already exists
	sqlite3_stmt* stmt;
	std::string check_query = "SELECT COUNT(*) FROM logs WHERE uuid = ?";

	if (sqlite3_prepare_v2(_db, check_query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing sync_log check: " << sqlite3_errmsg(_db) << std::endl;
		return false;
	}

	sqlite3_bind_text(stmt, 1, entry.uuid.c_str(), -1, SQLITE_STATIC);

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
		"INSERT INTO logs (uuid, remote_path, date, size_bytes, local_path, downloaded, uploaded) "
		"VALUES (?, ?, ?, ?, '', 0, 0)";

	if (sqlite3_prepare_v2(_db, insert_query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing sync_log insert: " << sqlite3_errmsg(_db) << std::endl;
		return false;
	}

	sqlite3_bind_text(stmt, 1, entry.uuid.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, entry.remote_path.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, entry.date.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 4, entry.size_bytes);

	bool success = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);

	if (success) {
		grandfather_from_legacy(entry);
	}

	return success;
}

bool ServerInterface::update_download_status(const std::string& uuid, const std::string& local_path, bool downloaded)
{
	std::string query = "UPDATE logs SET downloaded = ?, local_path = ? WHERE uuid = ?";
	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing update_download_status: " << sqlite3_errmsg(_db) << std::endl;
		return false;
	}

	sqlite3_bind_int(stmt, 1, downloaded ? 1 : 0);
	sqlite3_bind_text(stmt, 2, local_path.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, uuid.c_str(), -1, SQLITE_STATIC);

	bool success = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);

	return success;
}

bool ServerInterface::delete_log(const std::string& uuid)
{
	std::string query = "DELETE FROM logs WHERE uuid = ?";
	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing delete_log: " << sqlite3_errmsg(_db) << std::endl;
		return false;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

	bool success = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);

	return success;
}

bool ServerInterface::local_path_in_use(const std::string& local_path, const std::string& excluding_uuid)
{
	std::string query = "SELECT COUNT(*) FROM logs WHERE local_path = ? AND uuid != ?";
	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing local_path_in_use: " << sqlite3_errmsg(_db) << std::endl;
		return false;
	}

	sqlite3_bind_text(stmt, 1, local_path.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, excluding_uuid.c_str(), -1, SQLITE_STATIC);

	bool in_use = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) > 0;
	sqlite3_finalize(stmt);

	return in_use;
}

uint32_t ServerInterface::num_logs_to_upload()
{
	if (!_settings.upload_enabled || _should_exit) {
		return false;
	}

	sqlite3_stmt* stmt;
	std::string query =
		"SELECT COUNT(*) FROM logs "
		"WHERE downloaded = 1 AND uploaded = 0 AND local_path != '' "
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
		"SELECT uuid, remote_path, date, size_bytes, local_path, downloaded FROM logs "
		"WHERE downloaded = 1 AND uploaded = 0 AND local_path != '' "
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

ServerInterface::UploadResult ServerInterface::upload_log(const std::string& uuid, const std::string& filepath)
{
	if (!_settings.upload_enabled || _should_exit) {
		return {false, 0, "Upload disabled or shutting down"};
	}

	if (uuid.empty()) {
		return {false, 0, "Missing UUID"};
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

std::vector<ServerInterface::DatabaseEntry> ServerInterface::get_logs_to_download()
{
	std::vector<DatabaseEntry> entries;

	sqlite3_stmt* stmt;
	std::string query =
		"SELECT uuid, remote_path, date, size_bytes, local_path, downloaded "
		"FROM logs WHERE downloaded = 0 "
		"ORDER BY date DESC, remote_path DESC";

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing get_logs_to_download: " << sqlite3_errmsg(_db) << std::endl;
		return entries;
	}

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		entries.push_back(row_to_db_entry(stmt));
	}

	sqlite3_finalize(stmt);
	return entries;
}

std::string ServerInterface::filepath_from_uuid(const std::string& uuid) const
{
	sqlite3_stmt* stmt;
	std::string query = "SELECT local_path FROM logs WHERE uuid = ?";

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing filepath_from_uuid: " << sqlite3_errmsg(_db) << std::endl;
		return "";
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

	std::string filepath;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char* path_text = sqlite3_column_text(stmt, 0);

		if (path_text != nullptr) {
			filepath = reinterpret_cast<const char*>(path_text);
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

	migrate_legacy_schema();

	// Create logs table
	const char* create_logs_table =
		"CREATE TABLE IF NOT EXISTS logs ("
		"  uuid TEXT PRIMARY KEY,"  // UUID of the log
		"  remote_path TEXT,"       // Path relative to the vehicle log root
		"  date TEXT,"              // ISO8601 date, empty when unknown
		"  size_bytes INTEGER,"     // Size in bytes
		"  local_path TEXT DEFAULT '',"   // Where the download ended up
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

bool ServerInterface::table_exists(const std::string& name) const
{
	sqlite3_stmt* stmt;
	const char* query = "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ?";

	if (sqlite3_prepare_v2(_db, query, -1, &stmt, nullptr) != SQLITE_OK) {
		return false;
	}

	sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);

	bool exists = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) > 0;
	sqlite3_finalize(stmt);

	return exists;
}

bool ServerInterface::column_exists(const std::string& table, const std::string& column) const
{
	sqlite3_stmt* stmt;
	const char* query = "SELECT COUNT(*) FROM pragma_table_info(?) WHERE name = ?";

	if (sqlite3_prepare_v2(_db, query, -1, &stmt, nullptr) != SQLITE_OK) {
		return false;
	}

	sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, column.c_str(), -1, SQLITE_STATIC);

	bool exists = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) > 0;
	sqlite3_finalize(stmt);

	return exists;
}

void ServerInterface::migrate_legacy_schema()
{
	if (table_exists("logs_legacy") && !column_exists("logs_legacy", "migrated")) {
		execute_query("ALTER TABLE logs_legacy ADD COLUMN migrated INTEGER DEFAULT 0");
	}

	// Pre-FTP databases keyed logs on (LOG_ENTRY date, size)
	if (table_exists("logs") && !column_exists("logs", "remote_path")) {
		LOG("Migrating " << _settings.db_path << " to the MAVLink FTP log schema");

		if (table_exists("logs_legacy")) {
			// An older logloader ran again after this one (the ARK-OS installer
			// allows downgrades), so fold its state into the existing table.
			execute_query(
				"INSERT OR IGNORE INTO logs_legacy (uuid, id, date, size_bytes, downloaded, uploaded, migrated) "
				"SELECT uuid, id, date, size_bytes, downloaded, uploaded, 0 FROM logs");
			execute_query("DROP TABLE logs");

		} else {
			execute_query("ALTER TABLE logs RENAME TO logs_legacy");
			execute_query("ALTER TABLE logs_legacy ADD COLUMN migrated INTEGER DEFAULT 0");
		}
	}

	_has_legacy_table = table_exists("logs_legacy");
}

void ServerInterface::grandfather_from_legacy(const LogEntry& entry)
{
	if (!_has_legacy_table) {
		return;
	}

	struct LegacyRow {
		std::string uuid;
		int id;
		std::string date;
		bool downloaded;
		bool uploaded;
	};

	std::vector<LegacyRow> candidates;

	sqlite3_stmt* stmt;

	const char* query =
		"SELECT uuid, id, date, downloaded, uploaded FROM logs_legacy "
		"WHERE migrated = 0 AND size_bytes = ?";

	if (sqlite3_prepare_v2(_db, query, -1, &stmt, nullptr) != SQLITE_OK) {
		return;
	}

	sqlite3_bind_int64(stmt, 1, entry.size_bytes);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		LegacyRow row;
		const unsigned char* uuid_text = sqlite3_column_text(stmt, 0);
		const unsigned char* date_text = sqlite3_column_text(stmt, 2);
		row.uuid = uuid_text ? reinterpret_cast<const char*>(uuid_text) : "";
		row.id = sqlite3_column_int(stmt, 1);
		row.date = date_text ? reinterpret_cast<const char*>(date_text) : "";
		row.downloaded = sqlite3_column_int(stmt, 3) != 0;
		row.uploaded = sqlite3_column_int(stmt, 4) != 0;
		candidates.push_back(row);
	}

	sqlite3_finalize(stmt);

	if (candidates.empty()) {
		return;
	}

	// The legacy date is the log's modification time; the new one is either
	// that as well or the start time parsed from the path. Same log, so they
	// are at most a flight apart. Without a time to compare, only an
	// unambiguous single candidate is safe to claim.
	const LegacyRow* match = nullptr;
	const auto entry_time = parse_iso8601_utc(entry.date);

	if (entry_time.has_value()) {
		constexpr int64_t kMaxDelta = 48 * 3600;
		int64_t best_delta = kMaxDelta;

		for (const auto& candidate : candidates) {
			const auto legacy_time = parse_iso8601_utc(candidate.date);

			if (!legacy_time.has_value()) {
				continue;
			}

			const int64_t delta = std::abs(legacy_time.value() - entry_time.value());

			if (delta <= best_delta) {
				best_delta = delta;
				match = &candidate;
			}
		}

	} else if (candidates.size() == 1) {
		match = &candidates.front();
	}

	if (match == nullptr) {
		return;
	}

	// Reconstruct the file name the legacy naming scheme used
	std::ostringstream legacy_name;
	legacy_name << _settings.logs_directory << "LOG" << std::setfill('0') << std::setw(4)
		    << match->id << "_" << match->date << ".ulg";

	const std::string legacy_path = legacy_name.str();
	const bool file_exists = fs::exists(legacy_path);

	// A log whose file went missing before it was uploaded is re-downloaded
	const bool downloaded = match->downloaded && (file_exists || match->uploaded);
	const std::string local_path = (match->downloaded && file_exists) ? legacy_path : "";

	const char* update_query = "UPDATE logs SET downloaded = ?, uploaded = ?, local_path = ? WHERE uuid = ?";

	if (sqlite3_prepare_v2(_db, update_query, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, downloaded ? 1 : 0);
		sqlite3_bind_int(stmt, 2, match->uploaded ? 1 : 0);
		sqlite3_bind_text(stmt, 3, local_path.c_str(), -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 4, entry.uuid.c_str(), -1, SQLITE_STATIC);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}

	if (is_blacklisted(match->uuid)) {
		add_to_blacklist(entry.uuid, "Migrated from legacy uuid " + match->uuid);
	}

	const char* mark_query = "UPDATE logs_legacy SET migrated = 1 WHERE uuid = ?";

	if (sqlite3_prepare_v2(_db, mark_query, -1, &stmt, nullptr) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, match->uuid.c_str(), -1, SQLITE_STATIC);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}

	LOG_DEBUG("Migrated legacy log " << match->uuid << " -> " << entry.uuid
		  << " (" << entry.remote_path << ")");
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
	entry.uuid = uuid_text ? reinterpret_cast<const char*>(uuid_text) : "";

	const unsigned char* path_text = sqlite3_column_text(stmt, 1);
	entry.remote_path = path_text ? reinterpret_cast<const char*>(path_text) : "";

	const unsigned char* date_text = sqlite3_column_text(stmt, 2);
	entry.date = date_text ? reinterpret_cast<const char*>(date_text) : "";

	entry.size_bytes = static_cast<uint32_t>(sqlite3_column_int64(stmt, 3));

	const unsigned char* local_text = sqlite3_column_text(stmt, 4);
	entry.local_path = local_text ? reinterpret_cast<const char*>(local_text) : "";

	entry.downloaded = sqlite3_column_int(stmt, 5) != 0;

	return entry;
}
