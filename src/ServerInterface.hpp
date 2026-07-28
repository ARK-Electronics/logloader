#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <sqlite3.h>

class ServerInterface
{
public:
	struct Settings {
		std::string server_url;
		std::string user_email;
		std::string logs_directory;
		std::string db_path;         // Path to this server's database
		bool upload_enabled {};
		bool public_logs {};
	};

	struct UploadResult {
		bool success;
		int status_code;    // HTTP status code, or 0 if not applicable
		std::string message;
	};

	// A log as tracked in the database. Identity is the remote path (relative
	// to the vehicle log root) plus the size: the path alone is not enough
	// because ArduPilot wraps its log numbering and reuses file names.
	struct LogEntry {
		std::string uuid;
		std::string remote_path;
		std::string date;       // ISO8601 UTC, empty when the vehicle cannot report a time
		uint32_t size_bytes {};
	};

	struct DatabaseEntry {
		std::string uuid;
		std::string remote_path;
		std::string date;
		uint32_t size_bytes {};
		std::string local_path; // Empty until downloaded
		bool downloaded {};
	};

	ServerInterface(const Settings& settings);
	~ServerInterface();

	// Database initialization
	bool init_database();
	void close_database();

	// Log entry management
	static std::string generate_uuid(const std::string& remote_path, uint32_t size_bytes);
	bool sync_log(const LogEntry& entry);
	bool update_download_status(const std::string& uuid, const std::string& local_path, bool downloaded);
	bool delete_log(const std::string& uuid);
	uint32_t num_logs_to_download();
	std::vector<DatabaseEntry> get_logs_to_download();
	bool local_path_in_use(const std::string& local_path, const std::string& excluding_uuid);

	// Upload management
	uint32_t num_logs_to_upload();
	DatabaseEntry get_next_log_to_upload();
	UploadResult upload_log(const std::string& uuid, const std::string& filepath);

	// Query methods
	bool is_blacklisted(const std::string& uuid);

	std::string filepath_from_uuid(const std::string& uuid) const;

	void start();
	void stop();

private:
	enum class Protocol {
		Http,
		Https
	};

	void sanitize_url_and_determine_protocol();
	UploadResult upload(const std::string& filepath);
	bool server_reachable();

	// Database operations
	bool execute_query(const std::string& query);
	bool add_to_blacklist(const std::string& uuid, const std::string& reason);
	DatabaseEntry row_to_db_entry(sqlite3_stmt* stmt);

	// Databases written before the MAVLink FTP transition keyed logs on the
	// LOG_ENTRY timestamp, which FTP cannot reproduce. Their rows are moved
	// aside on startup and matched back up by size as the FTP index comes in,
	// so download/upload state survives the upgrade.
	bool table_exists(const std::string& name) const;
	bool column_exists(const std::string& table, const std::string& column) const;
	void migrate_legacy_schema();
	void grandfather_from_legacy(const LogEntry& entry);

	Settings _settings;
	Protocol _protocol {Protocol::Https};
	bool _should_exit = false;
	bool _has_legacy_table = false;
	sqlite3* _db = nullptr;
};
