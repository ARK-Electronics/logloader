#include "Config.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>

#include <toml.hpp>

namespace fs = std::filesystem;

namespace
{

std::string env_or(const char* name, const std::string& fallback)
{
	const char* value = std::getenv(name);
	return value != nullptr && value[0] != '\0' ? value : fallback;
}

std::string with_trailing_slash(std::string path)
{
	if (!path.empty() && path.back() != '/') {
		path += '/';
	}

	return path;
}

std::string trim(std::string text)
{
	const auto first = text.find_first_not_of(" \t\r\n");

	if (first == std::string::npos) {
		return {};
	}

	const auto last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

// Set when a config file still uses the pre-overhaul flat key layout. Warning
// once is what makes the fallback deletable later rather than permanent.
bool g_used_legacy_key = false;

// Reads preferred, then the flat top-level key the pre-overhaul layout used, so
// an existing config.toml keeps working unchanged.
template <typename T, typename Preferred, typename Legacy>
T value_or(const Preferred& preferred, const Legacy& legacy, T fallback)
{
	if (auto value = preferred.template value<T>(); value.has_value()) {
		return value.value();
	}

	if (auto value = legacy.template value<T>(); value.has_value()) {
		g_used_legacy_key = true;
		return value.value();
	}

	return fallback;
}

} // namespace

std::string resolve_config_path(int argc, char** argv)
{
	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];

		if (arg == "--config" && i + 1 < argc) {
			return argv[i + 1];
		}

		if (arg.rfind("--config=", 0) == 0) {
			return arg.substr(std::string("--config=").size());
		}
	}

	const std::string home = env_or("HOME", "/tmp");
	const auto user_config = fs::path(home) / ".config/ark/logloader/config.toml";
	const auto installed_config = fs::path("/opt/ark/share/logloader/config.toml");

	return (fs::exists(user_config) ? user_config : installed_config).string();
}

Config load_config(const std::string& path)
{
	toml::table file;

	try {
		file = toml::parse_file(path);

	} catch (const toml::parse_error& error) {
		std::ostringstream message;
		message << "could not parse " << path << ": " << error.description();
		throw std::runtime_error(message.str());
	}

	Config config;

	config.connection_url = file["connection_url"].value_or("udp://:14551");

	const std::string level_text = file["log_level"].value_or("info");

	if (!logging::parse_level(level_text, config.log_level)) {
		LOG_WARN("Unknown log_level \"" << level_text << "\", using info");
	}

	const std::string home = env_or("HOME", "/tmp");
	const std::string xdg_data = env_or("XDG_DATA_HOME", home + "/.local/share");
	const std::string default_data = xdg_data + "/ark/logloader/";

	const auto data = file["data"];
	const auto api = file["api"];
	const auto download = file["download"];
	const auto upload = file["upload"];
	const auto local = upload["local"];
	const auto remote = upload["remote"];

	config.data_directory = with_trailing_slash(
					value_or<std::string>(data["directory"], file["application_directory"], default_data));
	config.logs_directory = config.data_directory + "logs/";

	config.api_enabled = api["enabled"].value_or(config.api_enabled);
	config.api_bind = api["bind"].value_or(config.api_bind);
	config.api_port = static_cast<uint16_t>(api["port"].value_or(config.api_port));

	config.auto_download = download["auto"].value_or(config.auto_download);
	config.download_latest_on_first_start =
		download["latest_on_first_start"].value_or(config.download_latest_on_first_start);
	config.auto_upload = upload["auto"].value_or(true);
	config.max_auto_queue = download["max_auto_queue"].value_or(config.max_auto_queue);
	config.index_interval_s = std::max(1, download["index_interval"].value_or(config.index_interval_s));
	config.upload_interval_s = std::max(1, upload["interval"].value_or(config.upload_interval_s));
	config.remote_log_directory =
		value_or<std::string>(download["remote_directory"], file["remote_log_directory"], "");
	config.use_burst = value_or<bool>(download["use_burst"], file["ftp_use_burst"], true);

	config.local.name = kTargetLocal;
	config.local.url = value_or<std::string>(local["url"], file["local_server"], "http://127.0.0.1:5006");
	config.local.enabled = local["enabled"].value_or(true);
	// Flight Review on the companion is open, and public is what makes a log
	// findable in its web UI.
	config.local.public_logs = local["public"].value_or(true);
	config.local.email = local["email"].value_or("");
	config.local.api_key = trim(local["api_key"].value_or<std::string>(""));

	config.remote.name = kTargetRemote;
	config.remote.url = value_or<std::string>(remote["url"], file["remote_server"], "https://logs.px4.io");
	config.remote.enabled = value_or<bool>(remote["enabled"], file["upload_enabled"], false);
	config.remote.public_logs = value_or<bool>(remote["public"], file["public_logs"], false);
	config.remote.email = value_or<std::string>(remote["email"], file["email"], "");
	config.remote.api_key = trim(value_or<std::string>(remote["api_key"], file["remote_api_key"], ""));

	if (config.local.url.empty()) {
		config.local.enabled = false;
	}

	if (config.remote.url.empty()) {
		config.remote.enabled = false;
	}

	if (g_used_legacy_key) {
		LOG_WARN(path << " uses the pre-overhaul flat key layout. It still works, but the "
			 "shipped config.toml shows the current one and the fallback will be removed.");
	}

	return config;
}
