/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_mp4_header.h"

#include <iterator>
#include <vector>

namespace Media::Streaming::Mp4 {

struct IndexRange {
	std::int64_t offset = 0;
	std::vector<char> bytes;
};

struct IndexCache {
	std::vector<IndexRange> ranges;
	std::size_t size = 0;

	[[nodiscard]] std::size_t copy(
		std::int64_t offset,
		std::span<char> buffer) const;

};

inline std::size_t IndexCache::copy(
		std::int64_t offset,
		std::span<char> buffer) const {
	const auto after = std::upper_bound(
		ranges.begin(),
		ranges.end(),
		offset,
		[](std::int64_t value, const IndexRange &range) {
			return value < range.offset;
		});
	if (after == ranges.begin()) {
		return 0;
	}
	const auto &range = *std::prev(after);
	const auto skip = std::uint64_t(offset - range.offset);
	if (skip >= range.bytes.size()) {
		return 0;
	}
	const auto position = std::size_t(skip);
	const auto count = std::min(buffer.size(), range.bytes.size() - position);
	std::copy_n(range.bytes.data() + position, count, buffer.data());
	return count;
}

template <typename Read>
[[nodiscard]] std::optional<IndexCache> BuildIndexCache(
		std::int64_t fileSize,
		Read &&read) {
	constexpr auto kMaximumAtoms = 65536;
	constexpr auto kMaximumBytes = std::size_t(16) * 1024 * 1024;
	constexpr auto kChunkSize = std::size_t(64) * 1024;
	if (fileSize < 8) {
		return std::nullopt;
	}
	auto result = IndexCache();
	auto budget = kMaximumAtoms;
	auto offset = std::int64_t(0);
	auto fragmented = false;
	while (offset < fileSize) {
		const auto atom = details::ReadAtom(offset, fileSize, read, budget);
		if (!atom) {
			return std::nullopt;
		}
		fragmented = fragmented || atom->is("moof");
		const auto headerOnly = atom->is("mdat")
			|| atom->is("free")
			|| atom->is("skip")
			|| atom->is("wide");
		const auto count = std::uint64_t(headerOnly
			? atom->headerSize
			: atom->size);
		if (count > kMaximumBytes - result.size) {
			return std::nullopt;
		}
		auto bytes = std::vector<char>(std::size_t(count));
		for (auto done = std::size_t(0); done < bytes.size();) {
			const auto part = std::span(bytes).subspan(
				done,
				std::min(bytes.size() - done, kChunkSize));
			if (!read(offset + std::int64_t(done), part)) {
				return std::nullopt;
			}
			done += part.size();
		}
		result.size += bytes.size();
		if (!result.ranges.empty()
			&& result.ranges.back().offset
				+ std::int64_t(result.ranges.back().bytes.size()) == offset) {
			auto &previous = result.ranges.back().bytes;
			previous.insert(previous.end(), bytes.begin(), bytes.end());
		} else {
			result.ranges.push_back({ offset, std::move(bytes) });
		}
		offset += atom->size;
	}
	return fragmented ? std::make_optional(std::move(result)) : std::nullopt;
}

} // namespace Media::Streaming::Mp4
