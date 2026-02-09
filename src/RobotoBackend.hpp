#pragma once

#include "UploadBackend.hpp"
#include <string>

class RobotoBackend : public UploadBackend
{
public:
	struct Settings {
		std::string api_url;      // e.g., "https://api.roboto.ai"
		std::string api_token;    // Bearer token
		std::string device_id;    // Device ID for dataset metadata
		std::string db_path;
		bool upload_enabled {};
	};

	RobotoBackend(const Settings& settings);

protected:
	UploadResult upload(const std::string& filepath) override;

private:
	struct DatasetInfo {
		std::string dataset_id;
	};

	struct UploadTransactionInfo {
		std::string transaction_id;
		std::string s3_uri;
	};

	struct S3Credentials {
		std::string access_key_id;
		std::string secret_access_key;
		std::string session_token;
		std::string bucket;
		std::string region;
	};

	// Roboto API steps
	bool create_dataset(const std::string& filename, DatasetInfo& out);
	bool begin_upload(const std::string& dataset_id, const std::string& filename,
			  size_t file_size, UploadTransactionInfo& out);
	bool get_credentials(const std::string& transaction_id, S3Credentials& out);
	bool upload_to_s3(const std::string& filepath, const S3Credentials& creds,
			  const std::string& s3_uri);
	bool report_progress(const std::string& transaction_id, const std::string& s3_uri);
	bool complete_upload(const std::string& transaction_id);

	// S3 helpers
	static bool parse_s3_uri(const std::string& uri, std::string& bucket, std::string& key);
	static std::string sign_s3_request(const std::string& method,
					   const std::string& bucket,
					   const std::string& key,
					   const std::string& region,
					   const S3Credentials& creds,
					   const std::string& payload_hash,
					   const std::string& date_iso,
					   const std::string& date_stamp);

	// Crypto helpers
	static std::string sha256_hex(const std::string& data);
	static std::string sha256_hex(const char* data, size_t len);
	static std::string hmac_sha256_raw(const std::string& key, const std::string& data);
	static std::string hmac_sha256_hex(const std::string& key, const std::string& data);
	static std::string to_hex(const unsigned char* data, size_t len);

	Settings _settings;
	std::string _api_host; // Host extracted from api_url (scheme stripped)
};
