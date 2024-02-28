#include "LogLoader.hpp"
#include <signal.h>
#include <iostream>
#include <toml.hpp>

static void signal_handler(int signum);

bool _should_exit = false;
std::shared_ptr<LogLoader> _log_loader;

int main(int argc, char* argv[])
{
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	setbuf(stdout, NULL); // Disable stdout buffering

	toml::table config;

	try {
		config = toml::parse_file("config.toml");

	} catch (const toml::parse_error& err) {
		std::cerr << "Parsing failed:\n" << err << "\n";
		return 0;

	} catch (const std::exception& err) {
		std::cerr << "Error: " << err.what() << "\n";
		return 0;
	}

	// Setup the LogLoader
	LogLoader::Settings settings = {
		.email = config["email"].value_or(""),
		.logging_directory = config["logging_directory"].value_or("logs/"),
		.uploaded_logs_file = config["uploaded_logs_file"].value_or("uploaded_logs.txt"),
		.mavsdk_connection_url = config["connection_url"].value_or("0.0.0"),
		.upload_enabled = config["upload_enabled"].value_or(false)
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

	return 0;
}

static void signal_handler(int signum)
{
	std::cout << "signal_handler!" << std::endl;

	if (_log_loader.get()) _log_loader->stop();

	_should_exit = true;
}
