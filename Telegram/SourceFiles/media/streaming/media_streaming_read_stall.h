/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Media::Streaming {

class ReadStallPolicy final {
public:
	static constexpr auto kStallTimeout = std::int64_t(4000);
	[[nodiscard]] static std::int64_t RetryDelay(int latencyMs, int jitterMs);

	void setRead(std::int64_t offset, std::int64_t amount, std::int64_t now);
	void progress(std::int64_t offset, std::int64_t amount, std::int64_t now);
	[[nodiscard]] bool contains(std::int64_t part, std::int64_t size) const;
	[[nodiscard]] std::int64_t waitingFor(std::int64_t now) const;
	[[nodiscard]] bool retryReady(
		std::int64_t now,
		std::int64_t sentAt,
		int latencyMs,
		int jitterMs) const;
	void retried(std::int64_t now);

private:
	static constexpr auto kRetryCooldown = std::int64_t(10000);
	static constexpr auto kRetryWindow = std::int64_t(30000);

	std::int64_t _from = -1;
	std::int64_t _till = -1;
	std::int64_t _startedAt = 0;
	std::int64_t _lastRetryAt = -1;
	std::int64_t _previousRetryAt = -1;
	bool _retried = false;

};

inline void ReadStallPolicy::setRead(
		std::int64_t offset,
		std::int64_t amount,
		std::int64_t now) {
	if (offset < 0
		|| amount <= 0
		|| amount > std::numeric_limits<std::int64_t>::max() - offset) {
		offset = -1;
		amount = 0;
	}
	const auto till = offset + amount;
	if (_from != offset || _till != till) {
		_from = offset;
		_till = till;
		_startedAt = now;
		_retried = false;
	}
}

inline void ReadStallPolicy::progress(
		std::int64_t offset,
		std::int64_t amount,
		std::int64_t now) {
	if (contains(offset, amount)) {
		_startedAt = now;
	}
}

inline bool ReadStallPolicy::contains(
		std::int64_t part,
		std::int64_t size) const {
	return _from >= 0
		&& part >= 0
		&& size > 0
		&& part < _till
		&& (part >= _from || size > _from - part);
}

inline std::int64_t ReadStallPolicy::waitingFor(std::int64_t now) const {
	return (_from >= 0) ? std::max(now - _startedAt, std::int64_t(0)) : 0;
}

inline std::int64_t ReadStallPolicy::RetryDelay(
		int latencyMs,
		int jitterMs) {
	return std::clamp(
		4 * std::int64_t(std::max(latencyMs, 0))
			+ 2 * std::int64_t(std::max(jitterMs, 0)),
		kStallTimeout,
		std::int64_t(10000));
}

inline bool ReadStallPolicy::retryReady(
		std::int64_t now,
		std::int64_t sentAt,
		int latencyMs,
		int jitterMs) const {
	const auto delay = RetryDelay(latencyMs, jitterMs);
	return !_retried
		&& waitingFor(now) >= delay
		&& now - sentAt >= delay
		&& (_lastRetryAt < 0 || now - _lastRetryAt >= kRetryCooldown)
		&& (_previousRetryAt < 0 || now - _previousRetryAt >= kRetryWindow);
}

inline void ReadStallPolicy::retried(std::int64_t now) {
	_previousRetryAt = _lastRetryAt;
	_lastRetryAt = now;
	_retried = true;
}

} // namespace Media::Streaming
