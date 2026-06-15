#include "LogLoader.hpp"
#include "Log.hpp"
#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

LogLoader::LogLoader(const LogLoader::Settings& settings)
	: _settings(settings)
{
	_logs_directory = _settings.application_directory + "logs/";

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

	fs::create_directories(_logs_directory);
}

void LogLoader::stop()
{
	{
		std::lock_guard<std::mutex> lock(_exit_cv_mutex);
		_should_exit = true;
	}
	// Abort any in-flight server work waiting on these flags.
	_local_server->stop();
	_remote_server->stop();
	_exit_cv.notify_all();
}

void LogLoader::run()
{
	auto upload_thread = std::thread(&LogLoader::upload_logs_thread, this);

	while (!_should_exit) {
		// Logs are downloaded by the pymavlink downloader (logloader_download.py) into
		// the logs directory; here we just pick up whatever has appeared and upload it.
		register_new_logs();

		if (!_should_exit) {
			std::unique_lock<std::mutex> lock(_exit_cv_mutex);
			_exit_cv.wait_for(lock, std::chrono::seconds(30), [this] { return _should_exit.load(); });
		}
	}

	LOG_DEBUG("Waiting for upload thread");
	upload_thread.join();
}

void LogLoader::register_new_logs()
{
	std::error_code ec;

	if (!fs::exists(_logs_directory, ec)) {
		return;
	}

	for (const auto& dir_entry : fs::directory_iterator(_logs_directory, ec)) {
		if (ec) {
			LOG("Error scanning logs directory: " << ec.message());
			return;
		}

		// Only completed downloads; the downloader writes ".part" files while in progress.
		if (dir_entry.path().extension() != ".bin") {
			continue;
		}

		const std::string filepath = dir_entry.path().string();
		_local_server->register_log_file(filepath);
		_remote_server->register_log_file(filepath);
	}
}

void LogLoader::upload_logs_thread()
{
	while (!_should_exit) {
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

		ServerInterface::UploadResult result = server->upload_log(filepath);

		if (result.success) {
			LOG("Log upload SUCCESS: " << result.message);

		} else if (result.status_code == 400) {
			LOG("Log upload failed (" << result.status_code << "): " << result.message);

		} else {
			LOG("Log upload TEMPORARILY FAILED (" << result.status_code << "): "
			    << result.message << " - Will retry later");
		}
	}
}
