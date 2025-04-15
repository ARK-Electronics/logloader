#include "LogDatabase.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <functional>

LogDatabase::LogDatabase(const std::string& db_path)
	: _db_path(db_path), _db(nullptr)
{
}

LogDatabase::~LogDatabase()
{
	if (_db) {
		sqlite3_close(_db);
	}
}

bool LogDatabase::init()
{
	int rc = sqlite3_open(_db_path.c_str(), &_db);

	if (rc != SQLITE_OK) {
		std::cerr << "Cannot open database: " << sqlite3_errmsg(_db) << std::endl;
		sqlite3_close(_db);
		return false;
	}

	// Create logs table if it doesn't exist
	const char* create_table_sql =
		"CREATE TABLE IF NOT EXISTS logs ("
		"uuid TEXT PRIMARY KEY,"
		"id INTEGER,"
		"date TEXT,"
		"size_bytes INTEGER,"
		"downloaded INTEGER DEFAULT 0,"
		"local_uploaded INTEGER DEFAULT 0,"
		"remote_uploaded INTEGER DEFAULT 0"
		");";

	return execute_query(create_table_sql);
}

bool LogDatabase::execute_query(const std::string& query)
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

std::string LogDatabase::generate_uuid(const mavsdk::LogFiles::Entry& entry)
{
	// Create a unique identifier based on date and size
	std::stringstream ss;
	ss << entry.date << "_" << entry.size_bytes;

	// Use a simple hash for the UUID (in practice, you might want a better hash function)
	std::hash<std::string> hasher;
	size_t hash = hasher(ss.str());

	ss.str("");
	ss << std::hex << std::setw(16) << std::setfill('0') << hash;
	return ss.str();
}

LogDatabase::LogRecord LogDatabase::entry_to_record(const mavsdk::LogFiles::Entry& entry)
{
	LogRecord record;
	record.uuid = generate_uuid(entry);
	record.id = entry.id;
	record.date = entry.date;
	record.size_bytes = entry.size_bytes;
	record.downloaded = false;
	record.local_uploaded = false;
	record.remote_uploaded = false;

	return record;
}

bool LogDatabase::add_log(const mavsdk::LogFiles::Entry& entry)
{
	std::string uuid = generate_uuid(entry);

	// Check if the log already exists
	sqlite3_stmt* stmt;
	std::string check_query = "SELECT COUNT(*) FROM logs WHERE uuid = ?";

	if (sqlite3_prepare_v2(_db, check_query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		return false;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		int count = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);

		if (count > 0) {
			// Log already exists, no need to add it again
			return true;
		}

	} else {
		sqlite3_finalize(stmt);
		return false;
	}

	// Insert the new log
	std::string insert_query =
		"INSERT INTO logs (uuid, id, date, size_bytes, downloaded, local_uploaded, remote_uploaded) "
		"VALUES (?, ?, ?, ?, 0, 0, 0)";

	if (sqlite3_prepare_v2(_db, insert_query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
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

bool LogDatabase::update_download_status(const std::string& uuid, bool downloaded)
{
	std::string query = "UPDATE logs SET downloaded = ? WHERE uuid = ?";

	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		return false;
	}

	sqlite3_bind_int(stmt, 1, downloaded ? 1 : 0);
	sqlite3_bind_text(stmt, 2, uuid.c_str(), -1, SQLITE_STATIC);

	bool success = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);

	return success;
}

bool LogDatabase::update_upload_status(const std::string& uuid, bool local_uploaded, bool remote_uploaded)
{
	std::string query = "UPDATE logs SET local_uploaded = ?, remote_uploaded = ? WHERE uuid = ?";

	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		return false;
	}

	sqlite3_bind_int(stmt, 1, local_uploaded ? 1 : 0);
	sqlite3_bind_int(stmt, 2, remote_uploaded ? 1 : 0);
	sqlite3_bind_text(stmt, 3, uuid.c_str(), -1, SQLITE_STATIC);

	bool success = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);

	return success;
}

bool LogDatabase::is_log_downloaded(const std::string& uuid)
{
	std::string query = "SELECT downloaded FROM logs WHERE uuid = ?";

	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		return false;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

	bool downloaded = false;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		downloaded = sqlite3_column_int(stmt, 0) != 0;
	}

	sqlite3_finalize(stmt);
	return downloaded;
}

bool LogDatabase::is_log_uploaded_local(const std::string& uuid)
{
	std::string query = "SELECT local_uploaded FROM logs WHERE uuid = ?";

	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		return false;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

	bool uploaded = false;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		uploaded = sqlite3_column_int(stmt, 0) != 0;
	}

	sqlite3_finalize(stmt);
	return uploaded;
}

bool LogDatabase::is_log_uploaded_remote(const std::string& uuid)
{
	std::string query = "SELECT remote_uploaded FROM logs WHERE uuid = ?";

	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		return false;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

	bool uploaded = false;

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		uploaded = sqlite3_column_int(stmt, 0) != 0;
	}

	sqlite3_finalize(stmt);
	return uploaded;
}

std::vector<LogDatabase::LogRecord> LogDatabase::get_logs_to_download(int limit, int offset)
{
	std::vector<LogRecord> logs;

	// Query logs that haven't been downloaded yet
	std::string query =
		"SELECT uuid, id, date, size_bytes, downloaded, local_uploaded, remote_uploaded "
		"FROM logs WHERE downloaded = 0 ORDER BY date DESC, size_bytes DESC LIMIT ? OFFSET ?";

	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		return logs;
	}

	sqlite3_bind_int(stmt, 1, limit);
	sqlite3_bind_int(stmt, 2, offset);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		logs.push_back(row_to_log_record(stmt));
	}

	sqlite3_finalize(stmt);
	return logs;
}

std::vector<LogDatabase::LogRecord> LogDatabase::get_logs_to_upload_local(int limit)
{
	std::vector<LogRecord> logs;

	// Query logs that have been downloaded but not uploaded to local server
	std::string query =
		"SELECT uuid, id, date, size_bytes, downloaded, local_uploaded, remote_uploaded "
		"FROM logs WHERE downloaded = 1 AND local_uploaded = 0 ORDER BY date DESC, size_bytes DESC LIMIT ?";

	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		return logs;
	}

	sqlite3_bind_int(stmt, 1, limit);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		logs.push_back(row_to_log_record(stmt));
	}

	sqlite3_finalize(stmt);
	return logs;
}

std::vector<LogDatabase::LogRecord> LogDatabase::get_logs_to_upload_remote(int limit)
{
	std::vector<LogRecord> logs;

	// Query logs that have been downloaded but not uploaded to remote server
	std::string query =
		"SELECT uuid, id, date, size_bytes, downloaded, local_uploaded, remote_uploaded "
		"FROM logs WHERE downloaded = 1 AND remote_uploaded = 0 ORDER BY date DESC, size_bytes DESC LIMIT ?";

	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		return logs;
	}

	sqlite3_bind_int(stmt, 1, limit);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		logs.push_back(row_to_log_record(stmt));
	}

	sqlite3_finalize(stmt);
	return logs;
}

LogDatabase::LogRecord LogDatabase::get_log_by_uuid(const std::string& uuid)
{
	LogRecord record;
	record.uuid = "";  // Empty UUID indicates not found

	std::string query =
		"SELECT uuid, id, date, size_bytes, downloaded, local_uploaded, remote_uploaded "
		"FROM logs WHERE uuid = ?";

	sqlite3_stmt* stmt;

	if (sqlite3_prepare_v2(_db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
		return record;
	}

	sqlite3_bind_text(stmt, 1, uuid.c_str(), -1, SQLITE_STATIC);

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		record = row_to_log_record(stmt);
	}

	sqlite3_finalize(stmt);
	return record;
}

LogDatabase::LogRecord LogDatabase::row_to_log_record(sqlite3_stmt* stmt)
{
	LogRecord record;

	const unsigned char* uuid_text = sqlite3_column_text(stmt, 0);

	if (uuid_text != nullptr) {
		record.uuid = reinterpret_cast<const char*>(uuid_text);

	} else {
		record.uuid = "";
	}

	record.id = sqlite3_column_int(stmt, 1);

	const unsigned char* date_text = sqlite3_column_text(stmt, 2);

	if (date_text != nullptr) {
		record.date = reinterpret_cast<const char*>(date_text);

	} else {
		record.date = "";
	}

	record.size_bytes = sqlite3_column_int(stmt, 3);
	record.downloaded = sqlite3_column_int(stmt, 4) != 0;
	record.local_uploaded = sqlite3_column_int(stmt, 5) != 0;
	record.remote_uploaded = sqlite3_column_int(stmt, 6) != 0;

	return record;
}
