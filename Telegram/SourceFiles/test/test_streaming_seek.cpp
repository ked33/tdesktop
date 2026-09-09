/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_mp4_seek.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <utility>
#include <vector>

namespace {

using Media::Streaming::ComputeMp4SampleRanges;
using Media::Streaming::Mp4SeekByteRange;
using Media::Streaming::Mp4SeekRanges;

struct ChunkEntry {
	std::uint32_t firstChunk = 0;
	std::uint32_t samplesPerChunk = 0;
};

struct Track {
	std::uint32_t sampleCount = 0;
	std::uint32_t constantSampleSize = 0;
	std::vector<ChunkEntry> stsc;
	std::vector<std::uint64_t> chunkOffsets;
	std::vector<std::uint32_t> sampleSizes;
};

auto TotalChecks = 0;

void Check(bool condition, const char *name) {
	++TotalChecks;
	if (!condition) {
		std::cerr << "FAILED: " << name << '\n';
		std::exit(1);
	}
}

Track ConstantTrack(
		std::vector<std::uint64_t> offsets,
		std::uint32_t perChunk,
		std::uint32_t sampleSize) {
	auto track = Track();
	track.sampleCount = std::uint32_t(offsets.size()) * perChunk;
	track.constantSampleSize = sampleSize;
	track.stsc = { { 1, perChunk } };
	track.chunkOffsets = std::move(offsets);
	return track;
}

bool Covers(const Mp4SeekRanges &plan, Mp4SeekByteRange sample) {
	for (auto i = 0; i != plan.count; ++i) {
		const auto &range = plan.ranges[i];
		if (range.offset <= sample.offset
			&& sample.offset + sample.amount <= range.offset + range.amount) {
			return true;
		}
	}
	return false;
}

void CheckDistantChunks() {
	const auto track = ConstantTrack({ 45000000, 234000000 }, 2, 500);
	const auto plan = ComputeMp4SampleRanges(track, 2, 3, 245000000);
	Check(plan.has_value(), "distant audio chunks have a plan");
	Check(plan->count == 2, "large physical gap stays out of critical ranges");
	Check(plan->bytes == 1000, "two audio samples do not download intervening video");
	Check(plan->ranges[0].offset == 45000500, "first partial chunk starts at sample");
	Check(plan->ranges[0].amount == 500, "first chunk stops at its own end");
	Check(plan->ranges[1].offset == 234000000, "second chunk keeps physical offset");
	Check(plan->ranges[1].amount == 500, "last partial chunk stops at sample");
	Check(!plan->limited, "ordinary distant chunks are fully represented");

	const auto backwards = ConstantTrack({ 234000000, 45000000 }, 2, 500);
	const auto reverse = ComputeMp4SampleRanges(backwards, 2, 3, 245000000);
	Check(reverse && reverse->count == 2, "non-monotonic chunk offsets are supported");
	Check(reverse->ranges[1].offset == 45000000, "backward chunk remains separate");
}

void CheckVariableSamples() {
	auto track = Track();
	track.sampleCount = 7;
	track.stsc = { { 1, 2 }, { 3, 3 } };
	track.chunkOffsets = { 100, 1000000, 2000000 };
	track.sampleSizes = { 10, 20, 30, 40, 50, 60, 70 };
	const auto plan = ComputeMp4SampleRanges(track, 2, 6, 3000000);
	Check(plan && plan->count == 3, "sample-to-chunk changes are respected");
	Check(plan->bytes == 200, "variable samples are summed within each chunk");
	Check(plan->ranges[0].offset == 110, "variable-size prefix is skipped");
	Check(plan->ranges[0].amount == 20, "first partial variable chunk");
	Check(plan->ranges[1].amount == 70, "complete middle variable chunk");
	Check(plan->ranges[2].amount == 110, "last partial variable chunk");
	const auto last = ComputeMp4SampleRanges(track, 6, 7, 3000000);
	Check(last && last->count == 1, "seek can start in a later chunk group");
	Check(last->ranges[0].offset == 2000050, "later group uses global sample sizes");
	Check(last->ranges[0].amount == 130, "later group excludes earlier samples");
}

void CheckMerging() {
	const auto samePart = ConstantTrack({ 100, 1000, 131000 }, 1, 10);
	const auto merged = ComputeMp4SampleRanges(samePart, 1, 3, 200000);
	Check(merged && merged->count == 1, "gaps within one downloaded part can merge");
	Check(merged->bytes == 130910, "merged range ends at last sample");
	const auto differentParts = ConstantTrack({ 131000, 132000 }, 1, 10);
	const auto separate = ComputeMp4SampleRanges(differentParts, 1, 2, 200000);
	Check(separate && separate->count == 2, "gap across parts is not expanded");
	const auto adjacent = ConstantTrack({ 131062, 131072 }, 1, 10);
	const auto joined = ComputeMp4SampleRanges(adjacent, 1, 2, 200000);
	Check(joined && joined->count == 1, "adjacent chunks merge across part boundary");
	Check(joined->bytes == 20, "adjacent chunks introduce no gap");
}

void CheckLimitsAndInvalidData() {
	const auto large = ConstantTrack({ 0 }, 16, 1024 * 1024);
	const auto bounded = ComputeMp4SampleRanges(large, 1, 16, 32 * 1024 * 1024);
	Check(bounded && bounded->limited, "large GOP yields a bounded partial plan");
	Check(bounded->bytes == Mp4SeekRanges::kByteLimit, "byte budget is enforced");
	const auto exact = ComputeMp4SampleRanges(large, 1, 8, 32 * 1024 * 1024);
	Check(exact && !exact->limited, "exact byte budget is a complete plan");

	auto offsets = std::vector<std::uint64_t>();
	for (auto i = 0; i != 40; ++i) {
		offsets.push_back(std::uint64_t(i) * 1024 * 1024);
	}
	const auto sparse = ConstantTrack(std::move(offsets), 1, 10);
	const auto clipped = ComputeMp4SampleRanges(sparse, 1, 40, 64 * 1024 * 1024);
	Check(clipped && clipped->limited, "many sparse chunks yield a partial plan");
	Check(clipped->count == Mp4SeekRanges::kRangeLimit, "range budget is enforced");
	Check(clipped->bytes == 320, "range budget does not merge large holes");

	auto valid = ConstantTrack({ 100 }, 2, 10);
	Check(!ComputeMp4SampleRanges(valid, 0, 1, 1000), "zero sample is rejected");
	Check(!ComputeMp4SampleRanges(valid, 2, 1, 1000), "reversed samples are rejected");
	Check(!ComputeMp4SampleRanges(valid, 1, 3, 1000), "missing samples are rejected");
	Check(!ComputeMp4SampleRanges(valid, 1, 2, 119), "sample past EOF is rejected");
	Check(!ComputeMp4SampleRanges(valid, 1, 2, -1), "invalid file size is rejected");
	Check(ComputeMp4SampleRanges(valid, 1, 2, 120).has_value(), "exact EOF is valid");
	valid.stsc[0].samplesPerChunk = 0;
	Check(!ComputeMp4SampleRanges(valid, 1, 2, 1000), "empty chunks are rejected");
	valid.stsc[0].samplesPerChunk = 2;
	valid.stsc[0].firstChunk = 2;
	Check(
		!ComputeMp4SampleRanges(valid, 1, 2, 1000),
		"missing first chunk is rejected");
	valid.stsc[0].firstChunk = 1;
	valid.chunkOffsets[0] = std::numeric_limits<std::uint64_t>::max();
	Check(!ComputeMp4SampleRanges(valid, 1, 2, 1000), "offset overflow is rejected");
	valid.chunkOffsets[0] = 100;
	valid.constantSampleSize = 0;
	valid.sampleSizes = { 10 };
	Check(!ComputeMp4SampleRanges(valid, 1, 2, 1000), "short size table is rejected");
}

void CheckGeneratedLayouts() {
	auto random = std::mt19937(0x5345454B);
	for (auto iteration = 0; iteration != 400; ++iteration) {
		auto track = Track();
		auto samples = std::vector<Mp4SeekByteRange>();
		track.constantSampleSize = (iteration % 2) ? 4096 : 0;
		const auto chunks = 1 + random() % 40;
		for (auto chunk = std::uint32_t(0); chunk != chunks; ++chunk) {
			const auto count = std::uint32_t(1 + random() % 4);
			if (track.stsc.empty() || track.stsc.back().samplesPerChunk != count) {
				track.stsc.push_back({ chunk + 1, count });
			}
			auto offset = std::uint64_t(random() % 2000) * 131072 + 128;
			track.chunkOffsets.push_back(offset);
			for (auto sample = std::uint32_t(0); sample != count; ++sample) {
				const auto bytes = track.constantSampleSize
					? track.constantSampleSize
					: std::uint32_t(1 + random() % 100000);
				samples.push_back({ offset, bytes });
				track.sampleSizes.push_back(bytes);
				offset += bytes;
				++track.sampleCount;
			}
		}
		const auto first = std::uint32_t(1 + random() % track.sampleCount);
		const auto last = first
			+ std::uint32_t(random() % (track.sampleCount - first + 1));
		const auto plan = ComputeMp4SampleRanges(
			track,
			first,
			last,
			512 * 1024 * 1024);
		Check(plan.has_value(), "generated valid layout has a plan");
		Check(
			plan->bytes <= Mp4SeekRanges::kByteLimit,
			"generated plan obeys byte budget");
		Check(
			plan->count <= Mp4SeekRanges::kRangeLimit,
			"generated plan obeys range budget");
		Check(
			Covers(*plan, samples[first - 1]),
			"generated plan includes initial sample");
		if (!plan->limited) {
			for (auto sample = first; sample <= last; ++sample) {
				Check(
					Covers(*plan, samples[sample - 1]),
					"complete plan covers every requested sample");
			}
		}
	}
}

} // namespace

int main() {
	CheckDistantChunks();
	CheckVariableSamples();
	CheckMerging();
	CheckLimitsAndInvalidData();
	CheckGeneratedLayouts();
	std::cout << "Streaming seek regression: " << TotalChecks << " checks passed.\n";
	return 0;
}
