#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>

namespace Storage {

using AvatarRetryKey = std::array<std::uint64_t, 4>;

struct AvatarRetryState {
	std::int64_t deadline = 0;
	std::int64_t retryAt = 0;
	int failures = 0;
	bool stopped = false;
	bool notified = false;
};

class AvatarRetryPolicy final {
public:
	static constexpr auto kFailureLimit = 3;
	static constexpr auto kWindow = std::int64_t(120000);
	static constexpr auto kCooldown = std::int64_t(300000);
	static constexpr auto kCapacity = std::size_t(1024);

	[[nodiscard]] std::int64_t blockedUntil(
			const AvatarRetryKey &key,
			std::int64_t now) {
		const auto i = _states.find(key);
		if (i == _states.end()) {
			return std::max(now, _overflowUntil);
		}
		auto &state = i->second;
		if (expiresAt(state) <= now) {
			_states.erase(i);
			return std::max(now, _overflowUntil);
		}
		if (!state.stopped && now >= state.deadline) {
			state.stopped = true;
			state.retryAt = std::max(state.retryAt, state.deadline + kCooldown);
		}
		return std::max(now, state.retryAt);
	}

	[[nodiscard]] bool notify(
			const AvatarRetryKey &key,
			std::int64_t now) {
		const auto state = ensure(key, now);
		if (!state || state->notified) {
			return false;
		}
		state->notified = true;
		return true;
	}

	[[nodiscard]] AvatarRetryState failed(
			const AvatarRetryKey &key,
			std::int64_t now,
			std::int64_t retryAt) {
		const auto state = ensure(key, now);
		if (!state) {
			_overflowUntil = std::max(_overflowUntil, retryAt);
			return {
				.deadline = now,
				.retryAt = _overflowUntil,
				.failures = kFailureLimit,
				.stopped = true,
			};
		}
		++state->failures;
		state->retryAt = std::max(state->retryAt, retryAt);
		if (state->stopped
			|| state->failures >= kFailureLimit
			|| state->retryAt >= state->deadline) {
			state->stopped = true;
			state->retryAt = std::max(state->retryAt, now + kCooldown);
		}
		return *state;
	}

	[[nodiscard]] std::int64_t expire(
			const AvatarRetryKey &key,
			std::int64_t now) {
		const auto state = ensure(key, now);
		if (!state) {
			return _overflowUntil;
		}
		state->stopped = true;
		state->retryAt = std::max(state->retryAt, now + kCooldown);
		return state->retryAt;
	}

	void succeeded(const AvatarRetryKey &key) {
		_states.erase(key);
	}

private:
	[[nodiscard]] static std::int64_t expiresAt(const AvatarRetryState &state) {
		return state.stopped ? state.retryAt : state.deadline + kCooldown;
	}

	[[nodiscard]] AvatarRetryState *ensure(
			const AvatarRetryKey &key,
			std::int64_t now) {
		std::erase_if(_states, [=](const auto &entry) {
			return expiresAt(entry.second) <= now;
		});
		const auto i = _states.find(key);
		if (i != _states.end()) {
			return &i->second;
		}
		if (_states.size() >= kCapacity) {
			_overflowUntil = std::max(_overflowUntil, now + kCooldown);
			return nullptr;
		}
		return &_states.emplace(key, AvatarRetryState{
			.deadline = now + kWindow,
		}).first->second;
	}

	std::map<AvatarRetryKey, AvatarRetryState> _states;
	std::int64_t _overflowUntil = 0;

};

} // namespace Storage
