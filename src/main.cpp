#include "LogLoader.hpp"
#include <signal.h>
#include <iostream>
#include <toml.hpp>

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
		config = toml::parse_file(std::string(getenv("HOME")) + "/.local/share/logloader/config.toml");

	} catch (const toml::parse_error& err) {
		std::cerr << "Parsing failed:\n" << err << "\n";
		return -1;

	} catch (const std::exception& err) {
		std::cerr << "Error: " << err.what() << "\n";
		return -1;
	}

	// Setup the LogLoader
	LogLoader::Settings settings = {
		.email = config["email"].value_or(""),
		.local_server = config["local_server"].value_or("http://127.0.0.1:5006"),
		.remote_server = config["remote_server"].value_or("https://logs.px4.io"),
		.mavsdk_connection_url = config["connection_url"].value_or("0.0.0"),
		.application_directory = std::string(getenv("HOME")) + "/.local/share/logloader",
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

	std::cout << "Exiting." << std::endl;

	return 0;
}

static void signal_handler(int signum)
{
	(void)signum;

	if (_log_loader.get()) _log_loader->stop();

	_should_exit = true;
}
