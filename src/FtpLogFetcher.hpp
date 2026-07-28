#pragma once

#include <mavsdk/system.h>
#include <mavsdk/plugins/ftp/ftp.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "FtpListClient.hpp"

// Indexes and downloads flight logs over MAVLink FTP.
//
// FILE_TRANSFER_PROTOCOL (msg 110) is addressed, and both PX4 and ArduPilot
// send replies back to the requesting sysid/compid, so everything here is
// unicast between the vehicle and logloader. The classic log protocol's
// LOG_ENTRY / LOG_DATA (msgs 118/120) carry no target fields, which forces a
// router in between to copy them to every endpoint it serves; logloader does
// not use it at all.
//
// The vehicle log directory is probed at "@MAV_LOG" (the virtual directory the
// MAVLink FTP spec defines for exactly this), then at the physical locations
// used by PX4 and ArduPilot.
class FtpLogFetcher
{
public:
	struct RemoteLog {
		// Path below the log root. The root can change (e.g. a firmware update
		// that adds @MAV_LOG support), the relative path cannot, so identity is
		// derived from this plus the size.
		std::string relative_path;
		uint32_t size_bytes {0};
		// Modification time when the vehicle supports ListDirectoryWithTime,
		// otherwise the start time PX4 encodes in the path, otherwise unknown.
		std::optional<int64_t> time_utc;
		// False while the file is still growing, i.e. the log the vehicle is
		// writing right now. See refresh().
		bool stable {false};
	};

	struct Settings {
		std::string temp_directory;   // Scratch space for in-progress transfers
		std::string remote_directory; // Empty probes the known locations
		bool use_burst {true};
	};

	using ProgressCallback = std::function<void(uint32_t bytes_transferred, uint32_t total_bytes)>;

	FtpLogFetcher(std::shared_ptr<mavsdk::System> system, const Settings& settings);

	// Closes any FTP session a previous run left open on the vehicle
	void reset_sessions();

	// (Re)indexes the vehicle log directory, probing for it first when it is
	// not known yet. Returns true only when a complete index was built, so a
	// missing log can be trusted to actually be gone from the vehicle.
	//
	// A log is marked stable once two consecutive listings agree on its size.
	// The log the vehicle is currently writing keeps growing, and downloading
	// it would only produce a file that no longer matches what was listed.
	bool refresh();

	bool available() const { return !_root.empty(); }
	const std::string& root() const { return _root; }
	const std::vector<RemoteLog>& logs() const { return _logs; }

	// Downloads a log into local_path, staging the transfer in temp_directory
	// so partial files never appear next to finished ones. The size reported by
	// the directory listing must match or the download is discarded.
	bool download(const RemoteLog& log, const std::string& local_path, const ProgressCallback& progress);

	void stop();

private:
	bool build_index(const std::string& root, std::vector<RemoteLog>& logs);
	void publish_index(std::vector<RemoteLog>&& logs);

	std::shared_ptr<FtpListClient> _list_client;
	std::shared_ptr<mavsdk::Ftp> _ftp;
	Settings _settings;

	std::string _root;
	std::vector<RemoteLog> _logs;

	// Sizes from the previous listing, keyed by relative path
	std::map<std::string, uint32_t> _previous_sizes;

	bool _reported_unavailable {false};

	std::atomic<bool> _should_exit {false};
};
