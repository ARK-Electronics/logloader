#include "LogLoader.hpp"
#include "Log.hpp"
#include <signal.h>
#include <iostream>
#include <filesystem>
#include <toml.hpp>

static void signal_handler(int signum);

std::atomic<bool> _should_exit = false;
std::shared_ptr<LogLoader> _log_loader;

int main(int argc, char** argv)
{
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	setbuf(stdout, NULL); // Disable stdout buffering

	// Config lookup: --config <path> (or --config=<path>) overrides everything;
	// otherwise user override > deb-installed default.
	const std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
	const auto user_config = std::filesystem::path(home) / ".config/ark/logloader/config.toml";
	const auto default_config = std::filesystem::path("/opt/ark/share/logloader/config.toml");
	std::string config_path = (std::filesystem::exists(user_config) ? user_config : default_config).string();

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];

		if (arg == "--config" && i + 1 < argc) {
			config_path = argv[++i];

		} else if (arg.rfind("--config=", 0) == 0) {
			config_path = arg.substr(std::string("--config=").size());
		}
	}

	toml::table config;

	try {
		config = toml::parse_file(config_path);

	} catch (const toml::parse_error& err) {
		std::cerr << "Parsing failed:\n" << err << "\n";
		return -1;

	} catch (const std::exception& err) {
		std::cerr << "Error: " << err.what() << "\n";
		return -1;
	}

	// Writable data directory for logs and SQLite DB
	const auto data_dir = std::filesystem::path(home) / ".local/share/ark/logloader";
	std::filesystem::create_directories(data_dir);

	// Setup the LogLoader. Note: connection_url is consumed by the pymavlink downloader
	// (logloader_download.py), not by this uploader, so it is not read here.
	LogLoader::Settings settings = {
		.email = config["email"].value_or(""),
		.local_server = config["local_server"].value_or("http://127.0.0.1:5006"),
		.remote_server = config["remote_server"].value_or("https://logs.px4.io"),
		.application_directory = config["application_directory"].value_or(data_dir.string() + "/"),
		.upload_enabled = config["upload_enabled"].value_or(false),
		.public_logs = config["public_logs"].value_or(false)
	};

	_log_loader = std::make_shared<LogLoader>(settings);

	if (!_should_exit) {
		_log_loader->run();
	}

	LOG("Exiting.");

	return 0;
}

static void signal_handler(int signum)
{
	(void)signum;

	if (_log_loader.get()) _log_loader->stop();

	_should_exit = true;
}
