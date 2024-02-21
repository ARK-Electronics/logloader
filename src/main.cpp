#include "LogLoader.hpp"
#include <signal.h>
#include <iostream>
#include <toml.hpp>

static void signal_handler(int signum);
bool _should_exit = false;

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
		.logging_dir = config["logdir"].value_or("logs/"),
		.mavsdk_connection_url = config["connection_url"].value_or("0.0.0")
	};

	LogLoader log_loader(settings);

	double timeout_ms = 3;

	if (!log_loader.wait_for_mavsdk_connection(timeout_ms)) {
		return 0;
	}

	while (!_should_exit) {
		log_loader.run();
	}

	std::cout << "exiting" << std::endl;

	return 0;
}

static void signal_handler(int signum)
{
	std::cout << "signal_handler!" << std::endl;
	_should_exit = true;
}
