/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <mutex>

namespace Media::Streaming {

struct PlaybackTrackBuffer {
	std::int64_t position = -1;
	std::int64_t receivedTill = -1;
	std::int64_t duration = -1;

	[[nodiscard]] bool valid() const {
		return position >= 0
			&& position < duration
			&& receivedTill >= 0
			&& duration > receivedTill;
	}
};

struct PlaybackBufferState {
	static constexpr auto kLifetime = std::int64_t(1500);
	std::uint64_t generation = 0;
	std::int64_t sampledAt = 0;
	PlaybackTrackBuffer audio;
	PlaybackTrackBuffer video;
	double speed = 1.;

	[[nodiscard]] bool fresh(std::int64_t now) const {
		return generation != 0
			&& sampledAt > 0
			&& now >= sampledAt
			&& now - sampledAt < kLifetime
			&& std::isfinite(speed)
			&& speed >= 0.25
			&& speed <= 4.;
	}
};

struct PlaybackPrefetchPlan {
	static constexpr auto kPartSize = std::int64_t(128 * 1024);
	static constexpr auto kPartLimit = 32;

	std::array<std::int64_t, kPartLimit> offsets = {};
	int count = 0;

	[[nodiscard]] bool contains(std::int64_t offset) const {
		return std::find(offsets.begin(), offsets.begin() + count, offset)
			!= offsets.begin() + count;
	}

	void append(std::int64_t offset, std::int64_t size, std::int64_t fileSize) {
		if (offset < 0
			|| size <= 0
			|| offset >= fileSize
			|| size > fileSize - offset) {
			return;
		}
		const auto till = offset + size;
		for (auto part = offset / kPartSize * kPartSize
			; part < till && count < kPartLimit
			; part += kPartSize) {
			if (!contains(part)) {
				offsets[count++] = part;
			}
			if (till - part <= kPartSize) {
				break;
			}
		}
	}
};

class PlaybackPrefetchPolicy final {
public:
	static constexpr auto kLowBuffer = std::int64_t(2000);
	static constexpr auto kTargetBuffer = std::int64_t(4000);

	[[nodiscard]] std::array<bool, 2> update(
			const PlaybackBufferState &state,
			std::int64_t now) {
		if (!state.fresh(now) || state.generation != _generation) {
			_refill = {};
			_generation = state.generation;
		}
		const auto tracks = std::array{ state.audio, state.video };
		for (auto i = 0; i != 2; ++i) {
			const auto &track = tracks[i];
			if (!state.fresh(now) || !track.valid()) {
				_refill[i] = false;
				continue;
			}
			const auto buffer = double(std::max(
				track.receivedTill - track.position,
				std::int64_t(0)))
				/ state.speed;
			if (buffer < double(kLowBuffer)) {
				_refill[i] = true;
			} else if (buffer >= double(kTargetBuffer)) {
				_refill[i] = false;
			}
		}
		return _refill;
	}

	[[nodiscard]] static PlaybackPrefetchPlan Merge(
			const PlaybackPrefetchPlan &first,
			const PlaybackPrefetchPlan &second,
			std::int64_t fileSize) {
		auto result = PlaybackPrefetchPlan();
		for (auto i = 0; i < std::max(first.count, second.count); ++i) {
			for (const auto plan : { &first, &second }) {
				if (i < plan->count) {
					const auto offset = plan->offsets[i];
					const auto amount = std::min(
						PlaybackPrefetchPlan::kPartSize,
						fileSize - offset);
					result.append(offset, amount, fileSize);
				}
			}
		}
		return result;
	}

private:
	std::uint64_t _generation = 0;
	std::array<bool, 2> _refill = {};

};

class PlaybackPrefetchState final {
public:
	struct Snapshot {
		PlaybackBufferState buffer;
		std::uint64_t revision = 0;
	};

	void update(PlaybackBufferState buffer) {
		const auto lock = std::lock_guard(_mutex);
		_buffer = buffer;
		_plan = {};
		_revision.fetch_add(1, std::memory_order_release);
	}

	[[nodiscard]] std::uint64_t revision() const {
		return _revision.load(std::memory_order_acquire);
	}

	[[nodiscard]] Snapshot snapshot() const {
		const auto lock = std::lock_guard(_mutex);
		return { _buffer, revision() };
	}

	void publish(std::uint64_t revision, PlaybackPrefetchPlan plan) {
		const auto lock = std::lock_guard(_mutex);
		if (revision == this->revision()) {
			_plan = plan;
		}
	}

	[[nodiscard]] PlaybackPrefetchPlan plan(std::int64_t now) const {
		const auto lock = std::lock_guard(_mutex);
		return _buffer.fresh(now) ? _plan : PlaybackPrefetchPlan();
	}

private:
	mutable std::mutex _mutex;
	PlaybackBufferState _buffer;
	PlaybackPrefetchPlan _plan;
	std::atomic<std::uint64_t> _revision = 0;

};

} // namespace Media::Streaming
