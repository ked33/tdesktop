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
#include <type_traits>
#include <vector>

namespace Api::PornSearchPolicy {

class RequestGate final {
public:
	using Time = std::int64_t;

	[[nodiscard]] bool canStart(Time now, std::size_t active, int limit) const;
	[[nodiscard]] Time delay(Time now) const;
	[[nodiscard]] bool waiting(Time now) const;
	[[nodiscard]] bool finishPause(Time now);
	void started(Time now);
	void pause(Time now, Time seconds);

private:
	Time _nextRequestAt = 0;
	Time _pausedUntil = 0;

};

inline bool RequestGate::canStart(
		Time now,
		std::size_t active,
		int limit) const {
	return limit > 0 && active < std::size_t(limit) && !delay(now);
}

inline RequestGate::Time RequestGate::delay(Time now) const {
	return std::max({ _nextRequestAt, _pausedUntil, now }) - now;
}

inline bool RequestGate::waiting(Time now) const {
	return _pausedUntil > now;
}

inline bool RequestGate::finishPause(Time now) {
	if (!_pausedUntil || waiting(now)) {
		return false;
	}
	_pausedUntil = 0;
	return true;
}

inline void RequestGate::started(Time now) {
	constexpr auto kRequestInterval = Time(500);
	_nextRequestAt = now + kRequestInterval;
}

inline void RequestGate::pause(Time now, Time seconds) {
	const auto maximum = (std::numeric_limits<Time>::max() - now) / 1000;
	const auto duration = std::clamp(seconds, Time(1), maximum) * 1000;
	_pausedUntil = std::max(_pausedUntil, now + duration);
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
