#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "LegacyImport.hpp"

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
		// False while the log is still growing. It is on the vehicle either way,
		// but only a log that has stopped changing is worth recording, since its
		// identity includes its size.
		bool stable {true};
	};

	LogDatabase(const std::string& db_path, const std::string& logs_directory,
		    const std::vector<std::string>& targets);
	~LogDatabase();

	LogDatabase(const LogDatabase&) = delete;
	LogDatabase& operator=(const LogDatabase&) = delete;

	bool ok() const { return _ok; }

	struct SyncResult {
		// Rows this call created, in the order the vehicle listed them.
		std::vector<int64_t> inserted;
		// Stable logs in this listing.
		size_t present_count {0};
		// This call reconciled the first listing that had anything in it. On a
		// fresh database every log looks new, and queueing them all is exactly
		// what must not happen.
		bool first_ever {false};
	};

	// Index -------------------------------------------------------------
	// Reconciles the table with a complete vehicle listing: inserts logs not
	// seen before, and marks every row present or absent.
	SyncResult sync_index(const std::vector<Discovered>& logs);

	// Bumped by every write. A reader that has already rendered a given
	// revision knows the log list has not moved since.
	uint64_t revision() const { return _revision.load(std::memory_order_relaxed); }

	// Intent -------------------------------------------------------------
	// Queues these logs: fetched if not already local, then uploaded to each
	// named target. Pass no targets to fetch without uploading.
	void request(const std::vector<int64_t>& ids, const std::vector<std::string>& targets);
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

	// Forgets the downloaded file (the caller removes it from disk). Pass the
	// path the caller believes is there to avoid clobbering a newer download;
	// returns false when the row had moved on.
	bool clear_local_file(int64_t id, const std::string& expected_local_path = {});

	// Queries -------------------------------------------------------------
	std::vector<Entry> all_logs() const;
	std::optional<Entry> log_by_id(int64_t id) const;
	// The most recent log still on the vehicle. Pass a non-empty among to
	// restrict the answer to those ids.
	std::optional<int64_t> newest_log_id(const std::vector<int64_t>& among = {}) const;
	bool local_path_in_use(const std::string& local_path, int64_t excluding_id) const;

	// Backing for the API's "everything" requests, as queries rather than a
	// full materialisation of the table filtered in the caller.
	std::vector<int64_t> ids_not_downloaded() const;
	std::vector<int64_t> ids_not_uploaded(const std::vector<std::string>& targets) const;

private:
	bool initialize(const std::string& db_path);
	bool create_schema();
	// A column added after a database was created has to be added explicitly:
	// CREATE TABLE IF NOT EXISTS is a no-op on a table that is already there.
	bool migrate_schema();
	void attach_uploads(std::vector<Entry>& entries) const;
	void ensure_upload_rows(int64_t id);

	mutable std::mutex _mutex;
	std::atomic<uint64_t> _revision {0};
	sqlite3* _db {nullptr};
	// Opening the file is not enough: the schema still has to exist, and a
	// read-only or full data directory fails only at CREATE TABLE.
	bool _ok {false};
	std::vector<std::string> _targets;
	std::unique_ptr<legacy::Importer> _legacy;
};
