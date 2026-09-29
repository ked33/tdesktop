#pragma once

#include <algorithm>
#include <cstdint>

namespace Storage {

class DownloadRateLimiter final {
public:
	void configure(int target, int burstBytes, std::int64_t now);
	void consume(int bytes, std::int64_t now);
	void suspend(std::int64_t until, std::int64_t recoverAt);
	void penalize(std::int64_t until, std::int64_t recoverAt);
	[[nodiscard]] std::int64_t delay(int bytes, std::int64_t now);
	[[nodiscard]] int rate() const { return _rate; }
	[[nodiscard]] int ceiling() const { return _ceiling; }
	[[nodiscard]] static int Target(int playback, int maximumKiB);

private:
	void refill(std::int64_t now);

	int _rate = 0;
	int _capacity = 0;
	int _ceiling = 0;
	std::int64_t _credit = 0;
	std::int64_t _updated = 0;
	std::int64_t _recoverAt = 0;

};

inline int DownloadRateLimiter::Target(int playback, int maximumKiB) {
	const auto automatic = (playback > 0)
		? std::clamp<std::int64_t>(
			std::int64_t(playback) * 5 / 4,
			128 * 1024,
			64 * 1024 * 1024)
		: 512 * 1024;
	return int((maximumKiB > 0)
		? std::min(automatic, std::int64_t(maximumKiB) * 1024)
		: automatic);
}

inline void DownloadRateLimiter::refill(std::int64_t now) {
	if (now > _updated) {
		if (_rate > 0) {
			const auto capacity = std::int64_t(_capacity) * 1000;
			const auto refillTime = (capacity - _credit + _rate - 1)
				/ _rate;
			_credit = std::min(
				capacity,
				_credit + std::min(now - _updated, refillTime) * _rate);
		}
		_updated = now;
	}
}

inline void DownloadRateLimiter::configure(
		int target,
		int burstBytes,
		std::int64_t now) {
	refill(now);
	if (_ceiling && now >= _recoverAt) {
		_ceiling = std::min(
			64 * 1024 * 1024,
			_ceiling + std::max(_ceiling / 10, 16 * 1024));
		_recoverAt = now + 30000;
	}
	const auto initialized = (_rate > 0);
	_rate = std::max(1, _ceiling ? std::min(target, _ceiling) : target);
	_capacity = std::max(128 * 1024, burstBytes);
	_credit = initialized
		? std::min(_credit, std::int64_t(_capacity) * 1000)
		: std::int64_t(_capacity) * 1000;
}

inline std::int64_t DownloadRateLimiter::delay(
		int bytes,
		std::int64_t now) {
	refill(now);
	if (!_rate) {
		return 0;
	}
	const auto missing = std::max(
		std::int64_t(bytes) * 1000 - _credit,
		std::int64_t(0));
	return std::max(_updated - now, std::int64_t(0))
		+ (missing + _rate - 1) / _rate;
}

inline void DownloadRateLimiter::consume(int bytes, std::int64_t now) {
	refill(now);
	_credit -= std::int64_t(bytes) * 1000;
}

inline void DownloadRateLimiter::penalize(
		std::int64_t until,
		std::int64_t recoverAt) {
	if (!_rate) {
		return;
	}
	_ceiling = std::max(16 * 1024, _rate * 3 / 4);
	_rate = std::min(_rate, _ceiling);
	suspend(until, recoverAt);
}

inline void DownloadRateLimiter::suspend(
		std::int64_t until,
		std::int64_t recoverAt) {
	_credit = 0;
	_updated = std::max(_updated, until);
	_recoverAt = std::max(_recoverAt, recoverAt);
}

} // namespace Storage
