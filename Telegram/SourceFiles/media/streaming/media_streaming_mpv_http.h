/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Media::Streaming::Mpv::Http {

enum class ReadResult {
	Success,
	Waiting,
	Failed,
	Cancelled,
};

template <typename Read, typename Cancelled, typename Wait>
[[nodiscard]] ReadResult ReadChunk(
		Read &&read,
		Cancelled &&cancelled,
		Wait &&wait) {
	while (!cancelled()) {
		const auto result = read();
		if (cancelled()) {
			return ReadResult::Cancelled;
		} else if (result != ReadResult::Waiting) {
			return result;
		} else if (!wait()) {
			return ReadResult::Waiting;
		}
	}
	return ReadResult::Cancelled;
}

} // namespace Media::Streaming::Mpv::Http
