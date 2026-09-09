/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_startup.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <iostream>

namespace {

using Media::Streaming::StartupBufferPolicy;

struct BufferSample {
	std::int64_t elapsed = 0;
	std::int64_t audio = 0;
	std::int64_t video = 0;
};

auto TotalChecks = 0;

void Check(bool condition, const char *name) {
	++TotalChecks;
	if (!condition) {
		std::cerr << "FAILED: " << name << '\n';
		std::exit(1);
	}
}

[[nodiscard]] std::int64_t FirstPlayable(
		bool seek,
		std::initializer_list<BufferSample> samples) {
	for (const auto &sample : samples) {
		const auto required = StartupBufferPolicy::RequiredBuffer(sample.elapsed, seek);
		if (std::min(sample.audio, sample.video) >= required) {
			return sample.elapsed;
		}
	}
	return -1;
}

void CheckStartupWaits() {
	for (const auto seek : { false, true }) {
		const auto deadline = StartupBufferPolicy::ExtraWaitLimit(seek);
		Check(
			FirstPlayable(seek, { { 0, 2000, 2000 } }) == 0,
			"cached startup never adds a fixed delay");
		Check(
			FirstPlayable(seek, { { 0, 200, 300 }, { 300, 1500, 1700 } }) == 300,
			"healthy buffering starts as soon as the target is available");
		Check(
			FirstPlayable(seek, {
				{ 0, 600, 600 },
				{ deadline - 1, 700, 900 },
				{ deadline, 700, 900 },
			}) == deadline,
			"slow network cannot impose unbounded extra target buffering");
		Check(
			FirstPlayable(seek, { { deadline, 499, 5000 } }) == -1,
			"video buffer cannot hide missing audio after deadline");
		Check(
			FirstPlayable(seek, { { deadline, 5000, 499 } }) == -1,
			"audio buffer cannot hide missing video after deadline");
		Check(
			FirstPlayable(seek, {
				{ deadline, 100, 200 },
				{ deadline + 900, 500, 700 },
			}) == deadline + 900,
			"necessary network data is still awaited after the extra-wait limit");
		Check(
			FirstPlayable(seek, { { deadline + 10000, 0, 0 } }) == -1,
			"deadline does not force playback with no data");
	}
	Check(
		StartupBufferPolicy::ExtraWaitLimit(true)
			< StartupBufferPolicy::ExtraWaitLimit(false),
		"interactive seek has a shorter optional buffering wait");
}

} // namespace

int main() {
	CheckStartupWaits();
	std::cout << "Streaming startup regression: "
		<< TotalChecks << " checks passed.\n";
	return 0;
}
