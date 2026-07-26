#include "FtpLogFetcher.hpp"
#include "Log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <sstream>
#include <type_traits>

namespace fs = std::filesystem;

namespace
{

struct Candidate {
	const char* directory;
	FtpLogFetcher::Flavor flavor;
	const char* extension;
};

// PX4 keeps ulog files under PX4_STORAGEDIR "/log", ArduPilot keeps dataflash logs under
// HAL_BOARD_STORAGE_DIRECTORY "/LOGS". Both are reachable over MAVLink FTP.
constexpr Candidate kCandidates[] = {
	{"/fs/microsd/log", FtpLogFetcher::Flavor::PX4, ".ulg"},
	{"/APM/LOGS", FtpLogFetcher::Flavor::ArduPilot, ".BIN"},
};

// PX4 nests logs one level deep (log/<date>/<time>.ulg), ArduPilot keeps them flat.
constexpr int kMaxListDepth = 2;

struct Listing {
	std::vector<std::string> directories;
	std::vector<std::pair<std::string, std::optional<uint32_t>>> files;
};

// MAVSDK 2.x returns the raw protocol entries, MAVSDK 3.x splits them up and drops the
// file sizes. Both shapes have to compile against whichever version is installed.
template <typename T>
Listing flatten_listing(const T& listing)
{
	Listing out;

	if constexpr(std::is_same_v<T, std::vector<std::string>>) {
		for (const auto& raw : listing) {
			if (raw.size() < 2) {
				continue;
			}

			const std::string body = raw.substr(1);

			if (raw[0] == 'D') {
				out.directories.push_back(body);

			} else if (raw[0] == 'F') {
				const auto tab = body.find('\t');

				if (tab == std::string::npos) {
					out.files.emplace_back(body, std::nullopt);
					continue;
				}

				std::optional<uint32_t> size_bytes;

				try {
					size_bytes = static_cast<uint32_t>(std::stoul(body.substr(tab + 1)));

				} catch (const std::exception&) {
					size_bytes = std::nullopt;
				}

				out.files.emplace_back(body.substr(0, tab), size_bytes);
			}
		}

	} else {
		for (const auto& directory : listing.dirs) {
			out.directories.push_back(directory);
		}

		for (const auto& file : listing.files) {
			out.files.emplace_back(file, std::nullopt);
		}
	}

	return out;
}

bool ends_with_ignore_case(const std::string& text, const std::string& suffix)
{
	if (text.size() < suffix.size()) {
		return false;
	}

	const size_t offset = text.size() - suffix.size();

	for (size_t i = 0; i < suffix.size(); i++) {
		if (std::tolower(text[offset + i]) != std::tolower(suffix[i])) {
			return false;
		}
	}

	return true;
}

std::optional<int64_t> parse_iso8601_utc(const std::string& text)
{
	std::tm tm {};
	std::istringstream ss(text);
	ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");

	if (ss.fail()) {
		return std::nullopt;
	}

	return static_cast<int64_t>(timegm(&tm));
}

// PX4 writes logs as <root>/<YYYY-MM-DD>/<HH_MM_SS>.ulg
std::optional<int64_t> parse_px4_start_time(const std::string& path)
{
	const fs::path parsed(path);
	const std::string day = parsed.parent_path().filename().string();
	const std::string time_of_day = parsed.stem().string();

	if (day.size() != 10 || time_of_day.size() != 8) {
		return std::nullopt;
	}

	std::string iso = day + "T" + time_of_day + "Z";
	std::replace(iso.begin(), iso.end(), '_', ':');

	return parse_iso8601_utc(iso);
}

// ArduPilot writes logs as <root>/<%08u>.BIN
std::optional<uint32_t> parse_ardupilot_log_number(const std::string& path)
{
	const std::string stem = fs::path(path).stem().string();

	if (stem.empty() || stem.size() > 9) {
		return std::nullopt;
	}

	for (char c : stem) {
		if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
			return std::nullopt;
		}
	}

	return static_cast<uint32_t>(std::stoul(stem));
}

// ArduPilot lists logs oldest first while the file names carry the log number, and log
// numbers wrap around. Once they have, every number above the most recent one belongs to
// the older half of the list. Returns positions into log_numbers, oldest first.
std::vector<size_t> ardupilot_list_order(const std::vector<std::optional<uint32_t>>& log_numbers,
		std::optional<uint32_t> last_log_number)
{
	std::vector<size_t> numbered;

	for (size_t i = 0; i < log_numbers.size(); i++) {
		if (log_numbers[i].has_value()) {
			numbered.push_back(i);
		}
	}

	if (numbered.empty()) {
		return {};
	}

	std::sort(numbered.begin(), numbered.end(), [&log_numbers](size_t lhs, size_t rhs) {
		return log_numbers[lhs].value() < log_numbers[rhs].value();
	});

	const uint32_t last = last_log_number.value_or(log_numbers[numbered.back()].value());

	std::vector<size_t> order;

	for (size_t i : numbered) {
		if (log_numbers[i].value() > last) {
			order.push_back(i);
		}
	}

	for (size_t i : numbered) {
		if (log_numbers[i].value() <= last) {
			order.push_back(i);
		}
	}

	return order;
}

std::string join_path(const std::string& directory, const std::string& name)
{
	if (!directory.empty() && directory.back() == '/') {
		return directory + name;
	}

	return directory + "/" + name;
}

} // namespace

FtpLogFetcher::FtpLogFetcher(std::shared_ptr<mavsdk::System> system, const FtpLogFetcher::Settings& settings)
	: _ftp(std::make_shared<mavsdk::Ftp>(system))
	, _settings(settings)
{
	std::error_code ec;
	fs::create_directories(_settings.temp_directory, ec);
}

void FtpLogFetcher::stop()
{
	_should_exit = true;
}

std::string FtpLogFetcher::flavor_name() const
{
	switch (_flavor) {
	case Flavor::PX4: return "PX4";

	case Flavor::ArduPilot: return "ArduPilot";

	default: return "unknown";
	}
}

std::string FtpLogFetcher::log_extension() const
{
	return _extension;
}

bool FtpLogFetcher::detect()
{
	// A flight stack that answers with an empty listing rather than an error would let the
	// wrong candidate win, so take a directory that actually holds logs over one that
	// merely exists. A vehicle with no logs yet falls through to the second pass.
	for (bool require_logs : {true, false}) {
		for (const auto& candidate : kCandidates) {
			const std::string directory = _settings.remote_directory.empty()
						      ? std::string(candidate.directory)
						      : _settings.remote_directory;

			if (!build_index(directory, candidate.extension, candidate.flavor)) {
				continue;
			}

			if (require_logs && _index.empty()) {
				continue;
			}

			_flavor = candidate.flavor;
			_remote_directory = directory;
			_extension = candidate.extension;

			LOG("MAVLink FTP available, " << flavor_name() << " logs in " << _remote_directory
			    << " (" << _index.size() << " files)");

			return true;
		}
	}

	LOG("MAVLink FTP unavailable, falling back to the MAVLink log protocol");

	return false;
}

bool FtpLogFetcher::refresh()
{
	if (_flavor == Flavor::Unknown) {
		// The SD card may have shown up after we connected
		return detect();
	}

	return build_index(_remote_directory, _extension, _flavor);
}

bool FtpLogFetcher::build_index(const std::string& root, const std::string& extension, Flavor flavor)
{
	_index.clear();
	_ardupilot_order.clear();

	if (!list_recursive(root, kMaxListDepth, extension)) {
		return false;
	}

	if (flavor == Flavor::ArduPilot) {
		build_ardupilot_order(root);
	}

	return true;
}

bool FtpLogFetcher::list_recursive(const std::string& directory, int depth_remaining, const std::string& extension)
{
	if (depth_remaining <= 0 || _should_exit) {
		return true;
	}

	auto response = _ftp->list_directory(directory);

	if (response.first != mavsdk::Ftp::Result::Success) {
		LOG_DEBUG("FTP list of " << directory << " failed: " << response.first);
		return false;
	}

	const Listing listing = flatten_listing(response.second);

	for (const auto& [name, size_bytes] : listing.files) {
		if (!ends_with_ignore_case(name, extension)) {
			continue;
		}

		RemoteFile file;
		file.path = join_path(directory, name);
		file.size_bytes = size_bytes;
		file.start_time = parse_px4_start_time(file.path);
		file.log_number = parse_ardupilot_log_number(file.path);
		_index.push_back(file);
	}

	for (const auto& name : listing.directories) {
		if (name == "." || name == "..") {
			continue;
		}

		// A subdirectory we cannot read is not fatal, the logs we did find are still usable
		list_recursive(join_path(directory, name), depth_remaining - 1, extension);
	}

	return true;
}

void FtpLogFetcher::build_ardupilot_order(const std::string& root)
{
	std::vector<std::optional<uint32_t>> log_numbers;
	log_numbers.reserve(_index.size());

	bool any_numbered = false;

	for (const auto& file : _index) {
		log_numbers.push_back(file.log_number);
		any_numbered = any_numbered || file.log_number.has_value();
	}

	if (!any_numbered) {
		return;
	}

	_ardupilot_order = ardupilot_list_order(log_numbers, read_last_log_number(root));
}

std::optional<uint32_t> FtpLogFetcher::read_last_log_number(const std::string& root)
{
	const std::string remote_path = join_path(root, "LASTLOG.TXT");
	// Deliberately not LASTLOG.TXT, that is where the transfer is staged
	const std::string local_path = join_path(_settings.temp_directory, "last_log_number.txt");

	std::error_code ec;
	fs::remove(local_path, ec);

	// Burst mode is pointless for a handful of bytes and not every build supports it
	if (!download(remote_path, local_path, 0, nullptr)) {
		LOG_DEBUG("No LASTLOG.TXT, assuming the log numbers have not wrapped");
		return std::nullopt;
	}

	std::ifstream file(local_path);
	uint32_t last_log_number = 0;
	const bool parsed = static_cast<bool>(file >> last_log_number);
	file.close();
	fs::remove(local_path, ec);

	if (!parsed || last_log_number == 0) {
		return std::nullopt;
	}

	return last_log_number;
}

std::string FtpLogFetcher::resolve(const mavsdk::LogFiles::Entry& entry)
{
	// An exact size match is unambiguous when MAVSDK reports sizes, which is the common
	// case. Otherwise fall back to reproducing the ordering the flight stack listed.
	RemoteFile* file = match_by_size(entry.size_bytes);

	if (file == nullptr) {
		file = (_flavor == Flavor::ArduPilot) ? match_ardupilot(entry) : match_px4(entry);
	}

	if (file == nullptr) {
		return "";
	}

	file->claimed = true;

	return file->path;
}

FtpLogFetcher::RemoteFile* FtpLogFetcher::match_by_size(uint32_t size_bytes)
{
	if (size_bytes == 0) {
		return nullptr;
	}

	RemoteFile* match = nullptr;

	for (auto& file : _index) {
		if (file.claimed || file.size_bytes.value_or(0) != size_bytes) {
			continue;
		}

		if (match != nullptr) {
			// Two logs of exactly the same size, let the caller's fallback sort it out
			return nullptr;
		}

		match = &file;
	}

	return match;
}

FtpLogFetcher::RemoteFile* FtpLogFetcher::match_px4(const mavsdk::LogFiles::Entry& entry)
{
	// PX4 reports the file mtime in LOG_ENTRY, which is when the log was last written to.
	// The log that was open at that moment is the newest one started at or before it.
	const auto modified_time = parse_iso8601_utc(entry.date);

	if (!modified_time.has_value()) {
		return nullptr;
	}

	RemoteFile* match = nullptr;

	for (auto& file : _index) {
		if (file.claimed || !file.start_time.has_value() || file.start_time.value() > modified_time.value()) {
			continue;
		}

		if (match == nullptr || file.start_time.value() > match->start_time.value()) {
			match = &file;
		}
	}

	return match;
}

FtpLogFetcher::RemoteFile* FtpLogFetcher::match_ardupilot(const mavsdk::LogFiles::Entry& entry)
{
	// ArduPilot reports a list entry number, not the log number in the file name
	if (entry.id == 0 || entry.id > _ardupilot_order.size()) {
		return nullptr;
	}

	RemoteFile* file = &_index[_ardupilot_order[entry.id - 1]];

	return file->claimed ? nullptr : file;
}

void FtpLogFetcher::release(const std::string& remote_path)
{
	for (auto& file : _index) {
		if (file.path == remote_path) {
			file.claimed = false;
			return;
		}
	}
}

bool FtpLogFetcher::download(const std::string& remote_path, const std::string& local_path, uint32_t expected_size,
			     const FtpLogFetcher::ProgressCallback& progress)
{
	std::error_code ec;
	fs::create_directories(_settings.temp_directory, ec);

	// MAVSDK writes to <local_dir>/<remote basename>, so stage the transfer and move it
	// into place afterwards. A partial file never appears in the logs directory.
	const std::string staged_path = join_path(_settings.temp_directory, fs::path(remote_path).filename().string());
	fs::remove(staged_path, ec);

	struct Transfer {
		std::promise<mavsdk::Ftp::Result> promise;
		std::atomic<bool> settled {false};
	};

	// Shared so the callback stays valid if we bail out early on shutdown
	auto transfer = std::make_shared<Transfer>();
	auto future_result = transfer->promise.get_future();

	_ftp->download_async(remote_path, _settings.temp_directory, _settings.use_burst,
	[transfer, progress, this](mavsdk::Ftp::Result result, mavsdk::Ftp::ProgressData data) {
		if (transfer->settled) {
			return;
		}

		if (_should_exit) {
			transfer->settled = true;
			transfer->promise.set_value(mavsdk::Ftp::Result::Timeout);
			return;
		}

		if (result == mavsdk::Ftp::Result::Next) {
			if (progress) {
				progress(data.bytes_transferred, data.total_bytes);
			}

			return;
		}

		transfer->settled = true;
		transfer->promise.set_value(result);
	});

	const auto result = future_result.get();

	if (result != mavsdk::Ftp::Result::Success) {
		LOG_DEBUG("FTP download of " << remote_path << " failed: " << result);
		fs::remove(staged_path, ec);
		return false;
	}

	if (!fs::exists(staged_path)) {
		LOG("FTP reported success but " << staged_path << " is missing");
		return false;
	}

	// The vehicle picked the file, we picked which entry it belongs to. The size is the
	// only thing tying the two together, so refuse anything that does not line up.
	const uint32_t downloaded_size = static_cast<uint32_t>(fs::file_size(staged_path, ec));

	if (expected_size != 0 && downloaded_size != expected_size) {
		LOG("Size mismatch for " << remote_path << ": got " << downloaded_size << " bytes, expected " << expected_size);
		fs::remove(staged_path, ec);
		return false;
	}

	if (staged_path == local_path) {
		return true;
	}

	fs::remove(local_path, ec);
	fs::rename(staged_path, local_path, ec);

	if (ec) {
		// Different filesystems, fall back to a copy
		ec.clear();
		fs::copy_file(staged_path, local_path, fs::copy_options::overwrite_existing, ec);
		std::error_code remove_ec;
		fs::remove(staged_path, remove_ec);

		if (ec) {
			LOG("Failed to move " << staged_path << " to " << local_path << ": " << ec.message());
			return false;
		}
	}

	return true;
}
