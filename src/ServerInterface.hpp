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

	// Minimal log description, decoupled from any MAVLink library. The pymavlink
	// downloader writes log files into the logs directory; everything the uploader
	// needs (id, date, size) is recovered from the on-disk filename + file size.
	struct LogInfo {
		uint32_t id {};
		std::string date;
		uint32_t size_bytes {};
	};

	struct UploadResult {
		bool success;
		int status_code;    // HTTP status code, or 0 if not applicable
		std::string message;
	};

	struct DatabaseEntry {
		std::string uuid;
		uint32_t id;
		std::string date;
		uint32_t size_bytes;
		bool downloaded;
	};

	ServerInterface(const Settings& settings);
	~ServerInterface();

	// Database initialization
	bool init_database();
	void close_database();

	// Log entry management
	static std::string generate_uuid(const LogInfo& info);
	bool add_log_entry(const LogInfo& info);
	bool update_download_status(const std::string& uuid, bool downloaded);

	// Register a downloaded log file (parsed from its filename) as ready to upload.
	bool register_log_file(const std::string& filepath);

	// Upload management
	uint32_t num_logs_to_upload();
	DatabaseEntry get_next_log_to_upload();
	UploadResult upload_log(const std::string& filepath);

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

	Settings _settings;
	Protocol _protocol {Protocol::Https};
	bool _should_exit = false;
	sqlite3* _db = nullptr;
};
