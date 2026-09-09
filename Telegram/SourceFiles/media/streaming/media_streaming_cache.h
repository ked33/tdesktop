/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <cstdint>

namespace Media::Streaming::CachePolicy {

struct SlicePreload {
	int first = 0;
	int second = 0;
};

[[nodiscard]] inline SlicePreload SplitPreload(
		std::uint32_t firstTill,
		bool readCrossesBoundary,
		std::uint32_t sliceSize,
		int partSize,
		int preloadParts) {
	if (partSize <= 0
		|| !sliceSize
		|| sliceSize % partSize
		|| firstTill > sliceSize
		|| preloadParts <= 0) {
		return {};
	} else if (readCrossesBoundary) {
		return { 0, preloadParts };
	}
	const auto first = int(std::min(
		std::uint32_t(preloadParts),
		(sliceSize - firstTill) / partSize));
	return { first, preloadParts - first };
}

template <typename Parts, typename AddPart>
void RestoreHeaderParts(
		const Parts &header,
		std::uint32_t sliceOffset,
		std::uint32_t sliceSize,
		AddPart &&addPart) {
	const auto till = std::uint64_t(sliceOffset) + sliceSize;
	auto i = std::lower_bound(
		header.begin(),
		header.end(),
		sliceOffset,
		[](const auto &part, std::uint32_t offset) {
			return part.first < offset;
		});
	for (; i != header.end() && i->first < till; ++i) {
		addPart(i->first - sliceOffset, i->second);
	}
}

} // namespace Media::Streaming::CachePolicy
