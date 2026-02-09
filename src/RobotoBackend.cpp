#include "RobotoBackend.hpp"
#include "Log.hpp"

#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

RobotoBackend::RobotoBackend(const RobotoBackend::Settings& settings)
	: UploadBackend(settings.db_path, settings.upload_enabled)
	, _settings(settings)
{
	// Strip scheme from API URL to get host for httplib
	std::string url = _settings.api_url;
	const std::string https_prefix = "https://";
	const std::string http_prefix = "http://";

	if (url.find(https_prefix) == 0) {
		_api_host = url.substr(https_prefix.size());

	} else if (url.find(http_prefix) == 0) {
		_api_host = url.substr(http_prefix.size());

	} else {
		_api_host = url;
	}
}

UploadBackend::UploadResult RobotoBackend::upload(const std::string& filepath)
{
	// Skip files that are in progress (have a .lock file)
	if (fs::exists(filepath + ".lock")) {
		return {false, 0, "File is locked (currently being downloaded)"};
	}

	// Skip files that don't exist
	if (!fs::exists(filepath)) {
		return {false, 404, "Log file does not exist: " + filepath};
	}

	// Skip files with size zero
	auto file_size = fs::file_size(filepath);

	if (file_size == 0) {
		return {false, 0, "Skipping zero-size log file: " + filepath};
	}

	std::string filename = fs::path(filepath).filename().string();

	LOG("Uploading " << filename << " to Roboto");

	// Step 1: Create dataset
	DatasetInfo dataset;

	if (!create_dataset(filename, dataset)) {
		return {false, 0, "Failed to create Roboto dataset"};
	}

	LOG_DEBUG("Created Roboto dataset: " << dataset.dataset_id);

	// Step 2: Begin upload transaction
	UploadTransactionInfo txn;

	if (!begin_upload(dataset.dataset_id, filename, file_size, txn)) {
		return {false, 0, "Failed to begin Roboto upload transaction"};
	}

	LOG_DEBUG("Upload transaction: " << txn.transaction_id);

	// Step 3: Get S3 credentials
	S3Credentials creds;

	if (!get_credentials(txn.transaction_id, creds)) {
		return {false, 0, "Failed to get Roboto upload credentials"};
	}

	LOG_DEBUG("Got S3 credentials for bucket: " << creds.bucket << " region: " << creds.region);

	// Step 4: Upload file to S3
	if (!upload_to_s3(filepath, creds, txn.s3_uri)) {
		return {false, 0, "Failed to upload file to S3"};
	}

	LOG_DEBUG("File uploaded to S3");

	// Step 5: Report progress
	if (!report_progress(txn.transaction_id, txn.s3_uri)) {
		return {false, 0, "Failed to report upload progress"};
	}

	// Step 6: Complete transaction
	if (!complete_upload(txn.transaction_id)) {
		return {false, 0, "Failed to complete upload transaction"};
	}

	LOG("Uploaded " << filename << " to Roboto dataset " << dataset.dataset_id);
	return {true, 200, "Uploaded to Roboto dataset " + dataset.dataset_id};
}

bool RobotoBackend::create_dataset(const std::string& filename, DatasetInfo& out)
{
	json body;
	body["name"] = filename;
	body["tags"] = json::array({"auto-upload", "logloader"});

	json metadata;
	metadata["source"] = "ark-logloader";

	if (!_settings.device_id.empty()) {
		metadata["device_id"] = _settings.device_id;
		body["device_id"] = _settings.device_id;
	}

	body["metadata"] = metadata;

	httplib::SSLClient cli(_api_host);
	cli.set_connection_timeout(10);
	cli.set_read_timeout(30);
	cli.set_bearer_token_auth(_settings.api_token);

	auto res = cli.Post("/v1/datasets",
			    body.dump(),
			    "application/json");

	if (!res) {
		LOG("Roboto API unreachable");
		return false;
	}

	if (res->status == 401 || res->status == 403) {
		LOG("Roboto API auth failed: " << res->status);
		return false;
	}

	if (res->status < 200 || res->status >= 300) {
		LOG("Roboto create dataset failed: " << res->status << " " << res->body);
		return false;
	}

	try {
		auto resp = json::parse(res->body);
		out.dataset_id = resp["data"]["dataset_id"].get<std::string>();
		return true;

	} catch (const json::exception& e) {
		LOG("Failed to parse create dataset response: " << e.what());
		return false;
	}
}

bool RobotoBackend::begin_upload(const std::string& dataset_id, const std::string& filename,
				 size_t file_size, UploadTransactionInfo& out)
{
	json association;
	association["association_id"] = dataset_id;
	association["association_type"] = "dataset";

	json manifest;
	manifest[filename] = file_size;

	json body;
	body["association"] = association;
	body["origination"] = "ark-logloader";
	body["resource_manifest"] = manifest;

	httplib::SSLClient cli(_api_host);
	cli.set_connection_timeout(10);
	cli.set_read_timeout(30);
	cli.set_bearer_token_auth(_settings.api_token);

	auto res = cli.Post("/v1/files/upload",
			    body.dump(),
			    "application/json");

	if (!res || res->status < 200 || res->status >= 300) {
		LOG("Roboto begin upload failed: " << (res ? std::to_string(res->status) + " " + res->body : "No response"));
		return false;
	}

	try {
		auto resp = json::parse(res->body);
		out.transaction_id = resp["data"]["transaction_id"].get<std::string>();
		auto& mappings = resp["data"]["upload_mappings"];

		if (mappings.contains(filename)) {
			out.s3_uri = mappings[filename].get<std::string>();

		} else if (!mappings.empty()) {
			// Take the first mapping
			out.s3_uri = mappings.begin().value().get<std::string>();

		} else {
			LOG("No upload mappings in response");
			return false;
		}

		return true;

	} catch (const json::exception& e) {
		LOG("Failed to parse begin upload response: " << e.what());
		return false;
	}
}

bool RobotoBackend::get_credentials(const std::string& transaction_id, S3Credentials& out)
{
	httplib::SSLClient cli(_api_host);
	cli.set_connection_timeout(10);
	cli.set_read_timeout(30);
	cli.set_bearer_token_auth(_settings.api_token);

	auto res = cli.Get(("/v1/files/upload/" + transaction_id + "/credentials").c_str());

	if (!res || res->status < 200 || res->status >= 300) {
		LOG("Roboto get credentials failed: " << (res ? std::to_string(res->status) + " " + res->body : "No response"));
		return false;
	}

	try {
		auto resp = json::parse(res->body);
		auto& creds_list = resp["data"];

		if (creds_list.empty()) {
			LOG("No credentials in response");
			return false;
		}

		auto& creds = creds_list[0];
		out.access_key_id = creds["access_key_id"].get<std::string>();
		out.secret_access_key = creds["secret_access_key"].get<std::string>();
		out.session_token = creds["session_token"].get<std::string>();
		out.bucket = creds["bucket"].get<std::string>();
		out.region = creds["region"].get<std::string>();
		return true;

	} catch (const json::exception& e) {
		LOG("Failed to parse credentials response: " << e.what());
		return false;
	}
}

bool RobotoBackend::upload_to_s3(const std::string& filepath, const S3Credentials& creds,
				 const std::string& s3_uri)
{
	// Parse S3 URI
	std::string bucket, key;

	if (!parse_s3_uri(s3_uri, bucket, key)) {
		LOG("Failed to parse S3 URI: " << s3_uri);
		return false;
	}

	// Read file
	std::ifstream file(filepath, std::ios::binary);

	if (!file) {
		LOG("Could not open file: " << filepath);
		return false;
	}

	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

	// Compute payload hash
	std::string payload_hash = sha256_hex(content.c_str(), content.size());

	// Get current UTC time
	auto now = std::chrono::system_clock::now();
	auto now_t = std::chrono::system_clock::to_time_t(now);
	std::tm utc_tm;
	gmtime_r(&now_t, &utc_tm);

	char date_iso[17]; // YYYYMMDDTHHMMSSZ
	strftime(date_iso, sizeof(date_iso), "%Y%m%dT%H%M%SZ", &utc_tm);

	char date_stamp[9]; // YYYYMMDD
	strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", &utc_tm);

	// Build SigV4 Authorization header
	std::string auth_header = sign_s3_request(
					  "PUT", bucket, key, creds.region, creds,
					  payload_hash, date_iso, date_stamp);

	// Construct S3 endpoint host
	std::string s3_host = bucket + ".s3." + creds.region + ".amazonaws.com";

	httplib::SSLClient s3_cli(s3_host);
	s3_cli.set_read_timeout(300); // 5 minutes for large files

	httplib::Headers headers = {
		{"Authorization", auth_header},
		{"x-amz-date", date_iso},
		{"x-amz-content-sha256", payload_hash},
		{"x-amz-security-token", creds.session_token},
		{"Host", s3_host}
	};

	std::string path = "/" + key;
	auto res = s3_cli.Put(path, headers, content, "application/octet-stream");

	if (!res) {
		LOG("S3 upload failed: no response");
		return false;
	}

	if (res->status == 200 || res->status == 204) {
		return true;
	}

	LOG("S3 upload failed: " << res->status << " " << res->body);
	return false;
}

bool RobotoBackend::report_progress(const std::string& transaction_id, const std::string& s3_uri)
{
	json body;
	body["manifest_items"] = json::array({s3_uri});

	httplib::SSLClient cli(_api_host);
	cli.set_connection_timeout(10);
	cli.set_read_timeout(30);
	cli.set_bearer_token_auth(_settings.api_token);

	auto res = cli.Put(("/v1/files/upload/" + transaction_id + "/progress").c_str(),
			   body.dump(),
			   "application/json");

	if (!res || res->status < 200 || res->status >= 300) {
		LOG("Roboto report progress failed: " << (res ? std::to_string(res->status) + " " + res->body : "No response"));
		return false;
	}

	return true;
}

bool RobotoBackend::complete_upload(const std::string& transaction_id)
{
	httplib::SSLClient cli(_api_host);
	cli.set_connection_timeout(10);
	cli.set_read_timeout(30);
	cli.set_bearer_token_auth(_settings.api_token);

	auto res = cli.Put(("/v1/files/upload/" + transaction_id + "/complete").c_str(),
			   "",
			   "application/json");

	if (!res || res->status < 200 || res->status >= 300) {
		LOG("Roboto complete upload failed: " << (res ? std::to_string(res->status) + " " + res->body : "No response"));
		return false;
	}

	return true;
}

// --- S3 Helpers ---

bool RobotoBackend::parse_s3_uri(const std::string& uri, std::string& bucket, std::string& key)
{
	const std::string prefix = "s3://";

	if (uri.substr(0, prefix.size()) != prefix) {
		return false;
	}

	auto rest = uri.substr(prefix.size());
	auto slash = rest.find('/');

	if (slash == std::string::npos) {
		return false;
	}

	bucket = rest.substr(0, slash);
	key = rest.substr(slash + 1);
	return true;
}

std::string RobotoBackend::sign_s3_request(const std::string& method,
		const std::string& bucket,
		const std::string& key,
		const std::string& region,
		const S3Credentials& creds,
		const std::string& payload_hash,
		const std::string& date_iso,
		const std::string& date_stamp)
{
	std::string s3_host = bucket + ".s3." + region + ".amazonaws.com";
	std::string canonical_uri = "/" + key;
	std::string canonical_querystring;

	// Canonical headers (must be sorted)
	std::string canonical_headers =
		"host:" + s3_host + "\n"
		"x-amz-content-sha256:" + payload_hash + "\n"
		"x-amz-date:" + date_iso + "\n"
		"x-amz-security-token:" + creds.session_token + "\n";

	std::string signed_headers = "host;x-amz-content-sha256;x-amz-date;x-amz-security-token";

	// Canonical request
	std::string canonical_request =
		method + "\n" +
		canonical_uri + "\n" +
		canonical_querystring + "\n" +
		canonical_headers + "\n" +
		signed_headers + "\n" +
		payload_hash;

	// Credential scope
	std::string credential_scope = date_stamp + std::string("/") + region + "/s3/aws4_request";

	// String to sign
	std::string string_to_sign =
		"AWS4-HMAC-SHA256\n" +
		date_iso + std::string("\n") +
		credential_scope + "\n" +
		sha256_hex(canonical_request);

	// Derive signing key
	std::string k_date = hmac_sha256_raw("AWS4" + creds.secret_access_key, date_stamp);
	std::string k_region = hmac_sha256_raw(k_date, region);
	std::string k_service = hmac_sha256_raw(k_region, "s3");
	std::string k_signing = hmac_sha256_raw(k_service, "aws4_request");

	// Compute signature
	std::string signature = hmac_sha256_hex(k_signing, string_to_sign);

	// Build Authorization header
	std::string authorization =
		"AWS4-HMAC-SHA256 Credential=" + creds.access_key_id + "/" + credential_scope +
		", SignedHeaders=" + signed_headers +
		", Signature=" + signature;

	return authorization;
}

// --- Crypto Helpers ---

std::string RobotoBackend::sha256_hex(const std::string& data)
{
	return sha256_hex(data.c_str(), data.size());
}

std::string RobotoBackend::sha256_hex(const char* data, size_t len)
{
	unsigned char hash[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char*>(data), len, hash);
	return to_hex(hash, SHA256_DIGEST_LENGTH);
}

std::string RobotoBackend::hmac_sha256_raw(const std::string& key, const std::string& data)
{
	unsigned char result[EVP_MAX_MD_SIZE];
	unsigned int result_len = 0;
	HMAC(EVP_sha256(),
	     key.c_str(), static_cast<int>(key.size()),
	     reinterpret_cast<const unsigned char*>(data.c_str()), data.size(),
	     result, &result_len);
	return std::string(reinterpret_cast<char*>(result), result_len);
}

std::string RobotoBackend::hmac_sha256_hex(const std::string& key, const std::string& data)
{
	unsigned char result[EVP_MAX_MD_SIZE];
	unsigned int result_len = 0;
	HMAC(EVP_sha256(),
	     key.c_str(), static_cast<int>(key.size()),
	     reinterpret_cast<const unsigned char*>(data.c_str()), data.size(),
	     result, &result_len);
	return to_hex(result, result_len);
}

std::string RobotoBackend::to_hex(const unsigned char* data, size_t len)
{
	std::ostringstream ss;
	ss << std::hex << std::setfill('0');

	for (size_t i = 0; i < len; i++) {
		ss << std::setw(2) << static_cast<unsigned int>(data[i]);
	}

	return ss.str();
}
