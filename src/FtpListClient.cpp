#include "FtpListClient.hpp"
#include "Log.hpp"

#include <chrono>
#include <cstring>

namespace
{

constexpr int kMaxAttempts = 3;

// Generous enough for a listing that has to come back over a telemetry link.
// Servers answer a retransmitted request from their reply cache, so a retry
// after a dropped packet is cheap.
constexpr auto kReplyTimeout = std::chrono::milliseconds(2000);

} // namespace

FtpListClient::FtpListClient(std::shared_ptr<mavsdk::System> system)
	: _passthrough(std::make_shared<mavsdk::MavlinkPassthrough>(system))
{
	_subscription = _passthrough->subscribe_message(MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL,
	[this](const mavlink_message_t& message) {
		handle_message(message);
	});
}

FtpListClient::~FtpListClient()
{
	_passthrough->unsubscribe_message(MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL, _subscription);
}

void FtpListClient::stop()
{
	_should_exit = true;
	_cv.notify_all();
}

void FtpListClient::reset_sessions()
{
	PayloadHeader reply;

	if (transact(kOpcodeResetSessions, "", 0, reply)) {
		LOG_DEBUG("FTP sessions reset");

	} else {
		LOG_DEBUG("FTP session reset went unanswered");
	}
}

void FtpListClient::handle_message(const mavlink_message_t& message)
{
	mavlink_file_transfer_protocol_t ftp;
	mavlink_msg_file_transfer_protocol_decode(&message, &ftp);

	// The transfer runs unicast: the server addresses its replies to whoever
	// asked. Anything not for us is another client's traffic.
	if (ftp.target_system != _passthrough->get_our_sysid()) {
		return;
	}

	if (ftp.target_component != _passthrough->get_our_compid() && ftp.target_component != 0) {
		return;
	}

	PayloadHeader payload;
	std::memcpy(&payload, ftp.payload, sizeof(payload));

	if (payload.opcode != kOpcodeAck && payload.opcode != kOpcodeNak) {
		return;
	}

	std::lock_guard<std::mutex> lock(_mutex);

	// Match against the in-flight request so replies to MAVSDK's Ftp plugin
	// (which shares our system and component id) are ignored.
	if (!_expected_req_opcode.has_value() || payload.req_opcode != _expected_req_opcode.value()
	    || payload.seq_number != _expected_seq) {
		return;
	}

	_reply = payload;
	_cv.notify_all();
}

void FtpListClient::send_request(const PayloadHeader& request)
{
	_passthrough->queue_message([this, request](MavlinkAddress address, uint8_t channel) {
		mavlink_message_t message;
		mavlink_msg_file_transfer_protocol_pack_chan(address.system_id, address.component_id, channel, &message,
				0, // target_network
				_passthrough->get_target_sysid(),
				_passthrough->get_target_compid(),
				reinterpret_cast<const uint8_t*>(&request));
		return message;
	});
}

bool FtpListClient::transact(uint8_t opcode, const std::string& path, uint32_t offset, PayloadHeader& reply)
{
	if (path.size() >= kMaxDataLength) {
		return false;
	}

	PayloadHeader request {};
	request.seq_number = ++_seq;
	request.opcode = opcode;
	request.offset = offset;
	request.size = static_cast<uint8_t>(path.size() + 1);
	std::memcpy(request.data, path.c_str(), path.size() + 1);

	for (int attempt = 0; attempt < kMaxAttempts && !_should_exit; attempt++) {
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_reply.reset();
			// Servers reply with the request sequence number plus one, and
			// answer a retransmission (same sequence number) from their
			// duplicate-reply cache, so retries reuse the packet as-is.
			_expected_seq = static_cast<uint16_t>(request.seq_number + 1);
			_expected_req_opcode = opcode;
		}

		send_request(request);

		std::unique_lock<std::mutex> lock(_mutex);

		if (_cv.wait_for(lock, kReplyTimeout, [this] { return _reply.has_value() || _should_exit.load(); })) {
			_expected_req_opcode.reset();

			if (_should_exit || !_reply.has_value()) {
				return false;
			}

			reply = _reply.value();
			return true;
		}
	}

	std::lock_guard<std::mutex> lock(_mutex);
	_expected_req_opcode.reset();

	return false;
}

void FtpListClient::parse_entries(const PayloadHeader& reply, std::vector<Entry>& entries, uint32_t& next_offset)
{
	const size_t data_size = std::min(static_cast<size_t>(reply.size), kMaxDataLength);
	size_t i = 0;

	while (i < data_size) {
		const char* raw = reinterpret_cast<const char*>(&reply.data[i]);
		const size_t length = strnlen(raw, data_size - i);
		i += length + 1;

		// Every directory entry advances the server-side offset, including the
		// "skip" placeholders it emits for entries it does not report.
		next_offset++;

		if (length < 2) {
			continue;
		}

		const std::string body(raw + 1, length - 1);

		if (raw[0] == 'D') {
			Entry entry;
			entry.name = body;
			entry.is_directory = true;
			entries.push_back(entry);

		} else if (raw[0] == 'F') {
			// "F<name>\t<size>" plus "\t<mtime>" from ListDirectoryWithTime
			const size_t first_tab = body.find('\t');

			if (first_tab == std::string::npos) {
				continue;
			}

			Entry entry;
			entry.name = body.substr(0, first_tab);

			try {
				const size_t second_tab = body.find('\t', first_tab + 1);
				entry.size_bytes = static_cast<uint32_t>(std::stoul(body.substr(first_tab + 1)));

				if (second_tab != std::string::npos) {
					// The server reports zero when it does not know the time
					const int64_t mtime = std::stoll(body.substr(second_tab + 1));

					if (mtime > 0) {
						entry.mtime_utc = mtime;
					}
				}

			} catch (const std::exception&) {
				continue;
			}

			entries.push_back(entry);
		}
	}
}

FtpListClient::Result FtpListClient::list_directory(const std::string& path, std::vector<Entry>& entries)
{
	entries.clear();

	uint32_t offset = 0;

	while (!_should_exit) {
		const uint8_t opcode = _with_time_supported ? kOpcodeListDirectoryWithTime : kOpcodeListDirectory;

		PayloadHeader reply;

		if (!transact(opcode, path, offset, reply)) {
			return _should_exit ? Result::Stopped : Result::Timeout;
		}

		if (reply.opcode == kOpcodeNak) {
			const uint8_t error = reply.size > 0 ? reply.data[0] : 0;

			if (error == kErrUnknownCommand && opcode == kOpcodeListDirectoryWithTime) {
				LOG_DEBUG("ListDirectoryWithTime unsupported, falling back to ListDirectory");
				_with_time_supported = false;
				continue; // Same offset, plain opcode
			}

			if (error == kErrEOF) {
				return Result::Success;
			}

			if (offset == 0 && (error == kErrFileNotFound || error == kErrFailErrno)) {
				return Result::FileNotFound;
			}

			LOG_DEBUG("FTP list of " << path << " failed with error " << static_cast<int>(error)
				  << " at offset " << offset);
			return Result::ProtocolError;
		}

		const uint32_t previous_offset = offset;
		parse_entries(reply, entries, offset);

		// An ACK that advances nothing would loop forever; treat it as the end
		if (offset == previous_offset) {
			return Result::Success;
		}
	}

	return Result::Stopped;
}
