#include "UploadTarget.hpp"
#include "Log.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

// CPPHTTPLIB_OPENSSL_SUPPORT is set for the whole target in CMakeLists.txt.
// Defining it per-file would give translation units different definitions of
// httplib's socket types, which is an ODR violation the linker resolves by
// silently picking one.
#include <httplib.h>

namespace fs = std::filesystem;

namespace
{

// httplib::Client parses the scheme itself and picks TLS accordingly, so the
// url is passed through as configured. A url without a scheme keeps the old
// behaviour of assuming TLS rather than silently downgrading to port 80.
std::string with_scheme(const std::string& url)
{
	return url.find("://") == std::string::npos ? "https://" + url : url;
}

// Server error pages are HTML; this makes one readable line out of one.
std::string one_line(std::string text, size_t limit = 200)
{
	for (char& c : text) {
		if (c == '\n' || c == '\r' || c == '\t') {
			c = ' ';
		}
	}

	if (text.size() > limit) {
		text.resize(limit);
		text += "...";
	}

	return text;
}

} // namespace

UploadTarget::UploadTarget(const UploadTargetConfig& config)
	: _config(config)
{
	_config.url = with_scheme(_config.url);
}

bool UploadTarget::reachable()
{
	const auto now = std::chrono::steady_clock::now();

	if (now < _unreachable_until) {
		return false;
	}

	httplib::Client client(_config.url);
	client.set_connection_timeout(5, 0);
	client.set_read_timeout(5, 0);
	// Any answer proves the host is up. Flight Review redirects "/", so
	// requiring 200 would call a healthy server dead.
	client.set_follow_location(false);

	const httplib::Result response = client.Get("/");

	if (!response) {
		_unreachable_until = now + kUnreachableCooldown;

		if (!_reported_unreachable) {
			LOG_WARN("Upload target " << _config.name << " (" << _config.url << ") is unreachable; "
				 << "retrying in "
				 << std::chrono::duration_cast<std::chrono::seconds>(kUnreachableCooldown).count()
				 << "s");
			_reported_unreachable = true;
		}

		return false;
	}

	if (_reported_unreachable) {
		LOG("Upload target " << _config.name << " (" << _config.url << ") is reachable again");
		_reported_unreachable = false;
	}

	_unreachable_until = {};
	return true;
}

UploadTarget::Result UploadTarget::upload(const std::string& file_path)
{
	std::error_code ec;

	if (!fs::exists(file_path, ec)) {
		return {Outcome::Missing, 0, "local file is missing: " + file_path, ""};
	}

	const auto size = fs::file_size(file_path, ec);

	if (ec) {
		return {Outcome::Missing, 0, "cannot stat local file: " + file_path, ""};
	}

	// Not Missing: re-fetching a log the vehicle reports as zero bytes would
	// download nothing, succeed, and come straight back here forever.
	if (size == 0) {
		return {Outcome::Rejected, 0, "log is empty: " + file_path, ""};
	}

	if (!reachable()) {
		return {Outcome::Unreachable, 0, "server unreachable", ""};
	}

	auto file = std::make_shared<std::ifstream>(file_path, std::ios::binary);

	if (!*file) {
		return {Outcome::Missing, 0, "cannot open local file: " + file_path, ""};
	}

	const std::string name = fs::path(file_path).filename().string();

	// Flight Review reads every one of these fields off the form; the ones it
	// does not use for an automated upload still have to be present.
	const httplib::MultipartFormDataItems fields = {
		{"type", _config.public_logs ? "flightreport" : "personal", "", ""},
		{"description", "Uploaded by logloader", "", ""},
		{"feedback", "", "", ""},
		{"email", _config.email, "", ""},
		{"source", "auto", "", ""},
		{"videoUrl", "", "", ""},
		{"rating", "", "", ""},
		{"windSpeed", "", "", ""},
		{"public", _config.public_logs ? "true" : "false", "", ""},
	};

	// The log is streamed off disk rather than assembled in memory. httplib's
	// MultipartFormDataItems overload builds the whole body as a std::string
	// first, so a 400 MB ArduPilot log would need the better part of a gigabyte
	// on a companion computer that does not have it.
	const std::string boundary = httplib::detail::make_multipart_data_boundary();
	const std::string content_type = httplib::detail::serialize_multipart_formdata_get_content_type(boundary);

	const httplib::MultipartFormData log_part {"filearg", "", name, "application/octet-stream"};
	const std::string prologue = httplib::detail::serialize_multipart_formdata(fields, boundary, false)
				     + httplib::detail::serialize_multipart_formdata_item_begin(log_part, boundary);
	const std::string epilogue = httplib::detail::serialize_multipart_formdata_item_end()
				     + httplib::detail::serialize_multipart_formdata_finish(boundary);

	const size_t body_size = prologue.size() + size + epilogue.size();

	httplib::Headers headers;
	const bool use_api_key = !_config.api_key.empty();

	if (use_api_key) {
		headers.emplace("Authorization", "Bearer " + _config.api_key);
		headers.emplace("X-API-Key", _config.api_key);
	}

	LOG("Uploading " << name << " to " << _config.name << " (" << _config.url << ")"
	    << (use_api_key ? " with API key" : ""));

	httplib::Client client(_config.url);
	client.set_connection_timeout(30, 0);
	client.set_read_timeout(300, 0);
	client.set_write_timeout(300, 0);
	client.set_follow_location(false);

	// Content-Length is known up front, so this stays a plain request rather
	// than a chunked one -- Flight Review's multipart reader wants the length.
	auto provider = [prologue, epilogue, file, size](size_t offset, size_t, httplib::DataSink & sink) {
		constexpr size_t kChunk = 64 * 1024;

		if (offset < prologue.size()) {
			const size_t n = std::min(kChunk, prologue.size() - offset);
			return sink.write(prologue.data() + offset, n);
		}

		const size_t in_file = offset - prologue.size();

		if (in_file < size) {
			const size_t n = std::min(kChunk, static_cast<size_t>(size) - in_file);
			std::vector<char> buffer(n);
			file->seekg(static_cast<std::streamoff>(in_file));

			if (!file->read(buffer.data(), static_cast<std::streamsize>(n))) {
				return false;
			}

			return sink.write(buffer.data(), n);
		}

		const size_t in_epilogue = in_file - size;
		return sink.write(epilogue.data() + in_epilogue, epilogue.size() - in_epilogue);
	};

	const httplib::Result response = client.Post("/upload", headers, body_size, provider, content_type);


	if (!response) {
		// The connection died mid-transfer; the server may or may not have the
		// log, so this is worth another attempt rather than a rejection.
		return {Outcome::Retry, 0, "connection failed during upload", ""};
	}

	const int status = response->status;

	// Flight Review answers a successful upload with a redirect to the plot.
	if (status == 302 || status == 200) {
		return {Outcome::Success, status, "uploaded", response->get_header_value("Location")};
	}

	const std::string detail = one_line(response->body);

	if (status == 401 || status == 403) {
		return {Outcome::Unauthorized, status, detail.empty() ? "not authorized" : detail, ""};
	}

	if (status == 400) {
		return {Outcome::Rejected, status, detail.empty() ? "rejected by the server" : detail, ""};
	}

	return {Outcome::Retry, status, detail.empty() ? "server error" : detail, ""};
}
