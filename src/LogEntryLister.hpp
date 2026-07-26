#pragma once

#include <mavsdk/system.h>
#include <mavsdk/plugins/log_files/log_files.h>
#include <mavsdk/plugins/mavlink_passthrough/mavlink_passthrough.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// Enumerates the logs on the vehicle with LOG_REQUEST_LIST / LOG_ENTRY.
//
// MAVSDK's LogFiles plugin bakes in PX4's zero-based numbering: it discards any LOG_ENTRY
// whose id is not below num_logs, and it reads the collected entries back out at indices
// 0..num_logs-1. ArduPilot numbers list entries from one, so its final entry always trips
// that check and get_entries() reports NoLogfiles. This runs the same exchange without
// assuming where the numbering starts.
//
// Only the list travels this way. The bulk transfer goes over MAVLink FTP, see
// FtpLogFetcher.
class LogEntryLister
{
public:
	explicit LogEntryLister(std::shared_ptr<mavsdk::System> system);
	~LogEntryLister();

	// Blocks until the vehicle has listed every log or we give up on it.
	bool get_entries(std::vector<mavsdk::LogFiles::Entry>& entries);

	void stop();

private:
	void handle_log_entry(const mavlink_message_t& message);
	void request_list(uint16_t start, uint16_t end);
	void request_end();

	// True once every id the vehicle told us to expect has arrived. Caller holds _mutex.
	bool listing_complete() const;

	std::shared_ptr<mavsdk::MavlinkPassthrough> _passthrough;
	mavsdk::MavlinkPassthrough::MessageHandle _subscription;

	mutable std::mutex _mutex;
	std::condition_variable _cv;

	std::map<uint16_t, mavsdk::LogFiles::Entry> _entries;
	uint16_t _num_logs {0};
	uint16_t _last_log_id {0};
	bool _got_any {false};

	std::atomic<bool> _should_exit {false};
};
