/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_mp4_header.h"

#include <functional>
#include <utility>

namespace Media::Streaming::Mp4 {
namespace fragment_details {

constexpr auto kMaximumMoov = 4 * 1024 * 1024;
constexpr auto kMaximumMoof = 256 * 1024;
constexpr auto kMaximumRead = 64 * 1024;
constexpr auto kScanWindow = 4 * 1024 * 1024;
constexpr auto kReadBudget = 16 * 1024 * 1024;
constexpr auto kMaximumPoints = 8192;

struct Track {
	std::uint32_t id = 0;
	std::uint32_t timescale = 0;
	std::uint32_t duration = 0;
	std::uint32_t size = 0;
	std::uint32_t flags = 0;
	bool video = false;
};

struct Point {
	std::int64_t offset = 0;
	std::int64_t next = 0;
	std::int64_t time = 0;
	std::int64_t duration = 0;
	std::uint32_t track = 0;
};

struct Fragment {
	std::int64_t next = 0;
	std::vector<Point> points;
};

template <typename Callback>
[[nodiscard]] bool Children(
		std::span<const char> data,
		Callback &&callback) {
	while (!data.empty()) {
		const auto atom = details::ParseAtom(data, data.size());
		if (!atom || !callback(
				*atom,
				data.subspan(atom->headerSize, atom->size - atom->headerSize))) {
			return false;
		}
		data = data.subspan(std::size_t(atom->size));
	}
	return true;
}

[[nodiscard]] inline bool ReadTrack(
		std::span<const char> data,
		Track &track) {
	auto handler = false;
	const auto valid = Children(data, [&](const auto &atom, auto body) {
		if (atom.is("tkhd")) {
			if (body.empty() || body[0] > 1 || body[0] < 0) {
				return false;
			}
			const auto offset = (body[0] == 1) ? 20 : 12;
			if (body.size() < std::size_t(offset + 4) || track.id) {
				return false;
			}
			track.id = std::uint32_t(details::ReadBigEndian(body.subspan(offset, 4)));
		} else if (atom.is("mdia")) {
			return Children(body, [&](const auto &child, auto bytes) {
				if (child.is("mdhd")) {
					if (bytes.empty() || bytes[0] > 1 || bytes[0] < 0) {
						return false;
					}
					const auto offset = (bytes[0] == 1) ? 20 : 12;
					if (bytes.size() < std::size_t(offset + 4) || track.timescale) {
						return false;
					}
					track.timescale = std::uint32_t(details::ReadBigEndian(
						bytes.subspan(offset, 4)));
				} else if (child.is("hdlr")) {
					if (bytes.size() < 12 || handler) {
						return false;
					}
					const auto type = bytes.subspan(8, 4);
					track.video = std::equal(type.begin(), type.end(), "vide");
					handler = track.video || std::equal(type.begin(), type.end(), "soun");
					return handler;
				}
				return true;
			});
		}
		return true;
	});
	return valid && handler && track.id && track.timescale;
}

[[nodiscard]] inline std::vector<Track> ReadTracks(std::span<const char> data) {
	auto result = std::vector<Track>();
	auto defaults = std::vector<Track>();
	const auto valid = Children(data, [&](const auto &atom, auto body) {
		if (atom.is("trak")) {
			auto track = Track();
			if (result.size() >= 8 || !ReadTrack(body, track)) {
				return false;
			}
			for (const auto &existing : result) {
				if (existing.id == track.id) {
					return false;
				}
			}
			result.push_back(track);
		} else if (atom.is("mvex")) {
			return Children(body, [&](const auto &child, auto bytes) {
				if (child.is("trex")) {
					if (bytes.size() != 24 || defaults.size() >= 8
						|| details::ReadBigEndian(bytes.first(4))) {
						return false;
					}
					defaults.push_back({
						.id = std::uint32_t(details::ReadBigEndian(
							bytes.subspan(4, 4))),
						.duration = std::uint32_t(details::ReadBigEndian(
							bytes.subspan(12, 4))),
						.size = std::uint32_t(details::ReadBigEndian(
							bytes.subspan(16, 4))),
						.flags = std::uint32_t(details::ReadBigEndian(
							bytes.subspan(20, 4))),
					});
				}
				return true;
			});
		}
		return true;
	});
	if (!valid || result.empty() || result.size() != defaults.size()) {
		return {};
	}
	auto videos = 0;
	for (auto &track : result) {
		const auto i = std::find_if(defaults.begin(), defaults.end(), [&](const auto &value) {
			return value.id == track.id;
		});
		if (i == defaults.end()) {
			return {};
		}
		track.duration = i->duration;
		track.size = i->size;
		track.flags = i->flags;
		videos += track.video ? 1 : 0;
	}
	return (videos <= 1) ? result : std::vector<Track>();
}

[[nodiscard]] inline bool ReadTraf(
		std::span<const char> data,
		const std::vector<Track> &tracks,
		Point &point,
		std::uint64_t &mediaBytes) {
	auto defaults = Track();
	auto tfhd = false;
	auto tfdt = false;
	auto randomAccess = false;
	auto samples = false;
	auto compositionOffset = std::int64_t(0);
	auto samplesLeft = std::uint64_t(65536);
	auto runs = 0;
	const auto valid = Children(data, [&](const auto &atom, auto body) {
		if (atom.is("tfhd")) {
			if (tfhd || body.size() < 8 || body[0] != 0) {
				return false;
			}
			const auto flags = details::ReadBigEndian(body.first(4));
			if (flags & ~std::uint64_t(0x02003B)) {
				return false;
			}
			point.track = std::uint32_t(details::ReadBigEndian(body.subspan(4, 4)));
			const auto i = std::find_if(tracks.begin(), tracks.end(), [&](const auto &track) {
				return track.id == point.track;
			});
			if (i == tracks.end()) {
				return false;
			}
			defaults = *i;
			auto offset = std::size_t(8 + ((flags & 1) ? 8 : 0) + ((flags & 2) ? 4 : 0));
			for (const auto &[bit, value] : {
				std::pair{ 8, &defaults.duration },
				std::pair{ 16, &defaults.size },
				std::pair{ 32, &defaults.flags },
			}) {
				if (flags & bit) {
					if (offset > body.size() || body.size() - offset < 4) {
						return false;
					}
					*value = std::uint32_t(details::ReadBigEndian(body.subspan(offset, 4)));
					offset += 4;
				}
			}
			tfhd = (offset == body.size());
			return tfhd;
		} else if (atom.is("tfdt")) {
			if (tfdt || body.empty() || body[0] < 0 || body[0] > 1) {
				return false;
			}
			const auto width = (body[0] == 1) ? 8 : 4;
			if (body.size() != std::size_t(4 + width)
				|| details::ReadBigEndian(body.subspan(1, 3))) {
				return false;
			}
			const auto time = details::ReadBigEndian(body.subspan(4, width));
			if (time > std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
				return false;
			}
			point.time = std::int64_t(time);
			tfdt = true;
		} else if (atom.is("trun")) {
			if (!tfhd || !tfdt || body.size() < 8
				|| body[0] < 0 || body[0] > 1 || ++runs > 255) {
				return false;
			}
			const auto flags = details::ReadBigEndian(body.subspan(1, 3));
			const auto count = details::ReadBigEndian(body.subspan(4, 4));
			if (count > samplesLeft || (flags & ~std::uint64_t(0xF05))
				|| ((flags & 4) && (flags & 0x400))) {
				return false;
			}
			samplesLeft -= count;
			auto offset = std::size_t(8 + ((flags & 1) ? 4 : 0));
			auto firstFlags = defaults.flags;
			if (flags & 4) {
				if (offset > body.size() || body.size() - offset < 4) {
					return false;
				}
				firstFlags = std::uint32_t(details::ReadBigEndian(body.subspan(offset, 4)));
				offset += 4;
			}
			for (auto sample = std::uint64_t(0); sample < count; ++sample) {
				auto duration = defaults.duration;
				auto size = defaults.size;
				auto sampleFlags = sample ? defaults.flags : firstFlags;
				auto composition = std::uint32_t(0);
				for (const auto &[bit, value] : {
					std::pair{ 0x100, &duration },
					std::pair{ 0x200, &size },
					std::pair{ 0x400, &sampleFlags },
					std::pair{ 0x800, &composition },
				}) {
					if (flags & bit) {
						if (offset > body.size() || body.size() - offset < 4) {
							return false;
						}
						*value = std::uint32_t(details::ReadBigEndian(
							body.subspan(offset, 4)));
						offset += 4;
					}
				}
				if (!duration || !size
					|| point.duration > std::numeric_limits<std::int64_t>::max()
						- point.time - duration) {
					return false;
				}
				if (!samples) {
					samples = true;
					compositionOffset = (body[0] == 1 && composition > 0x7FFFFFFF)
						? std::int64_t(composition) - 0x100000000LL
						: std::int64_t(composition);
					randomAccess = !defaults.video
						|| (!(sampleFlags & 0x10000)
							&& (sampleFlags & 0x3000000) != 0x1000000);
				}
				point.duration += duration;
				mediaBytes += size;
			}
			if (offset != body.size()) {
				return false;
			}
		}
		return true;
	});
	if (!valid || !tfhd || !tfdt || !runs || (samples && !randomAccess)
		|| compositionOffset < -point.time
		|| compositionOffset > std::numeric_limits<std::int64_t>::max()
			- point.time - point.duration) {
		return false;
	}
	point.time += compositionOffset;
	return true;
}

} // namespace fragment_details

class FragmentIndex final {
public:
	using Read = std::function<bool(std::int64_t, std::span<char>)>;

	[[nodiscard]] static std::optional<FragmentIndex> Create(
		std::int64_t fileSize,
		std::int64_t durationMs,
		const Read &read);
	[[nodiscard]] std::optional<std::vector<HeaderPatch>> seek(
		std::int64_t positionMs,
		const Read &read);

private:
	[[nodiscard]] bool readBytes(
		std::int64_t offset,
		std::span<char> bytes,
		const Read &read);
	[[nodiscard]] std::optional<fragment_details::Fragment> readFragment(
		std::int64_t offset,
		const Read &read);
	[[nodiscard]] std::optional<fragment_details::Fragment> scan(
		std::int64_t offset,
		std::int64_t end,
		const Read &read);
	[[nodiscard]] bool remember(const fragment_details::Fragment &fragment);
	[[nodiscard]] std::optional<fragment_details::Point> find(
		const fragment_details::Track &track,
		std::int64_t positionMs,
		const Read &read);

	std::int64_t _fileSize = 0;
	std::int64_t _durationMs = 0;
	std::int64_t _firstFragment = -1;
	std::size_t _budget = 0;
	std::vector<fragment_details::Track> _tracks;
	std::vector<fragment_details::Point> _points;

};

inline std::optional<FragmentIndex> FragmentIndex::Create(
		std::int64_t fileSize,
		std::int64_t durationMs,
		const Read &read) {
	using namespace fragment_details;
	if (fileSize < 8 || durationMs <= 0) {
		return std::nullopt;
	}
	auto result = FragmentIndex();
	result._fileSize = fileSize;
	result._durationMs = durationMs;
	result._budget = kReadBudget;
	if (fileSize >= 16) {
		auto trailer = std::array<char, 16>();
		if (!result.readBytes(fileSize - 16, trailer, read)) {
			return std::nullopt;
		}
		const auto atom = details::ParseAtom(trailer, 16);
		if (atom && atom->is("mfro")) {
			return std::nullopt;
		}
	}
	auto offset = std::int64_t(0);
	auto budget = 64;
	auto bounded = [&](std::int64_t position, std::span<char> bytes) {
		return result.readBytes(position, bytes, read);
	};
	while (offset < fileSize && budget > 0) {
		const auto atom = details::ReadAtom(offset, fileSize, bounded, budget);
		if (!atom) {
			return std::nullopt;
		} else if (atom->is("moov")) {
			if (!result._tracks.empty() || atom->size > kMaximumMoov) {
				return std::nullopt;
			}
			auto bytes = std::vector<char>(std::size_t(atom->size - atom->headerSize));
			if (!bounded(offset + atom->headerSize, bytes)) {
				return std::nullopt;
			}
			result._tracks = ReadTracks(bytes);
			if (result._tracks.empty()) {
				return std::nullopt;
			}
		} else if (atom->is("sidx") || atom->is("mfra")) {
			return std::nullopt;
		} else if (atom->is("moof")) {
			if (result._tracks.empty()) {
				return std::nullopt;
			}
			if (result._firstFragment < 0) {
				result._firstFragment = offset;
			}
			const auto fragment = result.readFragment(offset, read);
			if (!fragment || !result.remember(*fragment)) {
				return std::nullopt;
			}
			const auto initialized = std::all_of(
				result._tracks.begin(),
				result._tracks.end(),
				[&](const auto &track) {
					return std::any_of(
						result._points.begin(),
						result._points.end(),
						[&](const auto &point) {
							return point.track == track.id
								&& point.time <= std::int64_t(track.timescale);
						});
				});
			if (initialized) {
				return result;
			}
			offset = fragment->next;
			continue;
		} else if (atom->is("mdat")) {
			return std::nullopt;
		}
		offset += atom->size;
	}
	return std::nullopt;
}

inline bool FragmentIndex::readBytes(
		std::int64_t offset,
		std::span<char> bytes,
		const Read &read) {
	using namespace fragment_details;
	if (offset < 0 || offset > _fileSize
		|| bytes.size() > std::uint64_t(_fileSize - offset)
		|| bytes.size() > _budget) {
		return false;
	}
	_budget -= bytes.size();
	while (!bytes.empty()) {
		const auto count = std::min(bytes.size(), std::size_t(kMaximumRead));
		if (!read(offset, bytes.first(count))) {
			return false;
		}
		offset += count;
		bytes = bytes.subspan(count);
	}
	return true;
}

inline std::optional<fragment_details::Fragment> FragmentIndex::readFragment(
		std::int64_t offset,
		const Read &read) {
	using namespace fragment_details;
	auto budget = 2;
	auto bounded = [&](std::int64_t position, std::span<char> bytes) {
		return readBytes(position, bytes, read);
	};
	const auto atom = details::ReadAtom(offset, _fileSize, bounded, budget);
	if (!atom || !atom->is("moof") || atom->size > kMaximumMoof) {
		return std::nullopt;
	}
	const auto media = details::ReadAtom(offset + atom->size, _fileSize, bounded, budget);
	if (!media || !media->is("mdat")) {
		return std::nullopt;
	}
	auto bytes = std::vector<char>(std::size_t(atom->size - atom->headerSize));
	if (!bounded(offset + atom->headerSize, bytes)) {
		return std::nullopt;
	}
	auto result = Fragment();
	result.next = offset + atom->size + media->size;
	auto sequence = false;
	auto trafs = 0;
	auto trackIds = std::vector<std::uint32_t>();
	auto mediaBytes = std::uint64_t(0);
	const auto valid = Children(bytes, [&](const auto &child, auto body) {
		if (child.is("mfhd")) {
			if (sequence || body.size() != 8 || details::ReadBigEndian(body.first(4))) {
				return false;
			}
			sequence = true;
		} else if (child.is("traf")) {
			if (++trafs > 255) {
				return false;
			}
			auto point = Point{
				.offset = offset,
				.next = result.next,
			};
			if (!ReadTraf(body, _tracks, point, mediaBytes)) {
				return false;
			}
			for (const auto existing : trackIds) {
				if (existing == point.track) {
					return false;
				}
			}
			trackIds.push_back(point.track);
			if (point.duration) {
				result.points.push_back(point);
			}
		}
		return true;
	});
	return (valid && sequence && trafs
		&& mediaBytes <= std::uint64_t(media->size - media->headerSize))
		? std::make_optional(std::move(result))
		: std::nullopt;
}

inline std::optional<fragment_details::Fragment> FragmentIndex::scan(
		std::int64_t offset,
		std::int64_t end,
		const Read &read) {
	using namespace fragment_details;
	end = std::min(end, offset + std::min<std::int64_t>(kScanWindow, _fileSize - offset));
	auto bytes = std::vector<char>(kMaximumRead + 7);
	auto overlap = std::size_t(0);
	while (offset < end) {
		const auto count = std::size_t(std::min<std::int64_t>(kMaximumRead, end - offset));
		if (!readBytes(offset, std::span(bytes).subspan(overlap, count), read)) {
			return std::nullopt;
		}
		const auto size = overlap + count;
		for (auto i = std::size_t(4); i + 4 <= size; ++i) {
			if (std::equal(bytes.data() + i, bytes.data() + i + 4, "moof")) {
				const auto position = offset - std::int64_t(overlap) + std::int64_t(i) - 4;
				if (const auto fragment = readFragment(position, read)) {
					return fragment;
				}
			}
		}
		overlap = std::min<std::size_t>(7, size);
		std::copy_n(bytes.data() + size - overlap, overlap, bytes.data());
		offset += count;
	}
	return std::nullopt;
}

inline bool FragmentIndex::remember(const fragment_details::Fragment &fragment) {
	for (const auto &point : fragment.points) {
		const auto exists = std::any_of(
			_points.begin(),
			_points.end(),
			[&](const auto &value) {
				return value.offset == point.offset && value.track == point.track;
			});
		if (!exists) {
			if (_points.size() >= fragment_details::kMaximumPoints) {
				return false;
			}
			_points.push_back(point);
		}
	}
	return true;
}

inline std::optional<fragment_details::Point> FragmentIndex::find(
		const fragment_details::Track &track,
		std::int64_t positionMs,
		const Read &read) {
	using namespace fragment_details;
	const auto scaled = (static_cast<long double>(positionMs) * track.timescale) / 1000;
	if (scaled >= std::numeric_limits<std::int64_t>::max()) {
		return std::nullopt;
	}
	const auto target = std::int64_t(scaled);
	auto ceiling = _fileSize;
	for (auto attempt = 0; attempt != 48; ++attempt) {
		auto before = std::optional<Point>();
		auto after = std::optional<Point>();
		for (const auto &point : _points) {
			if (point.track != track.id) {
				continue;
			} else if (point.time <= target) {
				if (!before || point.time > before->time
					|| (point.time == before->time && point.offset > before->offset)) {
					before = point;
				}
			} else if (!after || point.time < after->time) {
				after = point;
			}
		}
		if (!before) {
			return after && after->time <= std::int64_t(track.timescale)
				? after
				: std::nullopt;
		} else if (target < before->time + before->duration) {
			return before;
		}
		const auto end = std::min(after ? after->offset : _fileSize, ceiling);
		if (before->next >= end) {
			return (before->next == _fileSize) ? before : std::nullopt;
		}
		auto offset = before->next;
		const auto close = target - before->time - before->duration
			< 12 * std::int64_t(track.timescale);
		if (!close && end - offset > kScanWindow) {
			const auto until = after
				? static_cast<long double>(after->time)
				: (static_cast<long double>(_durationMs) * track.timescale) / 1000;
			const auto fraction = (until > before->time)
				? std::clamp((target - before->time) / (until - before->time), 0.1L, 0.9L)
				: 0.5L;
			offset = std::max(offset, before->offset
				+ std::int64_t((end - before->offset) * fraction) - kMaximumRead);
		}
		auto fragment = (offset == before->next)
			? readFragment(offset, read)
			: scan(offset, end, read);
		if (!fragment && offset != before->next) {
			// An interpolated probe and its ceiling can fall inside the
			// same media box. Finding no header there does not invalidate
			// the layout. Resume at the last verified fragment boundary,
			// keeping the existing read and iteration budgets in effect.
			offset = before->next;
			fragment = readFragment(offset, read);
		}
		auto matched = false;
		for (auto follow = 0; fragment && follow != 16; ++follow) {
			if (!remember(*fragment)) {
				return std::nullopt;
			}
			const auto i = std::find_if(
				fragment->points.begin(),
				fragment->points.end(),
				[&](const auto &point) {
					return point.track == track.id;
				});
			if (i != fragment->points.end()) {
				if (offset != before->next && i->time > target) {
					ceiling = std::min(ceiling, offset);
				}
				matched = true;
				break;
			}
			if (fragment->next == _fileSize && offset == before->next && !after) {
				return before;
			}
			fragment = readFragment(fragment->next, read);
		}
		if (!matched) {
			return std::nullopt;
		}
	}
	return std::nullopt;
}

inline std::optional<std::vector<HeaderPatch>> FragmentIndex::seek(
		std::int64_t positionMs,
		const Read &read) {
	if (positionMs < 0 || positionMs >= _durationMs) {
		return {};
	}
	_budget = fragment_details::kReadBudget;
	auto first = _fileSize;
	for (const auto &track : _tracks) {
		const auto point = find(track, positionMs, read);
		if (!point) {
			return {};
		}
		first = std::min(first, point->offset);
	}
	if (first == _firstFragment) {
		return std::vector<HeaderPatch>();
	} else if (first - _firstFragment < 16) {
		return std::nullopt;
	}
	// Each revision exposes an immutable view of the original byte stream.
	// A free box skips to the earliest fragment needed by the selected tracks.
	// Its 64-bit size preserves the file length and every media byte offset.
	// Fast opening then builds local sample indexes from that fragment group,
	// while subsequent fragments remain available for sequential playback.
	auto header = HeaderPatch{
		.offset = _firstFragment,
		.bytes = { 0, 0, 0, 1, 'f', 'r', 'e', 'e' },
		.size = 8,
	};
	auto size = HeaderPatch{ .offset = _firstFragment + 8, .size = 8 };
	details::WriteBigEndian(first - _firstFragment, size.bytes);
	return std::vector<HeaderPatch>{ header, size };
}

} // namespace Media::Streaming::Mp4
