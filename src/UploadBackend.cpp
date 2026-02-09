#include "UploadBackend.hpp"
#include "Log.hpp"

#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

UploadBackend::UploadBackend(const std::string& db_path, bool upload_enabled)
	: _db_path(db_path)
	, _upload_enabled(upload_enabled)
{
	if (!init_database()) {
		std::cerr << "Failed to initialize database: " << _db_path << std::endl;
	}
}

UploadBackend::~UploadBackend()
{
	close_database();
}

void UploadBackend::start()
{
	_should_exit = false;
}

void UploadBackend::stop()
{
	_should_exit = true;
}

void UploadBackend::register_log(const std::string& uuid)
{
	if (uuid.empty()) {
		return;
	}

	// Check if already exists
	sqlite3_stmt* stmt;
	std::string check_query = "SELECT COUNT(*) FROM logs WHERE uuid = ?";

	if (sqlite3_prepare_v2(_db, check_query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing register_log check: " << sqlite3_errmsg(_db) << std::endl;
		return;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

	bool exists = false;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		exists = sqlite3_column_int(stmt, 0) > 0;
	}

	sqlite3_finalize(stmt);

	if (exists) {
		return;
	}

	// Insert
	std::string insert_query = "INSERT INTO logs (uuid, uploaded) VALUES (?, 0)";

	if (sqlite3_prepare_v2(_db, insert_query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing register_log insert: " << sqlite3_errmsg(_db) << std::endl;
		return;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

uint32_t UploadBackend::num_logs_to_upload()
{
	if (!_upload_enabled || _should_exit) {
		return 0;
	}

	sqlite3_stmt* stmt;
	std::string query =
		"SELECT COUNT(*) FROM logs "
		"WHERE uploaded = 0 "
		"AND uuid NOT IN (SELECT uuid FROM blacklist)";

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing num_logs_to_upload: " << sqlite3_errmsg(_db) << std::endl;
		return 0;
	}

	uint32_t log_count = 0;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		log_count = sqlite3_column_int(stmt, 0);
	}

	sqlite3_finalize(stmt);
	return log_count;
}

std::string UploadBackend::get_next_log_to_upload()
{
	if (!_upload_enabled || _should_exit) {
		return "";
	}

	sqlite3_stmt* stmt;
	std::string query =
		"SELECT uuid FROM logs "
		"WHERE uploaded = 0 "
		"AND uuid NOT IN (SELECT uuid FROM blacklist) "
		"LIMIT 1";

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		std::cerr << "SQL error preparing get_next_log_to_upload: " << sqlite3_errmsg(_db) << std::endl;
		return "";
	}

	std::string uuid;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char* uuid_text = sqlite3_column_text(stmt, 0);

		if (uuid_text != nullptr) {
			uuid = reinterpret_cast<const char*>(uuid_text);
		}
	}

	sqlite3_finalize(stmt);
	return uuid;
}

UploadBackend::UploadResult UploadBackend::upload_log(const std::string& filepath, const std::string& uuid)
{
	if (!_upload_enabled || _should_exit) {
		return {false, 0, "Upload disabled or shutting down"};
	}

	if (uuid.empty()) {
		return {false, 0, "Empty UUID"};
	}

	// Check if already blacklisted
	if (is_blacklisted(uuid)) {
		return {false, 400, "Log is blacklisted"};
	}

	// Perform the upload (implemented by subclass)
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
		add_to_blacklist(uuid, "Permanent failure: " + result.message);
	}

	return result;
}

bool UploadBackend::is_blacklisted(const std::string& uuid)
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

bool UploadBackend::init_database()
{
	int rc = sqlite3_open(_db_path.c_str(), &_db);

	if (rc != SQLITE_OK) {
		std::cerr << "Cannot open database: " << sqlite3_errmsg(_db) << std::endl;
		sqlite3_close(_db);
		_db = nullptr;
		return false;
	}

	// Upload tracking table — just uuid and uploaded status
	const char* create_logs_table =
		"CREATE TABLE IF NOT EXISTS logs ("
		"  uuid TEXT PRIMARY KEY,"
		"  uploaded INTEGER DEFAULT 0"
		");";

	// Blacklist table
	const char* create_blacklist_table =
		"CREATE TABLE IF NOT EXISTS blacklist ("
		"  uuid TEXT PRIMARY KEY,"
		"  reason TEXT,"
		"  timestamp TEXT"
		");";

	bool success = execute_query(create_logs_table) && execute_query(create_blacklist_table);
	return success;
}

void UploadBackend::close_database()
{
	if (_db) {
		sqlite3_close(_db);
		_db = nullptr;
	}
}

bool UploadBackend::add_to_blacklist(const std::string& uuid, const std::string& reason)
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

bool UploadBackend::execute_query(const std::string& query)
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
