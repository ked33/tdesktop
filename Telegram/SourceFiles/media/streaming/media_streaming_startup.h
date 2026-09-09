/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>

namespace Media::Streaming {

struct StartupBufferPolicy {
	static constexpr auto kTargetMs = std::int64_t(1500);
	static constexpr auto kMinimumMs = std::int64_t(500);

	[[nodiscard]] static constexpr std::int64_t ExtraWaitLimit(bool seek) {
		return seek ? 1000 : 1500;
	}

	[[nodiscard]] static constexpr std::int64_t RequiredBuffer(
			std::int64_t elapsed,
			bool seek) {
		return (elapsed < ExtraWaitLimit(seek)) ? kTargetMs : kMinimumMs;
	}
};

} // namespace Media::Streaming
