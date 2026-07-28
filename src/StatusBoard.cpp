#include "StatusBoard.hpp"

StatusBoard::Snapshot StatusBoard::get() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _snapshot;
}

void StatusBoard::update(const std::function<void(Snapshot&)>& mutate)
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		mutate(_snapshot);
		_snapshot.revision++;
	}

	_cv.notify_all();
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
