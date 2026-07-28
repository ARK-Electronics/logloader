#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Log.hpp"

// Names used for the two upload targets, in the database and in the API.
constexpr const char* kTargetLocal = "local";
constexpr const char* kTargetRemote = "remote";

struct UploadTargetConfig {
	std::string name;
	bool enabled {false};
	std::string url;
	std::string email;
	// Per-account key for authenticated Flight Review instances. Empty means
	// no auth headers are sent at all, which is what open servers expect.
	std::string api_key;
	bool public_logs {false};
};

struct Config {
	std::string connection_url;
	logging::Level log_level {logging::Level::Info};

	// Absolute, with a trailing slash.
	std::string data_directory;
	std::string logs_directory;

	bool api_enabled {true};
	std::string api_bind {"127.0.0.1"};
	uint16_t api_port {3005};

	// Queue a log for download as soon as it appears in a listing. This is what
	// picks up the log a flight just produced.
	bool auto_download {true};
	// On a database that has never been indexed, queue only the newest log
	// rather than the vehicle's entire history. See LogLoader::apply_auto_policy.
	bool download_latest_on_first_start {true};
	// Upload whatever was queued for download, to every enabled target.
	bool auto_upload {true};
	// If a single listing turns up more new logs than this, the vehicle is not
	// handing us one flight's worth — it is a card we have never seen. Queue
	// only the newest and leave the rest to the operator. 0 disables the guard.
	int max_auto_queue {5};

	int index_interval_s {30};
	int upload_interval_s {10};

	// Empty probes the known vehicle log directories.
	std::string remote_log_directory;
	bool use_burst {true};

	UploadTargetConfig local;
	UploadTargetConfig remote;

	std::vector<const UploadTargetConfig*> targets() const { return {&local, &remote}; }
};

// --config <path> / --config=<path>, else the user override, else the
// deb-installed default.
std::string resolve_config_path(int argc, char** argv);

// Throws std::runtime_error when the file cannot be parsed.
Config load_config(const std::string& path);
