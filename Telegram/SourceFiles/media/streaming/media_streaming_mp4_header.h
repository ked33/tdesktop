/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace Media::Streaming::Mp4 {

enum class Layout {
	Unknown = 0,
	Fragmented = 1,
	Regular = 2,
	LargeFrontMoov = 3,
};

struct HeaderPatch {
	std::int64_t offset = -1;
	std::array<char, 8> bytes = {};
	int size = 0;

	void apply(std::int64_t readOffset, std::span<char> buffer) const;

};

struct StreamingHeader {
	Layout layout = Layout::Regular;
	HeaderPatch patch;
};

namespace details {

struct Atom {
	std::int64_t size = 0;
	int headerSize = 0;
	std::array<char, 4> type = {};

	[[nodiscard]] bool is(const char (&expected)[5]) const;

};

inline bool Atom::is(const char (&expected)[5]) const {
	return std::equal(type.begin(), type.end(), expected);
}

[[nodiscard]] inline std::uint64_t ReadBigEndian(std::span<const char> data) {
	auto result = std::uint64_t(0);
	for (const auto byte : data) {
		result = (result << 8) | static_cast<unsigned char>(byte);
	}
	return result;
}

inline void WriteBigEndian(std::uint64_t value, std::span<char> data) {
	for (auto i = int(data.size()); i > 0; --i) {
		data[i - 1] = char(value & 0xFF);
		value >>= 8;
	}
}

template <typename Read>
[[nodiscard]] std::optional<Atom> ReadAtom(
		std::int64_t offset,
		std::int64_t end,
		Read &read,
		int &budget) {
	if (budget <= 0
		|| offset < 0
		|| offset > end
		|| end - offset < 8) {
		return std::nullopt;
	}
	--budget;
	auto storage = std::array<char, 16>();
	const auto available = int(std::min<std::int64_t>(
		storage.size(),
		end - offset));
	const auto data = std::span(storage).first(available);
	if (!read(offset, data)) {
		return std::nullopt;
	}
	auto size = ReadBigEndian(data.first(4));
	auto headerSize = 8;
	if (size == 1) {
		if (available < 16) {
			return std::nullopt;
		}
		size = ReadBigEndian(data.subspan(8, 8));
		headerSize = 16;
	} else if (size == 0) {
		size = end - offset;
	}
	if (size < std::uint64_t(headerSize)
		|| size > std::uint64_t(end - offset)) {
		return std::nullopt;
	}
	auto result = Atom{
		.size = std::int64_t(size),
		.headerSize = headerSize,
	};
	std::copy_n(data.begin() + 4, result.type.size(), result.type.begin());
	return result;
}

} // namespace details

inline void HeaderPatch::apply(
		std::int64_t readOffset,
		std::span<char> buffer) const {
	if (offset < 0
		|| size <= 0
		|| size > int(bytes.size())
		|| readOffset < 0) {
		return;
	}
	const auto source = std::uint64_t(std::max(
		readOffset - offset,
		std::int64_t(0)));
	const auto target = std::uint64_t(std::max(
		offset - readOffset,
		std::int64_t(0)));
	if (source >= std::uint64_t(size) || target >= buffer.size()) {
		return;
	}
	const auto count = std::min<std::uint64_t>(
		std::uint64_t(size) - source,
		buffer.size() - target);
	std::copy_n(bytes.data() + source, count, buffer.data() + target);
}

template <typename Read>
[[nodiscard]] StreamingHeader ProbeForStreaming(
		std::int64_t fileSize,
		Read &&read) {
	constexpr auto kLargeFrontMoovThreshold = std::int64_t(2) * 1024 * 1024;
	constexpr auto kMaximumHeaderAtoms = 64;
	auto budget = kMaximumHeaderAtoms;
	auto offset = std::int64_t(0);
	auto sawMoov = false;
	auto result = StreamingHeader();
	while (offset < fileSize) {
		const auto atom = details::ReadAtom(offset, fileSize, read, budget);
		if (!atom) {
			return {};
		}
		if (atom->is("moov")) {
			if (sawMoov) {
				return {};
			}
			auto childOffset = offset + atom->headerSize;
			const auto end = offset + atom->size;
			auto hasTrack = false;
			while (childOffset < end) {
				const auto child = details::ReadAtom(
					childOffset,
					end,
					read,
					budget);
				if (!child) {
					return {};
				} else if (child->is("mvex")) {
					return { .layout = Layout::Fragmented };
				}
				hasTrack = hasTrack || child->is("trak");
				childOffset += child->size;
			}
			if (!hasTrack) {
				return {};
			}
			sawMoov = true;
			result.layout = (atom->size >= kLargeFrontMoovThreshold)
				? Layout::LargeFrontMoov
				: Layout::Regular;
		} else if (atom->is("moof")) {
			return { .layout = Layout::Fragmented };
		} else if (atom->is("mdat")) {
			if (!sawMoov) {
				return {};
			}
			const auto tailSize = std::uint64_t(fileSize - offset);
			if (std::uint64_t(atom->size) != tailSize) {
				// A front moov without mvex already indexes the samples.
				// Extend the first mdat to EOF so MOV seeking does not scan
				// intervening root atoms left unparsed by IGNIDX.
				// Only the served size changes; sample offsets stay intact.
				const auto extended = (atom->headerSize == 16);
				result.patch.offset = offset + (extended ? 8 : 0);
				result.patch.size = extended ? 8 : 4;
				const auto value = (extended
					|| tailSize <= std::numeric_limits<std::uint32_t>::max())
					? tailSize
					: 0;
				details::WriteBigEndian(
					value,
					std::span(result.patch.bytes).first(result.patch.size));
			}
			return result;
		}
		offset += atom->size;
	}
	return {};
}

} // namespace Media::Streaming::Mp4
