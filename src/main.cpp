#include "LogLoader.hpp"
#include <signal.h>

static void signal_handler(int signum);
bool _should_exit = false;

int main(int argc, char* argv[])
{
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	setbuf(stdout, NULL); // Disable stdout buffering

	std::cout << std::fixed << std::setprecision(2); // Set fixed-point notation and 2 decimal places

	// We want to disable the mavsdk logging which spams stdout
	mavsdk::log::subscribe([](...) {
		// https://mavsdk.mavlink.io/main/en/cpp/guide/logging.html
		return true;
	});

	std::string name;
	std::string version;
	std::string connection_url;
	std::string logdir;
	std::string email;

	try {
		auto config = toml::parse_file("config.toml");
		name = config["name"].value_or("logloader");
		version = config["version"].value_or("0.0.0");
		connection_url = config["connection_url"].value_or("0.0.0");
		logdir = config["logdir"].value_or("logs/");
		email = config["email"].value_or("");

	} catch (const toml::parse_error& err) {
		std::cerr << "Parsing failed:\n" << err << "\n";
		return 1;

	} catch (const std::exception& err) {
		std::cerr << "Error: " << err.what() << "\n";
		return 1;
	}

	// Setup the LogLoader
	LogLoader::Settings settings = {
		.email = email,
		.logging_dir = logdir,
		.mavsdk_connection_url = connection_url
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
