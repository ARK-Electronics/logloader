#pragma once

#include "UploadBackend.hpp"

class FlightReviewBackend : public UploadBackend
{
public:
	struct Settings {
		std::string server_url;
		std::string user_email;
		std::string db_path;
		bool upload_enabled {};
		bool public_logs {};
	};

	FlightReviewBackend(const Settings& settings);

protected:
	UploadResult upload(const std::string& filepath) override;

private:
	enum class Protocol {
		Http,
		Https
	};

	void sanitize_url_and_determine_protocol();
	bool server_reachable();

	Settings _settings;
	Protocol _protocol {Protocol::Https};
};
