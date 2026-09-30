#pragma once

#include <algorithm>
#include <cstdint>

namespace Storage {

class DownloadRateLimiter final {
public:
	void configure(
		int target,
		std::int64_t burstBytes,
		std::int64_t now,
		int catchUpTarget = 0,
		bool catchUpRequested = false);
	void consume(int bytes, std::int64_t now);
	void suspend(std::int64_t until, std::int64_t recoverAt);
	void penalize(std::int64_t until, std::int64_t recoverAt);
	[[nodiscard]] std::int64_t delay(int bytes, std::int64_t now);
	[[nodiscard]] int rate() const { return _rate; }
	[[nodiscard]] int ceiling() const { return _ceiling; }
	[[nodiscard]] bool catchingUp() const { return _rate > _steadyRate; }
	[[nodiscard]] static int Target(
		int playback,
		int maximumKiB,
		bool catchUp = false,
		bool manual = false,
		int targetPercent = 125);

private:
	static constexpr auto kCatchUpDuration = 8000;
	static constexpr auto kCatchUpInterval = 30000;
	static constexpr auto kCatchUpBytes = 16 * 1024 * 1024;
	static constexpr auto kCatchUpMaximumRate = 8 * 1024 * 1024;
	static constexpr auto kCatchUpStepDuration = 1000;

	void refill(std::int64_t now);

	int _rate = 0;
	int _steadyRate = 0;
	int _catchUpRate = 0;
	int _catchUpRemaining = 0;
	std::int64_t _capacity = 0;
	int _ceiling = 0;
	std::int64_t _credit = 0;
	std::int64_t _updated = 0;
	std::int64_t _recoverAt = 0;
	std::int64_t _catchUpUntil = 0;
	std::int64_t _nextCatchUpAt = 0;
	std::int64_t _catchUpStepAt = 0;

};

inline int DownloadRateLimiter::Target(
		int playback,
		int maximumKiB,
		bool catchUp,
		bool manual,
		int targetPercent) {
	if (manual && maximumKiB > 0) {
		return std::min(maximumKiB, 65536) * 1024;
	}
	const auto automatic = (playback > 0)
		? std::clamp<std::int64_t>(
			std::int64_t(playback) * std::clamp(targetPercent, 100, 1000) / 100,
			128 * 1024,
			64 * 1024 * 1024)
		: 512 * 1024;
	const auto target = catchUp
		? std::min(automatic * 3, std::max(
			automatic,
			std::int64_t(kCatchUpMaximumRate)))
		: automatic;
	return int((maximumKiB > 0)
		? std::min(target, std::int64_t(maximumKiB) * 1024)
		: target);
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
		std::int64_t burstBytes,
		std::int64_t now,
		int catchUpTarget,
		bool catchUpRequested) {
	if (burstBytes <= 0) {
		*this = DownloadRateLimiter();
		return;
	}
	refill(now);
	if (_ceiling && now >= _recoverAt) {
		_ceiling = std::min(
			64 * 1024 * 1024,
			_ceiling + std::max(_ceiling / 10, 16 * 1024));
		_recoverAt = now + 30000;
	}
	const auto initialized = (_rate > 0);
	_steadyRate = std::max(1, _ceiling ? std::min(target, _ceiling) : target);
	const auto maximum = std::max(_steadyRate, _ceiling
		? std::min(catchUpTarget, _ceiling)
		: catchUpTarget);
	if (now >= _catchUpUntil
		|| _catchUpRemaining <= 0
		|| maximum <= _steadyRate) {
		_catchUpUntil = 0;
		_catchUpRate = 0;
	}
	if (catchUpRequested
		&& maximum > _steadyRate
		&& now >= _nextCatchUpAt) {
		_catchUpUntil = now + kCatchUpDuration;
		_nextCatchUpAt = now + kCatchUpInterval;
		_catchUpRemaining = kCatchUpBytes;
		_catchUpRate = maximum;
		_catchUpStepAt = now + kCatchUpStepDuration;
	}
	if (_catchUpUntil > now) {
		if (catchUpRequested) {
			_catchUpRate = maximum;
			_catchUpStepAt = now + kCatchUpStepDuration;
		} else if (now >= _catchUpStepAt) {
			const auto steps = std::min<std::int64_t>(
				(now - _catchUpStepAt) / kCatchUpStepDuration + 1,
				kCatchUpDuration / kCatchUpStepDuration);
			for (auto i = 0; i != steps; ++i) {
				_catchUpRate = std::max(_steadyRate, _catchUpRate * 3 / 4);
			}
			_catchUpStepAt = now + kCatchUpStepDuration;
		}
		_rate = std::clamp(_catchUpRate, _steadyRate, maximum);
	} else {
		_rate = _steadyRate;
	}
	_capacity = std::max(std::int64_t(128 * 1024), std::int64_t(burstBytes));
	_credit = initialized
		? std::min(_credit, _capacity * 1000)
		: _capacity * 1000;
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
	if (!_rate) {
		return;
	}
	refill(now);
	_credit -= std::int64_t(bytes) * 1000;
	if (catchingUp()) {
		_catchUpRemaining = std::max(0, _catchUpRemaining - bytes);
	}
}

inline void DownloadRateLimiter::penalize(
		std::int64_t until,
		std::int64_t recoverAt) {
	if (!_rate) {
		return;
	}
	_ceiling = std::max(16 * 1024, std::min(_rate, _steadyRate) * 3 / 4);
	_rate = std::min(_rate, _ceiling);
	suspend(until, recoverAt);
}

inline void DownloadRateLimiter::suspend(
		std::int64_t until,
		std::int64_t recoverAt) {
	if (!_rate) {
		return;
	}
	_credit = 0;
	_catchUpUntil = 0;
	_catchUpRate = 0;
	_rate = std::min(_rate, _steadyRate);
	_updated = std::max(_updated, until);
	_recoverAt = std::max(_recoverAt, recoverAt);
	_nextCatchUpAt = std::max(_nextCatchUpAt, recoverAt);
}

} // namespace Storage
