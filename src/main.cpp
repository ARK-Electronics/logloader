#include "LogLoader.hpp"
#include <filesystem>
#include <signal.h>
#include <iostream>
#include <toml.hpp>
#include <pwd.h>

static void signal_handler(int signum);

static std::string get_user_name()
{
	uid_t uid = geteuid();
	struct passwd* pw = getpwuid(uid);

	if (pw) {
		return std::string(pw->pw_name);
	}

	return {};
}

bool _should_exit = false;
std::shared_ptr<LogLoader> _log_loader;

int main(int argc, char* argv[])
{
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	setbuf(stdout, NULL); // Disable stdout buffering

	toml::table config;

	std::string default_config_path = "config.toml";
	bool config_exists = std::filesystem::exists(default_config_path);

	try {
		std::string config_path = config_exists ? default_config_path : "/home/" + get_user_name() + "/logloader/config.toml";
		config = toml::parse_file(config_path);

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
		.server = config["server"].value_or("logs.px4.io"),
		.mavsdk_connection_url = config["connection_url"].value_or("0.0.0"),
		.logging_directory = config["logging_directory"].value_or("logs/"),
		.uploaded_logs_file = config["uploaded_logs_file"].value_or("uploaded_logs.txt"),
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

	return 0;
}

static void signal_handler(int signum)
{
	std::cout << "signal_handler!" << std::endl;

	if (_log_loader.get()) _log_loader->stop();

	_should_exit = true;
}
