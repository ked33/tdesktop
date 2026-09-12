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
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace Api::PornSearchPolicy {

template <typename Key, typename Value>
class QueryCache final {
public:
	QueryCache(std::size_t limit, std::size_t budget);

	[[nodiscard]] std::optional<Value> take(const Key &key);
	[[nodiscard]] bool put(Key key, Value value, std::size_t cost);
	template <typename Callback>
	void forEach(Callback callback);
	void clear();
	[[nodiscard]] std::size_t size() const;
	[[nodiscard]] std::size_t cost() const;

private:
	struct Entry {
		Key key;
		Value value;
		std::size_t cost = 0;
	};

	std::list<Entry> _entries;
	std::size_t _limit = 0;
	std::size_t _budget = 0;
	std::size_t _cost = 0;

};

template <typename Key, typename Value>
QueryCache<Key, Value>::QueryCache(std::size_t limit, std::size_t budget)
: _limit(limit)
, _budget(budget) {
}

template <typename Key, typename Value>
std::optional<Value> QueryCache<Key, Value>::take(const Key &key) {
	const auto i = std::find_if(_entries.begin(), _entries.end(), [&](auto &entry) {
		return entry.key == key;
	});
	if (i == _entries.end()) {
		return std::nullopt;
	}
	auto result = std::move(i->value);
	_cost -= i->cost;
	_entries.erase(i);
	return result;
}

template <typename Key, typename Value>
bool QueryCache<Key, Value>::put(Key key, Value value, std::size_t cost) {
	const auto i = std::find_if(_entries.begin(), _entries.end(), [&](auto &entry) {
		return entry.key == key;
	});
	if (i != _entries.end()) {
		_cost -= i->cost;
		_entries.erase(i);
	}
	if (!_limit || cost > _budget) {
		return false;
	}
	while (!_entries.empty()
		&& (_entries.size() >= _limit || _cost > _budget - cost)) {
		_cost -= _entries.back().cost;
		_entries.pop_back();
	}
	_entries.push_front({ std::move(key), std::move(value), cost });
	_cost += cost;
	return true;
}

template <typename Key, typename Value>
template <typename Callback>
void QueryCache<Key, Value>::forEach(Callback callback) {
	for (auto &entry : _entries) {
		callback(entry.value);
	}
}

template <typename Key, typename Value>
void QueryCache<Key, Value>::clear() {
	_entries.clear();
	_cost = 0;
}

template <typename Key, typename Value>
std::size_t QueryCache<Key, Value>::size() const {
	return _entries.size();
}

template <typename Key, typename Value>
std::size_t QueryCache<Key, Value>::cost() const {
	return _cost;
}

template <typename Key, typename Source>
void InvalidateSources(std::map<Key, Source> &sources, Key peer) {
	for (auto &[id, source] : sources) {
		if (id == peer || source.channel == peer) {
			source.changed = true;
		}
	}
}

template <typename Key, typename Source, typename Resolve>
[[nodiscard]] std::size_t RestoreSources(
		std::map<Key, Source> &sources,
		std::map<Key, Source> cached,
		Resolve resolve) {
	auto reused = std::size_t(0);
	for (auto &[peer, source] : sources) {
		const auto i = cached.find(peer);
		if (i != cached.end()
			&& source.channel == i->second.channel
			&& !i->second.changed
			&& std::all_of(
				i->second.messages.begin(),
				i->second.messages.end(),
				resolve)) {
			source = std::move(i->second);
			source.retry |= source.failed;
			source.failed = false;
			++reused;
		}
	}
	return reused;
}

template <typename Key, typename Source>
class SourceSnapshot final {
public:
	using Entries = std::map<Key, Source>;

	template <typename Collect>
	[[nodiscard]] bool prepare(bool complete, Collect collect);
	[[nodiscard]] bool ready() const;
	[[nodiscard]] Entries &entries();
	[[nodiscard]] const Entries &entries() const;

private:
	Entries _entries;
	bool _ready = false;

};

template <typename Key, typename Source>
template <typename Collect>
bool SourceSnapshot<Key, Source>::prepare(bool complete, Collect collect) {
	if (_ready || !complete) {
		return false;
	}
	_entries = collect();
	_ready = true;
	return true;
}

template <typename Key, typename Source>
bool SourceSnapshot<Key, Source>::ready() const {
	return _ready;
}

template <typename Key, typename Source>
auto SourceSnapshot<Key, Source>::entries()
-> Entries & {
	return _entries;
}

template <typename Key, typename Source>
auto SourceSnapshot<Key, Source>::entries() const
-> const Entries & {
	return _entries;
}

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
