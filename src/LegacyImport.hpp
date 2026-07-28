#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

// Carries state over from the pre-FTP logloader, which kept one database per
// server and identified a log by the index and timestamp LOG_ENTRY reported.
// MAVLink FTP cannot reproduce either, so the old rows are set aside and
// matched to the listing by size as it comes in.
//
// This exists only while a fleet upgrades. Once every device has run a version
// containing it, delete the file: nothing else depends on it.
namespace legacy
{

class Importer
{
public:
	// Folds local_server.db and remote_server.db, if they are next to db_path,
	// into a legacy_logs table in db. Doing nothing is the normal case.
	Importer(sqlite3* db, const std::string& db_path, const std::string& logs_directory,
		 const std::vector<std::string>& targets);

	// False when there is nothing left to match, which is the steady state.
	bool has_rows() const { return _has_rows; }

	// Claims the state of a matching old row for a log just inserted. The
	// caller holds the database lock and is inside its transaction.
	void adopt(int64_t log_id, uint32_t size_bytes, std::optional<int64_t> time_utc, const std::string& path);

private:
	void import_one(const std::string& file, const std::string& target);

	sqlite3* _db {nullptr};
	std::string _logs_directory;
	std::vector<std::string> _targets;
	bool _has_rows {false};
};

} // namespace legacy
