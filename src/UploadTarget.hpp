#pragma once

#include <chrono>
#include <string>

#include "Config.hpp"

// One Flight Review endpoint. Knows how to reach it and how to post a log to
// it; it holds no record of which logs exist or what has been uploaded, which
// is what LogDatabase is for.
class UploadTarget
{
public:
	enum class Outcome {
		Success,
		// The local copy is gone or empty. Nothing is wrong with the log itself,
		// so the caller should fetch it again rather than give up on it.
		Missing,
		// The server will not take this log however often it is asked.
		Rejected,
		// The account is not authorized (yet). The log is fine; every other
		// upload to this target would fail the same way, so stop the batch and
		// keep the log queued.
		Unauthorized,
		// Could not connect. Stop the batch and try again after the cooldown.
		Unreachable,
		// Anything else: worth another attempt later.
		Retry,
	};

	struct Result {
		Outcome outcome {Outcome::Retry};
		int status_code {0};
		std::string message;
		// Path the server redirected to, e.g. "/plot_app?log=<uuid>".
		std::string location;
	};

	explicit UploadTarget(const UploadTargetConfig& config);

	const std::string& name() const { return _config.name; }
	const std::string& url() const { return _config.url; }
	bool enabled() const { return _config.enabled; }

	Result upload(const std::string& file_path);

private:
	// Probes at most once per cooldown and logs only on the down/up
	// transitions, so a server that is simply off does not produce one failure
	// line per pending log.
	bool reachable();

	UploadTargetConfig _config;

	static constexpr auto kUnreachableCooldown = std::chrono::seconds(60);
	std::chrono::steady_clock::time_point _unreachable_until {};
	bool _reported_unreachable {false};
};
