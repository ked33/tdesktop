/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_mp4_header.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {

using namespace Media::Streaming::Mp4;
using Bytes = std::vector<char>;

auto TotalChecks = 0;

void Check(bool condition, const char *name) {
	++TotalChecks;
	if (!condition) {
		std::cerr << "FAILED: " << name << '\n';
		std::exit(1);
	}
}

[[nodiscard]] Bytes Number(std::uint64_t value, int width) {
	auto result = Bytes();
	for (auto i = width - 1; i >= 0; --i) {
		result.push_back(char((value >> (8 * i)) & 0xFF));
	}
	return result;
}

void Append(Bytes &to, const Bytes &from) {
	to.insert(to.end(), from.begin(), from.end());
}

[[nodiscard]] Bytes Join(std::initializer_list<Bytes> parts) {
	auto result = Bytes();
	for (const auto &part : parts) {
		Append(result, part);
	}
	return result;
}

[[nodiscard]] Bytes Atom(
		std::string_view type,
		const Bytes &payload = {},
		bool extended = false) {
	const auto size = payload.size() + (extended ? 16 : 8);
	auto result = Number(extended ? 1 : size, 4);
	result.insert(result.end(), type.begin(), type.end());
	if (extended) {
		Append(result, Number(size, 8));
	}
	Append(result, payload);
	return result;
}

[[nodiscard]] StreamingHeader Probe(
		const Bytes &data,
		std::int64_t fileSize = -1,
		int failAfter = -1) {
	auto calls = 0;
	return ProbeForStreaming(
		(fileSize < 0) ? std::int64_t(data.size()) : fileSize,
		[&](std::int64_t offset, std::span<char> buffer) {
			Check(++calls <= 64, "bounded header reads");
			Check(buffer.size() <= 16, "only atom headers are read");
			Check(offset >= 0, "nonnegative read offset");
			if (calls > failAfter && failAfter >= 0) {
				return false;
			}
			const auto start = std::uint64_t(offset);
			Check(start <= data.size(), "read starts inside fixture");
			Check(
				buffer.size() <= data.size() - start,
				"read ends inside fixture");
			std::copy_n(data.data() + start, buffer.size(), buffer.data());
			return true;
		});
}

void CheckRanges(
		const Bytes &original,
		const HeaderPatch &patch,
		const Bytes &expected) {
	for (auto offset = std::size_t(0); offset <= original.size(); ++offset) {
		for (auto size = std::size_t(0)
			; size <= original.size() - offset
			; ++size) {
			auto buffer = Bytes(
				original.data() + offset,
				original.data() + offset + size);
			patch.apply(std::int64_t(offset), buffer);
			Check(
				std::equal(buffer.begin(), buffer.end(), expected.data() + offset),
				"consistent partial ranges");
			patch.apply(std::int64_t(offset), buffer);
			Check(
				std::equal(buffer.begin(), buffer.end(), expected.data() + offset),
				"idempotent patch");
		}
	}
}

void CheckNoPatch(const Bytes &data, Layout layout = Layout::Regular) {
	const auto header = Probe(data);
	Check(header.layout == layout, "unpatched layout");
	Check(header.patch.size == 0, "unpatched size");
	auto copy = data;
	header.patch.apply(0, copy);
	Check(copy == data, "unpatched contents");
}

void TestRegularLayouts() {
	const auto ftyp = Atom("ftyp", Bytes(16));
	const auto track = Atom("trak", Bytes{ 'm', 'v', 'e', 'x' });
	for (const auto extendedMoov : { false, true }) {
		const auto moov = Atom("moov", track, extendedMoov);
		for (const auto extendedMdat : { false, true }) {
			const auto media = Atom("mdat", Bytes(16, 'a'), extendedMdat);
			const auto source = Join({
				ftyp,
				moov,
				media,
				Atom("mdat", Bytes(16, 'b')),
			});
			const auto firstMdat = ftyp.size() + moov.size();
			const auto header = Probe(source);
			Check(header.layout == Layout::Regular, "regular front moov");
			Check(
				header.patch.offset == std::int64_t(firstMdat
					+ (extendedMdat ? 8 : 0)),
				"correct size field offset");
			Check(
				header.patch.size == (extendedMdat ? 8 : 4),
				"correct size field width");
			const auto replacement = Number(
				source.size() - firstMdat,
				header.patch.size);
			auto expected = source;
			std::copy(
				replacement.begin(),
				replacement.end(),
				expected.data() + header.patch.offset);
			CheckRanges(source, header.patch, expected);
			CheckNoPatch(Join({ ftyp, moov, media }));
		}
	}
	const auto moov = Atom("moov", track);
	auto media = Atom("mdat", Bytes(16, 'a'));
	std::fill_n(media.begin(), 4, char(0));
	CheckNoPatch(Join({ ftyp, moov, media }));
	CheckNoPatch(Join({ ftyp, media, moov }));
	CheckNoPatch(Join({ ftyp, moov, moov, media }));
	CheckNoPatch(Join({ ftyp, Atom("moov"), media }));
	CheckNoPatch(Join({ ftyp, moov }));
	auto zeroTrack = track;
	std::fill_n(zeroTrack.begin(), 4, char(0));
	const auto zeroTrackHeader = Probe(Join({
		ftyp,
		Atom("moov", zeroTrack),
		Atom("mdat", Bytes(16)),
		Atom("mdat", Bytes(16)),
	}));
	Check(zeroTrackHeader.patch.size == 4, "zero size child atom");
}

void TestLargeAndFragmentedLayouts() {
	const auto ftyp = Atom("ftyp", Bytes(16), true);
	const auto media = Atom("mdat", Bytes(16));
	const auto largeTrack = Atom("trak", Bytes(3 * 1024 * 1024));
	const auto largeMoov = Atom("moov", largeTrack);
	const auto prefix = Join({ ftyp, Atom("free"), largeMoov });
	const auto source = Join({ prefix, media, media });
	const auto header = Probe(source);
	Check(header.layout == Layout::LargeFrontMoov, "large indexed moov");
	Check(
		header.patch.offset == std::int64_t(prefix.size()),
		"mdat after large moov");
	Check(header.patch.size == 4, "large moov patch");
	for (const auto extended : { false, true }) {
		const auto moov = Atom(
			"moov",
			Join({ largeTrack, Atom("mvex", Bytes(4), extended) }),
			extended);
		CheckNoPatch(Join({ ftyp, moov, media }), Layout::Fragmented);
	}
	const auto moov = Atom("moov", Atom("trak"));
	CheckNoPatch(
		Join({ ftyp, moov, Atom("moof"), media }),
		Layout::Fragmented);
	CheckNoPatch(Join({ ftyp, Atom("moof"), media }), Layout::Fragmented);
	CheckNoPatch(
		Join({ Atom("moov", Join({ Atom("mvex"), Atom("trak") })), media }),
		Layout::Fragmented);
}

void TestWideSizes() {
	const auto prefix = Join({ Atom("ftyp"), Atom("moov", Atom("trak")) });
	for (const auto fileSize : {
		std::int64_t(1) << 32,
		std::int64_t(1) << 33,
		std::numeric_limits<std::int64_t>::max(),
	}) {
		for (const auto extended : { false, true }) {
			const auto source = Join({
				prefix,
				Atom("mdat", Bytes(16), extended),
			});
			const auto header = Probe(source, fileSize);
			const auto mediaSize = std::uint64_t(
				fileSize - std::int64_t(prefix.size()));
			const auto expectedSize = (extended
				|| mediaSize <= std::numeric_limits<std::uint32_t>::max())
				? mediaSize
				: 0;
			Check(header.patch.size == (extended ? 8 : 4), "wide file patch");
			const auto expected = Number(expectedSize, header.patch.size);
			Check(
				std::equal(
					expected.begin(),
					expected.end(),
					header.patch.bytes.begin()),
				"wide mdat size or EOF marker");
		}
	}
	auto bytes = Bytes(4, 'x');
	const auto patch = HeaderPatch{
		.offset = std::numeric_limits<std::int64_t>::max() - 2,
		.bytes = { 'a', 'b', 'c', 'd' },
		.size = 4,
	};
	patch.apply(std::numeric_limits<std::int64_t>::max(), bytes);
	Check(bytes == Bytes{ 'c', 'd', 'x', 'x' }, "near maximum offset");
	patch.apply(0, bytes);
	Check(bytes == Bytes{ 'c', 'd', 'x', 'x' }, "distant range unchanged");
	HeaderPatch().apply(std::numeric_limits<std::int64_t>::max(), bytes);
	Check(
		bytes == Bytes{ 'c', 'd', 'x', 'x' },
		"empty patch at maximum offset");
	for (const auto size : { -1, 0, 9 }) {
		HeaderPatch{ .offset = 0, .size = size }.apply(0, bytes);
		Check(bytes == Bytes{ 'c', 'd', 'x', 'x' }, "invalid patch size");
	}
	patch.apply(-1, bytes);
	Check(bytes == Bytes{ 'c', 'd', 'x', 'x' }, "negative range offset");
}

void TestInvalidAndBoundedReads() {
	const auto moov = Atom("moov", Atom("trak"));
	const auto media = Atom("mdat", Bytes(16));
	const auto source = Join({ Atom("ftyp"), moov, media, media });
	for (auto count = 0; count < 4; ++count) {
		Check(
			Probe(source, -1, count).patch.size == 0,
			"read failure fallback");
	}
	for (auto length = std::size_t(0); length < moov.size(); ++length) {
		CheckNoPatch(Bytes(moov.data(), moov.data() + length));
	}
	for (const auto badSize : { 2, 7, 15, 1024 }) {
		auto invalid = source;
		const auto size = Number(badSize, 4);
		std::copy(size.begin(), size.end(), invalid.data() + 16);
		CheckNoPatch(invalid);
	}
	for (const auto badSize : {
		std::uint64_t(7),
		std::uint64_t(15),
		std::numeric_limits<std::uint64_t>::max(),
	}) {
		auto invalid = Atom("moov", Atom("trak"), true);
		const auto size = Number(badSize, 8);
		std::copy(size.begin(), size.end(), invalid.data() + 8);
		Append(invalid, media);
		CheckNoPatch(invalid);
	}
	auto truncated = Atom("moov", {}, true);
	truncated.resize(15);
	CheckNoPatch(truncated);
	auto incomplete = Atom("moov", Join({ Atom("trak"), Bytes(7) }));
	Append(incomplete, media);
	CheckNoPatch(incomplete);
	auto many = Bytes();
	for (auto i = 0; i != 80; ++i) {
		Append(many, Atom("free"));
	}
	CheckNoPatch(Join({ many, source }));
	CheckNoPatch(Join({ Atom("moov", Join({ Atom("trak"), many })), media }));
	for (const auto size : { std::int64_t(-1), std::int64_t(0) }) {
		const auto header = ProbeForStreaming(
			size,
			[](std::int64_t, std::span<char>) {
				Check(false, "empty input must not read");
				return false;
			});
		Check(header.patch.size == 0, "empty input fallback");
	}
}

} // namespace

int main() {
	TestRegularLayouts();
	TestLargeAndFragmentedLayouts();
	TestWideSizes();
	TestInvalidAndBoundedReads();
	std::cout << "MP4 header regression: "
		<< TotalChecks << " checks passed.\n";
	return 0;
}
