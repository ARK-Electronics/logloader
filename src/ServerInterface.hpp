#pragma once

#include <string>
#include <vector>
#include <sqlite3.h>
#include <mavsdk/plugins/log_files/log_files.h>

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
	static std::string generate_uuid(const mavsdk::LogFiles::Entry& entry);
	bool add_log_entry(const mavsdk::LogFiles::Entry& entry);
	bool update_download_status(const std::string& uuid, bool downloaded);

	// Upload management
	bool has_logs_to_upload();
	DatabaseEntry get_next_log_to_upload();
	UploadResult upload_log(const std::string& filepath);

	// Query methods
	bool is_blacklisted(const std::string& uuid);
	DatabaseEntry get_next_log_to_download();

	std::string filepath_from_entry(const mavsdk::LogFiles::Entry& entry) const ;
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
