#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <sqlite3.h>

class UploadBackend
{
public:
	struct UploadResult {
		bool success;
		int status_code;    // HTTP status code, or 0 if not applicable
		std::string message;
	};

	UploadBackend(const std::string& db_path, bool upload_enabled);
	virtual ~UploadBackend();

	// Database initialization
	bool init_database();
	void close_database();

	// Upload management
	void register_log(const std::string& uuid);
	uint32_t num_logs_to_upload();
	std::string get_next_log_to_upload();
	UploadResult upload_log(const std::string& filepath, const std::string& uuid);

	// Query methods
	bool is_blacklisted(const std::string& uuid);
	bool add_to_blacklist(const std::string& uuid, const std::string& reason);

	void start();
	void stop();

protected:
	// Subclasses implement this to perform the actual upload
	virtual UploadResult upload(const std::string& filepath) = 0;

	// Database operations
	bool execute_query(const std::string& query);

	std::string _db_path;
	bool _upload_enabled {};
	std::atomic<bool> _should_exit{false};
	sqlite3* _db = nullptr;
};
