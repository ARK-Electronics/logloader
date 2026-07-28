#include "LogLoader.hpp"
#include "Log.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include <mavsdk/log_callback.h>

namespace fs = std::filesystem;

namespace
{

// How many logs in a row may fail transiently before the batch is abandoned for
// this cycle. Enough to skip past a bad file, few enough not to hammer a server
// that is having a bad day.
constexpr int kMaxConsecutiveUploadRetries = 3;

constexpr auto kFastIndexInterval = std::chrono::seconds(5);

std::string human_size(uint32_t bytes)
{
	std::ostringstream out;
	out << std::fixed << std::setprecision(1) << bytes / 1e6 << " MB";
	return out.str();
}

} // namespace

LogLoader::LogLoader(const Config& config)
	: _config(config)
	, _database(config.data_directory + "logloader.db", config.logs_directory,
		    std::vector<std::string> {kTargetLocal, kTargetRemote})
{
	// MAVSDK's own logging is noisy and duplicates what we report ourselves.
	mavsdk::log::subscribe([](...) { return true; });

	std::error_code ec;
	fs::create_directories(_config.logs_directory, ec);

	for (const auto* target : _config.targets()) {
		_targets.push_back(std::make_unique<UploadTarget>(*target));
	}
}

LogLoader::~LogLoader()
{
	stop();

	if (_upload_thread.joinable()) {
		_upload_thread.join();
	}

	// Drop the MAVSDK plugins while everything they might call back into is
	// still alive. Member destruction alone would do this in the right order,
	// but only by accident of declaration order.
	_ftp.reset();
	_telemetry.reset();
	_mavsdk.reset();
}

std::vector<std::string> LogLoader::enabled_target_names() const
{
	std::vector<std::string> names;

	for (const auto& target : _targets) {
		if (target->enabled()) {
			names.push_back(target->name());
		}
	}

	return names;
}

void LogLoader::stop()
{
	_index_waiter.stop();
	_upload_waiter.stop();
	_status->shutdown();

	if (_ftp) {
		_ftp->stop();
	}
}

void LogLoader::wake()
{
	_index_waiter.wake();
	_upload_waiter.wake();
}

bool LogLoader::armed()
{
	return _telemetry && _telemetry->armed();
}

bool LogLoader::connect()
{
	while (!_index_waiter.stopped()) {
		LOG("Connecting to " << _config.connection_url);

		_mavsdk = std::make_shared<mavsdk::Mavsdk>(
				  mavsdk::Mavsdk::Configuration(1, MAV_COMP_ID_ONBOARD_COMPUTER, true));

		if (_mavsdk->add_any_connection(_config.connection_url) != mavsdk::ConnectionResult::Success) {
			LOG_ERROR("Could not open " << _config.connection_url);
			_mavsdk.reset();

			if (_index_waiter.wait(std::chrono::seconds(5))) {
				return false;
			}

			continue;
		}

		// Seconds, despite what the MAVSDK parameter name suggests.
		auto system = _mavsdk->first_autopilot(3.0);

		if (!system) {
			// Drop the connection before retrying: holding the UDP port open
			// while rebinding it is how "address in use" happens.
			_mavsdk.reset();
			continue;
		}

		LOG("Connected to the autopilot");

		_telemetry = std::make_shared<mavsdk::Telemetry>(system.value());

		FtpLogFetcher::Settings settings = {
			.temp_directory = _config.data_directory + "tmp/",
			.remote_directory = _config.remote_log_directory,
			.use_burst = _config.use_burst,
		};
		_ftp = std::make_shared<FtpLogFetcher>(system.value(), settings);

		// PX4 serves one FTP session at a time, so one left open by a run that
		// was killed mid-transfer blocks every transfer until the vehicle
		// reboots.
		_ftp->reset_sessions();

		_status->update([](StatusBoard::Snapshot & snapshot) { snapshot.connected = true; });
		return true;
	}

	return false;
}

void LogLoader::run()
{
	_upload_thread = std::thread(&LogLoader::upload_loop, this);
	index_loop();

	_upload_waiter.stop();

	if (_upload_thread.joinable()) {
		_upload_thread.join();
	}
}

// -------------------------------------------------------------------------
// Index loop: everything that talks to the vehicle
// -------------------------------------------------------------------------

void LogLoader::index_loop()
{
	while (!_index_waiter.stopped()) {
		const bool is_armed = armed();

		if (is_armed != _was_armed) {
			_was_armed = is_armed;
			_status->update([is_armed](StatusBoard::Snapshot & snapshot) { snapshot.armed = is_armed; });

			if (!is_armed) {
				// The logger needs a moment to close the file it was writing.
				LOG("Vehicle disarmed, looking for a new log");
				_fast_index_passes = 3;

				if (_index_waiter.wait(std::chrono::seconds(3))) {
					break;
				}
			}
		}

		if (is_armed) {
			// Downloading competes with the logger for the SD card.
			if (_index_waiter.wait(std::chrono::seconds(1))) {
				break;
			}

			continue;
		}

		std::vector<int64_t> new_ids;
		size_t stable_count = 0;

		if (refresh_index(new_ids, stable_count)) {
			apply_auto_policy(new_ids, stable_count);
			download_pending();
		}

		const auto interval = _fast_index_passes > 0
				      ? kFastIndexInterval
				      : std::chrono::seconds(_config.index_interval_s);

		if (_fast_index_passes > 0) {
			_fast_index_passes--;
		}

		if (_index_waiter.wait(interval)) {
			break;
		}
	}

	LOG_DEBUG("Index loop finished");
}

bool LogLoader::refresh_index(std::vector<int64_t>& new_ids, size_t& stable_count)
{
	if (!_ftp->refresh()) {
		_status->update([](StatusBoard::Snapshot & snapshot) { snapshot.ftp_available = false; });
		return false;
	}

	std::vector<LogDatabase::Discovered> discovered;
	size_t growing = 0;

	for (const auto& log : _ftp->logs()) {
		if (!log.stable) {
			// Almost certainly the log being written right now. Recording it
			// would enter it into the queue at a size it will not keep.
			growing++;
			continue;
		}

		discovered.push_back({log.relative_path, log.size_bytes, log.time_utc});
	}

	stable_count = discovered.size();
	new_ids = _database.sync_index(discovered);

	const std::string root = _ftp->root();
	_status->update([&root](StatusBoard::Snapshot & snapshot) {
		snapshot.ftp_available = true;
		snapshot.log_root = root;
	});

	LOG_DEBUG("Indexed " << discovered.size() << " logs in " << root
		  << (growing > 0 ? " (" + std::to_string(growing) + " still being written)" : ""));

	return true;
}

void LogLoader::apply_auto_policy(const std::vector<int64_t>& new_ids, size_t stable_count)
{
	std::vector<std::string> upload_to;

	if (_config.auto_upload) {
		upload_to = enabled_target_names();
	}

	// A database that has never been reconciled cannot tell a log produced by
	// the flight that just happened from one that has been on the card for a
	// year. Everything looks new, so almost nothing is fetched.
	if (!_database.first_index_seen()) {
		// A log only counts once two listings agree on its size, so the very
		// first listing after startup legitimately reconciles nothing. Calling
		// that the first index would make the next one -- the vehicle's entire
		// history -- look like logs that appeared while we were watching, which
		// is precisely the bulk download this policy exists to prevent.
		if (stable_count == 0) {
			return;
		}

		_database.set_first_index_seen();

		if (!_config.download_latest_on_first_start) {
			LOG("First index: " << stable_count
			    << " logs on the vehicle, none queued (download.latest_on_first_start is off)");
			return;
		}

		const auto newest = _database.newest_log_id();

		if (!newest.has_value()) {
			return;
		}

		_database.request_download({newest.value()}, upload_to);
		LOG("First index: " << stable_count << " logs on the vehicle, queueing only the newest. "
		    "Use the ARK-OS Logs page to fetch any of the others.");
		return;
	}

	if (!_config.auto_download || new_ids.empty()) {
		return;
	}

	// More than a flight's worth appearing at once means the vehicle is showing
	// us a card we have not seen, not that it flew ten times in 30 seconds.
	if (_config.max_auto_queue > 0 && static_cast<int>(new_ids.size()) > _config.max_auto_queue) {
		const auto newest = _database.newest_log_id(new_ids);

		if (newest.has_value()) {
			_database.request_download({newest.value()}, upload_to);
		}

		LOG_WARN(new_ids.size() << " new logs appeared at once, which looks like a different SD card. "
			 "Queued only the newest; use the ARK-OS Logs page for the rest.");
		return;
	}

	_database.request_download(new_ids, upload_to);
	LOG("Queued " << new_ids.size() << (new_ids.size() == 1 ? " new log" : " new logs"));
}

// -------------------------------------------------------------------------
// Download
// -------------------------------------------------------------------------

void LogLoader::download_pending()
{
	const auto pending = _database.logs_to_download();

	if (pending.empty()) {
		return;
	}

	size_t index = 0;

	for (const auto& entry : pending) {
		if (_index_waiter.stopped()) {
			return;
		}

		if (armed()) {
			LOG("Vehicle armed, pausing downloads");
			return;
		}

		index++;

		const FtpLogFetcher::RemoteLog* remote = _ftp->find(entry.path, entry.size_bytes);

		if (remote == nullptr) {
			// sync_index marks anything the listing omits absent, and the
			// queue only offers present logs, so this is a race with a
			// listing rather than a missing file.
			LOG_DEBUG(entry.path << " is no longer in the index, skipping");
			continue;
		}

		LOG("Downloading " << index << "/" << pending.size() << ": " << entry.path
		    << " (" << human_size(entry.size_bytes) << ")");

		if (download(entry, *remote)) {
			_upload_waiter.wake();

		} else {
			_database.record_download_failure(entry.id, "download failed");
		}
	}
}

bool LogLoader::download(const LogDatabase::Entry& entry, const FtpLogFetcher::RemoteLog& remote)
{
	const std::string local_path = local_path_for(entry);
	const auto started = std::chrono::steady_clock::now();

	_status->update([&entry](StatusBoard::Snapshot & snapshot) {
		snapshot.downloading_id = entry.id;
		snapshot.downloaded_bytes = 0;
		snapshot.download_total_bytes = entry.size_bytes;
	});

	// MAVSDK reports progress per chunk, which is far more often than anyone
	// needs to be told about it.
	auto last_published = std::make_shared<std::chrono::steady_clock::time_point>(started);

	auto progress = [status = _status, last_published](uint32_t transferred, uint32_t total) {
		const auto now = std::chrono::steady_clock::now();

		if (now - *last_published < std::chrono::milliseconds(500)) {
			return;
		}

		*last_published = now;
		status->update([transferred, total](StatusBoard::Snapshot & snapshot) {
			snapshot.downloaded_bytes = transferred;
			snapshot.download_total_bytes = total;
		});
	};

	const bool ok = _ftp->download(remote, local_path, progress);

	_status->update([](StatusBoard::Snapshot & snapshot) {
		snapshot.downloading_id = 0;
		snapshot.downloaded_bytes = 0;
		snapshot.download_total_bytes = 0;
	});

	if (!ok) {
		return false;
	}

	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	std::ostringstream rate;
	rate << std::fixed << std::setprecision(1) << (seconds > 0 ? entry.size_bytes / seconds / 1e3 : 0.);
	LOG("Downloaded " << entry.path << " in " << std::fixed << std::setprecision(1) << seconds
	    << "s (" << rate.str() << " kB/s)");

	_database.mark_downloaded(entry.id, local_path);
	return true;
}

std::string LogLoader::local_path_for(const LogDatabase::Entry& entry)
{
	std::string name = entry.path;
	std::replace(name.begin(), name.end(), '/', '_');

	const std::string path = _config.logs_directory + name;

	// ArduPilot reuses log file names once its numbering wraps at
	// LOG_MAX_FILES, so the name may already belong to a log we still hold.
	if (_database.local_path_in_use(path, entry.id)) {
		return _config.logs_directory + std::to_string(entry.id) + "_" + name;
	}

	return path;
}

bool LogLoader::delete_local_file(int64_t id)
{
	const auto entry = _database.log_by_id(id);

	if (!entry.has_value()) {
		return false;
	}

	if (!entry->local_path.empty()) {
		std::error_code ec;
		fs::remove(entry->local_path, ec);

		if (ec) {
			LOG_WARN("Could not remove " << entry->local_path << ": " << ec.message());
		}
	}

	_database.clear_local_file(id);
	return true;
}

// -------------------------------------------------------------------------
// Upload loop
// -------------------------------------------------------------------------

void LogLoader::upload_loop()
{
	while (!_upload_waiter.stopped()) {
		for (auto& target : _targets) {
			if (_upload_waiter.stopped()) {
				break;
			}

			if (target->enabled()) {
				upload_pending(*target);
			}
		}

		if (_upload_waiter.wait(std::chrono::seconds(_config.upload_interval_s))) {
			break;
		}
	}

	LOG_DEBUG("Upload loop finished");
}

void LogLoader::upload_pending(UploadTarget& target)
{
	const auto pending = _database.logs_to_upload(target.name());

	if (pending.empty()) {
		return;
	}

	LOG_DEBUG("Uploading " << pending.size() << " logs to " << target.name());

	int consecutive_retries = 0;

	for (const auto& entry : pending) {
		if (_upload_waiter.stopped() || armed()) {
			return;
		}

		_status->update([&entry, &target](StatusBoard::Snapshot & snapshot) {
			snapshot.uploading_id = entry.id;
			snapshot.uploading_target = target.name();
		});

		const UploadTarget::Result result = target.upload(entry.local_path);

		_status->update([](StatusBoard::Snapshot & snapshot) {
			snapshot.uploading_id = 0;
			snapshot.uploading_target.clear();
		});

		switch (result.outcome) {
		case UploadTarget::Outcome::Success:
			LOG("Uploaded " << entry.path << " to " << target.name()
			    << (result.location.empty() ? "" : ": " + target.url() + result.location));
			_database.mark_uploaded(entry.id, target.name(), result.location);
			consecutive_retries = 0;
			break;

		case UploadTarget::Outcome::Rejected:
			// Recorded so it is not offered again; the rest of the queue is
			// unaffected by one log the server will not take.
			LOG_WARN(target.name() << " rejected " << entry.path << ": " << result.message);
			_database.mark_upload_rejected(entry.id, target.name(), result.message);
			consecutive_retries = 0;
			break;

		case UploadTarget::Outcome::Unauthorized:
			LOG_WARN(target.name() << " will not accept uploads from this account ("
				 << result.status_code << "): " << result.message);
			LOG_WARN("Pausing " << target.name() << " uploads for this cycle; they resume "
				 "on their own once the account is authorized");
			_database.record_upload_failure(entry.id, target.name(), result.message);
			return;

		case UploadTarget::Outcome::Unreachable:
			// Already reported once, with a cooldown, by UploadTarget.
			return;

		case UploadTarget::Outcome::Retry:
			LOG_WARN("Upload of " << entry.path << " to " << target.name() << " failed ("
				 << result.status_code << "): " << result.message << "; will retry");
			_database.record_upload_failure(entry.id, target.name(), result.message);

			if (++consecutive_retries >= kMaxConsecutiveUploadRetries) {
				LOG_WARN(target.name() << " failed " << consecutive_retries
					 << " uploads in a row, leaving the rest for the next cycle");
				return;
			}

			break;
		}
	}
}
