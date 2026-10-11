#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>

namespace Storage {

struct MediaPreviewLimits {
	static constexpr auto kConcurrentMaximum = 32;
	static constexpr auto kRateMaximum = 120;
	static constexpr auto kBurstMaximum = 32;

	int concurrent = 0;
	int requestsPerSecond = 0;
	int burst = 0;

	[[nodiscard]] MediaPreviewLimits normalized() const {
		const auto normalize = [](int value, int maximum) {
			return (value >= 0 && value <= maximum) ? value : 0;
		};
		return {
			normalize(concurrent, kConcurrentMaximum),
			normalize(requestsPerSecond, kRateMaximum),
			normalize(burst, kBurstMaximum),
		};
	}

	[[nodiscard]] bool enabled() const {
		const auto value = normalized();
		return value.concurrent || value.requestsPerSecond;
	}
};

class MediaPreviewRequestLimiter final {
public:
	void configure(MediaPreviewLimits limits, std::int64_t now) {
		const auto value = limits.normalized();
		refill(now);
		if (!value.requestsPerSecond) {
			_credit = 0;
		} else if (!_rate) {
			_credit = (_updated > now)
				? 0
				: std::int64_t(std::max(value.burst, 1)) * kRequestCost;
		}
		_concurrent = value.concurrent;
		_rate = value.requestsPerSecond;
		_burst = value.burst;
		if (_burst) {
			_credit = std::min(_credit, std::int64_t(_burst) * kRequestCost);
		}
	}

	[[nodiscard]] bool hasCapacity() const {
		return !_concurrent || _active < _concurrent;
	}

	[[nodiscard]] bool rateLimited() const {
		return _rate > 0;
	}

	[[nodiscard]] int active() const {
		return _active;
	}

	[[nodiscard]] std::int64_t delay(std::int64_t now) const {
		if (!_rate) {
			return 0;
		}
		auto state = *this;
		state.refill(now);
		const auto missing = std::max(
			kRequestCost - state._credit,
			std::int64_t(0));
		const auto wait = std::max(state._updated - now, std::int64_t(0));
		const auto pacing = (missing + _rate - 1) / _rate;
		return std::min(wait, std::numeric_limits<std::int64_t>::max() - pacing)
			+ pacing;
	}

	void requestStarted(std::int64_t now) {
		assert(hasCapacity());
		assert(!delay(now));
		++_active;
		if (_rate) {
			refill(now);
			_credit -= kRequestCost;
		}
	}

	void requestFinished() {
		assert(_active > 0);
		--_active;
	}

	void suspend(std::int64_t until) {
		_credit = 0;
		_updated = std::max(_updated, until);
	}

private:
	static constexpr auto kRequestCost = std::int64_t(1000);

	void refill(std::int64_t now) {
		if (now <= _updated) {
			return;
		}
		if (_rate) {
			const auto capacity = _burst
				? std::int64_t(_burst) * kRequestCost
				: std::numeric_limits<std::int64_t>::max();
			const auto missing = capacity - _credit;
			const auto fullAfter = missing / _rate + (missing % _rate != 0);
			const auto elapsed = now - _updated;
			_credit = (elapsed >= fullAfter)
				? capacity
				: _credit + elapsed * _rate;
		}
		_updated = now;
	}

	int _concurrent = 0;
	int _rate = 0;
	int _burst = 0;
	int _active = 0;
	std::int64_t _credit = 0;
	std::int64_t _updated = 0;

};

} // namespace Storage
