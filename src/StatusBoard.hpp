#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

// What logloader is doing right now, shared between the worker loops that write
// it and the API server that streams it out.
//
// Every change bumps a revision, so a reader can block until something actually
// happened instead of polling. Progress during a transfer is the one thing the
// database does not hold: it changes many times a second and is worthless after
// a restart.
class StatusBoard
{
public:
	struct Snapshot {
		bool connected {false};
		bool armed {false};
		bool ftp_available {false};
		std::string log_root;

		// 0 when nothing is being transferred.
		int64_t downloading_id {0};
		uint32_t downloaded_bytes {0};
		uint32_t download_total_bytes {0};

		int64_t uploading_id {0};
		std::string uploading_target;

		// Bumped on every change; a reader compares against what it last saw.
		uint64_t revision {0};
	};

	Snapshot get() const;

	// Applies mutate under the lock, then bumps the revision and wakes readers.
	void update(const std::function<void(Snapshot&)>& mutate);

	// Blocks until the revision moves past known_revision or the timeout
	// expires, then returns the current snapshot either way.
	Snapshot wait_for_change(uint64_t known_revision, std::chrono::milliseconds timeout) const;

	// Releases every waiter so the API server's streams can close on shutdown.
	void shutdown();
	bool stopped() const;

private:
	mutable std::mutex _mutex;
	mutable std::condition_variable _cv;
	Snapshot _snapshot;
	bool _stopped {false};
};
