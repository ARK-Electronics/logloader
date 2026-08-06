#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

// What logloader is doing right now, shared between the worker loops that write
// it and the API server that streams it out.
//
// Progress during a transfer is the one thing the database does not hold: it
// changes several times a second and is worthless after a restart.
//
// Every change bumps a revision, so a reader can block until something actually
// happened instead of polling.
class StatusBoard
{
public:
	struct Snapshot {
		bool connected {false};
		bool armed {false};
		// The vehicle's logger is writing right now (PX4 v1.16+ report it).
		bool logging {false};
		bool ftp_available {false};
		std::string log_root;

		// 0 when nothing is being transferred.
		int64_t downloading_id {0};
		uint32_t downloaded_bytes {0};
		uint32_t download_total_bytes {0};

		int64_t uploading_id {0};
		std::string uploading_target;

		uint64_t revision {0};
	};

	Snapshot get() const;

	void set_connected(bool connected);
	void set_armed(bool armed);
	void set_logging(bool logging);
	void set_ftp(bool available, const std::string& log_root);
	void set_download(int64_t id, uint32_t transferred, uint32_t total);
	void set_upload(int64_t id, const std::string& target);

	// Wakes readers without changing anything, for a caller that has just
	// written to the database and wants the change streamed out now.
	void notify();

	// Blocks until the revision moves past known_revision or the timeout
	// expires, then returns the current snapshot either way.
	Snapshot wait_for_change(uint64_t known_revision, std::chrono::milliseconds timeout) const;

	// Releases every waiter so the API server's streams can close on shutdown.
	void shutdown();
	bool stopped() const;

private:
	void bump();

	mutable std::mutex _mutex;
	mutable std::condition_variable _cv;
	Snapshot _snapshot;
	bool _stopped {false};
};
