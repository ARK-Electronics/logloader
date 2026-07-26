#include "LogEntryLister.hpp"
#include "Log.hpp"

#include <chrono>
#include <ctime>

namespace
{

// How long to wait for another LOG_ENTRY before assuming the reply stalled
constexpr auto kQuietPeriod = std::chrono::seconds(3);

// How many times to re-ask for the entries we are still missing
constexpr int kMaxAttempts = 4;

std::string iso8601_utc(uint32_t time_utc)
{
	// Matches the formatting MAVSDK's LogFiles plugin uses, so the uuids the databases
	// are keyed on stay the same no matter which listing path produced the entry.
	char buffer[sizeof "2018-08-31T20:50:42Z"];
	const time_t as_time_t = time_utc;
	strftime(buffer, sizeof(buffer), "%FT%TZ", gmtime(&as_time_t));
	return buffer;
}

} // namespace

LogEntryLister::LogEntryLister(std::shared_ptr<mavsdk::System> system)
	: _passthrough(std::make_shared<mavsdk::MavlinkPassthrough>(system))
{
	_subscription = _passthrough->subscribe_message(MAVLINK_MSG_ID_LOG_ENTRY,
	[this](const mavlink_message_t& message) {
		handle_log_entry(message);
	});
}

LogEntryLister::~LogEntryLister()
{
	_passthrough->unsubscribe_message(MAVLINK_MSG_ID_LOG_ENTRY, _subscription);
}

void LogEntryLister::stop()
{
	_should_exit = true;
	_cv.notify_all();
}

void LogEntryLister::handle_log_entry(const mavlink_message_t& message)
{
	mavlink_log_entry_t log_entry;
	mavlink_msg_log_entry_decode(&message, &log_entry);

	{
		std::lock_guard<std::mutex> lock(_mutex);

		_num_logs = log_entry.num_logs;
		_last_log_id = log_entry.last_log_num;
		_got_any = true;

		if (log_entry.num_logs > 0) {
			mavsdk::LogFiles::Entry entry;
			entry.id = log_entry.id;
			entry.date = iso8601_utc(log_entry.time_utc);
			entry.size_bytes = log_entry.size;
			_entries[log_entry.id] = entry;
		}
	}

	_cv.notify_all();
}

bool LogEntryLister::listing_complete() const
{
	if (!_got_any) {
		return false;
	}

	if (_num_logs == 0) {
		return true;
	}

	// PX4 numbers from zero and ArduPilot from one, so derive the range the vehicle is
	// actually offering rather than assuming either.
	if (_last_log_id + 1 < _num_logs) {
		return false;
	}

	const uint16_t first_id = _last_log_id + 1 - _num_logs;

	for (uint16_t id = first_id; id <= _last_log_id; id++) {
		if (_entries.find(id) == _entries.end()) {
			return false;
		}
	}

	return true;
}

void LogEntryLister::request_list(uint16_t start, uint16_t end)
{
	_passthrough->queue_message([this, start, end](MavlinkAddress address, uint8_t channel) {
		mavlink_message_t message;
		mavlink_msg_log_request_list_pack_chan(address.system_id, address.component_id, channel, &message,
						       _passthrough->get_target_sysid(), _passthrough->get_target_compid(),
						       start, end);
		return message;
	});
}

void LogEntryLister::request_end()
{
	_passthrough->queue_message([this](MavlinkAddress address, uint8_t channel) {
		mavlink_message_t message;
		mavlink_msg_log_request_end_pack_chan(address.system_id, address.component_id, channel, &message,
						      _passthrough->get_target_sysid(), _passthrough->get_target_compid());
		return message;
	});
}

bool LogEntryLister::get_entries(std::vector<mavsdk::LogFiles::Entry>& entries)
{
	entries.clear();

	{
		std::lock_guard<std::mutex> lock(_mutex);
		_entries.clear();
		_num_logs = 0;
		_last_log_id = 0;
		_got_any = false;
	}

	uint16_t start = 0;

	for (int attempt = 0; attempt < kMaxAttempts && !_should_exit; attempt++) {
		request_list(start, 0xFFFF);

		// Wait for the reply to finish or go quiet. Every entry that lands resets the
		// clock, so a slow link gets as long as it needs.
		std::unique_lock<std::mutex> lock(_mutex);

		while (!_should_exit && !listing_complete()) {
			if (_cv.wait_for(lock, kQuietPeriod) == std::cv_status::timeout) {
				break;
			}
		}

		if (_should_exit) {
			return false;
		}

		if (listing_complete()) {
			for (const auto& [id, entry] : _entries) {
				entries.push_back(entry);
			}

			lock.unlock();
			request_end();
			return true;
		}

		// Pick up from the first gap rather than replaying the whole list
		if (_got_any && _num_logs > 0 && _last_log_id + 1 >= _num_logs) {
			const uint16_t first_id = _last_log_id + 1 - _num_logs;
			start = first_id;

			for (uint16_t id = first_id; id <= _last_log_id; id++) {
				if (_entries.find(id) == _entries.end()) {
					start = id;
					break;
				}
			}
		}

		LOG_DEBUG("Log listing incomplete, have " << _entries.size() << " of " << _num_logs
			  << ", retrying from " << start);
	}

	request_end();

	return false;
}
