#pragma once

#include <string>
#include <vector>

class ServerUploadManager
{
public:
	struct Settings {
		std::string server_url;
		std::string user_email;
		std::string logs_directory;
		std::string uploaded_logs_file;
		bool upload_enabled {};
		bool public_logs {};
	};

	ServerUploadManager(const Settings& settings);

	void upload_logs();
	bool upload_log(const std::string& log_path);

	void start();
	void stop();

private:
	void sanitize_url_and_determine_protocol();

	enum class Protocol {
		Http,
		Https
	};

	bool send_to_server(const std::string& file_path);
	bool server_reachable();

	bool is_uploaded(const std::string& file_path);
	void set_uploaded(const std::string& file_path);
