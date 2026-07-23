/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_file.h"

#include "base/algorithm.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "media/streaming/media_streaming_debug.h"
#include "media/streaming/media_streaming_file_delegate.h"
#include "media/streaming/media_streaming_loader.h"

#include <QtCore/QtEndian>

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace Media {
namespace Streaming {
namespace {

constexpr auto kMaxSingleReadAmount = 8 * 1024 * 1024;
constexpr auto kMaxQueuedPackets = 1024;
constexpr auto kSequentialOpenProbeSize = int64(2) * 1024 * 1024;
constexpr auto kSequentialOpenAnalyzeDuration = int64(3) * AV_TIME_BASE;
constexpr auto kSeekPrefetchBackAmount = int64(2) * 1024 * 1024;
constexpr auto kSeekPrefetchAheadAmount = int64(8) * 1024 * 1024;
constexpr auto kMp4TopLevelAtomHeaderSize = 16;
constexpr auto kMp4SeekMapMaximumMoovSize = int64(kMaxSingleReadAmount);
constexpr auto kMp4SeekMapMaximumTopLevelAtoms = 64;

[[nodiscard]] bool UnreliableFormatDuration(
		not_null<AVFormatContext*> format,
		not_null<AVStream*> stream,
		Mode mode) {
	return (mode == Mode::Video || mode == Mode::Inspection)
		&& stream->codecpar
		&& (stream->codecpar->codec_id == AV_CODEC_ID_VP9)
		&& format->iformat
		&& format->iformat->name
		&& QString::fromLatin1(
			format->iformat->name
		).split(QChar(',')).contains(u"webm");
}

struct Mp4Atom {
	int offset = 0;
	int headerSize = 0;
	quint64 size = 0;
	QByteArray type;

	[[nodiscard]] int payloadOffset() const {
		return offset + headerSize;
	}

	[[nodiscard]] int payloadSize() const {
		return int(size) - headerSize;
	}
};

struct Mp4SttsEntry {
	uint32 sampleCount = 0;
	uint32 sampleDelta = 0;
};

struct Mp4StscEntry {
	uint32 firstChunk = 0;
	uint32 samplesPerChunk = 0;
};

struct Mp4SeekTrack {
	uint32 timeScale = 0;
	uint32 sampleCount = 0;
	uint64 durationUnits = 0;
	crl::time duration = 0;
	std::vector<Mp4SttsEntry> stts;
	std::vector<uint32> stss;
	std::vector<Mp4StscEntry> stsc;
	std::vector<uint64> chunkOffsets;
	uint32 constantSampleSize = 0;
	std::vector<uint32> sampleSizes;
};

enum class Mp4SeekMapFailure {
	None,
	FormatUnsupported,
	AtomHeaderReadFailed,
	AtomHeaderInvalid,
	AtomScanLimit,
	MoovMissing,
	MoovTooLarge,
	MoovReadFailed,
	MoovInvalid,
	VideoTrackMissing,
	VideoMetadataMissing,
	SampleTableMissing,
	CompactSampleSizesUnsupported,
	SttsInvalid,
	StscInvalid,
	StszInvalid,
	ChunkOffsetsInvalid,
	StssInvalid,
	TrackInvalid,
	Fragmented,
	TargetSampleInvalid,
	SampleOffsetInvalid,
	SampleOffsetOutOfRange,
};

struct Mp4TopLevelAtom {
	int64 offset = 0;
	uint64 size = 0;
	QByteArray type;
};

struct Mp4SeekMapDiagnostic {
	Mp4SeekMapFailure failure = Mp4SeekMapFailure::None;
	int headerSize = 0;
	int64 scanOffset = 0;
	int64 moovOffset = -1;
	uint64 atomSize = 0;
	uint64 moovSize = 0;
	QByteArray atomType;
	int tracks = 0;
	int videoTracks = 0;
	uint32 tables = 0;
	bool fragmented = false;
};

struct Mp4VideoTrackParseResult {
	std::optional<Mp4SeekTrack> track;
	Mp4SeekMapFailure failure = Mp4SeekMapFailure::None;
	uint32 tables = 0;
	bool video = false;
};

struct Mp4SeekMapBuildResult {
	std::optional<Mp4SeekTrack> track;
	Mp4SeekMapDiagnostic diagnostic;
};

struct Mp4SeekPoint {
	crl::time duration = 0;
	uint32 targetSample = 0;
	uint32 syncSample = 0;
	int64 sampleOffset = 0;
};

struct Mp4SeekMapResolution {
	std::optional<Mp4SeekPoint> point;
	Mp4SeekMapDiagnostic diagnostic;
};

constexpr auto kMp4SeekTableStts = uint32(1) << 0;
constexpr auto kMp4SeekTableStsc = uint32(1) << 1;
constexpr auto kMp4SeekTableStsz = uint32(1) << 2;
constexpr auto kMp4SeekTableStz2 = uint32(1) << 3;
constexpr auto kMp4SeekTableStco = uint32(1) << 4;
constexpr auto kMp4SeekTableCo64 = uint32(1) << 5;
constexpr auto kMp4SeekTableStss = uint32(1) << 6;

template <typename Type>
[[nodiscard]] bool ReadBigEndianAt(
		bytes::const_span data,
		int offset,
		Type *out) {
	if (!out
		|| (offset < 0)
		|| (offset + int(sizeof(Type)) > data.size())) {
		return false;
	}
	*out = qFromBigEndian<Type>(
		reinterpret_cast<const uchar*>(data.data() + offset));
	return true;
}

[[nodiscard]] bool AtomTypeEquals(
		const Mp4Atom &atom,
		const char (&type)[5]) {
	return (atom.type.size() == 4)
		&& (std::memcmp(atom.type.constData(), type, 4) == 0);
}

[[nodiscard]] bool AtomTypeEquals(
		const Mp4TopLevelAtom &atom,
		const char (&type)[5]) {
	return (atom.type.size() == 4)
		&& (std::memcmp(atom.type.constData(), type, 4) == 0);
}

[[nodiscard]] std::optional<Mp4TopLevelAtom> ReadMp4TopLevelAtom(
		bytes::const_span data,
		int64 offset,
		int64 fileSize) {
	if (fileSize <= 0
		|| offset < 0
		|| offset >= fileSize
		|| data.size() < 8) {
		return std::nullopt;
	}
	uint32 size32 = 0;
	if (!ReadBigEndianAt(data, 0, &size32)) {
		return std::nullopt;
	}
	auto headerSize = 8;
	auto size = uint64(size32);
	if (size32 == 1) {
		if (!ReadBigEndianAt(data, 8, &size)) {
			return std::nullopt;
		}
		headerSize = 16;
	} else if (size32 == 0) {
		size = fileSize - offset;
	}
	if (size < uint64(headerSize)
		|| size > uint64(fileSize - offset)) {
		return std::nullopt;
	}
	return Mp4TopLevelAtom{
		.offset = offset,
		.size = size,
		.type = QByteArray(
			reinterpret_cast<const char*>(data.data() + 4),
			4),
	};
}

[[nodiscard]] std::optional<Mp4Atom> ReadMp4Atom(
		bytes::const_span data,
		int offset) {
	uint32 size32 = 0;
	if (!ReadBigEndianAt(data, offset, &size32)) {
		return std::nullopt;
	}
	auto headerSize = 8;
	auto size = quint64(size32);
	if (size32 == 1) {
		if (!ReadBigEndianAt(data, offset + 8, &size)) {
			return std::nullopt;
		}
		headerSize = 16;
	} else if (size32 == 0) {
		size = data.size() - offset;
	}
	if ((size < quint64(headerSize))
		|| (offset < 0)
		|| (size > quint64(data.size() - offset))) {
		return std::nullopt;
	}
	return Mp4Atom{
		.offset = offset,
		.headerSize = headerSize,
		.size = size,
		.type = QByteArray::fromRawData(
			reinterpret_cast<const char*>(data.data() + offset + 4),
			4),
	};
}

template <typename Callback>
[[nodiscard]] bool ForEachChildAtom(
		bytes::const_span data,
		const Mp4Atom &parent,
		Callback &&callback) {
	auto offset = parent.payloadOffset();
	const auto end = offset + parent.payloadSize();
	while (offset < end) {
		const auto atom = ReadMp4Atom(data, offset);
		if (!atom || (atom->offset + int(atom->size) > end)) {
			return false;
		}
		if (!callback(*atom)) {
			return true;
		}
		offset += int(atom->size);
	}
	return true;
}

[[nodiscard]] std::optional<Mp4Atom> FindChildAtom(
		bytes::const_span data,
		const Mp4Atom &parent,
		const char (&type)[5]) {
	auto result = std::optional<Mp4Atom>();
	ForEachChildAtom(data, parent, [&](const Mp4Atom &atom) {
		if (AtomTypeEquals(atom, type)) {
			result = atom;
			return false;
		}
		return true;
	});
	return result;
}

[[nodiscard]] std::vector<Mp4Atom> FindChildAtoms(
		bytes::const_span data,
		const Mp4Atom &parent,
		const char (&type)[5]) {
	auto result = std::vector<Mp4Atom>();
	ForEachChildAtom(data, parent, [&](const Mp4Atom &atom) {
		if (AtomTypeEquals(atom, type)) {
			result.push_back(atom);
		}
		return true;
	});
	return result;
}

[[nodiscard]] std::optional<uint32> ParseMdhdTimeScale(
		bytes::const_span data,
		const Mp4Atom &atom) {
	const auto payload = atom.payloadOffset();
	if (payload >= data.size() || atom.payloadSize() < 16) {
		return std::nullopt;
	}
	const auto version = static_cast<uchar>(data[payload]);
	const auto timeScaleOffset = payload + ((version == 1) ? 20 : 12);
	uint32 result = 0;
	return ReadBigEndianAt(data, timeScaleOffset, &result) && result
		? std::optional<uint32>(result)
		: std::nullopt;
}

[[nodiscard]] bool ParseHdlrIsVideo(
		bytes::const_span data,
		const Mp4Atom &atom) {
	const auto payload = atom.payloadOffset();
	return (payload + 12 <= data.size())
		&& (atom.payloadSize() >= 12)
		&& (std::memcmp(data.data() + payload + 8, "vide", 4) == 0);
}

[[nodiscard]] bool ParseStts(
		bytes::const_span data,
		const Mp4Atom &atom,
		Mp4SeekTrack *track) {
	if (!track) {
		return false;
	}
	const auto payload = atom.payloadOffset();
	uint32 entryCount = 0;
	if (!ReadBigEndianAt(data, payload + 4, &entryCount)) {
		return false;
	}
	auto offset = payload + 8;
	track->stts.clear();
	track->stts.reserve(entryCount);
	track->sampleCount = 0;
	track->durationUnits = 0;
	for (auto i = uint32(0); i != entryCount; ++i) {
		auto entry = Mp4SttsEntry();
		if (!ReadBigEndianAt(data, offset, &entry.sampleCount)
			|| !ReadBigEndianAt(data, offset + 4, &entry.sampleDelta)
			|| !entry.sampleCount
			|| !entry.sampleDelta) {
			return false;
		}
		track->sampleCount += entry.sampleCount;
		track->durationUnits += uint64(entry.sampleCount) * entry.sampleDelta;
		track->stts.push_back(entry);
		offset += 8;
	}
	return !track->stts.empty();
}

[[nodiscard]] bool ParseStss(
		bytes::const_span data,
		const Mp4Atom &atom,
		Mp4SeekTrack *track) {
	if (!track) {
		return false;
	}
	const auto payload = atom.payloadOffset();
	uint32 entryCount = 0;
	if (!ReadBigEndianAt(data, payload + 4, &entryCount)) {
		return false;
	}
	auto offset = payload + 8;
	track->stss.clear();
	track->stss.reserve(entryCount);
	for (auto i = uint32(0); i != entryCount; ++i) {
		uint32 sample = 0;
		if (!ReadBigEndianAt(data, offset, &sample) || !sample) {
			return false;
		}
		track->stss.push_back(sample);
		offset += 4;
	}
	return true;
}

[[nodiscard]] bool ParseStsc(
		bytes::const_span data,
		const Mp4Atom &atom,
		Mp4SeekTrack *track) {
	if (!track) {
		return false;
	}
	const auto payload = atom.payloadOffset();
	uint32 entryCount = 0;
	if (!ReadBigEndianAt(data, payload + 4, &entryCount)) {
		return false;
	}
	auto offset = payload + 8;
	track->stsc.clear();
	track->stsc.reserve(entryCount);
	for (auto i = uint32(0); i != entryCount; ++i) {
		auto entry = Mp4StscEntry();
		uint32 description = 0;
		if (!ReadBigEndianAt(data, offset, &entry.firstChunk)
			|| !ReadBigEndianAt(data, offset + 4, &entry.samplesPerChunk)
			|| !ReadBigEndianAt(data, offset + 8, &description)
			|| !entry.firstChunk
			|| !entry.samplesPerChunk) {
			return false;
		}
		track->stsc.push_back(entry);
		offset += 12;
	}
	return !track->stsc.empty();
}

[[nodiscard]] bool ParseStsz(
		bytes::const_span data,
		const Mp4Atom &atom,
		Mp4SeekTrack *track) {
	if (!track) {
		return false;
	}
	const auto payload = atom.payloadOffset();
	uint32 sampleSize = 0;
	uint32 sampleCount = 0;
	if (!ReadBigEndianAt(data, payload + 4, &sampleSize)
		|| !ReadBigEndianAt(data, payload + 8, &sampleCount)
		|| !sampleCount) {
		return false;
	}
	track->constantSampleSize = sampleSize;
	track->sampleSizes.clear();
	if (sampleSize) {
		return true;
	}
	auto offset = payload + 12;
	track->sampleSizes.reserve(sampleCount);
	for (auto i = uint32(0); i != sampleCount; ++i) {
		uint32 current = 0;
		if (!ReadBigEndianAt(data, offset, &current) || !current) {
			return false;
		}
		track->sampleSizes.push_back(current);
		offset += 4;
	}
	return true;
}

[[nodiscard]] bool ParseChunkOffsets(
		bytes::const_span data,
		const Mp4Atom &atom,
		Mp4SeekTrack *track) {
	if (!track) {
		return false;
	}
	const auto payload = atom.payloadOffset();
	uint32 entryCount = 0;
	if (!ReadBigEndianAt(data, payload + 4, &entryCount) || !entryCount) {
		return false;
	}
	auto offset = payload + 8;
	track->chunkOffsets.clear();
	track->chunkOffsets.reserve(entryCount);
	if (AtomTypeEquals(atom, "stco")) {
		for (auto i = uint32(0); i != entryCount; ++i) {
			uint32 current = 0;
			if (!ReadBigEndianAt(data, offset, &current)) {
				return false;
			}
			track->chunkOffsets.push_back(current);
			offset += 4;
		}
	} else {
		for (auto i = uint32(0); i != entryCount; ++i) {
			uint64 current = 0;
			if (!ReadBigEndianAt(data, offset, &current)) {
				return false;
			}
			track->chunkOffsets.push_back(current);
			offset += 8;
		}
	}
	return true;
}

[[nodiscard]] Mp4VideoTrackParseResult ParseVideoSeekTrack(
		bytes::const_span data,
		const Mp4Atom &trak) {
	auto result = Mp4VideoTrackParseResult();
	const auto mdia = FindChildAtom(data, trak, "mdia");
	if (!mdia) {
		return result;
	}
	const auto hdlr = FindChildAtom(data, *mdia, "hdlr");
	if (!hdlr || !ParseHdlrIsVideo(data, *hdlr)) {
		return result;
	}
	result.video = true;
	const auto mdhd = FindChildAtom(data, *mdia, "mdhd");
	const auto minf = FindChildAtom(data, *mdia, "minf");
	if (!mdhd || !minf) {
		result.failure = Mp4SeekMapFailure::VideoMetadataMissing;
		return result;
	}
	const auto stbl = FindChildAtom(data, *minf, "stbl");
	if (!stbl) {
		result.failure = Mp4SeekMapFailure::SampleTableMissing;
		return result;
	}
	auto track = Mp4SeekTrack();
	const auto timeScale = ParseMdhdTimeScale(data, *mdhd);
	const auto stts = FindChildAtom(data, *stbl, "stts");
	const auto stsc = FindChildAtom(data, *stbl, "stsc");
	const auto stsz = FindChildAtom(data, *stbl, "stsz");
	const auto stz2 = FindChildAtom(data, *stbl, "stz2");
	const auto stco = FindChildAtom(data, *stbl, "stco");
	const auto co64 = FindChildAtom(data, *stbl, "co64");
	const auto stss = FindChildAtom(data, *stbl, "stss");
	result.tables = (stts ? kMp4SeekTableStts : 0)
		| (stsc ? kMp4SeekTableStsc : 0)
		| (stsz ? kMp4SeekTableStsz : 0)
		| (stz2 ? kMp4SeekTableStz2 : 0)
		| (stco ? kMp4SeekTableStco : 0)
		| (co64 ? kMp4SeekTableCo64 : 0)
		| (stss ? kMp4SeekTableStss : 0);
	if (!stsz && stz2) {
		result.failure = Mp4SeekMapFailure::CompactSampleSizesUnsupported;
		return result;
	}
	if (!timeScale
		|| !stts
		|| !stsc
		|| !stsz
		|| (!stco && !co64)) {
		result.failure = Mp4SeekMapFailure::SampleTableMissing;
		return result;
	}
	if (!ParseStts(data, *stts, &track)) {
		result.failure = Mp4SeekMapFailure::SttsInvalid;
		return result;
	} else if (!ParseStsc(data, *stsc, &track)) {
		result.failure = Mp4SeekMapFailure::StscInvalid;
		return result;
	} else if (!ParseStsz(data, *stsz, &track)) {
		result.failure = Mp4SeekMapFailure::StszInvalid;
		return result;
	} else if (!ParseChunkOffsets(
			data,
			stco ? *stco : *co64,
			&track)) {
		result.failure = Mp4SeekMapFailure::ChunkOffsetsInvalid;
		return result;
	} else if (stss && !ParseStss(data, *stss, &track)) {
		result.failure = Mp4SeekMapFailure::StssInvalid;
		return result;
	}
	track.timeScale = *timeScale;
	track.duration = track.timeScale
		? crl::time((track.durationUnits * 1000) / track.timeScale)
		: 0;
	if (!track.duration
		|| !track.sampleCount
		|| track.chunkOffsets.empty()
		|| track.stsc.empty()
		|| (!track.constantSampleSize
			&& (track.sampleSizes.size() != track.sampleCount))) {
		result.failure = Mp4SeekMapFailure::TrackInvalid;
		return result;
	}
	result.track = std::move(track);
	return result;
}

template <typename Stop>
[[nodiscard]] std::optional<QByteArray> ReadSourceBytes(
		not_null<FileSource*> source,
		int64 offset,
		int size,
		Stop &&stop) {
	if ((offset < 0)
		|| (size <= 0)
		|| (offset + size > source->size())) {
		return std::nullopt;
	}
	auto result = QByteArray(size, Qt::Uninitialized);
	auto wait = crl::semaphore();
	auto buffer = bytes::span(
		reinterpret_cast<bytes::type*>(result.data()),
		size);
	while (true) {
		if (stop()) {
			return std::nullopt;
		}
		const auto state = source->fill(offset, buffer, &wait);
		if (state == FileSource::FillState::Success) {
			return result;
		} else if (state == FileSource::FillState::Failed) {
			return std::nullopt;
		}
		wait.acquire();
	}
}

[[nodiscard]] Mp4SeekMapBuildResult BuildMp4SeekTrack(
		not_null<FileSource*> source,
		const Stream &stream,
		const std::atomic<bool> &interrupted) {
	auto result = Mp4SeekMapBuildResult();
	result.diagnostic.headerSize = source->headerSize();
	const auto fileSize = source->size();
	auto offset = int64(0);
	for (auto atomIndex = 0
			; offset < fileSize
				&& atomIndex != kMp4SeekMapMaximumTopLevelAtoms
			; ++atomIndex) {
		result.diagnostic.scanOffset = offset;
		const auto bytesToRead = int(std::min<int64>(
			kMp4TopLevelAtomHeaderSize,
			fileSize - offset));
		const auto atomHeader = ReadSourceBytes(
			source,
			offset,
			bytesToRead,
			[&] { return interrupted.load(); });
		if (!atomHeader) {
			result.diagnostic.failure
				= Mp4SeekMapFailure::AtomHeaderReadFailed;
			return result;
		}
		const auto headerData = bytes::make_span(
			reinterpret_cast<const bytes::type*>(atomHeader->constData()),
			atomHeader->size());
		const auto atom = ReadMp4TopLevelAtom(
			headerData,
			offset,
			fileSize);
		if (!atom) {
			result.diagnostic.failure
				= Mp4SeekMapFailure::AtomHeaderInvalid;
			return result;
		}
		result.diagnostic.atomType = atom->type;
		result.diagnostic.atomSize = atom->size;
		if (AtomTypeEquals(*atom, "moof")) {
			result.diagnostic.fragmented = true;
		}
		if (!AtomTypeEquals(*atom, "moov")) {
			offset += int64(atom->size);
			continue;
		}
		result.diagnostic.moovOffset = atom->offset;
		result.diagnostic.moovSize = atom->size;
		if (atom->size > uint64(kMp4SeekMapMaximumMoovSize)) {
			result.diagnostic.failure = Mp4SeekMapFailure::MoovTooLarge;
			return result;
		}
		const auto moovBytes = ReadSourceBytes(
			source,
			atom->offset,
			int(atom->size),
			[&] { return interrupted.load(); });
		if (!moovBytes) {
			result.diagnostic.failure = Mp4SeekMapFailure::MoovReadFailed;
			return result;
		}
		const auto data = bytes::make_span(
			reinterpret_cast<const bytes::type*>(moovBytes->constData()),
			moovBytes->size());
		const auto moov = ReadMp4Atom(data, 0);
		if (!moov || !AtomTypeEquals(*moov, "moov")) {
			result.diagnostic.failure = Mp4SeekMapFailure::MoovInvalid;
			return result;
		}
		result.diagnostic.fragmented = result.diagnostic.fragmented
			|| FindChildAtom(data, *moov, "mvex").has_value();
		const auto tracks = FindChildAtoms(data, *moov, "trak");
		result.diagnostic.tracks = int(tracks.size());
		if (result.diagnostic.fragmented) {
			result.diagnostic.failure = Mp4SeekMapFailure::Fragmented;
			return result;
		}
		auto bestDelta = std::numeric_limits<crl::time>::max();
		auto videoFailure = Mp4SeekMapFailure::VideoTrackMissing;
		for (const auto &trak : tracks) {
			auto candidate = ParseVideoSeekTrack(data, trak);
			if (!candidate.video) {
				continue;
			}
			++result.diagnostic.videoTracks;
			result.diagnostic.tables = candidate.tables;
			if (!candidate.track) {
				videoFailure = candidate.failure;
				continue;
			}
			const auto delta = (candidate.track->duration > stream.duration)
				? (candidate.track->duration - stream.duration)
				: (stream.duration - candidate.track->duration);
			if (!result.track || (delta < bestDelta)) {
				result.track = std::move(*candidate.track);
				bestDelta = delta;
			}
		}
		if (result.track) {
			return result;
		}
		result.diagnostic.failure = videoFailure;
		return result;
	}
	if (offset < fileSize) {
		result.diagnostic.failure = Mp4SeekMapFailure::AtomScanLimit;
	} else if (result.diagnostic.fragmented) {
		result.diagnostic.failure = Mp4SeekMapFailure::Fragmented;
	} else {
		result.diagnostic.failure = Mp4SeekMapFailure::MoovMissing;
	}
	return result;
}

[[nodiscard]] std::optional<uint32> FindTargetSample(
		const Mp4SeekTrack &track,
		crl::time position) {
	if (!track.timeScale || !track.sampleCount) {
		return std::nullopt;
	}
	const auto clamped = std::clamp(
		position,
		crl::time(0),
		std::max(track.duration - 1, crl::time(0)));
	const auto targetUnits = uint64(clamped) * track.timeScale / 1000;
	auto sampleCursor = uint32(0);
	auto timeCursor = uint64(0);
	for (const auto &entry : track.stts) {
		const auto duration = uint64(entry.sampleCount) * entry.sampleDelta;
		if (targetUnits < timeCursor + duration) {
			return sampleCursor
				+ uint32((targetUnits - timeCursor) / entry.sampleDelta)
				+ 1;
		}
		sampleCursor += entry.sampleCount;
		timeCursor += duration;
	}
	return track.sampleCount;
}

[[nodiscard]] uint32 FindSyncSample(
		const Mp4SeekTrack &track,
		uint32 sample) {
	if (track.stss.empty()) {
		return sample;
	}
	const auto i = std::upper_bound(
		begin(track.stss),
		end(track.stss),
		sample);
	return (i == begin(track.stss)) ? track.stss.front() : *(i - 1);
}

[[nodiscard]] std::optional<uint64> ComputeSampleOffset(
		const Mp4SeekTrack &track,
		uint32 sample) {
	if (!sample || (sample > track.sampleCount)) {
		return std::nullopt;
	}
	const auto sampleZero = uint64(sample - 1);
	auto sampleCursor = uint64(0);
	for (auto i = 0, count = int(track.stsc.size()); i != count; ++i) {
		const auto &entry = track.stsc[i];
		const auto chunkStart = uint64(entry.firstChunk - 1);
		const auto chunkEnd = (i + 1 == count)
			? uint64(track.chunkOffsets.size())
			: uint64(track.stsc[i + 1].firstChunk - 1);
		if ((chunkStart >= chunkEnd)
			|| (chunkEnd > track.chunkOffsets.size())) {
			return std::nullopt;
		}
		const auto samplesPerChunk = uint64(entry.samplesPerChunk);
		const auto groupSamples = (chunkEnd - chunkStart) * samplesPerChunk;
		if (sampleZero >= sampleCursor + groupSamples) {
			sampleCursor += groupSamples;
			continue;
		}
		const auto relative = sampleZero - sampleCursor;
		const auto chunk = chunkStart + (relative / samplesPerChunk);
		const auto insideChunk = uint32(relative % samplesPerChunk);
		auto offset = track.chunkOffsets[chunk];
		if (track.constantSampleSize) {
			offset += uint64(insideChunk) * track.constantSampleSize;
			return offset;
		}
		const auto firstSample = size_t(sampleZero - insideChunk);
		if ((firstSample + insideChunk) > track.sampleSizes.size()) {
			return std::nullopt;
		}
		for (auto j = uint32(0); j != insideChunk; ++j) {
			offset += track.sampleSizes[firstSample + j];
		}
		return offset;
	}
	return std::nullopt;
}

[[nodiscard]] Mp4SeekMapResolution ResolveMp4SeekMap(
		not_null<FileSource*> source,
		const Stream &stream,
		const std::atomic<bool> &interrupted,
		crl::time position) {
	auto built = BuildMp4SeekTrack(source, stream, interrupted);
	auto result = Mp4SeekMapResolution();
	result.diagnostic = std::move(built.diagnostic);
	if (!built.track) {
		return result;
	}
	const auto targetSample = FindTargetSample(*built.track, position);
	if (!targetSample) {
		result.diagnostic.failure = Mp4SeekMapFailure::TargetSampleInvalid;
		return result;
	}
	const auto syncSample = FindSyncSample(*built.track, *targetSample);
	const auto sampleOffset = ComputeSampleOffset(*built.track, syncSample);
	if (!sampleOffset) {
		result.diagnostic.failure = Mp4SeekMapFailure::SampleOffsetInvalid;
		return result;
	} else if (*sampleOffset >= uint64(source->size())
		|| *sampleOffset > uint64(std::numeric_limits<int64>::max())) {
		result.diagnostic.failure
			= Mp4SeekMapFailure::SampleOffsetOutOfRange;
		return result;
	}
	result.point = Mp4SeekPoint{
		.duration = built.track->duration,
		.targetSample = *targetSample,
		.syncSample = syncSample,
		.sampleOffset = int64(*sampleOffset),
	};
	return result;
}

[[nodiscard]] QString Mp4SeekMapFailureName(Mp4SeekMapFailure failure) {
	switch (failure) {
	case Mp4SeekMapFailure::None:
		return u"none"_q;
	case Mp4SeekMapFailure::FormatUnsupported:
		return u"format_unsupported"_q;
	case Mp4SeekMapFailure::AtomHeaderReadFailed:
		return u"atom_header_read_failed"_q;
	case Mp4SeekMapFailure::AtomHeaderInvalid:
		return u"atom_header_invalid"_q;
	case Mp4SeekMapFailure::AtomScanLimit:
		return u"atom_scan_limit"_q;
	case Mp4SeekMapFailure::MoovMissing:
		return u"moov_missing"_q;
	case Mp4SeekMapFailure::MoovTooLarge:
		return u"moov_too_large"_q;
	case Mp4SeekMapFailure::MoovReadFailed:
		return u"moov_read_failed"_q;
	case Mp4SeekMapFailure::MoovInvalid:
		return u"moov_invalid"_q;
	case Mp4SeekMapFailure::VideoTrackMissing:
		return u"video_track_missing"_q;
	case Mp4SeekMapFailure::VideoMetadataMissing:
		return u"video_metadata_missing"_q;
	case Mp4SeekMapFailure::SampleTableMissing:
		return u"sample_table_missing"_q;
	case Mp4SeekMapFailure::CompactSampleSizesUnsupported:
		return u"compact_sample_sizes"_q;
	case Mp4SeekMapFailure::SttsInvalid:
		return u"stts_invalid"_q;
	case Mp4SeekMapFailure::StscInvalid:
		return u"stsc_invalid"_q;
	case Mp4SeekMapFailure::StszInvalid:
		return u"stsz_invalid"_q;
	case Mp4SeekMapFailure::ChunkOffsetsInvalid:
		return u"chunk_offsets_invalid"_q;
	case Mp4SeekMapFailure::StssInvalid:
		return u"stss_invalid"_q;
	case Mp4SeekMapFailure::TrackInvalid:
		return u"track_invalid"_q;
	case Mp4SeekMapFailure::Fragmented:
		return u"fragmented_mp4"_q;
	case Mp4SeekMapFailure::TargetSampleInvalid:
		return u"target_sample_invalid"_q;
	case Mp4SeekMapFailure::SampleOffsetInvalid:
		return u"sample_offset_invalid"_q;
	case Mp4SeekMapFailure::SampleOffsetOutOfRange:
		return u"sample_offset_out_of_range"_q;
	}
	Unexpected("Mp4SeekMapFailure value.");
}

[[nodiscard]] bool Mp4MoovOutsideHeader(
		const Mp4SeekMapDiagnostic &diagnostic) {
	return diagnostic.headerSize > 0
		&& diagnostic.moovOffset >= 0
		&& (uint64(diagnostic.moovOffset) + diagnostic.moovSize
			> uint64(diagnostic.headerSize));
}

void LogMp4SeekMapFailure(
		not_null<AVFormatContext*> format,
		int64 fileSize,
		crl::time position,
		const Mp4SeekMapDiagnostic &diagnostic) {
	const auto formatName = (format->iformat && format->iformat->name)
		? QString::fromLatin1(format->iformat->name)
		: u"unknown"_q;
	const auto atomName = diagnostic.atomType.isEmpty()
		? u"none"_q
		: QString::fromLatin1(
			diagnostic.atomType.constData(),
			diagnostic.atomType.size());
	const auto tailMoov = Mp4MoovOutsideHeader(diagnostic);
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File seek map unavailable "
		"target=%1 reason=%2 format=%3 header=%4 file=%5 scan=%6 "
		"atom=%7 atomSize=%8 moovOffset=%9 moovSize=%10 tail=%11 "
		"fragmented=%12 tracks=%13 videoTracks=%14 tables=%15.")
		.arg(qlonglong(position))
		.arg(Mp4SeekMapFailureName(diagnostic.failure))
		.arg(formatName)
		.arg(diagnostic.headerSize)
		.arg(qlonglong(fileSize))
		.arg(qlonglong(diagnostic.scanOffset))
		.arg(atomName)
		.arg(qulonglong(diagnostic.atomSize))
		.arg(qlonglong(diagnostic.moovOffset))
		.arg(qulonglong(diagnostic.moovSize))
		.arg(tailMoov ? 1 : 0)
		.arg(diagnostic.fragmented ? 1 : 0)
		.arg(diagnostic.tracks)
		.arg(diagnostic.videoTracks)
		.arg(QString::number(diagnostic.tables, 16)));
}

[[nodiscard]] bool IsMp4LikeFormat(not_null<AVFormatContext*> format) {
	return format->iformat
		&& format->iformat->name
		&& QString::fromLatin1(format->iformat->name)
			.split(QChar(','))
			.contains(u"mov");
}

} // namespace

File::Context::Context(
	not_null<FileDelegate*> delegate,
	not_null<FileSource*> source)
: _delegate(delegate)
, _source(source)
, _size(source->size()) {
}

File::Context::~Context() = default;

int File::Context::Read(void *opaque, uint8_t *buffer, int bufferSize) {
	return static_cast<Context*>(opaque)->read(
		bytes::make_span(buffer, bufferSize));
}

int64_t File::Context::Seek(void *opaque, int64_t offset, int whence) {
	return static_cast<Context*>(opaque)->seek(offset, whence);
}

void File::Context::prefetchAroundOffset(
		int64 offset,
		crl::time position,
		bool mapped) {
	if (!_source->smartStreamingEnabled()
		|| offset < 0
		|| offset >= _size) {
		return;
	}
	const auto start = std::max<int64>(
		0,
		offset - kSeekPrefetchBackAmount);
	const auto amount = std::min<int64>(
		_size - start,
		kSeekPrefetchBackAmount + kSeekPrefetchAheadAmount);
	_source->prefetch(start, amount);
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File seek prefetch "
		"target=%1 sourceOffset=%2 start=%3 amount=%4 mapped=%5 "
		"source=%6.")
		.arg(qlonglong(position))
		.arg(qlonglong(offset))
		.arg(qlonglong(start))
		.arg(qlonglong(amount))
		.arg(mapped ? 1 : 0)
		.arg(mapped ? u"map"_q : u"read"_q));
}

int File::Context::read(bytes::span buffer) {
	Expects(_size >= _offset);

	const auto amount = std::min(_size - _offset, int64(buffer.size()));
	const auto requestedOffset = _offset;

	if (unroll()) {
		return AVERROR_EXTERNAL;
	} else if (amount > kMaxSingleReadAmount) {
		LOG(("Streaming Error: Read callback asked for too much data: %1"
			).arg(amount));
		return AVERROR_EXTERNAL;
	} else if (!amount) {
		return AVERROR_EOF;
	}
	if (const auto position = base::take(_pendingSeekPrefetchPosition)) {
		prefetchAroundOffset(requestedOffset, position, false);
	}

	buffer = buffer.subspan(0, amount);
	if (_debugReadCalls < 12) {
		VIDEO_PLAYBACK_VERBOSE_LOG(("Video Playback: AVIO read enter offset=%1 amount=%2 requested=%3 size=%4.")
			.arg(qlonglong(requestedOffset))
			.arg(qlonglong(amount))
			.arg(qlonglong(buffer.size()))
			.arg(qlonglong(_size)));
	}
	while (true) {
		const auto result = _source->fill(_offset, buffer, &_semaphore);
		if (result == FileSource::FillState::Success) {
			break;
		} else if (result == FileSource::FillState::WaitingRemote) {
			++_debugWaitingCount;
			if ((_debugWaitingCount <= 8) || !(_debugWaitingCount % 25)) {
				VIDEO_PLAYBACK_VERBOSE_LOG(("Video Playback: AVIO read waiting offset=%1 amount=%2 waitCount=%3.")
					.arg(qlonglong(_offset))
					.arg(qlonglong(buffer.size()))
					.arg(_debugWaitingCount));
			}
			// Perhaps for the correct sleeping in case of enough packets
			// being read already we require SleepPolicy::Allowed here.
			// Otherwise if we wait for the remote frequently and
			// _queuedPackets never get to kMaxQueuedPackets and we don't call
			// processQueuedPackets(SleepPolicy::Allowed) ever.
			//
			// But right now we can't simply pass SleepPolicy::Allowed here,
			// it freezes because of two _semaphore.acquire one after another.
			processQueuedPackets(SleepPolicy::Disallowed);
			_delegate->fileWaitingForData();
		}
		_semaphore.acquire();
		if (_interrupted) {
			return AVERROR_EXTERNAL;
		} else if (const auto error = _source->streamingError()) {
			fail(*error);
			return AVERROR_EXTERNAL;
		}
	}

	sendFullInCache();

	_offset += amount;
	++_debugReadCalls;
	if (_debugReadCalls <= 12) {
		VIDEO_PLAYBACK_VERBOSE_LOG(("Video Playback: AVIO read success offset=%1 amount=%2 nextOffset=%3 waits=%4.")
			.arg(qlonglong(requestedOffset))
			.arg(qlonglong(amount))
			.arg(qlonglong(_offset))
			.arg(_debugWaitingCount));
	}
	return amount;
}

int64_t File::Context::seek(int64_t offset, int whence) {
	const auto checkedSeek = [&](int64_t offset) {
		if (_failed || offset < 0 || offset > _size) {
			return int64(-1);
		}
		return (_offset = offset);
	};
	auto result = int64(-1);
	switch (whence) {
	case SEEK_SET: result = checkedSeek(offset); break;
	case SEEK_CUR: result = checkedSeek(_offset + offset); break;
	case SEEK_END: result = checkedSeek(_size + offset); break;
	case AVSEEK_SIZE: result = _size; break;
	default: break;
	}
	if (whence != AVSEEK_SIZE) {
		VIDEO_PLAYBACK_VERBOSE_LOG(("Video Playback: AVIO seek request offset=%1 whence=%2 result=%3 currentOffset=%4 size=%5.")
			.arg(qlonglong(offset))
			.arg(whence)
			.arg(qlonglong(result))
			.arg(qlonglong(_offset))
			.arg(qlonglong(_size)));
	}
	return result;
}

void File::Context::logError(QLatin1String method) {
	if (!unroll()) {
		FFmpeg::LogError(method);
	}
}

void File::Context::logError(
		QLatin1String method,
		FFmpeg::AvErrorWrap error) {
	if (!unroll()) {
		FFmpeg::LogError(method, error);
	}
}

void File::Context::logFatal(QLatin1String method) {
	if (!unroll()) {
		FFmpeg::LogError(method);
		fail(_format ? Error::InvalidData : Error::OpenFailed);
	}
}

void File::Context::logFatal(
		QLatin1String method,
		FFmpeg::AvErrorWrap error) {
	if (!unroll()) {
		FFmpeg::LogError(method, error);
		fail(_format ? Error::InvalidData : Error::OpenFailed);
	}
}

Stream File::Context::initStream(
		not_null<AVFormatContext*> format,
		AVMediaType type,
		Mode mode,
		StartOptions options) {
	auto result = Stream();
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: initStream enter type=%1 hwAllow=%2 sequentialOpen=%3 streamCount=%4.")
		.arg(int(type))
		.arg(options.hwAllow ? 1 : 0)
		.arg(options.sequentialOpen ? 1 : 0)
		.arg(format->nb_streams));
	const auto index = result.index = av_find_best_stream(
		format,
		type,
		-1,
		-1,
		nullptr,
		0);
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: initStream best stream type=%1 index=%2.")
		.arg(int(type))
		.arg(index));
	if (index < 0) {
		return {};
	}

	const auto info = format->streams[index];
	if (type == AVMEDIA_TYPE_VIDEO) {
		if (info->disposition & AV_DISPOSITION_ATTACHED_PIC) {
			// ignore cover streams
			return Stream();
		}
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: initStream video codecId=%1 hwAllow=%2 width=%3 height=%4.")
			.arg(int(info->codecpar ? info->codecpar->codec_id : AV_CODEC_ID_NONE))
			.arg(options.hwAllow ? 1 : 0)
			.arg(info->codecpar ? info->codecpar->width : 0)
			.arg(info->codecpar ? info->codecpar->height : 0));
		result.codec = FFmpeg::MakeCodecPointer({
			.stream = info,
			.hwAllowed = options.hwAllow,
			.videoMaxArea = kMaxFrameArea,
		});
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: initStream video codec ready=%1 index=%2.")
			.arg(result.codec ? 1 : 0)
			.arg(index));
		if (!result.codec) {
			return result;
		}
		result.rotation = FFmpeg::ReadRotationFromMetadata(info);
		result.aspect = FFmpeg::ValidateAspectRatio(
			info->sample_aspect_ratio);
		const auto guessed = av_guess_frame_rate(format, info, nullptr);
		if (guessed.num > 0 && guessed.den > 0) {
			result.fps = av_q2d(guessed);
		}
	} else if (type == AVMEDIA_TYPE_AUDIO) {
		result.frequency = info->codecpar->sample_rate;
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: initStream audio codecId=%1 frequency=%2.")
			.arg(int(info->codecpar ? info->codecpar->codec_id : AV_CODEC_ID_NONE))
			.arg(result.frequency));
		if (!result.frequency) {
			return result;
		}
		result.codec = FFmpeg::MakeCodecPointer({ .stream = info });
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: initStream audio codec ready=%1 index=%2.")
			.arg(result.codec ? 1 : 0)
			.arg(index));
		if (!result.codec) {
			return result;
		}
	}

	result.decodedFrame = FFmpeg::MakeFramePointer();
	if (!result.decodedFrame) {
		result.codec = nullptr;
		return result;
	}
	result.timeBase = info->time_base;
	result.duration = options.durationOverride
		? options.durationOverride
		: (info->duration != AV_NOPTS_VALUE)
		? FFmpeg::PtsToTime(info->duration, result.timeBase)
		: UnreliableFormatDuration(format, info, mode)
		? kTimeUnknown
		: FFmpeg::PtsToTime(format->duration, FFmpeg::kUniversalTimeBase);
	if (result.duration == kTimeUnknown) {
		result.duration = kDurationUnavailable;
	} else if (result.duration <= 0) {
		result.codec = nullptr;
	} else {
		++result.duration;
		if (result.duration > kDurationMax) {
			result.duration = 0;
			result.codec = nullptr;
		}
	}
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: initStream exit type=%1 index=%2 codec=%3 duration=%4.")
		.arg(int(type))
		.arg(result.index)
		.arg(result.codec ? 1 : 0)
		.arg(qlonglong(result.duration)));
	return result;
}

void File::Context::seekToPosition(
		not_null<AVFormatContext*> format,
		const Stream &stream,
		StartOptions options,
		crl::time position) {
	auto error = FFmpeg::AvErrorWrap();

	if (!position) {
		return;
	} else if (stream.duration == kDurationUnavailable) {
		// Seek in files with unknown duration is not supported.
		return;
	}
	const auto timestamp = FFmpeg::TimeToPts(
		std::clamp(position, crl::time(0), stream.duration - 1),
		stream.timeBase);
	_pendingSeekPrefetchPosition = 0;
	auto mappedPrefetchOffset = int64(-1);
	const auto tryByteSeek = [&](int64 offset, const char *name) {
		error = av_seek_frame(
			format,
			-1,
			offset,
			AVSEEK_FLAG_BYTE);
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File seek attempt "
			"target=%1 flags=%2 name=%3 result=%4 error=%5.")
			.arg(qlonglong(position))
			.arg(AVSEEK_FLAG_BYTE)
			.arg(QString::fromLatin1(name))
			.arg(error.code())
			.arg(error.text()));
		if (!error) {
			return true;
		}
		return false;
	};
	const auto trySeek = [&](int flags, const char *name) {
		error = av_seek_frame(
			format,
			stream.index,
			timestamp,
			flags);
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File seek attempt "
			"target=%1 flags=%2 name=%3 result=%4 error=%5.")
			.arg(qlonglong(position))
			.arg(flags)
			.arg(QString::fromLatin1(name))
			.arg(error.code())
			.arg(error.text()));
		if (!error) {
			if (mappedPrefetchOffset >= 0) {
				prefetchAroundOffset(
					mappedPrefetchOffset,
					position,
					true);
			} else if (_source->smartStreamingEnabled()) {
				_pendingSeekPrefetchPosition = position;
				VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File seek "
					"prefetch deferred target=%1 currentOffset=%2.")
					.arg(qlonglong(position))
					.arg(qlonglong(_offset)));
			}
			return true;
		}
		return false;
	};
	const auto tryMappedSeek = [&](bool explicitSeek) {
		if (!IsMp4LikeFormat(format)) {
			const auto diagnostic = Mp4SeekMapDiagnostic{
				.failure = Mp4SeekMapFailure::FormatUnsupported,
				.headerSize = _source->headerSize(),
			};
			LogMp4SeekMapFailure(format, _size, position, diagnostic);
			return false;
		}
		const auto resolved = ResolveMp4SeekMap(
			_source,
			stream,
			_interrupted,
			position);
		if (!resolved.point) {
			if (!unroll()) {
				LogMp4SeekMapFailure(
					format,
					_size,
					position,
					resolved.diagnostic);
			}
			return false;
		}
		const auto &point = *resolved.point;
		const auto adjustedOffset = std::max<int64>(
			0,
			point.sampleOffset - kSeekPrefetchBackAmount);
		mappedPrefetchOffset = point.sampleOffset;
		const auto tailMoov = Mp4MoovOutsideHeader(resolved.diagnostic);
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File seek map "
			"target=%1 duration=%2 sample=%3 sync=%4 sampleOffset=%5 "
			"adjustedOffset=%6 explicit=%7 moovOffset=%8 moovSize=%9 "
			"tail=%10 fragmented=%11.")
			.arg(qlonglong(position))
			.arg(qlonglong(point.duration))
			.arg(point.targetSample)
			.arg(point.syncSample)
			.arg(qlonglong(point.sampleOffset))
			.arg(qlonglong(adjustedOffset))
			.arg(explicitSeek ? 1 : 0)
			.arg(qlonglong(resolved.diagnostic.moovOffset))
			.arg(qulonglong(resolved.diagnostic.moovSize))
			.arg(tailMoov ? 1 : 0)
			.arg(resolved.diagnostic.fragmented ? 1 : 0));
		if (!tryByteSeek(adjustedOffset, "byte-map")) {
			return false;
		}
		prefetchAroundOffset(mappedPrefetchOffset, position, true);
		return true;
	};
	if (options.sequentialOpen) {
		if (tryMappedSeek(true)) {
			return;
		} else if (unroll()) {
			return;
		}
		if (trySeek(AVSEEK_FLAG_ANY, "any")) {
			return;
		} else if (trySeek(0, "default")) {
			return;
		} else if (trySeek(AVSEEK_FLAG_BACKWARD, "backward")) {
			return;
		}
	} else if (!stream.frequency && _source->smartStreamingEnabled()) {
		if (tryMappedSeek(false)) {
			return;
		} else if (unroll()) {
			return;
		}
		if (trySeek(AVSEEK_FLAG_BACKWARD, "backward")) {
			return;
		}
	} else if (trySeek(AVSEEK_FLAG_BACKWARD, "backward")) {
		return;
	}
	return logFatal(qstr("av_seek_frame"), error);
}

std::variant<FFmpeg::Packet, FFmpeg::AvErrorWrap> File::Context::readPacket() {
	auto error = FFmpeg::AvErrorWrap();

	auto result = FFmpeg::Packet();
	error = av_read_frame(_format.get(), &result.fields());
	if (unroll()) {
		return FFmpeg::AvErrorWrap();
	} else if (!error) {
		return result;
	} else if (error.code() != AVERROR_EOF) {
		logFatal(qstr("av_read_frame"), error);
	}
	return error;
}

void File::Context::start(StartOptions options) {
	Expects(options.seekable || !options.position);

	auto error = FFmpeg::AvErrorWrap();

	if (unroll()) {
		return;
	}
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File start sourceSize=%1 seekable=%2 position=%3 durationOverride=%4 remote=%5.")
		.arg(qlonglong(_size))
		.arg(options.seekable ? 1 : 0)
		.arg(qlonglong(options.position))
		.arg(qlonglong(options.durationOverride))
		.arg(_source->isRemoteLoader() ? 1 : 0));
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File open strategy sequentialOpen=%1 seekableOnOpen=%2.")
		.arg(options.sequentialOpen ? 1 : 0)
		.arg((options.seekable && !options.sequentialOpen) ? 1 : 0));
	auto format = FFmpeg::MakeFormatPointer(
		static_cast<void*>(this),
		&Context::Read,
		nullptr,
		options.seekable ? &Context::Seek : nullptr,
		(options.seekable && !options.sequentialOpen));
	if (!format) {
		return fail(Error::OpenFailed);
	}
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File format context created sourceSize=%1.")
		.arg(qlonglong(_size)));
	if (options.sequentialOpen) {
		format->probesize = kSequentialOpenProbeSize;
		format->max_analyze_duration = kSequentialOpenAnalyzeDuration;
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File sequential analyze limits probesize=%1 analyzeUs=%2.")
			.arg(qlonglong(format->probesize))
			.arg(qlonglong(format->max_analyze_duration)));
	}
	if (options.sequentialOpen && options.seekable && format->pb) {
		format->pb->seek = nullptr;
		format->pb->seekable = 0;
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File sequential analyze seek disabled during stream info."));
	}

	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File calling avformat_find_stream_info size=%1 position=%2.")
		.arg(qlonglong(_size))
		.arg(qlonglong(options.position)));
	if ((error = avformat_find_stream_info(format.get(), nullptr))) {
		return logFatal(qstr("avformat_find_stream_info"), error);
	}
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File avformat_find_stream_info done size=%1.")
		.arg(qlonglong(_size)));
	if (options.sequentialOpen && options.seekable && format->pb) {
		format->pb->seek = &Context::Seek;
		format->pb->seekable = 1;
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File sequential analyze seek restored."));
	}

	const auto mode = _delegate->fileOpenMode();
	auto video = initStream(
		format.get(),
		AVMEDIA_TYPE_VIDEO,
		mode,
		options);
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File after video init interrupted=%1 failed=%2 readTillEnd=%3.")
		.arg(interrupted() ? 1 : 0)
		.arg(failed() ? 1 : 0)
		.arg(_readTillEnd ? 1 : 0));
	if (unroll()) {
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File unroll after video init interrupted=%1 failed=%2.")
			.arg(interrupted() ? 1 : 0)
			.arg(failed() ? 1 : 0));
		return;
	}

	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File before audio init mode=%1.")
		.arg(int(mode)));
	auto audio = initStream(
		format.get(),
		AVMEDIA_TYPE_AUDIO,
		mode,
		options);
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File after audio init interrupted=%1 failed=%2 readTillEnd=%3.")
		.arg(interrupted() ? 1 : 0)
		.arg(failed() ? 1 : 0)
		.arg(_readTillEnd ? 1 : 0));
	if (unroll()) {
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File unroll after audio init interrupted=%1 failed=%2.")
			.arg(interrupted() ? 1 : 0)
			.arg(failed() ? 1 : 0));
		return;
	}

	_source->headerDone();
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File header done headerSize=%1 remote=%2.")
		.arg(_source->headerSize())
		.arg(_source->isRemoteLoader() ? 1 : 0));
	if (_source->smartStreamingEnabled()) {
		const auto duration = std::max(
			(video.codec && video.duration != kDurationUnavailable)
				? video.duration
				: crl::time(0),
			(audio.codec && audio.duration != kDurationUnavailable)
				? audio.duration
				: crl::time(0));
		if (duration > 1) {
			const auto bytesPerSecond = int(std::clamp(
				double(_size) * 1000. / double(duration),
				0.,
				double(64 * 1024 * 1024)));
			_source->setSmartStreamingPlaybackRate(bytesPerSecond);
		}
	}
	if (_source->isRemoteLoader()) {
		sendFullInCache(true);
	}
	if (options.seekable && (video.codec || audio.codec)) {
		seekToPosition(
			format.get(),
			video.codec ? video : audio,
			options,
			options.position);
	}
	if (unroll()) {
		return;
	}

	if (video.codec) {
		_queuedPackets[video.index].reserve(kMaxQueuedPackets);
	}
	if (audio.codec) {
		_queuedPackets[audio.index].reserve(kMaxQueuedPackets);
	}

	const auto header = _source->headerSize();
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File ready callback header=%1 hasVideo=%2 hasAudio=%3 videoIndex=%4 audioIndex=%5.")
		.arg(header)
		.arg(video.codec ? 1 : 0)
		.arg(audio.codec ? 1 : 0)
		.arg(video.index)
		.arg(audio.index));
	if (!_delegate->fileReady(header, std::move(video), std::move(audio))) {
		return fail(Error::OpenFailed);
	}
	_format = std::move(format);
}

void File::Context::sendFullInCache(bool force) {
	const auto started = _fullInCache.has_value();
	if (force || started) {
		const auto nowFullInCache = _source->fullInCache();
		if (!started || *_fullInCache != nowFullInCache) {
			_fullInCache = nowFullInCache;
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File fullInCache changed value=%1 force=%2.")
				.arg(nowFullInCache ? 1 : 0)
				.arg(force ? 1 : 0));
			_delegate->fileFullInCache(nowFullInCache);
		}
	}
}

void File::Context::readNextPacket() {
	auto result = readPacket();
	if (unroll()) {
		return;
	} else if (const auto packet = std::get_if<FFmpeg::Packet>(&result)) {
		const auto index = packet->fields().stream_index;
		const auto i = _queuedPackets.find(index);
		if (i == end(_queuedPackets)) {
			return;
		}
		i->second.push_back(std::move(*packet));
		if (i->second.size() == kMaxQueuedPackets) {
			processQueuedPackets(SleepPolicy::Allowed);
		}
		Assert(i->second.size() < kMaxQueuedPackets);
	} else {
		// Still trying to read by drain.
		Assert(v::is<FFmpeg::AvErrorWrap>(result));
		Assert(v::get<FFmpeg::AvErrorWrap>(result).code() == AVERROR_EOF);
		processQueuedPackets(SleepPolicy::Allowed);
		if (!finished()) {
			handleEndOfFile();
		}
	}
}

void File::Context::handleEndOfFile() {
	_delegate->fileProcessEndOfFile();
	if (_delegate->fileReadMore()) {
		_readTillEnd = false;
		auto error = FFmpeg::AvErrorWrap(av_seek_frame(
			_format.get(),
			-1, // stream_index
			0, // timestamp
			AVSEEK_FLAG_BACKWARD));
		if (error) {
			logFatal(qstr("av_seek_frame"));
		}

		// If we loaded a file till the end then we think it is fully cached,
		// assume we finished loading and don't want to keep all other
		// download tasks throttled because of an active streaming.
		_source->tryRemoveLoaderAsync();
	} else {
		_readTillEnd = true;
	}
}

void File::Context::processQueuedPackets(SleepPolicy policy) {
	const auto more = _delegate->fileProcessPackets(_queuedPackets);
	if (!more && policy == SleepPolicy::Allowed) {
		do {
			_source->startSleep(&_semaphore);
			_semaphore.acquire();
			_source->stopSleep();
		} while (!unroll() && !_delegate->fileReadMore());
	}
}

void File::Context::interrupt() {
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File context interrupt offset=%1 failed=%2 readTillEnd=%3.")
		.arg(qlonglong(_offset))
		.arg(_failed ? 1 : 0)
		.arg(_readTillEnd ? 1 : 0));
	_interrupted = true;
	_semaphore.release();
}

void File::Context::wake() {
	_semaphore.release();
}

bool File::Context::interrupted() const {
	return _interrupted;
}

bool File::Context::failed() const {
	return _failed;
}

bool File::Context::unroll() const {
	return failed() || interrupted();
}

void File::Context::fail(Error error) {
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File context fail error=%1 interrupted=%2 offset=%3.")
		.arg(PlaybackErrorDebugString(error))
		.arg(_interrupted ? 1 : 0)
		.arg(qlonglong(_offset)));
	_failed = true;
	_delegate->fileError(error);
}

bool File::Context::finished() const {
	return unroll() || _readTillEnd;
}

void File::Context::stopStreamingAsync() {
	// If we finished loading we don't want to keep all other
	// download tasks throttled because of an active streaming.
	_source->stopStreamingAsync();
}

File::File(std::shared_ptr<FileSource> source)
: _source(std::move(source)) {
}

File::File(std::shared_ptr<Reader> reader)
: File(MakeFileSource(std::move(reader))) {
}

void File::start(not_null<FileDelegate*> delegate, StartOptions options) {
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File start thread launch seekable=%1 sequentialOpen=%2.")
		.arg(options.seekable ? 1 : 0)
		.arg(options.sequentialOpen ? 1 : 0));
	stop(true);

	_source->startStreaming();
	_context.emplace(delegate, _source.get());

	_thread = std::thread([=, context = &*_context] {
		crl::toggle_fp_exceptions(true);
		context->start(options);
		while (!context->finished()) {
			context->readNextPacket();
		}
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File thread exit interrupted=%1 failed=%2 readTillEnd=%3.")
			.arg(context->interrupted() ? 1 : 0)
			.arg(context->failed() ? 1 : 0)
			.arg(context->finished() ? 1 : 0));
		if (!context->interrupted()) {
			context->stopStreamingAsync();
		}
	});
}

void File::wake() {
	Expects(_context.has_value());

	_context->wake();
}

void File::stop(bool stillActive) {
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: File stop stillActive=%1 joinable=%2 hasContext=%3.")
		.arg(stillActive ? 1 : 0)
		.arg(_thread.joinable() ? 1 : 0)
		.arg(_context.has_value() ? 1 : 0));
	if (_thread.joinable()) {
		_context->interrupt();
		_thread.join();
	}
	_source->stopStreaming(stillActive);
	_context.reset();
}

bool File::isRemoteLoader() const {
	return _source->isRemoteLoader();
}

crl::time File::smartStreamingRecoveryBuffer() const {
	return _source->smartStreamingRecoveryBuffer();
}

void File::setLoaderPriority(int priority) {
	_source->setLoaderPriority(priority);
}

void File::setSmartStreamingBufferPressure(bool pressure) {
	_source->setSmartStreamingBufferPressure(pressure);
}

void File::setSmartStreamingPlaybackRate(int bytesPerSecond) {
	_source->setSmartStreamingPlaybackRate(bytesPerSecond);
}

void File::notifySmartStreamingSeek() {
	_source->notifySmartStreamingSeek();
}

int64 File::size() const {
	return _source->size();
}

rpl::producer<SpeedEstimate> File::speedEstimate() const {
	return _source->speedEstimate();
}

File::~File() {
	stop();
}

} // namespace Streaming
} // namespace Media
