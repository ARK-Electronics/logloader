#pragma once

#include <mavsdk/system.h>
#include <mavsdk/plugins/ftp/ftp.h>
#include <mavsdk/plugins/log_files/log_files.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Downloads flight logs over MAVLink FTP instead of the classic log protocol.
//
// LOG_DATA (msg 120) carries no target_system/target_component, so a router such as
// mavlink-router has to treat it as a broadcast and copies every chunk to every endpoint
// it serves -- including the telemetry radio. FILE_TRANSFER_PROTOCOL (msg 110) is
// addressed, and both PX4 and ArduPilot send the reply back to the requesting
// sysid/compid, so the bulk transfer is unicast to us and the other endpoints stay quiet.
//
// The log list still comes from LOG_ENTRY (msg 118). That is also untargeted, but it is
// a handful of bytes per log rather than megabytes, and it is the only source of the
// timestamp we key the databases on.
class FtpLogFetcher
{
public:
	enum class Flavor {
		Unknown,
		PX4,
		ArduPilot
	};

	struct Settings {
		std::string temp_directory;   // Scratch space for in-progress transfers
		std::string remote_directory; // Empty means probe for it
		bool use_burst {true};
	};

	using ProgressCallback = std::function<void(uint32_t bytes_transferred, uint32_t total_bytes)>;

	FtpLogFetcher(std::shared_ptr<mavsdk::System> system, const Settings& settings);

	// Probes the vehicle for a log directory and builds the initial index.
	// Call once after connecting, before any download is attempted.
	bool detect();

	// Rebuilds the remote file index and forgets previous path claims.
	bool refresh();

	bool available() const { return _flavor != Flavor::Unknown; }
	Flavor flavor() const { return _flavor; }
	const std::string& remote_directory() const { return _remote_directory; }
	std::string flavor_name() const;
	std::string log_extension() const;

	// Maps a log entry onto a remote path. Returns an empty string when the entry cannot
	// be matched, in which case the caller should fall back to the LOG_DATA protocol.
	std::string resolve(const mavsdk::LogFiles::Entry& entry);

	// Downloads remote_path and moves it onto local_path once the size checks out.
	bool download(const std::string& remote_path, const std::string& local_path, uint32_t expected_size,
		      const ProgressCallback& progress);

	// Hands a path back to the index after a failed download so a retry can pick it up.
	void release(const std::string& remote_path);

	void stop();

private:
	struct RemoteFile {
		std::string path;
		std::optional<uint32_t> size_bytes; // MAVSDK >= 3 does not report sizes
		std::optional<int64_t> start_time;  // PX4: parsed out of the path
		std::optional<uint32_t> log_number; // ArduPilot: parsed out of the file name
		bool claimed {false};
	};

	bool build_index(const std::string& root, const std::string& extension, Flavor flavor);
	bool list_recursive(const std::string& directory, int depth_remaining, const std::string& extension);
	void build_ardupilot_order(const std::string& root);

	RemoteFile* match_by_size(uint32_t size_bytes);
	RemoteFile* match_px4(const mavsdk::LogFiles::Entry& entry);
	RemoteFile* match_ardupilot(const mavsdk::LogFiles::Entry& entry);

	std::optional<uint32_t> read_last_log_number(const std::string& root);

	std::shared_ptr<mavsdk::Ftp> _ftp;
	Settings _settings;

	Flavor _flavor {Flavor::Unknown};
	std::string _remote_directory;
	std::string _extension;

	std::vector<RemoteFile> _index;
	std::vector<size_t> _ardupilot_order; // Index positions, oldest log first

	std::atomic<bool> _should_exit {false};
};
