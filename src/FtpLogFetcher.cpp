#include "FtpLogFetcher.hpp"
#include "Log.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <future>

namespace fs = std::filesystem;

namespace
{

// "@MAV_LOG" is the virtual log directory from the MAVLink FTP specification
// (PX4 >= v1.17). The physical paths cover firmware that predates it.
constexpr const char* kProbeRoots[] = {
	"@MAV_LOG",
	"/fs/microsd/log", // PX4 on hardware
	"/APM/LOGS",       // ArduPilot
	"/log",            // PX4 SITL, and hardware without an SD card mount point
};

bool ends_with_ignore_case(const std::string& text, const std::string& suffix)
{
	if (text.size() < suffix.size()) {
		return false;
	}

	const size_t offset = text.size() - suffix.size();

	for (size_t i = 0; i < suffix.size(); i++) {
		if (std::tolower(static_cast<unsigned char>(text[offset + i]))
		    != std::tolower(static_cast<unsigned char>(suffix[i]))) {
			return false;
		}
	}

	return true;
}

bool is_log_file(const std::string& name)
{
	return ends_with_ignore_case(name, ".ulg") || ends_with_ignore_case(name, ".bin");
}

// PX4 writes logs as <root>/<YYYY-MM-DD>/<HH_MM_SS>.ulg. The parsed time is the
// log start; used when the vehicle cannot report modification times.
std::optional<int64_t> parse_px4_start_time(const std::string& directory, const std::string& filename)
{
	const std::string stem = fs::path(filename).stem().string();

	if (directory.size() != 10 || stem.size() != 8) {
		return std::nullopt;
	}

	std::tm tm {};

	if (sscanf(directory.c_str(), "%4d-%2d-%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) {
		return std::nullopt;
	}

	if (sscanf(stem.c_str(), "%2d_%2d_%2d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 3) {
		return std::nullopt;
	}

	tm.tm_year -= 1900;
	tm.tm_mon -= 1;

	return static_cast<int64_t>(timegm(&tm));
}

std::string join_path(const std::string& directory, const std::string& name)
{
	if (!directory.empty() && directory.back() == '/') {
		return directory + name;
	}

	return directory + "/" + name;
}

} // namespace

FtpLogFetcher::FtpLogFetcher(std::shared_ptr<mavsdk::System> system, const FtpLogFetcher::Settings& settings)
	: _list_client(std::make_shared<FtpListClient>(system))
	, _ftp(std::make_shared<mavsdk::Ftp>(system))
	, _settings(settings)
{
	// A run killed mid-transfer leaves a partial file behind. Nothing here
	// resumes, so the staging directory starts empty every time.
	std::error_code ec;
	fs::remove_all(_settings.temp_directory, ec);
	fs::create_directories(_settings.temp_directory, ec);
}

void FtpLogFetcher::stop()
{
	_should_exit = true;
	_list_client->stop();
}

void FtpLogFetcher::reset_sessions()
{
	_list_client->reset_sessions();
}

bool FtpLogFetcher::refresh()
{
	if (_root.empty()) {
		// The override, when set, is the only candidate
		std::vector<std::string> roots;

		if (_settings.remote_directory.empty()) {
			roots.assign(std::begin(kProbeRoots), std::end(kProbeRoots));

		} else {
			roots.push_back(_settings.remote_directory);
		}

		for (const auto& root : roots) {
			std::vector<RemoteLog> logs;

			if (build_index(root, logs)) {
				_root = root;
				publish_index(std::move(logs));
				_reported_unavailable = false;
				LOG("MAVLink FTP available, indexed " << _logs.size() << " logs in " << _root);
				return true;
			}
		}

		if (!_reported_unavailable) {
			LOG("Could not index vehicle logs over MAVLink FTP. "
			    "Make sure the autopilot has MAVLink FTP enabled on this link.");
			_reported_unavailable = true;
		}

		return false;
	}

	std::vector<RemoteLog> logs;

	if (build_index(_root, logs)) {
		publish_index(std::move(logs));
		return true;
	}

	// The directory may have moved out from under us (SD card swap, firmware
	// update). Forget the root so the next refresh probes again.
	LOG_DEBUG("Re-indexing " << _root << " failed, will re-probe");
	_root.clear();
	_logs.clear();

	return false;
}

const FtpLogFetcher::RemoteLog* FtpLogFetcher::find(const std::string& relative_path, uint32_t size_bytes) const
{
	for (const auto& log : _logs) {
		if (log.relative_path == relative_path && log.size_bytes == size_bytes) {
			return &log;
		}
	}

	return nullptr;
}

void FtpLogFetcher::publish_index(std::vector<RemoteLog>&& logs)
{
	std::map<std::string, uint32_t> sizes;

	for (auto& log : logs) {
		auto previous = _previous_sizes.find(log.relative_path);
		log.stable = previous != _previous_sizes.end() && previous->second == log.size_bytes;
		sizes.emplace(log.relative_path, log.size_bytes);
	}

	_previous_sizes = std::move(sizes);
	_logs = std::move(logs);
}

bool FtpLogFetcher::build_index(const std::string& root, std::vector<RemoteLog>& logs)
{
	std::vector<FtpListClient::Entry> entries;

	if (_list_client->list_directory(root, entries) != FtpListClient::Result::Success) {
		return false;
	}

	std::vector<std::string> subdirectories;

	for (const auto& entry : entries) {
		if (entry.is_directory) {
			if (entry.name != "." && entry.name != "..") {
				subdirectories.push_back(entry.name);
			}

		} else if (is_log_file(entry.name)) {
			RemoteLog log;
			log.relative_path = entry.name;
			log.size_bytes = entry.size_bytes;
			log.time_utc = entry.mtime_utc;
			logs.push_back(log);
		}
	}

	// PX4 nests logs one directory deep (<root>/<date>/<time>.ulg). A partial
	// index would make missing logs look deleted, so any listing failure fails
	// the whole refresh.
	for (const auto& subdirectory : subdirectories) {
		if (_should_exit) {
			return false;
		}

		std::vector<FtpListClient::Entry> sub_entries;

		if (_list_client->list_directory(join_path(root, subdirectory), sub_entries)
		    != FtpListClient::Result::Success) {
			LOG_DEBUG("Listing " << join_path(root, subdirectory) << " failed");
			return false;
		}

		for (const auto& entry : sub_entries) {
			if (entry.is_directory || !is_log_file(entry.name)) {
				continue;
			}

			RemoteLog log;
			log.relative_path = subdirectory + "/" + entry.name;
			log.size_bytes = entry.size_bytes;
			log.time_utc = entry.mtime_utc;

			if (!log.time_utc.has_value()) {
				log.time_utc = parse_px4_start_time(subdirectory, entry.name);
			}

			logs.push_back(log);
		}
	}

	return true;
}

bool FtpLogFetcher::download(const RemoteLog& log, const std::string& local_path,
			     const FtpLogFetcher::ProgressCallback& progress)
{
	std::error_code ec;
	fs::create_directories(_settings.temp_directory, ec);

	const std::string remote_path = join_path(_root, log.relative_path);

	// MAVSDK writes to <local_dir>/<remote basename>, so stage the transfer and
	// move it into place afterwards. A partial file never appears in the logs
	// directory.
	const std::string staged_path = join_path(_settings.temp_directory, fs::path(log.relative_path).filename().string());
	fs::remove(staged_path, ec);

	struct Transfer {
		std::promise<mavsdk::Ftp::Result> promise;
		std::atomic<bool> settled {false};
	};

	// Shared so the callback stays valid if we abandon the wait on shutdown
	auto transfer = std::make_shared<Transfer>();
	auto future_result = transfer->promise.get_future();

	_ftp->download_async(remote_path, _settings.temp_directory, _settings.use_burst,
	[transfer, progress](mavsdk::Ftp::Result result, mavsdk::Ftp::ProgressData data) {
		if (transfer->settled.load()) {
			return;
		}

		if (result == mavsdk::Ftp::Result::Next) {
			if (progress) {
				progress(data.bytes_transferred, data.total_bytes);
			}

			return;
		}

		// The waiter abandons the transfer on shutdown by setting this, so
		// whoever gets there first is the one that settles the promise.
		if (!transfer->settled.exchange(true)) {
			transfer->promise.set_value(result);
		}
	});

	// MAVSDK offers no way to cancel the transfer, so on shutdown the wait is
	// abandoned instead of blocking until MAVSDK times out on its own.
	while (future_result.wait_for(std::chrono::milliseconds(500)) != std::future_status::ready) {
		if (_should_exit) {
			transfer->settled.store(true);
			fs::remove(staged_path, ec);
			return false;
		}
	}

	const auto result = future_result.get();

	if (result != mavsdk::Ftp::Result::Success) {
		LOG("FTP download of " << remote_path << " failed: " << result);
		fs::remove(staged_path, ec);
		return false;
	}

	if (!fs::exists(staged_path, ec)) {
		LOG("FTP reported success but " << staged_path << " is missing");
		return false;
	}

	const uint32_t downloaded_size = static_cast<uint32_t>(fs::file_size(staged_path, ec));

	if (downloaded_size != log.size_bytes) {
		// The log grew or was rotated since the listing; the next refresh will
		// index the current state.
		LOG("Size mismatch for " << remote_path << ": got " << downloaded_size
		    << " bytes, expected " << log.size_bytes);
		fs::remove(staged_path, ec);
		return false;
	}

	fs::remove(local_path, ec);
	ec.clear();
	fs::rename(staged_path, local_path, ec);

	if (ec) {
		// Different filesystems, fall back to a copy
		ec.clear();
		fs::copy_file(staged_path, local_path, fs::copy_options::overwrite_existing, ec);
		std::error_code remove_ec;
		fs::remove(staged_path, remove_ec);

		if (ec) {
			LOG("Failed to move " << staged_path << " to " << local_path << ": " << ec.message());
			return false;
		}
	}

	return true;
}
