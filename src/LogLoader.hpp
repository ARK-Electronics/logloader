#pragma once

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/telemetry/telemetry.h>

#include "Config.hpp"
#include "FtpLogFetcher.hpp"
#include "LogDatabase.hpp"
#include "StatusBoard.hpp"
#include "UploadTarget.hpp"
#include "Waiter.hpp"

// Ties the pieces together: what the vehicle has (FtpLogFetcher), what we mean
// to do about it (LogDatabase), and where finished logs go (UploadTarget).
//
// Two loops run concurrently. The index loop talks to the vehicle and owns the
// downloads; the upload loop talks to the servers. They share nothing but the
// database, which is its own monitor.
class LogLoader
{
public:
	explicit LogLoader(const Config& config);
	~LogLoader();

	LogLoader(const LogLoader&) = delete;
	LogLoader& operator=(const LogLoader&) = delete;

	bool database_ok() const { return _database.ok(); }

	// Blocks until an autopilot answers or stop() is called.
	bool connect();

	// Runs until stop(). Returns when both loops have finished.
	void run();
	void stop();

	// The API server reaches the same state the loops do.
	LogDatabase& database() { return _database; }
	StatusBoard& status() { return *_status; }

	// Nudges both loops, so a request made through the API is acted on now
	// rather than at the end of the current interval.
	void wake();

	// Removes a downloaded file from disk and forgets it. Returns false when
	// the log is unknown.
	bool delete_local_file(int64_t id);

	std::vector<std::string> enabled_target_names() const;

private:
	void index_loop();
	void upload_loop();

	// Reconciles the vehicle listing into the database. False when the vehicle
	// could not be listed, in which case nothing is assumed about what it has.
	bool refresh_index(std::vector<int64_t>& new_ids, size_t& stable_count);

	// Decides what, if anything, to fetch without being asked. This is the
	// guard against a freshly-installed companion pulling a whole SD card.
	void apply_auto_policy(const std::vector<int64_t>& new_ids, size_t stable_count);

	void download_pending();
	bool download(const LogDatabase::Entry& entry, const FtpLogFetcher::RemoteLog& remote);
	std::string local_path_for(const LogDatabase::Entry& entry);

	void upload_pending(UploadTarget& target);

	bool armed();

	Config _config;
	LogDatabase _database;
	// Shared, not owned by value: the download progress callback belongs to
	// MAVSDK and can fire after this object has begun tearing down.
	std::shared_ptr<StatusBoard> _status {std::make_shared<StatusBoard>()};
	std::vector<std::unique_ptr<UploadTarget>> _targets;

	std::shared_ptr<mavsdk::Mavsdk> _mavsdk;
	std::shared_ptr<mavsdk::Telemetry> _telemetry;
	std::shared_ptr<FtpLogFetcher> _ftp;

	Waiter _index_waiter;
	Waiter _upload_waiter;
	std::thread _upload_thread;

	bool _was_armed {false};
	// Index more often for a few passes after landing, so the log the flight
	// just produced is confirmed stable and fetched promptly.
	int _fast_index_passes {0};
};
