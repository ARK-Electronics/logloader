#include "ApiServer.hpp"
#include "Log.hpp"
#include "LogLoader.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>

#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace
{

// The SSE stream sends a keepalive at this interval when nothing has changed,
// so an idle connection is not mistaken for a dead one by anything in between.
constexpr auto kEventKeepalive = std::chrono::seconds(15);

std::string iso8601_utc(int64_t time_utc)
{
	char buffer[sizeof "2026-07-28T10:30:00Z"];
	const auto as_time_t = static_cast<time_t>(time_utc);
	std::tm tm {};
	gmtime_r(&as_time_t, &tm);
	strftime(buffer, sizeof(buffer), "%FT%TZ", &tm);
	return buffer;
}

json to_json(const LogDatabase::Entry& entry)
{
	json uploads = json::object();

	for (const auto& [target, state] : entry.uploads) {
		uploads[target] = {
			{"uploaded", state.uploaded},
			{"requested", state.requested},
			{"rejected", state.rejected},
			{"location", state.location},
			{"message", state.message},
		};
	}

	const auto slash = entry.path.find_last_of('/');

	return {
		{"id", entry.id},
		{"path", entry.path},
		{"name", slash == std::string::npos ? entry.path : entry.path.substr(slash + 1)},
		{"size_bytes", entry.size_bytes},
		{"time_utc", entry.time_utc.has_value() ? json(entry.time_utc.value()) : json(nullptr)},
		{"date", entry.time_utc.has_value() ? iso8601_utc(entry.time_utc.value()) : ""},
		{"present", entry.present},
		{"downloaded", entry.downloaded},
		{"download_requested", entry.download_requested},
		{"download_failures", entry.download_failures},
		{"last_error", entry.last_error},
		{"uploads", uploads},
	};
}

json to_json(const StatusBoard::Snapshot& snapshot)
{
	return {
		{"connected", snapshot.connected},
		{"armed", snapshot.armed},
		{"ftp_available", snapshot.ftp_available},
		{"log_root", snapshot.log_root},
		{"downloading_id", snapshot.downloading_id},
		{"downloaded_bytes", snapshot.downloaded_bytes},
		{"download_total_bytes", snapshot.download_total_bytes},
		{"uploading_id", snapshot.uploading_id},
		{"uploading_target", snapshot.uploading_target},
		{"revision", snapshot.revision},
	};
}

// Parses a request body, answering 400 rather than throwing on malformed input.
bool parse_body(const httplib::Request& request, httplib::Response& response, json& out)
{
	if (request.body.empty()) {
		out = json::object();
		return true;
	}

	out = json::parse(request.body, nullptr, false);

	if (out.is_discarded() || !out.is_object()) {
		response.status = 400;
		response.set_content(json {{"error", "body must be a JSON object"}}.dump(), "application/json");
		return false;
	}

	return true;
}

std::vector<int64_t> ids_from(const json& body)
{
	std::vector<int64_t> ids;

	if (!body.contains("ids") || !body["ids"].is_array()) {
		return ids;
	}

	for (const auto& id : body["ids"]) {
		if (id.is_number_integer()) {
			ids.push_back(id.get<int64_t>());
		}
	}

	return ids;
}

} // namespace

ApiServer::ApiServer(const Config& config, LogLoader& loader)
	: _config(config)
	, _loader(loader)
	, _server(std::make_unique<httplib::Server>())
{
	install_routes();
}

ApiServer::~ApiServer()
{
	stop();
}

bool ApiServer::start()
{
	if (!_server->bind_to_port(_config.api_bind, _config.api_port)) {
		LOG_ERROR("Could not bind the API to " << _config.api_bind << ":" << _config.api_port);
		return false;
	}

	_thread = std::thread([this] { _server->listen_after_bind(); });
	LOG("API listening on " << _config.api_bind << ":" << _config.api_port);
	return true;
}

void ApiServer::stop()
{
	if (_server) {
		_server->stop();
	}

	if (_thread.joinable()) {
		_thread.join();
	}
}

void ApiServer::install_routes()
{
	auto& database = _loader.database();

	_server->Get("/status", [this](const httplib::Request&, httplib::Response & response) {
		json payload = to_json(_loader.status().get());
		payload["targets"] = _loader.enabled_target_names();
		response.set_content(payload.dump(), "application/json");
	});

	_server->Get("/logs", [this, &database](const httplib::Request&, httplib::Response & response) {
		json logs = json::array();

		for (const auto& entry : database.all_logs()) {
			logs.push_back(to_json(entry));
		}

		const json payload = {
			{"logs", logs},
			{"status", to_json(_loader.status().get())},
			{"targets", _loader.enabled_target_names()},
		};
		response.set_content(payload.dump(), "application/json");
	});

	// One event per change, so the page reflects a transfer as it happens
	// rather than on the next poll.
	_server->Get("/events", [this, &database](const httplib::Request&, httplib::Response & response) {
		// nginx buffers a streaming response into uselessness without this.
		response.set_header("Cache-Control", "no-cache");
		response.set_header("X-Accel-Buffering", "no");

		auto last_revision = std::make_shared<uint64_t>(UINT64_MAX);

		response.set_chunked_content_provider("text/event-stream",
		[this, &database, last_revision](size_t, httplib::DataSink & sink) {
			StatusBoard& status = _loader.status();

			if (status.stopped()) {
				sink.done();
				return false;
			}

			const auto snapshot = status.wait_for_change(*last_revision, kEventKeepalive);

			if (snapshot.revision == *last_revision) {
				// Nothing happened; prove the connection is still good.
				return sink.write(": keepalive\n\n", 14);
			}

			*last_revision = snapshot.revision;

			json logs = json::array();

			for (const auto& entry : database.all_logs()) {
				logs.push_back(to_json(entry));
			}

			const json payload = {
				{"logs", logs},
				{"status", to_json(snapshot)},
				{"targets", _loader.enabled_target_names()},
			};
			const std::string message = "event: logs\ndata: " + payload.dump() + "\n\n";
			return sink.write(message.data(), message.size());
		});
	});

	_server->Post("/logs/download", [this, &database](const httplib::Request & request, httplib::Response & response) {
		json body;

		if (!parse_body(request, response, body)) {
			return;
		}

		std::vector<int64_t> ids = ids_from(body);

		if (body.value("all", false)) {
			ids.clear();

			for (const auto& entry : database.all_logs()) {
				if (entry.present && !entry.downloaded) {
					ids.push_back(entry.id);
				}
			}
		}

		// An explicit request says nothing about uploading, so only the
		// configured targets are asked for when the caller wants them.
		std::vector<std::string> targets;

		if (body.value("upload", true)) {
			targets = _loader.enabled_target_names();
		}

		database.request_download(ids, targets);
		_loader.wake();

		response.set_content(json {{"queued", ids.size()}}.dump(), "application/json");
	});

	_server->Post("/logs/upload", [this, &database](const httplib::Request & request, httplib::Response & response) {
		json body;

		if (!parse_body(request, response, body)) {
			return;
		}

		std::vector<std::string> targets = _loader.enabled_target_names();

		if (body.contains("targets") && body["targets"].is_array()) {
			std::vector<std::string> requested;

			for (const auto& target : body["targets"]) {
				if (target.is_string()) {
					const std::string name = target.get<std::string>();

					if (std::find(targets.begin(), targets.end(), name) != targets.end()) {
						requested.push_back(name);
					}
				}
			}

			targets = requested;
		}

		if (targets.empty()) {
			response.status = 400;
			response.set_content(json {{"error", "no enabled upload target named"}}.dump(),
			"application/json");
			return;
		}

		std::vector<int64_t> ids = ids_from(body);

		if (body.value("all", false)) {
			ids.clear();

			for (const auto& entry : database.all_logs()) {
				const bool wanted = std::any_of(targets.begin(), targets.end(),
				[&entry](const std::string & target) {
					const auto it = entry.uploads.find(target);
					return it != entry.uploads.end() && !it->second.uploaded;
				});

				if (wanted && (entry.downloaded || entry.present)) {
					ids.push_back(entry.id);
				}
			}
		}

		database.request_upload(ids, targets);
		_loader.wake();

		response.set_content(json {{"queued", ids.size()}}.dump(), "application/json");
	});

	_server->Post("/logs/cancel", [this, &database](const httplib::Request & request, httplib::Response & response) {
		json body;

		if (!parse_body(request, response, body)) {
			return;
		}

		const std::vector<int64_t> ids = ids_from(body);
		database.cancel_requests(ids);
		_loader.wake();

		response.set_content(json {{"cancelled", ids.size()}}.dump(), "application/json");
	});

	_server->Delete(R"(/logs/(\d+)/file)", [this](const httplib::Request & request, httplib::Response & response) {
		const int64_t id = std::stoll(request.matches[1]);

		if (!_loader.delete_local_file(id)) {
			response.status = 404;
			response.set_content(json {{"error", "no such log"}}.dump(), "application/json");
			return;
		}

		response.set_content(json {{"deleted", id}}.dump(), "application/json");
	});

	_server->set_exception_handler([](const httplib::Request&, httplib::Response & response,
	std::exception_ptr exception) {
		std::string message = "internal error";

		try {
			std::rethrow_exception(exception);

		} catch (const std::exception& error) {
			message = error.what();
		}

		LOG_ERROR("API request failed: " << message);
		response.status = 500;
		response.set_content(json {{"error", message}}.dump(), "application/json");
	});
}
