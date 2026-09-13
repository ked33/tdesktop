/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_mp4_index.h"
#include "media/streaming/media_streaming_mp4_fragment.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace Media::Streaming::Mp4;
using Bytes = std::vector<char>;

auto Checks = 0;

void Check(bool condition, const char *message) {
	++Checks;
	if (!condition) {
		std::cerr << "FAILED: " << message << '\n';
		std::exit(1);
	}
}

void Append(Bytes &data, const Bytes &part) {
	data.insert(data.end(), part.begin(), part.end());
}

[[nodiscard]] Bytes Atom(std::string_view type, const Bytes &body = {}) {
	auto result = Bytes(8);
	details::WriteBigEndian(body.size() + 8, std::span(result).first(4));
	std::copy(type.begin(), type.end(), result.begin() + 4);
	Append(result, body);
	return result;
}

[[nodiscard]] std::optional<IndexCache> Build(
		const Bytes &data,
		int failAfter = -1) {
	auto calls = 0;
	return BuildIndexCache(
		std::int64_t(data.size()),
		[&](std::int64_t offset, std::span<char> buffer) {
			Check(buffer.size() <= 65536, "bounded individual reads");
			if (failAfter >= 0 && calls++ >= failAfter) {
				return false;
			}
			Check(offset >= 0, "nonnegative reads");
			Check(std::uint64_t(offset) <= data.size(), "read starts within file");
			Check(buffer.size() <= data.size() - offset, "read ends within file");
			std::copy_n(data.data() + offset, buffer.size(), buffer.data());
			return true;
		});
}

void TestMetadataAndMediaBoundaries() {
	auto data = Atom("ftyp", Bytes(16, 'f'));
	Append(data, Atom("moov", Bytes(60, 'h')));
	Append(data, Atom("moof", Bytes(120, 'i')));
	const auto firstPayload = data.size() + 8;
	Append(data, Atom("mdat", Bytes(2000, 'v')));
	const auto secondHeader = data.size();
	Append(data, Atom("moof", Bytes(97, 'j')));
	const auto secondPayload = data.size() + 8;
	Append(data, Atom("mdat", Bytes(1000, 'a')));
	const auto index = Build(data);
	Check(bool(index), "fragmented metadata is retained");
	Check(index->size == data.size() - 3000, "media payload is not retained");
	Check(index->ranges.size() == 2, "adjacent metadata is coalesced");
	for (auto offset = std::size_t(0); offset <= data.size(); ++offset) {
		auto buffer = std::array<char, 43>();
		buffer.fill('?');
		const auto written = index->copy(
			std::int64_t(offset),
			std::span(buffer).subspan(1, 41));
		const auto available = (offset < firstPayload)
			? firstPayload - offset
			: (offset >= secondHeader && offset < secondPayload)
			? secondPayload - offset
			: 0;
		Check(written == std::min<std::size_t>(41, available), "copy stops at media");
		Check(std::equal(
			buffer.begin() + 1,
			buffer.begin() + 1 + written,
			data.begin() + offset), "cached bytes match the original exactly");
		Check(buffer.front() == '?' && buffer[written + 1] == '?', "copy respects bounds");
	}
	auto buffer = std::array<char, 1>();
	Check(index->copy(-1, buffer) == 0, "negative offset is a cache miss");
	Check(index->copy(std::numeric_limits<std::int64_t>::max(), buffer) == 0,
		"large offset is a cache miss");
}

void TestFailuresAndLimits() {
	auto data = Atom("moof", Bytes(140000, 'i'));
	Append(data, Atom("mdat", Bytes(16, 'v')));
	Check(!Build(data, 0), "cancelled header read cannot publish an index");
	Check(!Build(data, 2), "cancelled metadata read cannot publish a partial index");
	Check(bool(Build(data)), "large metadata is read in bounded chunks");
	data.pop_back();
	Check(!Build(data), "truncated media atom is rejected");
	Check(!Build(Atom("ftyp")), "ordinary files do not need this index");
	Check(!Build({}), "empty file is rejected");
	Check(!Build(Bytes(7)), "truncated atom header is rejected");
	data = Atom("moof");
	for (auto i = 0; i != 65536; ++i) {
		Append(data, Atom("mdat"));
	}
	Check(!Build(data), "atom count is bounded");
	data = Atom("moof", Bytes(16 * 1024 * 1024));
	Check(!Build(data), "retained metadata has a memory limit");
}

void TestWideMedia() {
	constexpr auto kMediaSize = std::int64_t(6) * 1024 * 1024 * 1024;
	const auto prefix = Atom("moof", Bytes(8, 'h'));
	auto mediaHeader = Bytes(16);
	details::WriteBigEndian(1, std::span(mediaHeader).first(4));
	std::copy_n("mdat", 4, mediaHeader.begin() + 4);
	details::WriteBigEndian(kMediaSize, std::span(mediaHeader).subspan(8));
	const auto tail = Atom("moof", Bytes(8, 't'));
	const auto tailOffset = std::int64_t(prefix.size()) + kMediaSize;
	auto bytesRead = std::size_t(0);
	const auto index = BuildIndexCache(
		tailOffset + std::int64_t(tail.size()),
		[&](std::int64_t offset, std::span<char> buffer) {
			bytesRead += buffer.size();
			for (auto i = std::size_t(0); i != buffer.size(); ++i) {
				const auto at = offset + std::int64_t(i);
				if (at < std::int64_t(prefix.size())) {
					buffer[i] = prefix[at];
				} else if (at < std::int64_t(prefix.size() + mediaHeader.size())) {
					buffer[i] = mediaHeader[at - prefix.size()];
				} else if (at >= tailOffset) {
					buffer[i] = tail[at - tailOffset];
				} else {
					Check(false, "large media payload must be skipped");
				}
			}
			return true;
		});
	Check(bool(index), "64-bit media atom is supported");
	Check(index->size == prefix.size() + mediaHeader.size() + tail.size(),
		"large file retains only metadata");
	Check(bytesRead < 256, "large file scan does not download media");
}

struct FragmentFixture {
	Bytes bytes;
	std::vector<std::int64_t> offsets;
};

[[nodiscard]] Bytes Integers(std::initializer_list<std::uint32_t> values) {
	auto result = Bytes(values.size() * 4);
	auto offset = std::size_t(0);
	for (const auto value : values) {
		details::WriteBigEndian(value, std::span(result).subspan(offset, 4));
		offset += 4;
	}
	return result;
}

[[nodiscard]] FragmentFixture MakeFragments(
		bool randomAccess = true,
		std::uint32_t composition = 0,
		std::uint32_t sampleDuration = 1000,
		std::uint32_t mediaScale = 1) {
	auto result = FragmentFixture();
	result.bytes = Atom("ftyp", Bytes(16));
	auto tracks = Bytes();
	auto defaults = Bytes();
	for (auto track = 1; track != 3; ++track) {
		auto handler = Integers({ 0, 0 });
		const auto name = (track == 1) ? "vide" : "soun";
		handler.insert(handler.end(), name, name + 4);
		auto media = Atom("mdhd", Integers({ 0, 0, 0, 1000, 64 * sampleDuration }));
		Append(media, Atom("hdlr", handler));
		auto data = Atom("tkhd", Integers({ 0, 0, 0, std::uint32_t(track), 0 }));
		Append(data, Atom("mdia", media));
		Append(tracks, Atom("trak", data));
		Append(defaults, Atom("trex", Integers({ 0, std::uint32_t(track), 1, sampleDuration, 8, 0 })));
	}
	Append(tracks, Atom("mvex", defaults));
	Append(result.bytes, Atom("moov", tracks));
	for (auto segment = 0; segment != 32; ++segment) {
		result.offsets.push_back(std::int64_t(result.bytes.size()));
		auto fragment = Atom("mfhd", Integers({ 0, std::uint32_t(segment) }));
		for (auto track = 1; track != 3; ++track) {
			auto data = Atom("tfhd", Integers({
				0x38,
				std::uint32_t(track),
				sampleDuration,
				8,
				randomAccess ? 0x2000000U : 0x1010000U,
			}));
			Append(data, Atom("tfdt", Integers({ 0x1000000, 0, segment * 2 * sampleDuration })));
			Append(data, Atom("trun", (track == 1 && composition)
				? Integers({ 0x800, 2, composition, composition })
				: Integers({ 0, 2 })));
			Append(fragment, Atom("traf", data));
		}
		Append(result.bytes, Atom("moof", fragment));
		auto payload = Bytes((256 + (segment % 5) * 64) * 1024 * mediaScale);
		const auto fake = Atom("moof", Bytes(19, 'x'));
		std::copy(fake.begin(), fake.end(), payload.begin() + 65532);
		Append(result.bytes, Atom("mdat", payload));
	}
	return result;
}

void TestFragmentSeeks() {
	const auto fixture = MakeFragments();
	auto bytesRead = std::size_t(0);
	auto fail = false;
	const auto read = [&](std::int64_t offset, std::span<char> buffer) {
		Check(offset >= 0 && std::uint64_t(offset) <= fixture.bytes.size(), "fragment read offset is bounded");
		Check(buffer.size() <= fixture.bytes.size() - offset, "fragment read end is bounded");
		Check(buffer.size() <= 65536, "fragment reads fit metadata request limits");
		bytesRead += buffer.size();
		if (fail) {
			return false;
		}
		std::copy_n(fixture.bytes.data() + offset, buffer.size(), buffer.data());
		return true;
	};
	auto index = FragmentIndex::Create(fixture.bytes.size(), 64000, read);
	Check(bool(index), "fragmented tracks with decode times support on-demand seeking");
	for (const auto target : { 59000, 3100, 47000, 1900, 35000, 0, 63000 }) {
		const auto before = bytesRead;
		const auto seek = index->seek(target, read);
		Check(bool(seek), "on-demand seek locates the requested fragment");
		if (target < 2000) {
			Check(seek->empty(), "the first fragment needs no prefix patch");
		} else {
			auto header = std::array<char, 16>();
			for (const auto &patch : *seek) {
				patch.apply(fixture.offsets.front(), header);
			}
			const auto atom = details::ParseAtom(
				header,
				fixture.bytes.size() - fixture.offsets.front());
			Check(atom && atom->is("free"), "the prefix becomes a skippable box");
			Check(atom->size + fixture.offsets.front() == fixture.offsets[target / 2000],
				"the prefix ends at the preceding independent fragment");
			Check(seek->size() == 2, "only the 16-byte prefix header changes");
		}
		Check(bytesRead - before <= fragment_details::kReadBudget, "each seek has a bounded probe budget");
	}
	Check(!index->seek(-1, read), "negative timestamps are rejected");
	Check(!index->seek(64000, read), "EOF timestamps do not start a scan");
	auto cancelled = FragmentIndex::Create(fixture.bytes.size(), 64000, read);
	Check(bool(cancelled), "cancellation fixture is initialized");
	fail = true;
	Check(!FragmentIndex::Create(fixture.bytes.size(), 64000, read), "initialization is cancellable");
	Check(!cancelled->seek(25000, read), "an interrupted probe does not publish a seek patch");
}

void TestFragmentProbeWindows() {
	const auto fixture = MakeFragments(true, 0, 5000, 4);
	auto bytesRead = std::size_t(0);
	const auto read = [&](std::int64_t offset, std::span<char> buffer) {
		Check(offset >= 0 && std::uint64_t(offset) <= fixture.bytes.size(),
			"probe-window read starts inside the file");
		Check(buffer.size() <= fixture.bytes.size() - offset,
			"probe-window read ends inside the file");
		bytesRead += buffer.size();
		std::copy_n(fixture.bytes.data() + offset, buffer.size(), buffer.data());
		return true;
	};
	auto index = FragmentIndex::Create(fixture.bytes.size(), 320000, read);
	Check(bool(index), "large-media fixture supports on-demand seeks");
	for (const auto target : { 208748, 85467, 263038, 250565, 123232, 189247 }) {
		const auto before = bytesRead;
		const auto patches = index->seek(target, read);
		Check(bool(patches), "a probe window inside media resumes at a known boundary");
		Check(bytesRead - before <= fragment_details::kReadBudget,
			"sequential probe recovery respects the per-seek read budget");
		auto header = std::array<char, 16>();
		for (const auto &patch : *patches) {
			patch.apply(fixture.offsets.front(), header);
		}
		const auto atom = details::ParseAtom(
			header,
			fixture.bytes.size() - fixture.offsets.front());
		Check(atom && atom->size + fixture.offsets.front() == fixture.offsets[target / 10000],
			"cached probes still locate the requested fragment");
	}
}

void TestFragmentFallbacks() {
	auto fixture = MakeFragments(false);
	const auto read = [&](std::int64_t offset, std::span<char> buffer) {
		if (offset < 0 || std::uint64_t(offset) > fixture.bytes.size()
			|| buffer.size() > fixture.bytes.size() - offset) {
			return false;
		}
		std::copy_n(fixture.bytes.data() + offset, buffer.size(), buffer.data());
		return true;
	};
	Check(!FragmentIndex::Create(fixture.bytes.size(), 64000, read), "non-independent video fragments keep the full-index fallback");
	fixture = MakeFragments();
	Check(!FragmentIndex::Create(fixture.bytes.size(), 0, read), "unknown duration keeps the full-index fallback");
	Append(fixture.bytes, Atom("mfro", Integers({ 0, 16 })));
	Check(!FragmentIndex::Create(fixture.bytes.size(), 64000, read), "existing random-access trailers are preserved");
	fixture.bytes.resize(32);
	Check(!FragmentIndex::Create(fixture.bytes.size(), 64000, read), "truncated movie metadata is rejected");
}

void TestFragmentPresentationAndEmptyTracks() {
	auto fixture = MakeFragments(true, 80);
	const auto read = [&](std::int64_t offset, std::span<char> buffer) {
		if (offset < 0 || std::uint64_t(offset) > fixture.bytes.size()
			|| buffer.size() > fixture.bytes.size() - offset) {
			return false;
		}
		std::copy_n(fixture.bytes.data() + offset, buffer.size(), buffer.data());
		return true;
	};
	auto index = FragmentIndex::Create(fixture.bytes.size(), 64000, read);
	Check(bool(index), "composition offsets are supported");
	const auto before = index->seek(2030, read);
	Check(before && before->empty(), "a seek before the next keyframe presentation keeps the prior fragment");
	const auto after = index->seek(2090, read);
	Check(after && after->size() == 2, "a seek after the next keyframe presentation skips the prefix");
	auto empty = Atom("mfhd", Integers({ 0, 1 }));
	auto traf = Atom("tfhd", Integers({ 0, 2 }));
	Append(traf, Atom("tfdt", Integers({ 0, 0 })));
	Append(traf, Atom("trun", Integers({ 0, 0 })));
	Append(empty, Atom("traf", traf));
	auto prefix = Atom("moof", empty);
	Append(prefix, Atom("mdat"));
	fixture.bytes.insert(
		fixture.bytes.begin() + fixture.offsets.front(),
		prefix.begin(),
		prefix.end());
	index = FragmentIndex::Create(fixture.bytes.size(), 64000, read);
	Check(index && index->seek(59000, read), "empty leading track fragments do not disable on-demand seeks");
	index = FragmentIndex::Create(fixture.bytes.size(), 70000, read);
	const auto tail = index ? index->seek(69000, read) : std::nullopt;
	Check(tail && tail->size() == 2, "overstated duration still locates the final physical fragment");
	index = FragmentIndex::Create(
		fixture.bytes.size(),
		std::numeric_limits<std::int64_t>::max(),
		read);
	Check(index && !index->seek(std::numeric_limits<std::int64_t>::max() - 1, read),
		"extreme scaled timestamps cannot overflow");
}

void TestTrackFragmentValidation() {
	const auto tracks = std::vector<fragment_details::Track>{ {
		.id = 1,
		.timescale = 1000,
		.duration = 1000,
		.size = 8,
		.flags = 0x2000000,
		.video = true,
	} };
	const auto parse = [&](const Bytes &run, std::uint32_t flags = 0) {
		auto traf = Atom("tfhd", Integers({ flags, 1 }));
		Append(traf, Atom("tfdt", Integers({ 0, 100 })));
		Append(traf, Atom("trun", run));
		auto point = fragment_details::Point();
		auto media = std::uint64_t(0);
		return fragment_details::ReadTraf(traf, tracks, point, media)
			? std::make_optional(point)
			: std::nullopt;
	};
	const auto empty = parse(Integers({ 0, 0 }));
	Check(empty && !empty->duration, "zero-sample track fragments remain valid");
	const auto shifted = parse(Integers({ 0x1000800, 2, 0xFFFFFFEC, 0 }));
	Check(shifted && shifted->time == 80, "signed composition offsets preserve presentation time");
	Check(!parse(Integers({ 0, 1 }), 4), "unknown track header flags are rejected");
	Check(!parse(Integers({ 8, 1 })), "unknown sample-run flags are rejected");
	Check(!parse(Integers({ 0x2000000, 1 })), "unknown sample-run versions are rejected");
	Check(!parse(Integers({ 0x800, 2, 0 })), "truncated sample fields are rejected");
	auto traf = Atom("tfhd", Integers({ 0, 1 }));
	Append(traf, Atom("tfdt", Integers({ 0, 0 })));
	Append(traf, Atom("trun", Integers({ 0, 65536 })));
	Append(traf, Atom("trun", Integers({ 0, 1 })));
	auto point = fragment_details::Point();
	auto media = std::uint64_t(0);
	Check(!fragment_details::ReadTraf(traf, tracks, point, media),
		"the sample budget is shared by every run in a track fragment");
}

[[nodiscard]] int InspectSeek(
		const char *path,
		std::int64_t durationMs,
		std::int64_t positionMs) {
	auto file = std::ifstream(path, std::ios::binary | std::ios::ate);
	const auto size = std::int64_t(file.tellg());
	if (!file || size < 8) {
		return 1;
	}
	auto reads = std::vector<std::pair<std::int64_t, std::size_t>>();
	const auto read = [&](std::int64_t offset, std::span<char> buffer) {
		reads.emplace_back(offset, buffer.size());
		file.clear();
		file.seekg(offset);
		file.read(buffer.data(), std::streamsize(buffer.size()));
		return bool(file);
	};
	auto index = FragmentIndex::Create(size, durationMs, read);
	const auto seek = index ? index->seek(positionMs, read) : std::nullopt;
	std::cout << "{\"on_demand\":" << (index ? "true" : "false")
		<< ",\"ready\":" << (seek ? "true" : "false")
		<< ",\"patches\":[";
	if (seek) {
		for (auto i = std::size_t(0); i != seek->size(); ++i) {
			const auto &patch = (*seek)[i];
			std::cout << (i ? "," : "") << "{\"offset\":" << patch.offset << ",\"bytes\":[";
			for (auto j = 0; j != patch.size; ++j) {
				std::cout << (j ? "," : "") << int(static_cast<unsigned char>(patch.bytes[j]));
			}
			std::cout << "]}";
		}
	}
	std::cout << "],\"reads\":[";
	for (auto i = std::size_t(0); i != reads.size(); ++i) {
		std::cout << (i ? "," : "") << '[' << reads[i].first << ',' << reads[i].second << ']';
	}
	std::cout << "]}\n";
	return 0;
}

[[nodiscard]] int Inspect(const char *path) {
	auto file = std::ifstream(path, std::ios::binary | std::ios::ate);
	const auto size = std::int64_t(file.tellg());
	if (!file || size < 8) {
		return 1;
	}
	auto reads = std::vector<std::pair<std::int64_t, std::size_t>>();
	const auto index = BuildIndexCache(size, [&](auto offset, auto buffer) {
		reads.emplace_back(offset, buffer.size());
		file.seekg(offset);
		file.read(buffer.data(), std::streamsize(buffer.size()));
		return bool(file);
	});
	if (!index) {
		return 1;
	}
	std::cout << "{\"size\":" << index->size << ",\"ranges\":[";
	auto first = true;
	for (const auto &range : index->ranges) {
		std::cout << (first ? "" : ",") << '[' << range.offset
			<< ',' << range.bytes.size() << ']';
		first = false;
	}
	std::cout << "],\"reads\":[";
	first = true;
	for (const auto &[offset, count] : reads) {
		std::cout << (first ? "" : ",") << '[' << offset << ',' << count << ']';
		first = false;
	}
	std::cout << "]}\n";
	return 0;
}

} // namespace

int main(int argc, char *argv[]) {
	if (argc == 3 && std::string_view(argv[1]) == "--inspect") {
		return Inspect(argv[2]);
	} else if (argc == 5 && std::string_view(argv[1]) == "--seek") {
		return InspectSeek(argv[2], std::stoll(argv[3]), std::stoll(argv[4]));
	} else if (argc != 1) {
		return 2;
	}
	TestMetadataAndMediaBoundaries();
	TestFailuresAndLimits();
	TestWideMedia();
	TestFragmentSeeks();
	TestFragmentProbeWindows();
	TestFragmentFallbacks();
	TestFragmentPresentationAndEmptyTracks();
	TestTrackFragmentValidation();
	std::cout << "MP4 index checks passed: " << Checks << '\n';
	return 0;
}
