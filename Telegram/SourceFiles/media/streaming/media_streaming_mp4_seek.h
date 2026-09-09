/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace Media::Streaming {

struct Mp4SeekByteRange {
	std::uint64_t offset = 0;
	std::uint64_t amount = 0;
};

struct Mp4SeekRanges {
	static constexpr auto kRangeLimit = 32;
	static constexpr auto kByteLimit = std::uint64_t(8 * 1024 * 1024);

	std::array<Mp4SeekByteRange, kRangeLimit> ranges;
	std::uint64_t bytes = 0;
	int count = 0;
	bool limited = false;
};

[[nodiscard]] inline bool AppendMp4SeekRange(
		Mp4SeekRanges &result,
		std::uint64_t offset,
		std::uint64_t amount) {
	constexpr auto kPartSize = std::uint64_t(128 * 1024);
	if (!amount) {
		return true;
	}
	const auto previous = result.count
		? &result.ranges[result.count - 1]
		: nullptr;
	const auto previousTill = previous ? previous->offset + previous->amount : 0;
	const auto merge = previous
		&& offset >= previous->offset
		&& (offset <= previousTill
			|| (previousTill - 1) / kPartSize == offset / kPartSize);
	if (!merge && result.count == Mp4SeekRanges::kRangeLimit) {
		result.limited = true;
		return false;
	}
	const auto additional = merge
		? std::max(previousTill, offset + amount) - previousTill
		: amount;
	const auto accepted = std::min(
		additional,
		Mp4SeekRanges::kByteLimit - result.bytes);
	if (merge) {
		previous->amount += accepted;
	} else if (accepted) {
		result.ranges[result.count++] = { offset, accepted };
	}
	result.bytes += accepted;
	result.limited = (accepted != additional);
	return !result.limited;
}

// A track's consecutive samples can live in distant or reordered chunks.
// Plan each intersecting chunk separately, merging gaps only when the same
// network part already covers them. Limits bound speculative work, not the
// media read: an incomplete plan is safe because required reads take priority.
template <typename Track>
[[nodiscard]] std::optional<Mp4SeekRanges> ComputeMp4SampleRanges(
		const Track &track,
		std::uint32_t firstSample,
		std::uint32_t lastSample,
		std::int64_t fileSize) {
	if (!firstSample
		|| firstSample > lastSample
		|| lastSample > track.sampleCount
		|| fileSize <= 0
		|| track.stsc.empty()
		|| track.stsc.front().firstChunk != 1) {
		return std::nullopt;
	}
	const auto size = std::uint64_t(fileSize);
	const auto first = std::uint64_t(firstSample - 1);
	const auto till = std::uint64_t(lastSample);
	auto result = Mp4SeekRanges();
	auto sampleCursor = std::uint64_t(0);
	for (auto i = std::size_t(0); i != track.stsc.size(); ++i) {
		const auto &entry = track.stsc[i];
		if (!entry.firstChunk || !entry.samplesPerChunk) {
			return std::nullopt;
		}
		const auto chunkFrom = std::uint64_t(entry.firstChunk - 1);
		const auto chunkTill = (i + 1 == track.stsc.size())
			? std::uint64_t(track.chunkOffsets.size())
			: std::uint64_t(track.stsc[i + 1].firstChunk) - 1;
		const auto perChunk = std::uint64_t(entry.samplesPerChunk);
		if (chunkFrom >= chunkTill
			|| chunkTill > track.chunkOffsets.size()
			|| chunkTill - chunkFrom
				> (std::numeric_limits<std::uint64_t>::max() - sampleCursor)
					/ perChunk) {
			return std::nullopt;
		}
		const auto groupTill = sampleCursor + (chunkTill - chunkFrom) * perChunk;
		if (first >= groupTill) {
			sampleCursor = groupTill;
			continue;
		}
		auto chunk = chunkFrom + (std::max(first, sampleCursor) - sampleCursor)
			/ perChunk;
		for (; chunk < chunkTill; ++chunk) {
			const auto chunkSample = sampleCursor + (chunk - chunkFrom) * perChunk;
			if (chunkSample >= till) {
				break;
			}
			const auto fromSample = std::max(first, chunkSample);
			const auto tillSample = std::min(till, chunkSample + perChunk);
			auto offset = std::uint64_t(track.chunkOffsets[chunk]);
			auto amount = std::uint64_t(0);
			if (offset >= size) {
				return std::nullopt;
			}
			if (track.constantSampleSize) {
				const auto skip = (fromSample - chunkSample)
					* track.constantSampleSize;
				if (skip > size - offset) {
					return std::nullopt;
				}
				offset += skip;
				amount = (tillSample - fromSample) * track.constantSampleSize;
				if (amount > size - offset) {
					return std::nullopt;
				}
			} else {
				if (tillSample > track.sampleSizes.size()) {
					return std::nullopt;
				}
				for (auto sample = chunkSample; sample != tillSample; ++sample) {
					const auto bytes = std::uint64_t(track.sampleSizes[sample]);
					if (bytes > size - offset - amount) {
						return std::nullopt;
					}
					if (sample < fromSample) {
						offset += bytes;
					} else {
						amount += bytes;
					}
				}
			}
			if (!AppendMp4SeekRange(result, offset, amount)) {
				return result;
			}
		}
		if (till <= groupTill) {
			return result.count ? std::make_optional(result) : std::nullopt;
		}
		sampleCursor = groupTill;
	}
	return std::nullopt;
}

} // namespace Media::Streaming
