#pragma once

#include <mavsdk/system.h>
#include <mavsdk/plugins/mavlink_passthrough/mavlink_passthrough.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Minimal MAVLink FTP client for directory listings.
//
// MAVSDK's Ftp plugin discards the size and modification-time fields from list
// replies, but those are exactly what identifies a log file. This implements
// ListDirectory / ListDirectoryWithTime over MavlinkPassthrough and keeps the
// full entries. Bulk transfers still go through MAVSDK's Ftp plugin.
//
// Delete this once a MAVSDK release carries mavlink/MAVSDK#2891, which returns
// listings as Ftp::FilesystemEntry (name, entry_type, size_bytes,
// modification_time_s) and makes this redundant. It is on MAVSDK main but not
// in any release up to v3.17.2.
//
// ListDirectoryWithTime is tried first and the client falls back to plain
// ListDirectory when the server NAKs it with "unknown command" (ArduPilot and
// PX4 releases before v1.17 do).
//
// One request is in flight at a time and requests are issued from a single
// thread; only stop() may be called from another.
class FtpListClient
{
public:
	struct Entry {
		std::string name;
		bool is_directory {false};
		uint32_t size_bytes {0};
		std::optional<int64_t> mtime_utc; // Requires ListDirectoryWithTime support
	};

	enum class Result {
		Success,
		Timeout,      // No reply after retries, FTP likely disabled
		FileNotFound, // Directory does not exist
		ProtocolError,
		Stopped
	};

	explicit FtpListClient(std::shared_ptr<mavsdk::System> system);
	~FtpListClient();

	// Closes any sessions a previous (possibly crashed) client left open on the
	// vehicle. Best effort: PX4 only has a single FTP session and a stale one
	// blocks transfers until it is reset.
	void reset_sessions();

	Result list_directory(const std::string& path, std::vector<Entry>& entries);

	void stop();

private:
	// From the MAV_FTP_OPCODE enum in the MAVLink specification
	static constexpr uint8_t kOpcodeResetSessions = 2;
	static constexpr uint8_t kOpcodeListDirectory = 3;
	static constexpr uint8_t kOpcodeListDirectoryWithTime = 16;
	static constexpr uint8_t kOpcodeAck = 128;
	static constexpr uint8_t kOpcodeNak = 129;

	// From the MAV_FTP_ERR enum
	static constexpr uint8_t kErrFailErrno = 2;
	static constexpr uint8_t kErrEOF = 6;
	static constexpr uint8_t kErrUnknownCommand = 7;
	static constexpr uint8_t kErrFileNotFound = 10;

	static constexpr size_t kMaxDataLength = 239;

	// mavlink_file_transfer_protocol_t.payload is 251 bytes: this 12 byte
	// header followed by kMaxDataLength bytes of data.
	struct __attribute__((packed)) PayloadHeader {
		uint16_t seq_number;
		uint8_t session;
		uint8_t opcode;
		uint8_t size;
		uint8_t req_opcode;
		uint8_t burst_complete;
		uint8_t padding;
		uint32_t offset;
		uint8_t data[kMaxDataLength];
	};

	static_assert(sizeof(PayloadHeader) == 251, "PayloadHeader must fill the FILE_TRANSFER_PROTOCOL payload");

	void handle_message(const mavlink_message_t& message);
	bool transact(uint8_t opcode, const std::string& path, uint32_t offset, PayloadHeader& reply);
	void send_request(const PayloadHeader& request);

	static void parse_entries(const PayloadHeader& reply, std::vector<Entry>& entries, uint32_t& next_offset);

	std::shared_ptr<mavsdk::MavlinkPassthrough> _passthrough;
	mavsdk::MavlinkPassthrough::MessageHandle _subscription;

	std::mutex _mutex;
	std::condition_variable _cv;
	std::optional<PayloadHeader> _reply;
	uint16_t _expected_seq {0};
	std::optional<uint8_t> _expected_req_opcode;

	uint16_t _seq {0};
	bool _with_time_supported {true}; // Optimistic until the server NAKs it

	std::atomic<bool> _should_exit {false};
};
