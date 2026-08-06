#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/mavlink_passthrough/mavlink_passthrough.h>
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
//
// The vehicle is never polled. The index loop sits idle until an event says the
// listing may have changed: the connection to the autopilot coming (back) up, a
// flight or logging session ending, or a request through the API.
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
	// rather than left for the next trigger.
	void wake();

	// Lists the vehicle again as soon as it is connected and disarmed. This is
	// the API's refresh, and the only way the index moves without an event.
	void request_refresh();

	// Removes a downloaded file from disk and forgets it. Returns false when
	// the log is unknown.
	bool delete_local_file(int64_t id);

	std::vector<std::string> enabled_target_names() const;
	// Name and base url of each enabled target. The url matters because an
	// upload records only the relative path the server redirected to.
	std::vector<std::pair<std::string, std::string>> enabled_targets() const;

private:
	void index_loop();
	void upload_loop();

	// Installs the armed, SYS_STATUS, heartbeat and connection-state callbacks
	// that drive the index loop. They run on MAVSDK's threads and only touch
	// members that outlive the plugins: atomics, the status board, the waiters.
	void install_subscriptions(std::shared_ptr<mavsdk::System> system);

	// Called when armed or logging changed. The active -> inactive edge is what
	// schedules the post-flight check for a new log.
	void on_activity_changed();

	// Asks the index loop for at least this many listing passes. More than one,
	// because a log is only trusted once two consecutive listings agree on its
	// size.
	void trigger_index(int passes);

	// Reconciles the vehicle listing into the database. False when the vehicle
	// could not be listed, in which case nothing is assumed about what it has.
	std::optional<LogDatabase::SyncResult> refresh_index();

	// Decides what, if anything, to fetch without being asked. This is the
	// guard against a freshly-installed companion pulling a whole SD card.
	void apply_auto_policy(const LogDatabase::SyncResult& sync);

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
	std::shared_ptr<mavsdk::MavlinkPassthrough> _passthrough;
	// Assigned by connect() on the main thread and read by stop() on the signal
	// thread, so it is not just a plain member.
	std::mutex _ftp_mutex;
	std::shared_ptr<FtpLogFetcher> _ftp;

	Waiter _index_waiter;
	Waiter _upload_waiter;
	std::thread _upload_thread;

	// Written by MAVSDK callbacks, read by the loops.
	std::atomic<bool> _connected {false};
	std::atomic<bool> _armed {false};
	std::atomic<bool> _logging {false};
	// Only PX4's logging bit is trusted; see install_subscriptions.
	std::atomic<bool> _is_px4 {false};

	// armed || logging, kept so either callback can see the combined edge.
	std::atomic<bool> _activity_active {false};
	// The flight (or logging session) just ended; check for its log after
	// giving the logger a moment to close the file.
	std::atomic<bool> _activity_settled {false};
	// The autopilot came back; its FTP session state is unknown and the log it
	// was writing when it went away is finished now.
	std::atomic<bool> _reconnected {false};
	// Listing passes the index loop still owes. Consecutive passes are what
	// prove a log's size has stopped changing.
	std::atomic<int> _index_passes {0};
};
