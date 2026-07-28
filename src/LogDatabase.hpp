#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

// The inventory of the vehicle's logs and what logloader intends to do with
// each one. There is exactly one of these; upload targets do not own state.
//
// A log is identified by its path below the vehicle log root plus its size.
// The path alone is not enough: ArduPilot wraps its log numbering and reuses
// file names, and a log that is still being written changes size.
//
// Every public method takes the internal mutex, so the download loop, the
// upload thread and the API server can all call in without further locking.
class LogDatabase
{
public:
	struct UploadState {
		bool uploaded {false};
		bool requested {false};
		// A server said no in a way that retrying cannot fix.
		bool rejected {false};
		// Path the server redirected to after a successful upload, e.g.
		// "/plot_app?log=<uuid>". Relative, so the UI can reach it through
		// whatever host it is talking to rather than the one logloader used.
		std::string location;
		std::string message; // Last thing the server said
	};

	struct Entry {
		int64_t id {0};
		std::string path;
		uint32_t size_bytes {0};
		std::optional<int64_t> time_utc;
		std::string local_path;
		bool downloaded {false};
		bool download_requested {false};
		// False once the log is gone from the vehicle. The row stays: a log we
		// already fetched is still ours, and its upload history still matters.
		bool present {true};
		int download_failures {0};
		std::string last_error;
		int64_t discovered_at {0};
		std::map<std::string, UploadState> uploads;
	};

	// A log as the vehicle listing reports it.
	struct Discovered {
		std::string path;
		uint32_t size_bytes {0};
		std::optional<int64_t> time_utc;
	};

	LogDatabase(const std::string& db_path, const std::string& logs_directory,
		    const std::vector<std::string>& targets);
	~LogDatabase();

	LogDatabase(const LogDatabase&) = delete;
	LogDatabase& operator=(const LogDatabase&) = delete;

	bool ok() const { return _db != nullptr; }

	// Index -------------------------------------------------------------
	// Reconciles the table with a complete vehicle listing: inserts logs not
	// seen before, and marks every row present or absent. Returns the ids of
	// the rows this call created, in newest-first order.
	std::vector<int64_t> sync_index(const std::vector<Discovered>& logs);

	// False until the first complete listing has been reconciled. The
	// difference matters: on a fresh database every log looks new, and
	// queueing them all is exactly what we must not do.
	bool first_index_seen() const;
	void set_first_index_seen();

	// Intent -------------------------------------------------------------
	// Queues a download, and an upload to every target named in targets.
	void request_download(const std::vector<int64_t>& ids, const std::vector<std::string>& targets);
	void request_upload(const std::vector<int64_t>& ids, const std::vector<std::string>& targets);
	// Clears pending requests. Work already done is left alone.
	void cancel_requests(const std::vector<int64_t>& ids);

	// Work ---------------------------------------------------------------
	// Queued, still on the vehicle, fewest failures first so one log that
	// cannot be fetched does not block the queue behind it.
	std::vector<Entry> logs_to_download() const;
	std::vector<Entry> logs_to_upload(const std::string& target) const;

	void mark_downloaded(int64_t id, const std::string& local_path);
	void record_download_failure(int64_t id, const std::string& error);
	void mark_uploaded(int64_t id, const std::string& target, const std::string& location);
	void mark_upload_rejected(int64_t id, const std::string& target, const std::string& message);
	void record_upload_failure(int64_t id, const std::string& target, const std::string& message);

	// Forgets the downloaded file (the caller removes it from disk).
	void clear_local_file(int64_t id);

	// Queries -------------------------------------------------------------
	std::vector<Entry> all_logs() const;
	std::optional<Entry> log_by_id(int64_t id) const;
	// The most recent log still on the vehicle. Pass a non-empty among to
	// restrict the answer to those ids.
	std::optional<int64_t> newest_log_id(const std::vector<int64_t>& among = {}) const;
	bool local_path_in_use(const std::string& local_path, int64_t excluding_id) const;

private:
	bool initialize(const std::string& db_path);
	bool create_schema();
	// Folds the pre-overhaul per-server databases into this one.
	void import_legacy_databases(const std::string& db_path);
	void import_legacy_database(const std::string& file, const std::string& target);
	// Claims the state of a matching pre-FTP row, which was keyed on a
	// timestamp that MAVLink FTP cannot reproduce, hence the match by size.
	void grandfather(int64_t id, const Discovered& log);

	void attach_uploads(std::vector<Entry>& entries) const;
	void ensure_upload_rows(int64_t id);

	mutable std::mutex _mutex;
	sqlite3* _db {nullptr};
	std::string _logs_directory;
	std::vector<std::string> _targets;
	bool _has_legacy_rows {false};
};
