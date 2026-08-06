#include <atomic>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <thread>

#include <csignal>
#include <ctime>
#include <pthread.h>

#include "ApiServer.hpp"
#include "Config.hpp"
#include "Log.hpp"
#include "LogLoader.hpp"

namespace
{

void print_usage()
{
	std::cout
			<< "logloader - download flight logs over MAVLink FTP and upload them to Flight Review\n\n"
			<< "  --config <path>   configuration file to use\n"
			<< "  --help            show this message\n\n"
			<< "Without --config, ~/.config/ark/logloader/config.toml is used when it exists,\n"
			<< "otherwise /opt/ark/share/logloader/config.toml.\n";
}

// SIGINT/SIGTERM are blocked in every thread and consumed here instead. A
// handler cannot safely take a lock or signal a condition variable, and stopping
// cleanly needs both.
std::thread start_signal_thread(const sigset_t& mask, const std::atomic<bool>& running,
				const std::function<void()>& on_signal)
{
	return std::thread([&mask, &running, on_signal] {
		const timespec timeout {0, 200 * 1000 * 1000};

		while (running.load())
		{
			siginfo_t info {};
			const int signum = sigtimedwait(&mask, &info, &timeout);

			if (signum > 0) {
				LOG("Received signal " << signum << ", shutting down");
				on_signal();
				return;
			}
		}
	});
}

} // namespace

int main(int argc, char** argv)
{
	setbuf(stdout, nullptr); // journald wants lines as they happen

	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];

		if (arg == "--help" || arg == "-h") {
			print_usage();
			return 0;
		}
	}

	const std::string config_path = resolve_config_path(argc, argv);
	Config config;

	try {
		config = load_config(config_path);

	} catch (const std::exception& error) {
		LOG_ERROR(error.what());
		return 1;
	}

	logging::set_level(config.log_level);
	LOG("logloader starting, configuration from " << config_path);

	std::error_code ec;
	std::filesystem::create_directories(config.logs_directory, ec);

	if (ec) {
		LOG_ERROR("Cannot create " << config.logs_directory << ": " << ec.message());
		return 1;
	}

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &mask, nullptr);

	LogLoader loader(config);

	if (!loader.database_ok()) {
		return 1;
	}

	ApiServer api({config.api_bind, config.api_port}, loader);

	// Serve before connecting, so the UI can say the vehicle is not there yet
	// rather than failing to load at all.
	if (config.api_enabled && !api.start()) {
		return 1;
	}

	std::atomic<bool> running {true};
	std::thread signals = start_signal_thread(mask, running, [&loader] { loader.stop(); });

	if (loader.connect()) {
		loader.run();
	}

	api.stop();
	running = false;
	signals.join();

	LOG("logloader stopped");
	return 0;
}
