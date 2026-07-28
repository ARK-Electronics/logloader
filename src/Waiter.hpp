#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

// An interruptible sleep. A loop that would otherwise sit out its whole
// interval can be woken as soon as there is something to do (a new request from
// the API, say) or told to stop.
//
// Each loop owns its own Waiter, so a wake meant for one cannot be consumed by
// the other.
class Waiter
{
public:
	// Sleeps for at most duration. Returns true when the caller should stop.
	template <typename Rep, typename Period>
	bool wait(std::chrono::duration<Rep, Period> duration)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		_cv.wait_for(lock, duration, [this] { return _stop || _wake; });
		_wake = false;
		return _stop;
	}

	void wake()
	{
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_wake = true;
		}

		_cv.notify_all();
	}

	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_stop = true;
		}

		_cv.notify_all();
	}

	bool stopped() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _stop;
	}

private:
	mutable std::mutex _mutex;
	std::condition_variable _cv;
	bool _wake {false};
	bool _stop {false};
};
