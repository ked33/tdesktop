/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace Api::PornSearchPolicy {

class RequestGate final {
public:
	using Time = std::int64_t;

	[[nodiscard]] bool canStart(Time now, std::size_t active, int limit) const;
	[[nodiscard]] Time delay(Time now) const;
	[[nodiscard]] Time pauseRemaining(Time now) const;
	[[nodiscard]] Time takePauseNotice(Time now);
	[[nodiscard]] bool waiting(Time now) const;
	[[nodiscard]] bool finishPause(Time now);
	void setInterval(Time interval);
	void started(Time now);
	void pause(Time now, Time seconds);

private:
	Time _lastRequestAt = -1;
	Time _interval = 500;
	Time _pausedUntil = 0;
	bool _pauseNoticePending = false;

};

inline bool RequestGate::canStart(
		Time now,
		std::size_t active,
		int limit) const {
	return limit > 0 && active < std::size_t(limit) && !delay(now);
}

inline RequestGate::Time RequestGate::delay(Time now) const {
	const auto next = (_lastRequestAt < 0) ? now : _lastRequestAt + _interval;
	return std::max({ next, _pausedUntil, now }) - now;
}

inline RequestGate::Time RequestGate::pauseRemaining(Time now) const {
	return std::max(_pausedUntil - now, Time(0));
}

inline RequestGate::Time RequestGate::takePauseNotice(Time now) {
	if (!_pauseNoticePending) {
		return 0;
	}
	_pauseNoticePending = false;
	return pauseRemaining(now);
}

inline bool RequestGate::waiting(Time now) const {
	return _pausedUntil > now;
}

inline bool RequestGate::finishPause(Time now) {
	if (!_pausedUntil || waiting(now)) {
		return false;
	}
	_pausedUntil = 0;
	_pauseNoticePending = false;
	return true;
}

inline void RequestGate::setInterval(Time interval) {
	_interval = std::max(interval, Time(0));
}

inline void RequestGate::started(Time now) {
	_lastRequestAt = now;
}

inline void RequestGate::pause(Time now, Time seconds) {
	_pauseNoticePending |= !waiting(now);
	const auto maximum = (std::numeric_limits<Time>::max() - now) / 1000;
	const auto duration = std::clamp(seconds, Time(1), maximum) * 1000;
	_pausedUntil = std::max(_pausedUntil, now + duration);
}

class ElapsedTime final {
public:
	using Time = std::int64_t;

	void setRunning(Time now, bool running);
	[[nodiscard]] Time elapsed(Time now) const;
	[[nodiscard]] bool running() const;

private:
	Time _elapsed = 0;
	Time _started = 0;
	bool _running = false;

};

inline void ElapsedTime::setRunning(Time now, bool running) {
	if (_running == running) {
		return;
	}
	if (_running) {
		_elapsed += now - _started;
	} else {
		_started = now;
	}
	_running = running;
}

inline ElapsedTime::Time ElapsedTime::elapsed(Time now) const {
	return _elapsed + (_running ? now - _started : 0);
}

inline bool ElapsedTime::running() const {
	return _running;
}

[[nodiscard]] inline std::string FormatDuration(std::int64_t seconds) {
	seconds = std::max(seconds, std::int64_t(0));
	return (seconds < 60 ? std::string() : std::to_string(seconds / 60) + 'm')
		+ std::to_string(seconds % 60) + 's';
}

[[nodiscard]] inline bool ExactCountReached(
		std::size_t loaded,
		int exactCount) {
	return exactCount >= 0 && loaded >= std::size_t(exactCount);
}

[[nodiscard]] inline bool PageAdvanced(
		std::int64_t previous,
		std::int64_t next) {
	return next > 0 && (!previous || next < previous);
}

template <typename Item, typename Key, typename Date>
[[nodiscard]] std::vector<Item> MergeResults(
		const std::vector<Item> &native,
		const std::vector<Item> &additional,
		bool enabled,
		Key key,
		Date date) {
	auto result = std::vector<Item>();
	result.reserve(native.size() + (enabled ? additional.size() : 0));
	auto seen = std::set<std::decay_t<std::invoke_result_t<Key, Item>>>();
	const auto append = [&](const std::vector<Item> &list) {
		for (const auto &item : list) {
			if (seen.insert(key(item)).second) {
				result.push_back(item);
			}
		}
	};
	append(native);
	if (enabled) {
		append(additional);
		std::sort(result.begin(), result.end(), [&](const auto &a, const auto &b) {
			if (date(a) != date(b)) {
				return date(a) > date(b);
			}
			const auto first = key(a);
			const auto second = key(b);
			return (first.peer != second.peer)
				? first.peer < second.peer
				: first.msg > second.msg;
		});
	}
	return result;
}

} // namespace Api::PornSearchPolicy
