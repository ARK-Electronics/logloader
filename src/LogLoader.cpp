#include "LogLoader.hpp"
#include "Log.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>

namespace fs = std::filesystem;

namespace
{

std::string iso8601_utc(int64_t time_utc)
{
	char buffer[sizeof "2018-08-31T20:50:42Z"];
	const time_t as_time_t = static_cast<time_t>(time_utc);
	std::tm tm {};
	gmtime_r(&as_time_t, &tm);
	strftime(buffer, sizeof(buffer), "%FT%TZ", &tm);
	return buffer;
}

} // namespace

LogLoader::LogLoader(const LogLoader::Settings& settings)
	: _settings(settings)
{
	// Disable mavsdk noise
	mavsdk::log::subscribe([](...) {
		// https://mavsdk.mavlink.io/main/en/cpp/guide/logging.html
		return true;
	});

	_logs_directory = _settings.application_directory + "logs/";

	// Both databases live in the application directory, so it has to exist
	// before the server interfaces open them
	fs::create_directories(_logs_directory);

	// Setup local server interface
	ServerInterface::Settings local_server_settings = {
		.server_url = settings.local_server,
		.user_email = "",
		.logs_directory = _logs_directory,
		.db_path = _settings.application_directory + "local_server.db",
		.upload_enabled = true, // Always upload to local server
		.public_logs = true, // Public required true for searching using Web UI
	};

	// Setup remote server interface
	ServerInterface::Settings remote_server_settings = {
		.server_url = settings.remote_server,
		.user_email = settings.email,
		.logs_directory = _logs_directory,
		.db_path = _settings.application_directory + "remote_server.db",
		.upload_enabled = settings.upload_enabled,
		.public_logs = settings.public_logs,
	};

	_local_server = std::make_shared<ServerInterface>(local_server_settings);
	_remote_server = std::make_shared<ServerInterface>(remote_server_settings);

	std::cout << std::fixed << std::setprecision(8);
}

void LogLoader::stop()
{
	{
		std::lock_guard<std::mutex> lock(_exit_cv_mutex);
		_should_exit = true;
	}
	_exit_cv.notify_all();

	if (_ftp_fetcher) {
		_ftp_fetcher->stop();
	}
}

bool LogLoader::wait_for(std::chrono::seconds duration)
{
	std::unique_lock<std::mutex> lock(_exit_cv_mutex);
	_exit_cv.wait_for(lock, duration, [this] { return _should_exit.load(); });
	return _should_exit;
}

bool LogLoader::wait_for_mavsdk_connection(double timeout_ms)
{
	LOG("Connecting to " << _settings.mavsdk_connection_url);
	_mavsdk = std::make_shared<mavsdk::Mavsdk>(mavsdk::Mavsdk::Configuration(1, MAV_COMP_ID_ONBOARD_COMPUTER,
			true)); // Emit heartbeats (Client)
	auto result = _mavsdk->add_any_connection(_settings.mavsdk_connection_url);

	if (result != mavsdk::ConnectionResult::Success) {
		LOG("Connection failed: " << result);
		return false;
	}

	auto system = _mavsdk->first_autopilot(timeout_ms);

	if (!system) {
		LOG("Timed out waiting for system");
		return false;
	}

	LOG("Connected.");

	// MAVSDK plugins
	_telemetry = std::make_shared<mavsdk::Telemetry>(system.value());

	FtpLogFetcher::Settings ftp_settings = {
		.temp_directory = _settings.application_directory + "tmp/",
		.remote_directory = _settings.remote_log_directory,
		.use_burst = _settings.ftp_use_burst,
	};

	_ftp_fetcher = std::make_shared<FtpLogFetcher>(system.value(), ftp_settings);

	// PX4 serves a single FTP session, so one left open by a previous run (a
	// hard kill mid-transfer) blocks every transfer until the vehicle reboots.
	_ftp_fetcher->reset_sessions();

	return true;
}

void LogLoader::run()
{
	auto upload_thread = std::thread(&LogLoader::upload_logs_thread, this);

	while (!_should_exit) {
		// Check if vehicle is armed or if the logger is running
		// TODO: use SYS_STATUS flags to check logger status -- needs MAVSDK impl
		// bool logger_running = _telemetry->sys_status_sensors().enabled & MAV_SYS_STATUS_LOGGING;
		bool logger_running = false;
		bool vehicle_armed = _telemetry->armed();

		if (logger_running || vehicle_armed) {
			_loop_disabled = true;
			_remote_server->stop();
			_local_server->stop();
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;

		} else if (_loop_disabled) {
			_loop_disabled = false;
			_remote_server->start();
			_local_server->start();
			// Stall for a few seconds to allow logger to finish writing
			std::this_thread::sleep_for(std::chrono::seconds(3));
		}

		if (!refresh_log_index()) {
			if (wait_for(std::chrono::seconds(5))) {
				break;
			}

			continue;
		}

		download_pending_logs();

		// Periodically re-index the vehicle
		if (wait_for(std::chrono::seconds(30))) {
			break;
		}
	}

	LOG_DEBUG("Waiting for upload thread");
	upload_thread.join();
}

bool LogLoader::refresh_log_index()
{
	auto request_start = std::chrono::steady_clock::now();

	if (!_ftp_fetcher->refresh()) {
		return false;
	}

	const auto& logs = _ftp_fetcher->logs();
	size_t growing = 0;

	for (const auto& log : logs) {
		if (!log.stable) {
			// Almost certainly the log being written right now. Leave it out of
			// the databases entirely until it stops changing, so it never
			// enters the download queue at a size it will not keep.
			growing++;
			continue;
		}

		ServerInterface::LogEntry entry;
		entry.remote_path = log.relative_path;
		entry.size_bytes = log.size_bytes;
		entry.date = log.time_utc.has_value() ? iso8601_utc(log.time_utc.value()) : "";
		entry.uuid = ServerInterface::generate_uuid(entry.remote_path, entry.size_bytes);

		_local_server->sync_log(entry);
		_remote_server->sync_log(entry);
	}

	std::chrono::duration<double> duration = std::chrono::steady_clock::now() - request_start;
	LOG_DEBUG("Indexed " << logs.size() - growing << " of " << logs.size() << " logs in "
		  << duration.count() << " seconds (" << growing << " still being written)");

	return true;
}

const FtpLogFetcher::RemoteLog* LogLoader::find_remote_log(const ServerInterface::DatabaseEntry& db_entry) const
{
	for (const auto& log : _ftp_fetcher->logs()) {
		if (log.relative_path == db_entry.remote_path && log.size_bytes == db_entry.size_bytes) {
			return &log;
		}
	}

	return nullptr;
}

void LogLoader::download_pending_logs()
{
	auto pending = _local_server->get_logs_to_download();

	if (pending.empty()) {
		return;
	}

	// Retry the logs that have failed the least first, so a file that cannot be
	// fetched never blocks the ones behind it.
	std::stable_sort(pending.begin(), pending.end(),
	[this](const ServerInterface::DatabaseEntry & lhs, const ServerInterface::DatabaseEntry & rhs) {
		auto lhs_it = _download_failures.find(lhs.uuid);
		auto rhs_it = _download_failures.find(rhs.uuid);
		const int lhs_failures = lhs_it == _download_failures.end() ? 0 : lhs_it->second;
		const int rhs_failures = rhs_it == _download_failures.end() ? 0 : rhs_it->second;
		return lhs_failures < rhs_failures;
	});

	size_t index = 0;

	for (const auto& db_entry : pending) {
		if (_should_exit) {
			return;
		}

		// Downloading competes with the logger for the SD card, so give the
		// rest of the queue back to the outer loop as soon as the vehicle arms
		if (_telemetry->armed()) {
			LOG("Vehicle armed, pausing downloads");
			return;
		}

		index++;

		const FtpLogFetcher::RemoteLog* log = find_remote_log(db_entry);

		if (log == nullptr) {
			// The index is only published when the whole directory listed, so
			// a log that is not in it is genuinely gone from the vehicle.
			LOG_DEBUG("Log " << db_entry.remote_path << " is no longer on the vehicle, forgetting it");
			_local_server->delete_log(db_entry.uuid);
			_remote_server->delete_log(db_entry.uuid);
			continue;
		}

		LOG("Downloading log " << index << "/" << pending.size() << ": " << db_entry.remote_path);

		if (download_log(db_entry, *log)) {
			_download_failures.erase(db_entry.uuid);

		} else {
			_download_failures[db_entry.uuid]++;
		}
	}
}

std::string LogLoader::local_path_for(const ServerInterface::DatabaseEntry& db_entry)
{
	std::string name = db_entry.remote_path;
	std::replace(name.begin(), name.end(), '/', '_');

	const std::string path = _logs_directory + name;

	// ArduPilot reuses log file names once its numbering wraps at
	// LOG_MAX_FILES, so the name may already belong to a log we still have.
	if (_local_server->local_path_in_use(path, db_entry.uuid)) {
		return _logs_directory + db_entry.uuid.substr(0, 8) + "_" + name;
	}

	return path;
}

bool LogLoader::download_log(const ServerInterface::DatabaseEntry& db_entry, const FtpLogFetcher::RemoteLog& log)
{
	const std::string local_path = local_path_for(db_entry);

	auto time_start = std::chrono::steady_clock::now();

	FtpLogFetcher::ProgressCallback progress;

#ifdef DEBUG_BUILD
	// Held by the callback rather than captured by reference: MAVSDK owns the
	// callback and this function returns without it on shutdown.
	auto last_print = std::make_shared<std::chrono::steady_clock::time_point>(time_start);

	progress = [remote_path = db_entry.remote_path, size_bytes = db_entry.size_bytes, time_start, last_print](
	uint32_t bytes_transferred, uint32_t total_bytes) {
		const auto now = std::chrono::steady_clock::now();

		// One line per chunk buries everything else in the journal
		if (now - *last_print < std::chrono::seconds(1)) {
			return;
		}

		*last_print = now;

		auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - time_start).count();
		double rate_kbps = elapsed_ms > 0 ? (bytes_transferred * 8.0) / elapsed_ms : 0.;
		int percent = total_bytes > 0 ? int((100.0 * bytes_transferred) / total_bytes) : 0;

		LOG_DEBUG("Downloading: "
			  << std::setw(24) << std::left << remote_path
			  << std::setw(8) << std::fixed << std::setprecision(2) << size_bytes / 1e6 << "MB"
			  << std::setw(6) << std::right << percent << "%"
			  << std::setw(12) << std::fixed << std::setprecision(2) << rate_kbps << " Kbps"
			  << std::flush);
	};
#endif

	bool success = _ftp_fetcher->download(log, local_path, progress);

	if (!success) {
		LOG("Download failed");
		return false;
	}

	double seconds = std::chrono::duration_cast<std::chrono::milliseconds>(
				 std::chrono::steady_clock::now() - time_start).count() / 1000.;
	LOG("Finished in " << std::setprecision(2) << seconds << " seconds");

	// Update downloaded status in both databases
	_local_server->update_download_status(db_entry.uuid, local_path, true);
	_remote_server->update_download_status(db_entry.uuid, local_path, true);

	return true;
}

void LogLoader::upload_logs_thread()
{
	while (!_should_exit) {
		if (_loop_disabled) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}

		// Query the number of pending log uploads for both servers
		uint32_t num_logs_local = _local_server->num_logs_to_upload();
		uint32_t num_logs_remote = _remote_server->num_logs_to_upload();

		// Process uploads for local server
		if (!_should_exit && !_settings.local_server.empty() && num_logs_local) {
			LOG_DEBUG("Uploading " << num_logs_local << " logs to LOCAL server");
			upload_pending_logs(_local_server);
		}

		// Process uploads for remote server
		if (!_should_exit && !_settings.remote_server.empty() && _settings.upload_enabled && num_logs_remote) {
			LOG_DEBUG("Uploading " << num_logs_remote << " logs to REMOTE server");
			upload_pending_logs(_remote_server);
		}

		if (!_should_exit) {
			std::unique_lock<std::mutex> lock(_exit_cv_mutex);
			_exit_cv.wait_for(lock, std::chrono::seconds(10), [this] { return _should_exit.load(); });
		}
	}

	LOG_DEBUG("upload_logs_thread exiting");
}

void LogLoader::upload_pending_logs(std::shared_ptr<ServerInterface> server)
{
	// Upload all pending logs for this server
	while (!_should_exit && server->num_logs_to_upload()) {

		// Get one log at a time to upload
		ServerInterface::DatabaseEntry log_entry = server->get_next_log_to_upload();

		if (log_entry.uuid.empty()) {
			LOG("Log with empty uuid!");
			return;
		}

		std::string filepath = server->filepath_from_uuid(log_entry.uuid);

		if (filepath.empty()) {
			LOG("Could not determine file path for UUID: " << log_entry.uuid);
			return;
		}

		ServerInterface::UploadResult result = server->upload_log(log_entry.uuid, filepath);

		if (result.success) {
			LOG("Log upload SUCCESS: " << result.message);

		} else if (result.status_code == 400) {
			LOG("Log upload failed (" << result.status_code << "): " << result.message);

		} else {
			LOG("Log upload TEMPORARILY FAILED (" << result.status_code << "): "
			    << result.message << " - Will retry later");
			// Retry on the next pass rather than hammering an unhappy server
			return;
		}
	}
}
