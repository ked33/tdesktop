/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_playback_prefetch.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using namespace Media::Streaming;

auto TotalChecks = 0;

void Check(bool condition, const char *name) {
	++TotalChecks;
	if (!condition) {
		std::cerr << "FAILED: " << name << '\n';
		std::exit(1);
	}
}

void CheckBufferPolicy() {
	auto policy = PlaybackPrefetchPolicy();
	auto buffer = PlaybackBufferState{
		.generation = 1,
		.sampledAt = 1000,
		.audio = { 10000, 11000, 100000 },
		.video = { 10000, 15000, 100000 },
	};
	Check(policy.update(buffer, 1000) == std::array{ true, false },
		"only the underfilled track is promoted");
	buffer.audio.receivedTill = 13000;
	Check(policy.update(buffer, 1000)[0], "refill survives the low watermark");
	buffer.audio.receivedTill = 14000;
	Check(!policy.update(buffer, 1000)[0], "refill stops at the high watermark");
	buffer.audio.receivedTill = 13000;
	Check(!policy.update(buffer, 1000)[0], "hysteresis prevents flapping");
	buffer.speed = 2.;
	Check(policy.update(buffer, 1000)[0], "buffer duration accounts for speed");
	buffer.speed = 1.;
	buffer.generation = 2;
	Check(!policy.update(buffer, 1000)[0], "new seek clears previous hysteresis");
	buffer.audio = { 99000, 100000, 100000 };
	Check(!policy.update(buffer, 1000)[0], "completed tracks need no prefetch");
	buffer.audio = {};
	Check(!policy.update(buffer, 1000)[0], "missing tracks need no prefetch");
	buffer.audio = { 10000, 9990, 100000 };
	Check(policy.update(buffer, 1000)[0], "an underrun still needs prefetch");
	buffer.video = { 10000, 11000, 100000 };
	Check(!policy.update(buffer, 2500)[1], "stale samples disable prefetch");
	Check(!policy.update(buffer, 999)[1], "future samples are rejected");
	buffer.speed = std::numeric_limits<double>::quiet_NaN();
	Check(!policy.update(buffer, 1000)[1], "invalid speed disables prefetch");
}

void CheckPartPlanning() {
	constexpr auto part = PlaybackPrefetchPlan::kPartSize;
	constexpr auto size = 1000 * part;
	auto audio = PlaybackPrefetchPlan();
	audio.append(20 * part + 10, 100, size);
	audio.append(20 * part + 500, 200, size);
	audio.append(30 * part - 10, 20, size);
	Check(audio.count == 3, "overlapping packets share one network part");
	Check(!audio.contains(25 * part), "unrelated chunk gaps are not prefetched");
	Check(audio.contains(29 * part) && audio.contains(30 * part),
		"packets crossing a network boundary include both parts");
	auto video = PlaybackPrefetchPlan();
	video.append(100 * part, 100 * part, size);
	Check(video.count == PlaybackPrefetchPlan::kPartLimit,
		"large samples cannot exceed the plan budget");
	const auto merged = PlaybackPrefetchPolicy::Merge(audio, video, size);
	Check(merged.count == PlaybackPrefetchPlan::kPartLimit, "shared budget is bounded");
	Check(merged.offsets[0] == 20 * part && merged.offsets[1] == 100 * part,
		"the urgent track goes first without starving the other track");
	const auto shared = PlaybackPrefetchPolicy::Merge(audio, audio, size);
	Check(shared.count == audio.count, "interleaved tracks do not duplicate parts");
	auto tail = PlaybackPrefetchPlan();
	tail.append(size - 5, 5, size);
	Check(tail.count == 1, "short final parts are accepted");
	tail.append(size - 5, 6, size);
	tail.append(-1, 1, size);
	tail.append(size, 1, size);
	Check(tail.count == 1, "invalid ranges do not modify a plan");
	constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
	tail.append(maximum - 1, 1, maximum);
	Check(tail.count == 2, "end alignment cannot overflow");
}

void CheckPublication() {
	auto state = PlaybackPrefetchState();
	const auto buffer = PlaybackBufferState{
		.generation = 1,
		.sampledAt = 1000,
		.audio = { 0, 1000, 10000 },
	};
	state.update(buffer);
	const auto first = state.snapshot();
	auto plan = PlaybackPrefetchPlan();
	plan.append(0, 100, 10000);
	state.publish(first.revision, plan);
	Check(state.plan(1000).count == 1, "current plan is published");
	Check(state.plan(2500).count == 0, "a plan expires without new playback data");
	state.update({});
	state.publish(first.revision, plan);
	Check(state.plan(1000).count == 0, "in-flight publication cannot undo pause");
	auto next = buffer;
	next.generation = 2;
	state.update(next);
	state.publish(first.revision, plan);
	Check(state.plan(1000).count == 0, "old seek cannot publish into new playback");
	state.publish(state.snapshot().revision, plan);
	Check(state.plan(1000).count == 1, "new seek can publish its own plan");
	state.update(next);
	Check(state.plan(1000).count == 0, "updated demand retires the old plan");
}

} // namespace

int main() {
	CheckBufferPolicy();
	CheckPartPlanning();
	CheckPublication();
	std::cout << "OK playback prefetch: " << TotalChecks << " checks\n";
}
