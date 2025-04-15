#pragma once

#include <string>
#include <vector>
#include <sqlite3.h>
#include <mavsdk/plugins/log_files/log_files.h>

class LogDatabase
{
public:
	struct LogRecord {
		std::string uuid;  // Unique identifier for the log
		uint32_t id;       // Original log ID
		std::string date;  // ISO8601 date from log
		uint32_t size_bytes; // Size in bytes
		bool downloaded;   // Has it been downloaded
		bool local_uploaded; // Has it been uploaded to local server
		bool remote_uploaded; // Has it been uploaded to remote server
	};

	LogDatabase(const std::string& db_path);
	~LogDatabase();

	// Initialize database
	bool init();

	// Log management
	std::string generate_uuid(const mavsdk::LogFiles::Entry& entry);
	bool add_log(const mavsdk::LogFiles::Entry& entry);
	bool update_download_status(const std::string& uuid, bool downloaded);
	bool update_upload_status(const std::string& uuid, bool local_uploaded, bool remote_uploaded);

	// Query methods
	bool is_log_downloaded(const std::string& uuid);
	bool is_log_uploaded_local(const std::string& uuid);
	bool is_log_uploaded_remote(const std::string& uuid);

	// Get logs that need to be downloaded
	std::vector<LogRecord> get_logs_to_download(int limit = 1, int offset = 0);

	// Get logs that need to be uploaded
	std::vector<LogRecord> get_logs_to_upload_local(int limit = 1);
	std::vector<LogRecord> get_logs_to_upload_remote(int limit = 1);

	// Get log by UUID
	LogRecord get_log_by_uuid(const std::string& uuid);

	// Convert entry to database record
	LogRecord entry_to_record(const mavsdk::LogFiles::Entry& entry);

private:
	sqlite3* _db;
	std::string _db_path;

	// Helper methods
	bool execute_query(const std::string& query);
	LogRecord row_to_log_record(sqlite3_stmt* stmt);
};
