#include "LogLoader.hpp"
#include <filesystem>
#include <signal.h>
#include <iostream>
#include <toml.hpp>
#include <unistd.h>
#include <sys/types.h>

static void signal_handler(int signum);

std::atomic<bool> _should_exit = false;
std::shared_ptr<LogLoader> _log_loader;

int main()
{
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	setbuf(stdout, NULL); // Disable stdout buffering

	toml::table config;

	try {
		std::string config_path = std::string(getenv("HOME")) + "/.config/logloader/config.toml";
		config = toml::parse_file(config_path);

	} catch (const toml::parse_error& err) {
		std::cerr << "Parsing failed:\n" << err << "\n";
		return -1;

	} catch (const std::exception& err) {
		std::cerr << "Error: " << err.what() << "\n";
		return -1;
	}

	std::string logs_dir = std::string(getenv("HOME")) + "/.config/logloader/logs/";
	std::string uploaded_logs_file = std::string(getenv("HOME")) + "/.config/logloader/uploaded_logs.txt";

	// Setup the LogLoader
	LogLoader::Settings settings = {
		.email = config["email"].value_or(""),
		.server = config["server"].value_or("logs.px4.io"),
		.mavsdk_connection_url = config["connection_url"].value_or("0.0.0"),
		.logging_directory = logs_dir,
		.uploaded_logs_file = uploaded_logs_file,
		.upload_enabled = config["upload_enabled"].value_or(false),
		.public_logs = config["public_logs"].value_or(false)
	};

	_log_loader = std::make_shared<LogLoader>(settings);

	bool connected = false;

	while (!_should_exit && !connected) {
		connected = _log_loader->wait_for_mavsdk_connection(3);
	}

	if (!_should_exit && connected) {
		_log_loader->run();
	}

	std::cout << "exiting" << std::endl;

	return -1;
}

static void signal_handler(int signum)
{
	(void)signum;

	if (_log_loader.get()) _log_loader->stop();

	_should_exit = true;
}
