#pragma once

#include "storage/download_rate_limiter.h"

#include <cassert>

namespace Storage {

struct MediaPreviewLimits {
	static constexpr auto kConcurrentDefault = 2;
	static constexpr auto kRateDefault = 4;
	static constexpr auto kBurstDefault = 2;
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

	[[nodiscard]] MediaPreviewLimits resolved() const {
		const auto value = normalized();
		return {
			value.concurrent ? value.concurrent : kConcurrentDefault,
			value.requestsPerSecond ? value.requestsPerSecond : kRateDefault,
			value.burst ? value.burst : kBurstDefault,
		};
	}
};

class MediaPreviewRequestLimiter final {
public:
	void configure(MediaPreviewLimits limits, std::int64_t now) {
		const auto value = limits.resolved();
		_concurrent = value.concurrent;
		_rate.configure(
			value.requestsPerSecond * kRequestCost,
			std::int64_t(value.burst) * kRequestCost,
			now);
	}

	[[nodiscard]] bool hasCapacity() const {
		return _active < _concurrent;
	}

	[[nodiscard]] int active() const {
		return _active;
	}

	[[nodiscard]] std::int64_t delay(std::int64_t now) const {
		auto rate = _rate;
		return rate.delay(kRequestCost, now);
	}

	void requestStarted(std::int64_t now) {
		assert(hasCapacity());
		++_active;
		_rate.consume(kRequestCost, now);
	}

	void requestFinished() {
		assert(_active > 0);
		--_active;
	}

	void suspend(std::int64_t until, std::int64_t recoverAt) {
		_rate.suspend(until, recoverAt);
	}

private:
	static constexpr auto kRequestCost = 128 * 1024;

	DownloadRateLimiter _rate;
	int _concurrent = MediaPreviewLimits::kConcurrentDefault;
	int _active = 0;

};

} // namespace Storage
