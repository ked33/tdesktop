/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_cache.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <string>

namespace {

namespace Policy = Media::Streaming::CachePolicy;

constexpr auto kPart = std::uint32_t(128 * 1024);
constexpr auto kSlice = 64 * kPart;

auto TotalChecks = 0;

void Check(bool condition, const char *name) {
	++TotalChecks;
	if (!condition) {
		std::cerr << "FAILED: " << name << '\n';
		std::exit(1);
	}
}

void CheckHeaderAfterEviction() {
	constexpr auto kMissingPart = std::uint32_t(502136832);
	constexpr auto kSliceOffset = (kMissingPart / kSlice) * kSlice;
	const auto header = std::map<std::uint32_t, std::string>{
		{ 0, "file header" },
		{ kSliceOffset - kPart, "previous slice" },
		{ kMissingPart, "last media bytes and moov" },
		{ kMissingPart + kPart, "moov continuation" },
		{ kMissingPart + 2 * kPart, "file end" },
		{ kSliceOffset + kSlice, "next slice" },
	};
	auto data = std::map<std::uint32_t, std::string>();
	const auto restore = [&] {
		Policy::RestoreHeaderParts(
			header,
			kSliceOffset,
			kSlice,
			[&](std::uint32_t offset, const std::string &bytes) {
				data.try_emplace(offset, bytes);
			});
	};
	restore();
	Check(data.size() == 3, "only the requested slice is hydrated");
	Check(
		data.at(kMissingPart - kSliceOffset) == "last media bytes and moov",
		"media bytes sharing the moov block are readable");
	data.clear();
	restore();
	Check(data.size() == 3, "evicted header blocks remain reusable");
	Check(
		data.at(kMissingPart - kSliceOffset) == "last media bytes and moov",
		"tail playback does not depend on a second network response");
	data[kMissingPart - kSliceOffset] = "existing data";
	restore();
	Check(
		data.at(kMissingPart - kSliceOffset) == "existing data",
		"hydration preserves data already present in the slice");
	Check(header.size() == 6, "hydration does not consume the header cache");
}

void CheckHeaderBoundaries() {
	constexpr auto kLast = std::numeric_limits<std::uint32_t>::max();
	const auto header = std::map<std::uint32_t, int>{
		{ 0, 1 },
		{ kSlice - kPart, 2 },
		{ kSlice, 3 },
		{ kLast, 4 },
	};
	auto restored = std::map<std::uint32_t, int>();
	const auto collect = [&](std::uint32_t offset, int value) {
		restored.emplace(offset, value);
	};
	Policy::RestoreHeaderParts(header, 0, kSlice, collect);
	Check(restored.size() == 2, "slice end is exclusive");
	Check(restored.contains(0), "file offset zero is retained");
	restored.clear();
	Policy::RestoreHeaderParts(header, kLast - kPart, kSlice, collect);
	Check(
		restored.size() == 1 && restored.at(kPart) == 4,
		"last slice range cannot wrap at the four-gigabyte boundary");
	restored.clear();
	Policy::RestoreHeaderParts(header, kSlice, 0, collect);
	Check(restored.empty(), "empty slice does not hydrate data");
}

void CheckPreloadBudget() {
	const auto remainder = Policy::SplitPreload(
		40 * kPart,
		false,
		kSlice,
		kPart,
		75);
	Check(remainder.first == 24, "first slice uses only its remaining capacity");
	Check(remainder.second == 51, "second slice receives only the unused budget");
	const auto crossing = Policy::SplitPreload(
		kSlice,
		true,
		kSlice,
		kPart,
		75);
	Check(crossing.first == 0, "cross-boundary read adds no preload to first slice");
	Check(crossing.second == 75, "preload follows the end of the actual read");
	const auto unaligned = Policy::SplitPreload(
		kSlice - 1,
		false,
		kSlice,
		kPart,
		8);
	Check(
		unaligned.first == 0 && unaligned.second == 8,
		"partial last block is not counted twice");
	for (const auto parts : std::array{ 0, 1, 8, 32, 64, 75, 128 }) {
		for (auto till = std::uint32_t(1); till <= kSlice; till += kPart - 1) {
			const auto plan = Policy::SplitPreload(
				till,
				false,
				kSlice,
				kPart,
				parts);
			Check(
				plan.first >= 0 && plan.second >= 0
					&& plan.first + plan.second == parts,
				"all slice positions share one bounded preload budget");
			Check(
				std::uint32_t(plan.first) <= (kSlice - till) / kPart,
				"first preload stays within the current slice");
		}
	}
	const auto invalid = Policy::SplitPreload(
		kSlice + 1,
		false,
		kSlice,
		kPart,
		8);
	Check(invalid.first == 0 && invalid.second == 0, "invalid range is rejected");
	const auto empty = Policy::SplitPreload(0, false, kSlice, 0, 8);
	Check(empty.first == 0 && empty.second == 0, "invalid part size is rejected");
}

} // namespace

int main() {
	CheckHeaderAfterEviction();
	CheckHeaderBoundaries();
	CheckPreloadBudget();
	std::cout << "Streaming cache regression: "
		<< TotalChecks << " checks passed.\n";
	return 0;
}
