/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_mp4_index.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
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
	} else if (argc != 1) {
		return 2;
	}
	TestMetadataAndMediaBoundaries();
	TestFailuresAndLimits();
	TestWideMedia();
	std::cout << "MP4 index checks passed: " << Checks << '\n';
	return 0;
}
