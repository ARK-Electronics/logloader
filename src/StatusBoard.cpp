#include "StatusBoard.hpp"

StatusBoard::Snapshot StatusBoard::get() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _snapshot;
}

// Callers hold _mutex.
void StatusBoard::bump()
{
	_snapshot.revision++;
	_cv.notify_all();
}

void StatusBoard::set_connected(bool connected)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_snapshot.connected = connected;
	bump();
}

void StatusBoard::set_armed(bool armed)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_snapshot.armed = armed;
	bump();
}

void StatusBoard::set_logging(bool logging)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_snapshot.logging = logging;
	bump();
}

void StatusBoard::set_ftp(bool available, const std::string& log_root)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_snapshot.ftp_available = available;
	_snapshot.log_root = log_root;
	bump();
}

void StatusBoard::set_download(int64_t id, uint32_t transferred, uint32_t total)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_snapshot.downloading_id = id;
	_snapshot.downloaded_bytes = transferred;
	_snapshot.download_total_bytes = total;
	bump();
}

void StatusBoard::set_upload(int64_t id, const std::string& target)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_snapshot.uploading_id = id;
	_snapshot.uploading_target = target;
	bump();
}

void StatusBoard::notify()
{
	std::lock_guard<std::mutex> lock(_mutex);
	bump();
}

StatusBoard::Snapshot StatusBoard::wait_for_change(uint64_t known_revision,
		std::chrono::milliseconds timeout) const
{
	std::unique_lock<std::mutex> lock(_mutex);
	_cv.wait_for(lock, timeout, [this, known_revision] {
		return _stopped || _snapshot.revision != known_revision;
	});
	return _snapshot;
}

void StatusBoard::shutdown()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_stopped = true;
	}

	_cv.notify_all();
}

bool StatusBoard::stopped() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _stopped;
}
