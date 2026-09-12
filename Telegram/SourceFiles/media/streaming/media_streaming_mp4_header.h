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

struct ReadAheadRange {
	static constexpr auto kMaximumSize = std::int64_t(8) * 1024 * 1024;

	std::int64_t offset = 0;
	std::int64_t size = 0;

	[[nodiscard]] bool intersects(
		std::int64_t readOffset,
		std::int64_t amount) const;
	[[nodiscard]] int preloadParts(
		std::int64_t readOffset,
		std::int64_t amount,
		int partSize,
		int requestLimit) const;

};

class HeaderReadAhead final {
public:
	explicit HeaderReadAhead(std::int64_t fileSize) : _fileSize(fileSize) {
	}

	[[nodiscard]] std::optional<ReadAheadRange> observe(
		std::int64_t offset,
		std::span<const char> data);

private:
	std::int64_t _fileSize = 0;
	std::int64_t _nextOffset = 0;
	std::array<char, 16> _header = {};
	int _headerBytes = 0;
	int _atomsLeft = 64;

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

[[nodiscard]] inline std::optional<Atom> ParseAtom(
		std::span<const char> data,
		std::int64_t available) {
	if (data.size() < 8 || available < 8) {
		return std::nullopt;
	}
	auto size = ReadBigEndian(data.first(4));
	auto headerSize = 8;
	if (size == 1) {
		if (data.size() < 16) {
			return std::nullopt;
		}
		size = ReadBigEndian(data.subspan(8, 8));
		headerSize = 16;
	} else if (size == 0) {
		size = available;
	}
	if (size < std::uint64_t(headerSize)
		|| size > std::uint64_t(available)) {
		return std::nullopt;
	}
	auto result = Atom{
		.size = std::int64_t(size),
		.headerSize = headerSize,
	};
	std::copy_n(data.begin() + 4, result.type.size(), result.type.begin());
	return result;
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
	return read(offset, data) ? ParseAtom(data, end - offset) : std::nullopt;
}

} // namespace details

inline bool ReadAheadRange::intersects(
		std::int64_t readOffset,
		std::int64_t amount) const {
	constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
	return offset >= 0
		&& size > 0
		&& size <= kMaximumSize
		&& size <= kMax - offset
		&& readOffset >= 0
		&& amount > 0
		&& amount <= kMax - readOffset
		&& readOffset < offset + size
		&& readOffset + amount > offset;
}

inline int ReadAheadRange::preloadParts(
		std::int64_t readOffset,
		std::int64_t amount,
		int partSize,
		int requestLimit) const {
	if (!intersects(readOffset, amount) || partSize <= 0 || requestLimit <= 0) {
		return 0;
	}
	const auto first = (readOffset + amount - 1) / partSize + 1;
	const auto last = (offset + size - 1) / partSize + 1;
	return int(std::clamp<std::int64_t>(last - first, 0, requestLimit));
}

inline std::optional<ReadAheadRange> HeaderReadAhead::observe(
		std::int64_t offset,
		std::span<const char> data) {
	if (offset < 0
		|| offset > _fileSize
		|| data.size() > std::uint64_t(_fileSize - offset)) {
		return std::nullopt;
	}
	while (_atomsLeft > 0 && _fileSize - _nextOffset >= 8) {
		const auto needed = (_headerBytes >= 8
			&& details::ReadBigEndian(std::span(_header).first(4)) == 1)
			? 16
			: 8;
		const auto next = _nextOffset + _headerBytes;
		if (next < offset || std::uint64_t(next - offset) >= data.size()) {
			return std::nullopt;
		}
		const auto count = std::min<std::size_t>(
			needed - _headerBytes,
			data.size() - std::size_t(next - offset));
		std::copy_n(
			data.data() + std::size_t(next - offset),
			count,
			_header.data() + _headerBytes);
		_headerBytes += int(count);
		if (_headerBytes < needed) {
			return std::nullopt;
		} else if (needed == 8
			&& details::ReadBigEndian(std::span(_header).first(4)) == 1) {
			continue;
		}
		const auto atom = details::ParseAtom(
			std::span(_header).first(_headerBytes),
			_fileSize - _nextOffset);
		--_atomsLeft;
		if (!atom
			|| (_nextOffset == 0
				&& !atom->is("ftyp")
				&& !atom->is("moov")
				&& !atom->is("mdat")
				&& !atom->is("wide")
				&& !atom->is("free")
				&& !atom->is("skip"))) {
			_atomsLeft = 0;
			return std::nullopt;
		} else if (atom->is("moov")) {
			_atomsLeft = 0;
			if (atom->size <= ReadAheadRange::kMaximumSize) {
				return ReadAheadRange{ _nextOffset, atom->size };
			}
			return std::nullopt;
		}
		_nextOffset += atom->size;
		_headerBytes = 0;
	}
	return std::nullopt;
}

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
				// Extend the first mdat to EOF so MOV opening and seeking
				// do not scan intervening root atoms between media blocks.
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
