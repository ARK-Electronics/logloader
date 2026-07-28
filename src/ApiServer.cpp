#include "ApiServer.hpp"
#include "Config.hpp"
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

constexpr size_t kWorkerThreads = 16;
constexpr size_t kMaxStreams = 8;
constexpr size_t kMaxRequestBody = 1 << 20;

std::string iso8601_utc(int64_t time_utc)
{
	// A vehicle is free to report any mtime it likes; a year that does not fit
	// makes strftime return 0 and leave the buffer unspecified.
	char buffer[sizeof "2026-07-28T10:30:00Z"];
	const auto as_time_t = static_cast<time_t>(time_utc);
	std::tm tm {};

	if (gmtime_r(&as_time_t, &tm) == nullptr || strftime(buffer, sizeof(buffer), "%FT%TZ", &tm) == 0) {
		return {};
	}

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

// Flight Review answers an upload with a *relative* redirect, so the stored
// location is only half a link. Shipping each target's base url lets the UI
// join them itself rather than guessing an origin.
json targets_json(LogLoader& loader)
{
	json targets = json::array();

	for (const auto& [name, url] : loader.enabled_targets()) {
		targets.push_back(json {{"name", name}, {"url", url}});
	}

	return targets;
}

json list_payload(LogDatabase& database, const StatusBoard::Snapshot& snapshot, const json& targets)
{
	json logs = json::array();

	for (const auto& entry : database.all_logs()) {
		logs.push_back(to_json(entry));
	}

	return {{"logs", logs}, {"status", to_json(snapshot)}, {"targets", targets}};
}

} // namespace

ApiServer::ApiServer(const Settings& settings, LogLoader& loader)
	: _settings(settings)
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
	// Every /events stream holds one worker for as long as it is open, so the
	// pool has to be bigger than the number of streams we are willing to serve.
	_server->new_task_queue = [] { return new httplib::ThreadPool(kWorkerThreads); };
	// Nothing here accepts a large body; the only uploads go the other way.
	_server->set_payload_max_length(kMaxRequestBody);

	if (!_server->bind_to_port(_settings.bind, _settings.port)) {
		LOG_ERROR("Could not bind the API to " << _settings.bind << ":" << _settings.port);
		return false;
	}

	_thread = std::thread([this] { _server->listen_after_bind(); });
	LOG("API listening on " << _settings.bind << ":" << _settings.port);
	return true;
}

void ApiServer::stop()
{
	_stopping = true;
	// Releases the streams blocked in wait_for_change, which would otherwise
	// keep a worker each and hang the join below.
	_loader.status().shutdown();

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
		payload["targets"] = targets_json(_loader);
		response.set_content(payload.dump(), "application/json");
	});

	_server->Get("/logs", [this, &database](const httplib::Request&, httplib::Response & response) {
		const json payload =
			list_payload(database, _loader.status().get(), targets_json(_loader));
		response.set_content(payload.dump(), "application/json");
	});

	// One event per change, so the page reflects a transfer as it happens
	// rather than on the next poll.
	_server->Get("/events", [this, &database](const httplib::Request&, httplib::Response & response) {
		if (_streams.fetch_add(1) >= kMaxStreams) {
			_streams--;
			response.status = 503;
			response.set_content(json {{"error", "too many event streams"}}.dump(), "application/json");
			return;
		}

		// nginx buffers a streaming response into uselessness without this.
		response.set_header("Cache-Control", "no-cache");
		response.set_header("X-Accel-Buffering", "no");

		auto seen_status = std::make_shared<uint64_t>(UINT64_MAX);
		auto seen_logs = std::make_shared<uint64_t>(UINT64_MAX);

		// The releaser runs from ~Response on every path -- client disconnect,
		// write failure, shutdown -- which the provider itself does not: httplib
		// simply stops calling it when a write fails.
		response.set_chunked_content_provider("text/event-stream",
		[this, &database, seen_status, seen_logs](size_t, httplib::DataSink & sink) {
			StatusBoard& status = _loader.status();

			// Either side may be shutting down; _stopping covers the case where
			// the API is stopped on its own, which nothing else would unblock.
			if (_stopping.load() || status.stopped()) {
				sink.done();
				return true;
			}

			const auto snapshot = status.wait_for_change(*seen_status, kEventKeepalive);
			const uint64_t log_revision = database.revision();

			if (snapshot.revision == *seen_status && log_revision == *seen_logs) {
				// Nothing happened; prove the connection is still good.
				static constexpr char keepalive[] = ": keepalive\n\n";
				return sink.write(keepalive, sizeof(keepalive) - 1);
			}

			*seen_status = snapshot.revision;

			// Transfer progress ticks several times a second. Re-reading and
			// re-serialising every log for each of those would be the hottest
			// path in the program, so the list only goes out when it changed.
			std::string message;

			if (log_revision != *seen_logs) {
				*seen_logs = log_revision;
				message = "event: logs\ndata: "
					  + list_payload(database, snapshot, targets_json(_loader)).dump();

			} else {
				message = "event: status\ndata: " + to_json(snapshot).dump();
			}

			message += "\n\n";
			return sink.write(message.data(), message.size());
		},
		[this](bool) { _streams--; });
	});

	_server->Post("/logs/download", [this, &database](const httplib::Request & request, httplib::Response & response) {
		json body;

		if (!parse_body(request, response, body)) {
			return;
		}

		const std::vector<int64_t> ids =
			body.value("all", false) ? database.ids_not_downloaded() : ids_from(body);

		// Fetching and uploading normally go together; "upload": false is how a
		// caller asks for the file without publishing it.
		const std::vector<std::string> targets =
			body.value("upload", true) ? _loader.enabled_target_names() : std::vector<std::string> {};

		database.request(ids, targets);
		_loader.wake();
		_loader.status().notify();

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

		const std::vector<int64_t> ids =
			body.value("all", false) ? database.ids_not_uploaded(targets) : ids_from(body);

		database.request(ids, targets);
		_loader.wake();
		_loader.status().notify();

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
		_loader.status().notify();

		response.set_content(json {{"cancelled", ids.size()}}.dump(), "application/json");
	});

	_server->Delete(R"(/logs/(\d+)/file)", [this](const httplib::Request & request, httplib::Response & response) {
		int64_t id = 0;

		try {
			id = std::stoll(request.matches[1]);

		} catch (const std::exception&) {
			response.status = 404;
			response.set_content(json {{"error", "no such log"}}.dump(), "application/json");
			return;
		}

		if (!_loader.delete_local_file(id)) {
			response.status = 404;
			response.set_content(json {{"error", "no such log"}}.dump(), "application/json");
			return;
		}

		_loader.status().notify();
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
