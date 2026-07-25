/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_reader.h"

#include "media/streaming/media_streaming_boost.h"
#include "media/streaming/media_streaming_common.h"
#include "media/streaming/media_streaming_debug.h"
#include "media/streaming/media_streaming_loader.h"
#include "settings.h"
#include "storage/cache/storage_cache_database.h"
#include "storage/storage_non_premium_delay.h"

namespace Media {
namespace Streaming {
namespace {

constexpr auto kPartSize = Loader::kPartSize;
constexpr auto kPartsInSlice = 64;
constexpr auto kInSlice = uint32(kPartsInSlice * kPartSize);
constexpr auto kMaxPartsInHeader = 64;
constexpr auto kMaxOnlyInHeader = 80 * kPartSize;
constexpr auto kPartsOutsideFirstSliceGood = 8;
constexpr auto kSlicesInMemory = 2;
constexpr auto kSmartPreloadRecoveryFullDuration
	= 15 * crl::time(1000);
constexpr auto kSmartPreloadRecoveryTaperDuration
	= 15 * crl::time(1000);
constexpr auto kSmartPreloadRecoveryDuration
	= kSmartPreloadRecoveryFullDuration
	+ kSmartPreloadRecoveryTaperDuration;
constexpr auto kSmartPreloadRecoveryFullPercent = 100;
constexpr auto kSmartPreloadRecoveryTaperPercent = 75;
constexpr auto kSmartSeekPrefetchTargetDuration = crl::time(8000);
constexpr auto kSmartSeekUrgentTargetDuration = crl::time(2000);
constexpr auto kSmartSeekPrefetchHighBitrateDuration = crl::time(12000);
constexpr auto kSmartSeekUrgentHighBitrateDuration = crl::time(3000);
constexpr auto kSmartSeekLocalRecoveryDuration = 15 * crl::time(1000);
// Pressure stays local only while the urgent seek window fills. Long freezes
// block DC catch-up after first frame on high-bitrate streams.
constexpr auto kSmartSeekPressureLocalDuration = 4 * crl::time(1000);
// Once pressure latches true, ignore early clears to stop thrash toggles.
constexpr auto kSmartPressureMinHoldDuration = 2 * crl::time(1000);
// Up to ~2 slices of 128KB parts (16MB) so high-bitrate streams can bank
// more than the current-slice remainder when throughput briefly exceeds rate.
constexpr auto kSmartMaxPreloadParts = 128;
constexpr auto kSmartServerRecoveryMinimumDuration = 4 * crl::time(1000);
constexpr auto kSmartServerRecoveryPenaltyStep = 2 * crl::time(1000);
constexpr auto kSmartServerRecoveryWaitMargin = 2 * crl::time(1000);
constexpr auto kSmartServerRecoveryMaximumDuration = 20 * crl::time(1000);
constexpr auto kSmartSeekPrefetchMinimum = int64(4) * 1024 * 1024;
constexpr auto kSmartSeekPrefetchMaximum = int64(16) * 1024 * 1024;
constexpr auto kSmartSeekPrefetchHighBitrateMaximum = int64(20) * 1024 * 1024;
constexpr auto kSmartSeekUrgentMinimum = int64(2) * 1024 * 1024;
constexpr auto kSmartSeekUrgentMaximum = int64(4) * 1024 * 1024;
constexpr auto kSmartSeekUrgentHighBitrateMaximum = int64(8) * 1024 * 1024;
constexpr auto kSmartCancelLogMinInterval = crl::time(1000);
// Sparse Smart diagnostics: state transitions + at most one snapshot / interval.
constexpr auto kSmartCatchupLogMinInterval = 2 * crl::time(1000);
// After seek grace, require this much sustained low speed before Throttle
// (Smart non-Premium never uses Throttle; this guards other boost levels).
constexpr auto kSmartThrottleConfirmSamples = 3;

using PartsMap = base::flat_map<uint32, QByteArray>;

[[nodiscard]] int DownloadBoostLevel() {
	const auto boost = GetEnhancedInt("net_download_speed_boost");
	return (boost < 0) ? 0 : (boost > 6) ? 6 : boost;
}

[[nodiscard]] int StreamingRequestsLimit() {
	return BoostProfileFor(DownloadBoostLevel()).requestsLimit;
}

[[nodiscard]] bool StreamingSeekCancelEnabled() {
	return BoostProfileFor(DownloadBoostLevel()).seekCancelEnabled;
}

[[nodiscard]] int64 StreamingSeekCancelJumpBytes() {
	return int64(BoostProfileFor(DownloadBoostLevel()).seekCancelJumpParts)
		* kPartSize;
}

[[nodiscard]] int64 StreamingSeekCancelGuardBytes() {
	return int64(BoostProfileFor(DownloadBoostLevel()).seekCancelGuardParts)
		* kPartSize;
}

[[nodiscard]] bool StreamingTailPrefetchEnabled() {
	return BoostProfileFor(DownloadBoostLevel()).tailPrefetchParts > 0;
}

[[nodiscard]] int64 StreamingTailPrefetchBytes() {
	return int64(BoostProfileFor(DownloadBoostLevel()).tailPrefetchParts)
		* kPartSize;
}

// Burst-mode adaptive preload multiplier: 100 = base depth, 200 = 2x.
// Per-Reader state is stored on the Reader; these free helpers return the
// static base values and the Reader scales them via atomics before passing
// the final counts into Slices::fill.

[[nodiscard]] int PreloadPartsAhead() {
	return BoostProfileFor(DownloadBoostLevel()).preloadPartsAhead;
}

[[nodiscard]] int64 BytesForDuration(
		int bytesPerSecond,
		crl::time duration) {
	return (bytesPerSecond > 0 && duration > 0)
		? (int64(bytesPerSecond) * duration + 999) / 1000
		: 0;
}

[[nodiscard]] int UpdateIntegerEma(int previous, int sample) {
	return previous
		? int((int64(previous) * 3 + int64(sample) * 2) / 5)
		: sample;
}

struct ParsedCacheEntry {
	PartsMap parts;
	std::optional<PartsMap> included;
};

bool IsContiguousSerialization(int serializedSize, int maxSliceSize) {
	return !(serializedSize % kPartSize) || (serializedSize == maxSliceSize);
}

bool IsFullInHeader(int64 size) {
	return (size <= kMaxOnlyInHeader);
}

bool ComputeIsGoodHeader(int64 size, const PartsMap &header) {
	if (IsFullInHeader(size)) {
		return false;
	}
	const auto outsideFirstSliceIt = ranges::lower_bound(
		header,
		kInSlice,
		ranges::less(),
		&PartsMap::value_type::first);
	const auto outsideFirstSlice = end(header) - outsideFirstSliceIt;
	return (outsideFirstSlice <= kPartsOutsideFirstSliceGood);
}

int SlicesCount(uint32 size) {
	const auto result = (size + kInSlice - 1) / kInSlice;

	Ensures(result < 0x1FFU);
	return result;
}

int MaxSliceSize(int sliceNumber, uint32 size) {
	return !sliceNumber
		? size
		: (sliceNumber == SlicesCount(size))
		? (size - (sliceNumber - 1) * kInSlice)
		: kInSlice;
}

bytes::const_span ParseComplexCachedMap(
		PartsMap &result,
		bytes::const_span data,
		int maxSize) {
	const auto takeInt = [&]() -> std::optional<uint32> {
		if (data.size() < sizeof(uint32)) {
			return std::nullopt;
		}
		const auto bytes = data.data();
		const auto result = *reinterpret_cast<const uint32*>(bytes);
		data = data.subspan(sizeof(uint32));
		return result;
	};
	const auto takeBytes = [&](int count) {
		if (count <= 0 || data.size() < count) {
			return bytes::const_span();
		}
		const auto result = data.subspan(0, count);
		data = data.subspan(count);
		return result;
	};
	const auto maybeCount = takeInt();
	if (!maybeCount) {
		return {};
	}
	const auto count = *maybeCount;
	if (!count || count > (kMaxOnlyInHeader / kPartSize)) {
		return data;
	}
	for (auto i = 0; i != count; ++i) {
		const auto offset = takeInt().value_or(0);
		const auto size = takeInt().value_or(0);
		const auto bytes = takeBytes(size);
		if (offset >= maxSize
			|| !size
			|| size > maxSize
			|| offset + size > maxSize
			|| bytes.size() != size) {
			return {};
		}
		result.try_emplace(
			offset,
			reinterpret_cast<const char*>(bytes.data()),
			bytes.size());
	}
	return data;
}

bytes::const_span ParseCachedMap(
		PartsMap &result,
		bytes::const_span data,
		int maxSize) {
	const auto size = int(data.size());
	if (IsContiguousSerialization(size, maxSize)) {
		if (size > maxSize) {
			return {};
		}
		for (auto offset = int64(); offset < size; offset += kPartSize) {
			const auto part = data.subspan(
				offset,
				std::min(kPartSize, size - offset));
			result.try_emplace(
				uint32(offset),
				reinterpret_cast<const char*>(part.data()),
				part.size());
		}
		return {};
	}
	return ParseComplexCachedMap(result, data, maxSize);
}

ParsedCacheEntry ParseCacheEntry(
		bytes::const_span data,
		int sliceNumber,
		int64 size) {
	auto result = ParsedCacheEntry();
	const auto remaining = ParseCachedMap(
		result.parts,
		data,
		MaxSliceSize(sliceNumber, size));
	if (!sliceNumber && ComputeIsGoodHeader(size, result.parts)) {
		result.included = PartsMap();
		ParseCachedMap(*result.included, remaining, MaxSliceSize(1, size));
	}
	return result;
}

template <typename Range> // Range::value_type is Pair<int, QByteArray>
uint32 FindNotLoadedStart(Range &&parts, uint32 offset) {
	auto result = offset;
	for (const auto &part : parts) {
		const auto partStart = part.first;
		const auto partEnd = partStart + part.second.size();
		if (partStart <= result && partEnd >= result) {
			result = partEnd;
		} else {
			break;
		}
	}
	return result;
}

template <typename Range> // Range::value_type is Pair<uint32, QByteArray>
void CopyLoaded(
		bytes::span buffer,
		Range &&parts,
		uint32 offset,
		uint32 till) {
	auto filled = offset;
	for (const auto &part : parts) {
		const auto bytes = bytes::make_span(part.second);
		const auto partStart = part.first;
		const auto partEnd = uint32(partStart + bytes.size());
		const auto copyTill = std::min(partEnd, till);
		Assert(partStart <= filled && filled < copyTill);

		const auto from = filled - partStart;
		const auto copy = copyTill - filled;
		bytes::copy(buffer, bytes.subspan(from, copy));
		buffer = buffer.subspan(copy);
		filled += copy;
	}
}

} // namespace

template <int Size>
bool Reader::StackIntVector<Size>::add(uint32 value) {
	using namespace rpl::mappers;

	const auto i = ranges::find_if(_storage, _1 == uint32(-1));
	if (i == end(_storage)) {
		return false;
	}
	*i = value;
	const auto next = i + 1;
	if (next != end(_storage)) {
		*next = -1;
	}
	return true;
}

template <int Size>
auto Reader::StackIntVector<Size>::values() const {
	using namespace rpl::mappers;

	return ranges::views::all(
		_storage
	) | ranges::views::take_while(_1 != uint32(-1));
}

struct Reader::CacheHelper {
	explicit CacheHelper(Storage::Cache::Key baseKey);

	Storage::Cache::Key key(int sliceNumber) const;

	const Storage::Cache::Key baseKey;

	QMutex mutex;
	base::flat_map<uint32, PartsMap> results;
	std::vector<int> sizes;
	std::atomic<crl::semaphore*> waiting = nullptr;
};

Reader::CacheHelper::CacheHelper(Storage::Cache::Key baseKey)
: baseKey(baseKey) {
}

Storage::Cache::Key Reader::CacheHelper::key(int sliceNumber) const {
	return Storage::Cache::Key{ baseKey.high, baseKey.low + sliceNumber };
}

void Reader::Slice::processCacheData(PartsMap &&data) {
	Expects((flags & Flag::LoadingFromCache) != 0);
	Expects(!(flags & Flag::LoadedFromCache));

	const auto guard = gsl::finally([&] {
		flags |= Flag::LoadedFromCache;
		flags &= ~Flag::LoadingFromCache;
	});
	if (parts.empty()) {
		parts = std::move(data);
	} else {
		for (auto &[offset, bytes] : data) {
			parts.emplace(offset, std::move(bytes));
		}
	}
}

void Reader::Slice::addPart(uint32 offset, QByteArray bytes) {
	Expects(!parts.contains(offset));

	parts.emplace(offset, std::move(bytes));
	if (flags & Flag::LoadedFromCache) {
		flags |= Flag::ChangedSinceCache;
	}
}

auto Reader::Slice::prepareFill(
		uint32 from,
		uint32 till,
		int preloadParts,
		int requestsLimit) -> PrepareFillResult {
	auto result = PrepareFillResult();

	result.ready = false;
	const auto fromOffset = (from / kPartSize) * kPartSize;
	const auto tillPart = (till + kPartSize - 1) / kPartSize;
	const auto preloadTillOffset = (tillPart + preloadParts)
		* kPartSize;

	const auto after = ranges::upper_bound(
		parts,
		from,
		ranges::less(),
		&PartsMap::value_type::first);
	if (after == begin(parts)) {
		result.offsetsFromLoader = offsetsFromLoader(
			fromOffset,
			preloadTillOffset,
			requestsLimit);
		return result;
	}

	const auto start = after - 1;
	const auto finish = ranges::lower_bound(
		start,
		end(parts),
		till,
		ranges::less(),
		&PartsMap::value_type::first);
	const auto haveTill = FindNotLoadedStart(
		ranges::make_subrange(start, finish),
		fromOffset);
	if (haveTill < till) {
		result.offsetsFromLoader = offsetsFromLoader(
			haveTill,
			preloadTillOffset,
			requestsLimit);
		return result;
	}
	result.ready = true;
	result.start = start;
	result.finish = finish;
	result.offsetsFromLoader = offsetsFromLoader(
		tillPart * kPartSize,
		preloadTillOffset,
		requestsLimit);
	return result;
}

auto Reader::Slice::offsetsFromLoader(
	uint32 from,
	uint32 till,
	int requestsLimit) const
-> StackIntVector<Reader::kLoadFromRemoteMax> {
	auto result = StackIntVector<kLoadFromRemoteMax>();
	const auto limit = requestsLimit;
	auto added = 0;

	const auto after = ranges::upper_bound(
		parts,
		from,
		ranges::less(),
		&PartsMap::value_type::first);
	auto check = (after == begin(parts)) ? after : (after - 1);
	const auto end = parts.end();
	for (auto offset = from; offset != till; offset += kPartSize) {
			while (check != end && check->first < offset) {
				++check;
			}
			if (check != end && check->first == offset) {
				continue;
			} else if (added >= limit || !result.add(offset)) {
				break;
			}
			++added;
		}
	return result;
}

Reader::Slices::Slices(uint32 size, bool useCache)
: _size(size) {
	Expects(size > 0);

	if (useCache) {
		_header.flags |= Slice::Flag::LoadingFromCache;
	} else {
		_headerMode = HeaderMode::NoCache;
	}
	if (!isFullInHeader()) {
		_data.resize(SlicesCount(_size));
	}
}

bool Reader::Slices::headerModeUnknown() const {
	return (_headerMode == HeaderMode::Unknown);
}

bool Reader::Slices::isFullInHeader() const {
	return IsFullInHeader(_size);
}

bool Reader::Slices::isGoodHeader() const {
	return (_headerMode == HeaderMode::Good);
}

bool Reader::Slices::computeIsGoodHeader() const {
	return ComputeIsGoodHeader(_size, _header.parts);
}

void Reader::Slices::headerDone(bool fromCache) {
	if (_headerMode != HeaderMode::Unknown) {
		return;
	}
	_headerMode = isFullInHeader()
		? HeaderMode::Full
		: computeIsGoodHeader()
		? HeaderMode::Good
		: HeaderMode::Small;
	if (!fromCache) {
		for (auto &slice : _data) {
			using Flag = Slice::Flag;
			Assert(!(slice.flags
				& (Flag::LoadingFromCache | Flag::LoadedFromCache)));
			slice.flags |= Slice::Flag::LoadedFromCache;
		}
	}
}

int Reader::Slices::headerSize() const {
	return _header.parts.size() * kPartSize;
}

bool Reader::Slices::fullInCache() const {
	return _fullInCache;
}

int Reader::Slices::requestSliceSizesCount() const {
	if (!headerModeUnknown() || isFullInHeader()) {
		return 0;
	}
	return _data.size();
}

bool Reader::Slices::headerWontBeFilled() const {
	return headerModeUnknown()
		&& (_header.parts.size() >= kMaxPartsInHeader);
}

void Reader::Slices::applyHeaderCacheData() {
	using namespace rpl::mappers;

	const auto applyWhile = [&](auto &&predicate) {
		for (const auto &[offset, part] : _header.parts) {
			const auto index = int(offset / kInSlice);
			if (!predicate(index)) {
				break;
			}
			_data[index].addPart(
				offset - index * kInSlice,
				base::duplicate(part));
		}
	};
	if (_header.parts.empty()) {
		return;
	} else if (_headerMode == HeaderMode::Good) {
		// Always apply data to first block if it is cached in the header.
		applyWhile(_1 == 0);
	} else if (_headerMode != HeaderMode::Unknown) {
		return;
	} else if (isFullInHeader()) {
		headerDone(true);
	} else {
		applyWhile(_1 < int(_data.size()));
		headerDone(true);
	}
}

void Reader::Slices::processCacheResult(int sliceNumber, PartsMap &&result) {
	Expects(sliceNumber >= 0 && sliceNumber <= _data.size());

	auto &slice = (sliceNumber ? _data[sliceNumber - 1] : _header);
	if (!sliceNumber && isGoodHeader()) {
		// We've loaded header slice because really we wanted first slice.
		if (!(_data[0].flags & Slice::Flag::LoadingFromCache)) {
			// We could've already unloaded this slice using LRU _usedSlices.
			return;
		}
		// So just process whole result even if we didn't want header really.
		slice.flags |= Slice::Flag::LoadingFromCache;
		slice.flags &= ~Slice::Flag::LoadedFromCache;
	}
	if (!(slice.flags & Slice::Flag::LoadingFromCache)) {
		// We could've already unloaded this slice using LRU _usedSlices.
		return;
	}
	slice.processCacheData(std::move(result));
	checkSliceFullLoaded(sliceNumber);
	if (!sliceNumber) {
		applyHeaderCacheData();
		if (isGoodHeader()) {
			// When we first read header we don't request the first slice.
			// But we get it, so let's apply it anyway.
			_data[0].flags |= Slice::Flag::LoadingFromCache;
		}
	}
}

void Reader::Slices::processCachedSizes(const std::vector<int> &sizes) {
	Expects(sizes.size() == _data.size());

	using Flag = Slice::Flag;
	const auto count = int(sizes.size());
	auto loadedCount = 0;
	for (auto i = 0; i != count; ++i) {
		const auto sliceNumber = (i + 1);
		const auto sliceSize = (sliceNumber < _data.size())
			? kInSlice
			: (_size - (sliceNumber - 1) * kInSlice);
		const auto loaded = (sizes[i] == sliceSize);

		if (_data[i].flags & Flag::FullInCache) {
			++loadedCount;
		} else if (loaded) {
			_data[i].flags |= Flag::FullInCache;
			++loadedCount;
		}
	}
	_fullInCache = (loadedCount == count);
}

void Reader::Slices::checkSliceFullLoaded(int sliceNumber) {
	if (!sliceNumber && !isFullInHeader()) {
		return;
	}
	const auto partsCount = [&] {
		if (!sliceNumber) {
			return (_size + kPartSize - 1) / kPartSize;
		}
		return (sliceNumber < _data.size())
			? kPartsInSlice
			: ((_size - (sliceNumber - 1) * kInSlice + kPartSize - 1)
				/ kPartSize);
	}();
	auto &slice = (sliceNumber ? _data[sliceNumber - 1] : _header);
	const auto loaded = (slice.parts.size() == partsCount);

	using Flag = Slice::Flag;
	if ((slice.flags & Flag::FullInCache) && !loaded) {
		slice.flags &= ~Flag::FullInCache;
		_fullInCache = false;
	} else if (!(slice.flags & Flag::FullInCache) && loaded) {
		slice.flags |= Flag::FullInCache;
		_fullInCache = checkFullInCache();
	}
}

bool Reader::Slices::checkFullInCache() const {
	using Flag = Slice::Flag;
	if (isFullInHeader()) {
		return (_header.flags & Flag::FullInCache);
	}
	return ranges::none_of(_data, [](const Slice &slice) {
		return !(slice.flags & Flag::FullInCache);
	});
}

void Reader::Slices::processPart(
		uint32 offset,
		QByteArray &&bytes) {
	Expects(isFullInHeader() || (offset / kInSlice < _data.size()));

	if (isFullInHeader()) {
		_header.addPart(offset, bytes);
		checkSliceFullLoaded(0);
		return;
	//} else if (_headerMode == HeaderMode::Unknown) {
	//	if (_header.parts.contains(offset)) {
	//		return;
	//	} else if (_header.parts.size() < kMaxPartsInHeader) {
	//		_header.addPart(offset, bytes);
	//	}
	}
	const auto index = offset / kInSlice;
	_data[index].addPart(offset - index * kInSlice, std::move(bytes));
	checkSliceFullLoaded(index + 1);
}

auto Reader::Slices::fill(
		uint32 offset,
		bytes::span buffer,
		int preloadParts,
		int requestsLimit) -> FillResult {
	Expects(!buffer.empty());
	Expects(offset < _size);
	Expects(offset + buffer.size() <= _size);
	Expects(buffer.size() <= kInSlice);

	using Flag = Slice::Flag;

	if (_headerMode != HeaderMode::NoCache
		&& !(_header.flags & Flag::LoadedFromCache)) {
		// Waiting for initial cache query.
		Assert(waitingForHeaderCache());
		return {};
	} else if (isFullInHeader()) {
		return fillFromHeader(offset, buffer, preloadParts, requestsLimit);
	}

	auto result = FillResult();
	const auto till = uint32(offset + buffer.size());
	const auto fromSlice = offset / kInSlice;
	const auto readTillSlice = (till + kInSlice - 1) / kInSlice;
	Assert((fromSlice + 1 == readTillSlice || fromSlice + 2 == readTillSlice)
		&& readTillSlice <= _data.size());

	const auto cacheNotLoaded = [&](int sliceIndex) {
		return (_headerMode != HeaderMode::NoCache)
			&& (_headerMode != HeaderMode::Unknown)
			&& !(_data[sliceIndex].flags & Flag::LoadedFromCache);
	};
	const auto handlePrepareResult = [&](
			int sliceIndex,
			const Slice::PrepareFillResult &prepared) {
		if (cacheNotLoaded(sliceIndex)) {
			return;
		}
		for (const auto partOffset : prepared.offsetsFromLoader.values()) {
			const auto full = partOffset + sliceIndex * kInSlice;
			if (partOffset < kInSlice && full < _size) {
				result.offsetsFromLoader.add(full);
			}
		}
	};
	const auto handleReadFromCache = [&](int sliceIndex, bool block) {
		if (cacheNotLoaded(sliceIndex)) {
			if (!(_data[sliceIndex].flags & Flag::LoadingFromCache)) {
				_data[sliceIndex].flags |= Flag::LoadingFromCache;
				result.sliceNumbersFromCache.add(sliceIndex + 1);
			}
			if (block) {
				result.state = FillState::WaitingCache;
			}
		}
	};
	const auto addToHeader = [&](int slice, auto parts) {
		if (_headerMode == HeaderMode::Unknown) {
			for (const auto &part : parts) {
				const auto totalOffset = slice * kInSlice + part.first;
				if (!_header.parts.contains(totalOffset)
					&& _header.parts.size() < kMaxPartsInHeader) {
					_header.addPart(totalOffset, part.second);
				}
			}
		}
	};
	const auto firstFrom = offset - fromSlice * kInSlice;
	const auto firstTill = std::min(kInSlice, till - fromSlice * kInSlice);
	const auto secondFrom = 0;
	const auto secondTill = (till > (fromSlice + 1) * kInSlice)
		? (till - (fromSlice + 1) * kInSlice)
		: 0;
	// When preload needs more than the remainder of the current 8MB slice,
	// also schedule the next slice. Read readiness still only depends on
	// slices covered by the actual buffer span.
	const auto remainingFirstParts = std::max(
		1,
		int((kInSlice - firstFrom + kPartSize - 1) / kPartSize));
	const auto wantNextPreload = (preloadParts > remainingFirstParts)
		&& (fromSlice + 1 < uint32(_data.size()));
	const auto useSecondSlice = (readTillSlice > fromSlice + 1)
		|| wantNextPreload;
	const auto first = _data[fromSlice].prepareFill(
		firstFrom,
		firstTill,
		preloadParts,
		requestsLimit);
	const auto second = useSecondSlice
		? _data[fromSlice + 1].prepareFill(
			secondFrom,
			// till=0 still issues forward preload requests inside the slice.
			secondTill,
			preloadParts,
			requestsLimit)
		: Slice::PrepareFillResult();
	handlePrepareResult(fromSlice, first);
	if (useSecondSlice) {
		if (cacheNotLoaded(fromSlice + 1)) {
			// Prefetch-only next slice must not block current-frame reads.
			handleReadFromCache(fromSlice + 1, secondTill > 0);
		} else {
			handlePrepareResult(fromSlice + 1, second);
		}
	}
	const auto readReady = first.ready
		&& (secondTill == 0 || second.ready);
	if (readReady) {
		markSliceUsed(fromSlice);
		auto &&list = ranges::make_subrange(first.start, first.finish);
		CopyLoaded(buffer, list, firstFrom, firstTill);
		addToHeader(fromSlice, list);
		if (secondTill > 0) {
			markSliceUsed(fromSlice + 1);
			auto &&list = ranges::make_subrange(second.start, second.finish);
			CopyLoaded(
				buffer.subspan(firstTill - firstFrom),
				list,
				secondFrom,
				secondTill);
			addToHeader(fromSlice + 1, list);
		}
		result.toCache = serializeAndUnloadUnused();
		result.state = FillState::Success;
	} else {
		handleReadFromCache(fromSlice, true);
		if (secondTill > 0) {
			handleReadFromCache(fromSlice + 1, true);
		}
	}
	return result;
}

auto Reader::Slices::fillFromHeader(
		uint32 offset,
		bytes::span buffer,
		int preloadParts,
		int requestsLimit) -> FillResult {
	auto result = FillResult();
	const auto from = offset;
	const auto till = uint32(offset + buffer.size());

	const auto prepared = _header.prepareFill(
		from,
		till,
		preloadParts,
		requestsLimit);
	for (const auto full : prepared.offsetsFromLoader.values()) {
		if (full < _size) {
			result.offsetsFromLoader.add(full);
		}
	}
	if (prepared.ready) {
		CopyLoaded(
			buffer,
			ranges::make_subrange(prepared.start, prepared.finish),
			from,
			till);
		result.state = FillState::Success;
	}
	return result;
}

QByteArray Reader::Slices::partForDownloader(uint32 offset) const {
	Expects(offset < _size);

	if (const auto i = _header.parts.find(offset); i != end(_header.parts)) {
		return i->second;
	} else if (isFullInHeader()) {
		return QByteArray();
	}
	const auto index = offset / kInSlice;
	const auto &slice = _data[index];
	const auto i = slice.parts.find(offset - index * kInSlice);
	return (i != end(slice.parts)) ? i->second : QByteArray();
}

bool Reader::Slices::hasPart(uint32 offset) const {
	Expects(offset < _size);

	if (_header.parts.contains(offset)) {
		return true;
	} else if (isFullInHeader()) {
		return false;
	}
	const auto index = offset / kInSlice;
	const auto relative = offset - index * kInSlice;
	return _data[index].parts.contains(relative);
}

bool Reader::Slices::waitingForHeaderCache() const {
	return (_header.flags & Slice::Flag::LoadingFromCache);
}

bool Reader::Slices::readCacheForDownloaderRequired(uint32 offset) {
	Expects(offset < _size);
	Expects(!waitingForHeaderCache());

	if (isFullInHeader()) {
		return false;
	}
	const auto index = offset / kInSlice;
	auto &slice = _data[index];
	return !(slice.flags & Slice::Flag::LoadedFromCache);
}

void Reader::Slices::markSliceUsed(int sliceIndex) {
	const auto i = ranges::find(_usedSlices, sliceIndex);
	const auto end = _usedSlices.end();
	if (i == end) {
		_usedSlices.push_back(sliceIndex);
	} else {
		const auto next = i + 1;
		if (next != end) {
			std::rotate(i, next, end);
		}
	}
}

int Reader::Slices::maxSliceSize(int sliceNumber) const {
	return MaxSliceSize(sliceNumber, _size);
}

Reader::SerializedSlice Reader::Slices::serializeAndUnloadUnused() {
	using Flag = Slice::Flag;

	if (_headerMode == HeaderMode::Unknown
		|| _usedSlices.size() <= kSlicesInMemory) {
		return {};
	}
	const auto purgeSlice = _usedSlices.front();
	_usedSlices.pop_front();
	if (!(_data[purgeSlice].flags & Flag::LoadedFromCache)) {
		// If the only data in this slice was from _header, just leave it.
		return {};
	}
	const auto noNeedToSaveToCache = [&] {
		if (_headerMode == HeaderMode::NoCache) {
			// Cache is not used.
			return true;
		} else if (!(_data[purgeSlice].flags & Flag::ChangedSinceCache)) {
			// If no data was changed we should still save first slice,
			// if header data was changed since loading from cache.
			// Otherwise in destructor we won't be able to unload header.
			if (!isGoodHeader()
				|| (purgeSlice > 0)
				|| (!(_header.flags & Flag::ChangedSinceCache))) {
				return true;
			}
		}
		return false;
	}();
	if (noNeedToSaveToCache) {
		unloadSlice(_data[purgeSlice]);
		return {};
	}
	return serializeAndUnloadSlice(purgeSlice + 1);
}

Reader::SerializedSlice Reader::Slices::serializeAndUnloadSlice(
		int sliceNumber) {
	Expects(_headerMode != HeaderMode::Unknown);
	Expects(_headerMode != HeaderMode::NoCache);
	Expects(sliceNumber >= 0 && sliceNumber <= _data.size());

	if (isGoodHeader() && (sliceNumber == 1)) {
		return serializeAndUnloadSlice(0);
	}
	const auto writeHeaderAndSlice = isGoodHeader() && !sliceNumber;

	auto &slice = sliceNumber ? _data[sliceNumber - 1] : _header;
	const auto count = slice.parts.size();
	Assert(count > 0);

	auto result = SerializedSlice();
	result.number = sliceNumber;

	// We always use complex serialization for header + first slice.
	const auto continuousTill = writeHeaderAndSlice
		? 0
		: FindNotLoadedStart(slice.parts, 0);
	const auto continuous = (continuousTill > slice.parts.back().first);
	if (continuous) {
		// All data is continuous.
		result.data.reserve(count * kPartSize);
		for (const auto &[offset, part] : slice.parts) {
			result.data.append(part);
		}
	} else {
		result.data = serializeComplexSlice(slice);
		if (writeHeaderAndSlice) {
			result.data.append(serializeAndUnloadFirstSliceNoHeader());
		}

		// Make sure this data won't be taken for full continuous data.
		const auto maxSize = maxSliceSize(sliceNumber);
		while (IsContiguousSerialization(result.data.size(), maxSize)) {
			result.data.push_back(char(0));
		}
	}

	// We may serialize header in the middle of streaming, if we use
	// HeaderMode::Good and we unload first slice. We still require
	// header data to continue working, so don't really unload the header.
	if (sliceNumber) {
		unloadSlice(slice);
	} else {
		slice.flags &= ~Slice::Flag::ChangedSinceCache;
	}
	return result;
}

void Reader::Slices::unloadSlice(Slice &slice) const {
	const auto full = (slice.flags & Slice::Flag::FullInCache);
	slice = Slice();
	if (full) {
		slice.flags |= Slice::Flag::FullInCache;
	}
}

QByteArray Reader::Slices::serializeComplexSlice(const Slice &slice) const {
	return SerializeComplexPartsMap(slice.parts);
}

QByteArray Reader::Slices::serializeAndUnloadFirstSliceNoHeader() {
	Expects(_data[0].flags & Slice::Flag::LoadedFromCache);

	auto &slice = _data[0];
	for (const auto &[offset, part] : _header.parts) {
		slice.parts.erase(offset);
	}
	auto result = serializeComplexSlice(slice);
	unloadSlice(slice);
	return result;
}

Reader::SerializedSlice Reader::Slices::unloadToCache() {
	if (_headerMode == HeaderMode::Unknown
		|| _headerMode == HeaderMode::NoCache) {
		return {};
	}
	if (_header.flags & Slice::Flag::ChangedSinceCache) {
		return serializeAndUnloadSlice(0);
	}
	for (auto i = 0, count = int(_data.size()); i != count; ++i) {
		if (_data[i].flags & Slice::Flag::ChangedSinceCache) {
			return serializeAndUnloadSlice(i + 1);
		}
	}
	return {};
}

Reader::Reader(
	std::unique_ptr<Loader> loader,
	Storage::Cache::Database *cache)
: _loader(std::move(loader))
, _premiumSession(_loader->premiumSession())
, _cache(cache)
, _cacheHelper(cache ? InitCacheHelper(_loader->baseCacheKey()) : nullptr)
, _slices(_loader->size(), _cacheHelper != nullptr) {
	_loader->parts(
	) | rpl::on_next([=](LoadedPart &&part) {
		if (_attachedDownloader && !part.cancelled) {
			_partsForDownloader.fire_copy(part);
		}
		if (_streamingActive.load(std::memory_order_acquire)) {
			_loadedParts.emplace(std::move(part));
		}
		if (const auto waiting = _waiting.load(std::memory_order_acquire)) {
			_waiting.store(nullptr, std::memory_order_release);
			waiting->release();
		}
	}, _lifetime);

	const auto applyServerDelay = [=](ServerDelay delay) {
		if (_premiumSession) {
			return;
		}
		const auto now = crl::now();
		const auto remainingWait = int(std::clamp(
			delay.limitedUntil - now,
			crl::time(0),
			crl::time(std::numeric_limits<int>::max())));
		const auto observedWait = std::max(delay.waitMs, remainingWait);
		const auto previousRecoveryUntil = _serverRecoveryUntil.load(
			std::memory_order_relaxed);
		if (previousRecoveryUntil <= now) {
			_serverObservedWaitMs.store(
				observedWait,
				std::memory_order_relaxed);
		} else if (observedWait > 0) {
			_serverObservedWaitMs.store(
				std::max(
					_serverObservedWaitMs.load(std::memory_order_relaxed),
					observedWait),
				std::memory_order_relaxed);
		}
		_serverDcId.store(delay.dcId, std::memory_order_relaxed);
		const auto limitedUntil = std::max(
			_serverLimitedUntil.load(std::memory_order_relaxed),
			delay.limitedUntil);
		const auto recoveryUntil = std::max(
			previousRecoveryUntil,
			delay.recoveryUntil);
		if (recoveryUntil <= now) {
			return;
		}
		_serverLimitedUntil.store(limitedUntil, std::memory_order_relaxed);
		_serverRecoveryUntil.store(recoveryUntil, std::memory_order_relaxed);
		_serverPenalty.store(delay.penalty, std::memory_order_relaxed);
		if (DownloadBoostLevel() != 6) {
			return;
		}
		const auto state = Storage::NonPremiumDelayState{
			.limitedUntil = limitedUntil,
			.recoveryUntil = recoveryUntil,
			.penalty = delay.penalty,
		};
		const auto dispatchLimit = std::min(
			_loader->smartStreamingRequestLimit(),
			Storage::NonPremiumRequestLimit(
				state,
				now,
				BoostProfileFor(6).smartInitialRequestLimit,
				BoostProfileFor(6).smartMinimumRequestLimit,
				BoostProfileFor(6).smartMaximumRequestLimit));
		_serverLimitPhase.store(
			(limitedUntil > now) ? 1 : 2,
			std::memory_order_relaxed);
		_serverLimitRequests.store(
			dispatchLimit,
			std::memory_order_relaxed);
		_pendingTailPrefetchBytes.store(0, std::memory_order_release);
		_speedState = SpeedState::Normal;
		_burstSpeedEma = 0.0;
		_burstSpeedInitialized = false;
		_adaptivePreloadPercent.store(100, std::memory_order_relaxed);
		_adaptiveLimitPercent.store(100, std::memory_order_relaxed);
		_speedIsThrottled.store(false, std::memory_order_relaxed);
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: server limited "
			"dc=%1 waitMs=%2 limitedFor=%3 recoveryFor=%4 penalty=%5 "
			"preloadBase=%6 queueLimit=%7 dispatchLimit=%8 bufferMs=%9.")
			.arg(delay.dcId)
			.arg(delay.waitMs)
			.arg(qlonglong(std::max(limitedUntil - now, crl::time(0))))
			.arg(qlonglong(recoveryUntil - std::max(limitedUntil, now)))
			.arg(delay.penalty)
			.arg(PreloadPartsAhead())
			.arg((limitedUntil > now)
				? BoostProfileFor(DownloadBoostLevel()).smartMinimumRequests
				: dispatchLimit)
			.arg(dispatchLimit)
			.arg(qlonglong(smartStreamingRecoveryBuffer())));
	};
	applyServerDelay(_loader->serverDelayState());
	_loader->serverDelays(
	) | rpl::on_next(applyServerDelay, _lifetime);

	// Adaptive scheduling driven by measured download speed. Three states:
	//   - Burst: sustained >= 1.5 MB/s. Grow preload depth. Premium and fixed
	//     levels may also grow request depth, while non-Premium Smart keeps
	//     requests within its DC-wide cap.
	//   - Normal: between thresholds. Baseline behaviour.
	//   - Throttle: sustained <= 200 KB/s. Smart mode keeps preload depth while
	//     the non-Premium DC controller owns request depth. Fixed levels stay
	//     at their configured baseline. The state also keeps far-seek
	//     cancellation from thrashing requests.
	// Only active when net_download_speed_boost > 0; state stays at Normal
	// for boost level 0 and atomics stay at 100/100.
	_loader->speedEstimate(
	) | rpl::on_next([=](SpeedEstimate estimate) {
		if (DownloadBoostLevel() == 0
			|| estimate.unreliable
			|| estimate.bytesPerSecond <= 0) {
			return;
		}
		const auto seekGrace = smartStreamingEnabled()
			&& (crl::now() < _smartSeekRecoveryUntil.load(
				std::memory_order_relaxed));
		if (smartStreamingEnabled() && !seekGrace) {
			const auto throughput = UpdateIntegerEma(
				_streamThroughputBytesPerSecond.load(
					std::memory_order_relaxed),
				estimate.bytesPerSecond);
			const auto latency = UpdateIntegerEma(
				_streamLatencyMs.load(std::memory_order_relaxed),
				estimate.latencyMs);
			const auto jitter = UpdateIntegerEma(
				_streamJitterMs.load(std::memory_order_relaxed),
				estimate.jitterMs);
			_streamThroughputBytesPerSecond.store(
				throughput,
				std::memory_order_relaxed);
			_streamLatencyMs.store(latency, std::memory_order_relaxed);
			_streamJitterMs.store(jitter, std::memory_order_relaxed);
			const auto target = crl::time(SmartAdaptiveBufferMs(
				_loader->smartStreamingPlaybackRate(),
				throughput,
				latency,
				jitter));
			const auto logged = _smartBufferTargetLoggedMs.load(
				std::memory_order_relaxed);
			const auto change = (target >= logged)
				? (target - logged)
				: (logged - target);
			if (!logged || change >= crl::time(1000)) {
				_smartBufferTargetLoggedMs.store(
					target,
					std::memory_order_relaxed);
				VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Smart stream "
					"metrics throughput=%1 latency=%2 jitter=%3 "
					"playback=%4 bufferMs=%5.")
					.arg(throughput)
					.arg(latency)
					.arg(jitter)
					.arg(_loader->smartStreamingPlaybackRate())
					.arg(qlonglong(target)));
			}
		}
		if (DownloadBoostLevel() == 6
			&& !_premiumSession
			&& (crl::now() < _serverRecoveryUntil.load(
					std::memory_order_relaxed)
				|| seekGrace)) {
			return;
		}
		constexpr auto kAlpha = 0.4;
		if (!_burstSpeedInitialized) {
			_burstSpeedEma = double(estimate.bytesPerSecond);
			_burstSpeedInitialized = true;
		} else {
			_burstSpeedEma = (_burstSpeedEma * (1.0 - kAlpha))
				+ (double(estimate.bytesPerSecond) * kAlpha);
		}

		constexpr auto kBurstEnter = 1'500'000.0;
		constexpr auto kBurstLeave = 1'000'000.0;
		constexpr auto kThrottleEnter = 200'000.0;
		constexpr auto kThrottleLeave = 500'000.0;

		const auto apply = [&](SpeedState next) {
			if (_speedState == next) {
				return;
			}
			_speedState = next;
			const auto smart = (DownloadBoostLevel() == 6);
			const auto conservative = smart && !_premiumSession;
			const auto burstPreloadPercent = conservative
				? 125
				: (smart ? 150 : 200);
			const auto burstLimitPercent = conservative
				? 100
				: (smart ? 125 : 150);
			switch (next) {
			case SpeedState::Burst:
				_adaptivePreloadPercent.store(
					burstPreloadPercent,
					std::memory_order_relaxed);
				_adaptiveLimitPercent.store(
					burstLimitPercent,
					std::memory_order_relaxed);
				_speedIsThrottled.store(
					false,
					std::memory_order_relaxed);
				break;
			case SpeedState::Normal:
				_adaptivePreloadPercent.store(
					100,
					std::memory_order_relaxed);
				_adaptiveLimitPercent.store(
					100,
					std::memory_order_relaxed);
				_speedIsThrottled.store(
					false,
					std::memory_order_relaxed);
				break;
			case SpeedState::Throttle:
				_adaptivePreloadPercent.store(
					100,
					std::memory_order_relaxed);
				_adaptiveLimitPercent.store(
					conservative ? 100 : (smart ? 50 : 100),
					std::memory_order_relaxed);
				_speedIsThrottled.store(
					true,
					std::memory_order_relaxed);
				break;
			}
			if (next != SpeedState::Throttle) {
				_throttleConfirmCount = 0;
			}
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: adaptive state=%1 boost=%2 speed=%3 ema=%4 preloadPercent=%5 limitPercent=%6 throttled=%7.")
				.arg(int(next))
				.arg(DownloadBoostLevel())
				.arg(estimate.bytesPerSecond)
				.arg(_burstSpeedEma, 0, 'f', 0)
				.arg(_adaptivePreloadPercent.load(std::memory_order_relaxed))
				.arg(_adaptiveLimitPercent.load(std::memory_order_relaxed))
				.arg(_speedIsThrottled.load(std::memory_order_relaxed) ? 1 : 0));
		};

		// Non-Premium Smart: DC concurrency + bitrate floor own catch-up.
		// Throttle from seek-cancel dips only creates false low-speed state.
		const auto smartNonPremiumAdaptive = smartStreamingEnabled();
		switch (_speedState) {
		case SpeedState::Burst:
			if (_burstSpeedEma <= kBurstLeave) {
				apply(SpeedState::Normal);
			}
			break;
		case SpeedState::Throttle:
			if (smartNonPremiumAdaptive
				|| _burstSpeedEma >= kThrottleLeave) {
				apply(SpeedState::Normal);
			}
			break;
		case SpeedState::Normal:
			if (_burstSpeedEma >= kBurstEnter) {
				apply(SpeedState::Burst);
			} else if (!smartNonPremiumAdaptive
				&& _burstSpeedEma <= kThrottleEnter) {
				++_throttleConfirmCount;
				if (_throttleConfirmCount >= kSmartThrottleConfirmSamples) {
					apply(SpeedState::Throttle);
				}
			} else {
				_throttleConfirmCount = 0;
			}
			break;
		}
	}, _lifetime);

	if (_cacheHelper) {
		readFromCache(0);
	}
}

void Reader::startSleep(not_null<crl::semaphore*> wake) {
	_sleeping.store(wake, std::memory_order_release);
	processDownloaderRequests();
}

void Reader::wakeFromSleep() {
	if (const auto sleeping = _sleeping.load(std::memory_order_acquire)) {
		_sleeping.store(nullptr, std::memory_order_release);
		sleeping->release();
	}
}

void Reader::stopSleep() {
	_sleeping.store(nullptr, std::memory_order_release);
}

void Reader::stopStreamingAsync() {
	_stopStreamingAsync = true;
	setSmartStreamingBufferPressure(false);
	_loader->setSmartStreamingPlaybackRate(0);
	crl::on_main(this, [=] {
		if (_stopStreamingAsync) {
			stopStreaming(false);
		}
	});
}

void Reader::tryRemoveLoaderAsync() {
	_loader->tryRemoveFromQueue();
}

void Reader::requestTailPrefetch(int64 bytes) {
	if (bytes <= 0) {
		return;
	}
	if (DownloadBoostLevel() == 6
		&& !_premiumSession
		&& crl::now() < _serverRecoveryUntil.load(
			std::memory_order_relaxed)) {
		return;
	}
	// Consumed on the streaming thread in consumePendingTailPrefetch().
	_pendingTailPrefetchBytes.store(bytes, std::memory_order_release);
}

void Reader::syncSmartStreamingBufferPressure(crl::time now) {
	while (true) {
		const auto pressure = _smartBufferPressure.load(
			std::memory_order_acquire);
		const auto seekRecoveryUntil = _smartSeekRecoveryUntil.load(
			std::memory_order_acquire);
		const auto smart = smartStreamingEnabled();
		const auto playbackReady = !smart
			|| (_loader->smartStreamingPlaybackRate() > 0);
		const auto pressureLocalUntil = _smartSeekPressureLocalUntil.load(
			std::memory_order_acquire);
		const auto forwarded = pressure
			&& playbackReady
			&& (!smart || now >= pressureLocalUntil);
		_loader->setSmartStreamingBufferPressure(forwarded);

		const auto currentNow = crl::now();
		const auto currentPressure = _smartBufferPressure.load(
			std::memory_order_acquire);
		const auto currentPressureLocalUntil
			= _smartSeekPressureLocalUntil.load(std::memory_order_acquire);
		const auto currentSmart = smartStreamingEnabled();
		const auto currentPlaybackReady = !currentSmart
			|| (_loader->smartStreamingPlaybackRate() > 0);
		const auto currentForwarded = currentPressure
			&& currentPlaybackReady
			&& (!currentSmart || currentNow >= currentPressureLocalUntil);
		if (currentForwarded == forwarded) {
			return;
		}
		now = currentNow;
	}
}

void Reader::setSmartStreamingBufferPressure(bool pressure) {
	if (pressure
		&& !_streamingActive.load(std::memory_order_acquire)) {
		return;
	}
	const auto now = crl::now();
	if (!pressure && smartStreamingEnabled()) {
		const auto current = _smartBufferPressure.load(
			std::memory_order_acquire);
		if (current) {
			const auto since = _smartPressureStickySince.load(
				std::memory_order_acquire);
			if (since
				&& now < since + kSmartPressureMinHoldDuration) {
				// Hysteresis: ignore premature clear while still catching up.
				return;
			}
		}
	}
	if (pressure) {
		const auto was = _smartBufferPressure.load(std::memory_order_acquire);
		if (!was) {
			_smartPressureStickySince.store(now, std::memory_order_release);
		}
	} else {
		_smartPressureStickySince.store(0, std::memory_order_release);
	}
	const auto previous = _smartBufferPressure.exchange(
		pressure,
		std::memory_order_acq_rel);
	const auto recoveryBuffer = smartStreamingRecoveryBuffer();
	if (pressure || previous) {
		_smartPreloadRecoveryUntil.store(
			now + kSmartPreloadRecoveryDuration,
			std::memory_order_release);
	}
	const auto pressureLocalUntil = _smartSeekPressureLocalUntil.load(
		std::memory_order_acquire);
	const auto localRecovery = pressure
		&& smartStreamingEnabled()
		&& (now < pressureLocalUntil);
	syncSmartStreamingBufferPressure(now);
	if (previous != pressure) {
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Smart pressure "
			"pressure=%1 localOnly=%2 playback=%3 waitMs=%4 bufferMs=%5 "
			"seekRemaining=%6.")
			.arg(pressure ? 1 : 0)
			.arg(localRecovery ? 1 : 0)
			.arg(_loader->smartStreamingPlaybackRate())
			.arg(_serverObservedWaitMs.load(std::memory_order_relaxed))
			.arg(qlonglong(recoveryBuffer))
			.arg(qlonglong(std::max(
				pressureLocalUntil - now,
				crl::time(0)))));
	}
}

void Reader::setSmartStreamingPlaybackRate(int bytesPerSecond) {
	_loader->setSmartStreamingPlaybackRate(bytesPerSecond);
	syncSmartStreamingBufferPressure(crl::now());
}

void Reader::notifySmartStreamingSeek() {
	if (!_streamingActive.load(std::memory_order_acquire)) {
		return;
	}
	const auto now = crl::now();
	if (smartStreamingEnabled()) {
		const auto recoveryUntil = now + kSmartSeekLocalRecoveryDuration;
		const auto pressureLocalUntil = now
			+ kSmartSeekPressureLocalDuration;
		SmartClearDualKeep(_dualKeep);
		_smartSeekRecoveryUntil.store(
			recoveryUntil,
			std::memory_order_release);
		_smartSeekPressureLocalUntil.store(
			pressureLocalUntil,
			std::memory_order_release);
		// Drop Throttle state caused by seek-cancel throughput dips.
		_speedState = SpeedState::Normal;
		_burstSpeedEma = 0.0;
		_burstSpeedInitialized = false;
		_throttleConfirmCount = 0;
		_adaptivePreloadPercent.store(100, std::memory_order_relaxed);
		_adaptiveLimitPercent.store(100, std::memory_order_relaxed);
		_speedIsThrottled.store(false, std::memory_order_relaxed);
		syncSmartStreamingBufferPressure(now);
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Smart seek recovery "
			"localMs=%1 pressureLocalMs=%2 playback=%3 bufferMs=%4.")
			.arg(qlonglong(recoveryUntil - now))
			.arg(qlonglong(pressureLocalUntil - now))
			.arg(_loader->smartStreamingPlaybackRate())
			.arg(qlonglong(smartStreamingRecoveryBuffer())));
	}
	_loader->notifySmartStreamingSeek();
}

void Reader::startStreaming() {
	_seekCancelGeneration.fetch_add(1, std::memory_order_release);
	_streamingActive.store(true, std::memory_order_release);
	refreshLoaderPriority();
}

void Reader::continueStreamingForSoftSeek() {
	Expects(_sleeping == nullptr);

	_waiting.store(nullptr, std::memory_order_release);
	if (_cacheHelper && _cacheHelper->waiting != nullptr) {
		QMutexLocker lock(&_cacheHelper->mutex);
		_cacheHelper->waiting.store(nullptr, std::memory_order_release);
	}
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Reader soft seek continue "
		"streaming keep-loads active=%1.")
		.arg(_streamingActive.load(std::memory_order_acquire) ? 1 : 0));
}

void Reader::primeSeekPrefetch(
		int64 offset,
		int64 amount,
		int64 urgentOffset) {
	if (!_streamingActive.load(std::memory_order_acquire)
		|| !smartStreamingEnabled()) {
		return;
	}
	prefetch(offset, amount, urgentOffset);
	consumePendingSeekPrefetch();
	const auto urgentStart = _seekPrefetchUrgentWindowStart;
	const auto urgentTill = _seekPrefetchUrgentWindowTill;
	if (urgentStart < 0 || urgentTill <= urgentStart) {
		return;
	}
	const auto &profile = BoostProfileFor(DownloadBoostLevel());
	const auto requestLimit = std::clamp(
		_loader->smartStreamingRequestLimit(),
		profile.smartMinimumRequestLimit,
		profile.smartMaximumRequestLimit);
	auto requested = 0;
	for (auto part = urgentStart;
		part < urgentTill && requested < requestLimit;
		part += kPartSize) {
		if (_slices.hasPart(uint32(part))) {
			continue;
		}
		loadAtOffset(uint32(part));
		++requested;
	}
	if (requested > 0) {
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Reader soft seek prime "
			"urgentStart=%1 urgentTill=%2 requested=%3 limit=%4.")
			.arg(qlonglong(urgentStart))
			.arg(qlonglong(urgentTill))
			.arg(requested)
			.arg(requestLimit));
	}
}

void Reader::stopStreaming(bool stillActive) {
	Expects(_sleeping == nullptr);

	_stopStreamingAsync = false;
	_waiting.store(nullptr, std::memory_order_release);
	if (_cacheHelper && _cacheHelper->waiting != nullptr) {
		QMutexLocker lock(&_cacheHelper->mutex);
		_cacheHelper->waiting.store(nullptr, std::memory_order_release);
	}
	if (stillActive) {
		if (StreamingSeekCancelEnabled()) {
			cancelStreamingLoads(smartStreamingEnabled());
		}
		_seekPrefetchWindowStart = -1;
		_seekPrefetchWindowTill = -1;
		_seekPrefetchUrgentWindowStart = -1;
		_seekPrefetchUrgentWindowTill = -1;
		_seekPrefetchBackgroundActive = false;
		_seekPrefetchConsumedGeneration = _seekPrefetchGeneration.load(
			std::memory_order_acquire);
	}
	if (!stillActive) {
		_streamingActive.store(false, std::memory_order_release);
		setSmartStreamingBufferPressure(false);
		_loader->setSmartStreamingPlaybackRate(0);
		_seekCancelLogQueued = 0;
		_seekCancelLogSentCompleted = 0;
		_seekCancelLogLastTime = 0;
		_smartCatchupLogged = false;
		_smartUnderPlaybackSkipLogged = false;
		_smartCatchupLogLastTime = 0;
		_smartUnderPlaybackSkipLogLastTime = 0;
		_smartForceCancelLogLastTime = 0;
		_smartCatchupLogPreload = -1;
		_smartCatchupLogRequests = -1;
		SmartClearDualKeep(_dualKeep);
		_smartPreloadRecoveryUntil.store(0, std::memory_order_release);
		_smartSeekRecoveryUntil.store(0, std::memory_order_release);
		_smartSeekPressureLocalUntil.store(0, std::memory_order_release);
		_smartPressureStickySince.store(0, std::memory_order_release);
		_smartPreloadRecoveryLoggedPercent.store(
			0,
			std::memory_order_relaxed);
		_seekPrefetchOffset.store(-1, std::memory_order_relaxed);
		_seekPrefetchBytes.store(0, std::memory_order_release);
		_seekPrefetchUrgentOffset.store(-1, std::memory_order_relaxed);
		_seekPrefetchUrgentBytes.store(0, std::memory_order_release);
		_seekPrefetchGeneration.store(0, std::memory_order_release);
		_seekPrefetchConsumedGeneration = 0;
		_seekPrefetchWindowStart = -1;
		_seekPrefetchWindowTill = -1;
		_seekPrefetchUrgentWindowStart = -1;
		_seekPrefetchUrgentWindowTill = -1;
		_seekPrefetchBackgroundActive = false;
		_streamThroughputBytesPerSecond.store(0, std::memory_order_relaxed);
		_streamLatencyMs.store(0, std::memory_order_relaxed);
		_streamJitterMs.store(0, std::memory_order_relaxed);
		_smartBufferTargetLoggedMs.store(0, std::memory_order_relaxed);
		_seekCancelGeneration.fetch_add(1, std::memory_order_release);
		refreshLoaderPriority();
		cancelStreamingLoads();
		_seekCancellationOffsets.clear();
		processDownloaderRequests();
	}
}

rpl::producer<LoadedPart> Reader::partsForDownloader() const {
	return _partsForDownloader.events();
}

void Reader::loadForDownloader(
		not_null<Storage::StreamedFileDownloader*> downloader,
		int64 offset) {
	Expects(offset >= 0 && offset <= std::numeric_limits<uint32>::max());

	if (_attachedDownloader != downloader) {
		if (_attachedDownloader) {
			cancelForDownloader(_attachedDownloader);
		}
		_attachedDownloader = downloader;
		_loader->attachDownloader(downloader);
	}
	_downloaderOffsetRequests.emplace(uint32(offset));
	// Will be processed in continueDownloaderFromMainThread()
	// from StreamedFileDownloader::requestParts().
}

void Reader::doneForDownloader(int64 offset) {
	Expects(offset >= 0 && offset <= std::numeric_limits<uint32>::max());

	_downloaderOffsetAcks.emplace(offset);
	// Will be processed in continueDownloaderFromMainThread()
	// from StreamedFileDownloader::requestParts().
}

void Reader::cancelForDownloader(
		not_null<Storage::StreamedFileDownloader*> downloader) {
	if (_attachedDownloader == downloader) {
		_downloaderOffsetRequests.take();
		_attachedDownloader = nullptr;
		_loader->clearAttachedDownloader();
	}
}

void Reader::enqueueDownloaderOffsets() {
	auto offsets = _downloaderOffsetRequests.take();
	if (!empty(offsets)) {
		if (!empty(_offsetsForDownloader)) {
			_offsetsForDownloader.insert(
				end(_offsetsForDownloader),
				std::make_move_iterator(begin(offsets)),
				std::make_move_iterator(end(offsets)));
			checkForDownloaderChange(offsets.size() + 1);
		} else {
			_offsetsForDownloader = std::move(offsets);
			checkForDownloaderChange(offsets.size());
		}
	}
}

void Reader::checkForDownloaderChange(int checkItemsCount) {
	Expects(checkItemsCount <= _offsetsForDownloader.size());

	// If a requested offset is less-or-equal of some previously requested
	// offset, it means that the downloader was changed, ignore old offsets.
	const auto end = _offsetsForDownloader.end();
	const auto changed = std::adjacent_find(
		end - checkItemsCount,
		end,
		[](uint32 first, uint32 second) { return (second <= first); });
	if (changed != end) {
		_offsetsForDownloader.erase(
			begin(_offsetsForDownloader),
			changed + 1);
		_downloaderReadCache.clear();
		_downloaderOffsetAcks.take();
	}
}

void Reader::checkForDownloaderReadyOffsets() {
	// If a requested part is available right now we simply fire it on the
	// main thread, until the first not-available-right-now offset is found.
	const auto unavailableInBytes = [&](uint32 offset, QByteArray &&bytes) {
		if (bytes.isEmpty()) {
			return true;
		}
		crl::on_main(this, [=, bytes = std::move(bytes)]() mutable {
			_partsForDownloader.fire({ int64(offset), std::move(bytes) });
		});
		return false;
	};
	const auto unavailableInCache = [&](uint32 offset) {
		const auto index = (offset / kInSlice);
		const auto sliceNumber = index + 1;
		const auto i = _downloaderReadCache.find(sliceNumber);
		if (i == end(_downloaderReadCache) || !i->second) {
			return true;
		}
		const auto j = i->second->find(offset - index * kInSlice);
		if (j == end(*i->second)) {
			return true;
		}
		return unavailableInBytes(offset, std::move(j->second));
	};
	const auto unavailable = [&](uint32 offset) {
		return unavailableInBytes(offset, _slices.partForDownloader(offset))
			&& unavailableInCache(offset);
	};
	_offsetsForDownloader.erase(
		begin(_offsetsForDownloader),
		ranges::find_if(_offsetsForDownloader, unavailable));
}

void Reader::processDownloaderRequests() {
	processCacheResults();
	enqueueDownloaderOffsets();
	checkForDownloaderReadyOffsets();
	pruneDoneDownloaderRequests();
	if (!empty(_offsetsForDownloader)) {
		pruneDownloaderCache(_offsetsForDownloader.front());
		sendDownloaderRequests();
	}
}

void Reader::pruneDownloaderCache(uint32 minimalOffset) {
	const auto minimalSliceNumber = (minimalOffset / kInSlice) + 1;
	const auto removeTill = ranges::lower_bound(
		_downloaderReadCache,
		minimalSliceNumber,
		ranges::less(),
		&base::flat_map<uint32, std::optional<PartsMap>>::value_type::first);
	_downloaderReadCache.erase(_downloaderReadCache.begin(), removeTill);
}

void Reader::pruneDoneDownloaderRequests() {
	for (const auto done : _downloaderOffsetAcks.take()) {
		_downloaderOffsetsRequested.remove(done);
		const auto i = ranges::find(_offsetsForDownloader, done);
		if (i != end(_offsetsForDownloader)) {
			_offsetsForDownloader.erase(i);
		}
	}
}

void Reader::sendDownloaderRequests() {
	auto &&offsets = ranges::views::all(
		_offsetsForDownloader
	) | ranges::views::take(StreamingRequestsLimit());
	for (const auto offset : offsets) {
		if ((!_cacheHelper || !downloaderWaitForCachedSlice(offset))
			&& _downloaderOffsetsRequested.emplace(offset).second) {
			_loader->load(offset);
		}
	}
}

bool Reader::downloaderWaitForCachedSlice(uint32 offset) {
	if (_slices.waitingForHeaderCache()) {
		return true;
	}
	if (!_slices.readCacheForDownloaderRequired(offset)) {
		return false;
	}
	const auto sliceNumber = (offset / kInSlice) + 1;
	auto i = _downloaderReadCache.find(sliceNumber);
	if (i == _downloaderReadCache.end()) {
		// If we didn't request that slice yet, try requesting it.
		// If there is no need to (header mode is unknown) - place empty map.
		// Otherwise place std::nullopt and wait for the cache result.
		i = _downloaderReadCache.emplace(
			sliceNumber,
			(readFromCacheForDownloader(sliceNumber)
				? std::nullopt
				: std::make_optional(PartsMap()))).first;
	}
	return !i->second;
}

void Reader::checkCacheResultsForDownloader() {
	continueDownloaderFromMainThread();
}

void Reader::continueDownloaderFromMainThread() {
	if (_streamingActive.load(std::memory_order_acquire)) {
		wakeFromSleep();
	} else {
		processDownloaderRequests();
	}
}

rpl::producer<SpeedEstimate> Reader::speedEstimate() const {
	return _loader->speedEstimate();
}

void Reader::setLoaderPriority(int priority) {
	if (_realPriority == priority) {
		return;
	}
	_realPriority = priority;
	refreshLoaderPriority();
}

void Reader::refreshLoaderPriority() {
	_loader->setPriority(
		_streamingActive.load(std::memory_order_acquire)
			? _realPriority
			: 0);
}

bool Reader::isRemoteLoader() const {
	return _loader->baseCacheKey().valid();
}

bool Reader::smartStreamingEnabled() const {
	return isRemoteLoader()
		&& (DownloadBoostLevel() == 6)
		&& !_premiumSession;
}

crl::time Reader::smartStreamingBackgroundBuffer() const {
	const auto playback = _loader->smartStreamingPlaybackRate();
	if (!smartStreamingEnabled() || playback <= 0) {
		return 0;
	}
	const auto adaptiveBuffer = crl::time(SmartAdaptiveBufferMs(
		playback,
		_streamThroughputBytesPerSecond.load(std::memory_order_relaxed),
		_streamLatencyMs.load(std::memory_order_relaxed),
		_streamJitterMs.load(std::memory_order_relaxed)));
	if (crl::now() >= _serverRecoveryUntil.load(
			std::memory_order_relaxed)) {
		return adaptiveBuffer;
	}
	const auto penalty = std::clamp(
		_serverPenalty.load(std::memory_order_relaxed),
		1,
		3);
	const auto minimum = kSmartServerRecoveryMinimumDuration
		+ (penalty - 1) * kSmartServerRecoveryPenaltyStep;
	const auto observed = crl::time(
		_serverObservedWaitMs.load(std::memory_order_relaxed));
	const auto serverBuffer = std::clamp(
		observed + kSmartServerRecoveryWaitMargin,
		minimum,
		kSmartServerRecoveryMaximumDuration);
	return std::max(adaptiveBuffer, serverBuffer);
}

crl::time Reader::smartStreamingRecoveryBuffer() const {
	const auto background = smartStreamingBackgroundBuffer();
	if (background <= 0
		|| crl::now() >= _smartSeekRecoveryUntil.load(
			std::memory_order_relaxed)) {
		return background;
	}
	return crl::time(SmartSeekBootstrapWaitMs(
		_loader->smartStreamingPlaybackRate(),
		background));
}

std::shared_ptr<Reader::CacheHelper> Reader::InitCacheHelper(
		Storage::Cache::Key baseKey) {
	if (!baseKey) {
		return nullptr;
	}
	return std::make_shared<Reader::CacheHelper>(baseKey);
}

// 0 is for headerData, slice index = sliceNumber - 1.
void Reader::readFromCache(int sliceNumber) {
	Expects(_cache != nullptr);
	Expects(_cacheHelper != nullptr);
	Expects(!sliceNumber || !_slices.headerModeUnknown());

	if (sliceNumber == 1 && _slices.isGoodHeader()) {
		return readFromCache(0);
	}
	const auto size = _loader->size();
	const auto key = _cacheHelper->key(sliceNumber);
	const auto cache = std::weak_ptr<CacheHelper>(_cacheHelper);
	const auto weak = base::make_weak(this);
	const auto ready = [=](
			QByteArray &&result,
			std::vector<int> &&sizes = {}) {
		crl::async([
			=,
			result = std::move(result),
			sizes = std::move(sizes)
		]() mutable{
			auto entry = ParseCacheEntry(
				bytes::make_span(result),
				sliceNumber,
				size);
			if (const auto strong = cache.lock()) {
				QMutexLocker lock(&strong->mutex);
				strong->results.emplace(sliceNumber, std::move(entry.parts));
				if (!sliceNumber && entry.included) {
					strong->results.emplace(1, std::move(*entry.included));
				}
				strong->sizes = std::move(sizes);
				if (const auto waiting = strong->waiting.load()) {
					strong->waiting.store(nullptr, std::memory_order_release);
					waiting->release();
				} else {
					crl::on_main(weak, [=] {
						checkCacheResultsForDownloader();
					});
				}
			}
		});
	};
	auto keys = std::vector<Storage::Cache::Key>();
	const auto count = _slices.requestSliceSizesCount();
	for (auto i = 0; i != count; ++i) {
		keys.push_back(_cacheHelper->key(i + 1));
	}
	_cache->getWithSizes(key, std::move(keys), ready);
}

bool Reader::readFromCacheForDownloader(int sliceNumber) {
	Expects(_cacheHelper != nullptr);
	Expects(sliceNumber > 0);

	if (_slices.headerModeUnknown()) {
		return false;
	}
	readFromCache(sliceNumber);
	return true;
}

void Reader::putToCache(SerializedSlice &&slice) {
	Expects(_cache != nullptr);
	Expects(_cacheHelper != nullptr);
	Expects(slice.number >= 0);

	_cache->put(_cacheHelper->key(slice.number), std::move(slice.data));
}

int64 Reader::size() const {
	return _loader->size();
}

std::optional<Error> Reader::streamingError() const {
	return _streamingError;
}

void Reader::headerDone() {
	_slices.headerDone(false);
}

int Reader::headerSize() const {
	return _slices.headerSize();
}

bool Reader::fullInCache() const {
	return _slices.fullInCache();
}

Reader::FillState Reader::fill(
		int64 offset,
		bytes::span buffer,
		not_null<crl::semaphore*> notify) {
	Expects(offset + buffer.size() <= size());
	Expects(offset >= 0 && size() <= std::numeric_limits<uint32>::max());

	const auto seekCancelEnabled = StreamingSeekCancelEnabled();
	if (seekCancelEnabled) {
		const auto generation = _seekCancelGeneration.load(
			std::memory_order_acquire);
		if (_seekCancelObservedGeneration != generation
			|| !_seekCancelEnabledLastFill) {
			_seekCancelObservedGeneration = generation;
			_seekCancelLastOffset = -1;
		}
		_seekCancelEnabledLastFill = true;
		const auto previous = _seekCancelLastOffset;
		_seekCancelLastOffset = offset;
		if (smartStreamingEnabled()) {
			noteDualKeepRead(offset, int64(buffer.size()));
			if (previous >= 0) {
				noteDualKeepRead(previous, int64(buffer.size()));
			}
		}
		if (previous >= 0 && !_loadingOffsets.empty()) {
			const auto delta = (offset >= previous)
				? (offset - previous)
				: (previous - offset);
			if (delta >= StreamingSeekCancelJumpBytes()) {
				const auto guard = StreamingSeekCancelGuardBytes();
				const auto fileSize = size();
				const auto start = std::max<int64>(0, offset - guard);
				const auto till = std::min<int64>(
					fileSize,
					offset + buffer.size() + guard);
				if (start < till) {
					const auto seekRecovery = smartStreamingEnabled()
						&& (crl::now() < _smartSeekRecoveryUntil.load(
							std::memory_order_acquire));
					// During seek recovery, dual A/V jumps must not cancel
					// each other — dual-keep already tracks both envelopes.
					// Only install-window force cancel clears the old position.
					if (seekRecovery) {
						VIDEO_PLAYBACK_VERBOSE_LOG(("Video Playback: Reader seek jump cancel skipped dual-keep recovery previous=%1 offset=%2 delta=%3 boost=%4.")
							.arg(qlonglong(previous))
							.arg(qlonglong(offset))
							.arg(qlonglong(delta))
							.arg(DownloadBoostLevel()));
					} else {
						VIDEO_PLAYBACK_VERBOSE_LOG(("Video Playback: Reader seek jump cancel previous=%1 offset=%2 delta=%3 windowStart=%4 windowTill=%5 loadingActive=%6 boost=%7.")
							.arg(qlonglong(previous))
							.arg(qlonglong(offset))
							.arg(qlonglong(delta))
							.arg(qlonglong(start))
							.arg(qlonglong(till))
							.arg(_loadingOffsets.empty() ? 0 : 1)
							.arg(DownloadBoostLevel()));
						cancelLoadOutsideWindow(
							uint32(start),
							uint32(till),
							false);
					}
				}
			}
		}
	} else if (_seekCancelEnabledLastFill) {
		_seekCancelEnabledLastFill = false;
		_seekCancelLastOffset = -1;
	}

	// Sample forward-consumption rate (bytes of video consumed per second of
	// wall time). FFmpeg/AVIO drive fill() in bursts, so we accumulate over
	// at least 500ms to get a stable reading. A backward seek or long idle
	// just re-anchors without touching the EMA.
	{
		const auto now = crl::now();
		if (_consumptionLastOffset < 0) {
			_consumptionLastOffset = offset;
			_consumptionLastTime = now;
		} else {
			const auto deltaMs = now - _consumptionLastTime;
			if (deltaMs >= 500) {
				const auto deltaBytes = offset - _consumptionLastOffset;
				constexpr auto kMaxSequentialJump = int64(4 * 1024 * 1024);
				if (deltaBytes > 0
					&& deltaBytes <= kMaxSequentialJump
					&& deltaMs <= 5000) {
					const auto instantBps = deltaBytes * 1000.0
						/ double(deltaMs);
					constexpr auto kAlpha = 0.3;
					_consumptionBytesPerSec = _consumptionBytesPerSec
						* (1.0 - kAlpha)
						+ instantBps * kAlpha;
				}
				_consumptionLastOffset = offset;
				_consumptionLastTime = now;
			}
		}
	}

	const auto startWaiting = [&] {
		if (_cacheHelper) {
			_cacheHelper->waiting = notify.get();
		}
		_waiting.store(notify.get(), std::memory_order_release);
	};
	const auto clearWaiting = [&] {
		_waiting.store(nullptr, std::memory_order_release);
		if (_cacheHelper) {
			_cacheHelper->waiting.store(nullptr, std::memory_order_release);
		}
	};
	const auto done = [&] {
		clearWaiting();
		return FillState::Success;
	};
	const auto failed = [&] {
		clearWaiting();
		notify->release();
		return FillState::Failed;
	};

	checkForSomethingMoreReceived();
	if (_streamingError) {
		return FillState::Failed;
	}

	auto lastResult = FillState();
	do {
		lastResult = fillFromSlices(uint32(offset), buffer);
		if (lastResult == FillState::Success) {
			return done();
		}
		startWaiting();
	} while (checkForSomethingMoreReceived());

	return _streamingError ? failed() : lastResult;
}

void Reader::prefetch(
		int64 offset,
		int64 amount,
		int64 urgentOffset) {
	if (!_streamingActive.load(std::memory_order_acquire)
		|| !smartStreamingEnabled()
		|| offset < 0
		|| amount <= 0
		|| offset >= size()
		|| urgentOffset < 0
		|| urgentOffset >= size()) {
		return;
	}
	const auto playback = _loader->smartStreamingPlaybackRate();
	const auto highBitrate = IsHighBitratePlaybackRate(playback);
	auto urgentAmount = kSmartSeekUrgentMinimum;
	if (playback > 0) {
		const auto recoveryBuffer = smartStreamingBackgroundBuffer();
		const auto prefetchDuration = highBitrate
			? kSmartSeekPrefetchHighBitrateDuration
			: kSmartSeekPrefetchTargetDuration;
		const auto prefetchMaximum = highBitrate
			? kSmartSeekPrefetchHighBitrateMaximum
			: kSmartSeekPrefetchMaximum;
		const auto urgentDuration = highBitrate
			? kSmartSeekUrgentHighBitrateDuration
			: kSmartSeekUrgentTargetDuration;
		const auto urgentMaximum = highBitrate
			? kSmartSeekUrgentHighBitrateMaximum
			: kSmartSeekUrgentMaximum;
		const auto target = BytesForDuration(
			playback,
			std::max(
				recoveryBuffer,
				prefetchDuration));
		amount = std::clamp(
			std::max(amount, target),
			kSmartSeekPrefetchMinimum,
			prefetchMaximum);
		urgentAmount = std::clamp(
			BytesForDuration(playback, urgentDuration),
			kSmartSeekUrgentMinimum,
			urgentMaximum);
	}
	amount = std::min(amount, size() - offset);
	urgentAmount = std::min(urgentAmount, size() - urgentOffset);
	_seekPrefetchOffset.store(offset, std::memory_order_relaxed);
	_seekPrefetchBytes.store(amount, std::memory_order_relaxed);
	_seekPrefetchUrgentOffset.store(
		urgentOffset,
		std::memory_order_relaxed);
	_seekPrefetchUrgentBytes.store(
		urgentAmount,
		std::memory_order_relaxed);
	_seekPrefetchGeneration.fetch_add(1, std::memory_order_release);
}

Reader::FillState Reader::fillFromSlices(uint32 offset, bytes::span buffer) {
	using namespace rpl::mappers;

	consumePendingSeekPrefetch();
	const auto boost = DownloadBoostLevel();
	const auto &profile = BoostProfileFor(boost);
	const auto smartNonPremium = (boost == 6) && !_premiumSession;
	const auto preloadBase = PreloadPartsAhead();
	const auto limitBase = StreamingRequestsLimit();
	const auto preloadMinimum = (boost == 6)
		? profile.smartMinimumPreload
		: preloadBase;
	const auto limitMinimum = (boost == 6)
		? profile.smartMinimumRequests
		: limitBase;
	const auto now = crl::now();
	const auto smartPlaybackRate = smartNonPremium
		? _loader->smartStreamingPlaybackRate()
		: 0;
	const auto recoveryBuffer = smartStreamingBackgroundBuffer();
	const auto bufferPressure = _smartBufferPressure.load(
		std::memory_order_acquire);
	const auto seekLocalRecovery = smartNonPremium
		&& (now < _smartSeekRecoveryUntil.load(
			std::memory_order_acquire));
	if (smartNonPremium && bufferPressure && !seekLocalRecovery) {
		syncSmartStreamingBufferPressure(now);
	}
	const auto preloadRecoveryUntil = _smartPreloadRecoveryUntil.load(
		std::memory_order_acquire);
	const auto preloadRecoveryRemaining = std::max(
		preloadRecoveryUntil - now,
		crl::time(0));
	const auto streamThroughput = _streamThroughputBytesPerSecond.load(
		std::memory_order_relaxed);
	const auto underPlayback = smartNonPremium
		&& SmartIsUnderPlayback(smartPlaybackRate, streamThroughput);
	// Keep catch-up recovery armed while pressure or under-playback lasts.
	if (smartNonPremium && (bufferPressure || underPlayback)) {
		_smartPreloadRecoveryUntil.store(
			now + kSmartPreloadRecoveryDuration,
			std::memory_order_release);
	}
	const auto recoveryPreloadPercent = [&] {
		if (!smartNonPremium) {
			return 0;
		}
		if (bufferPressure
			|| underPlayback
			|| preloadRecoveryRemaining
				> kSmartPreloadRecoveryTaperDuration) {
			return kSmartPreloadRecoveryFullPercent;
		}
		return (preloadRecoveryRemaining > crl::time(0))
			? kSmartPreloadRecoveryTaperPercent
			: 0;
	}();
	const auto limitedUntil = _serverLimitedUntil.load(
		std::memory_order_relaxed);
	const auto recoveryUntil = _serverRecoveryUntil.load(
		std::memory_order_relaxed);
	const auto serverLimited = smartNonPremium
		&& (now < limitedUntil);
	const auto serverRecovering = !serverLimited
		&& smartNonPremium
		&& (now < recoveryUntil);
	const auto serverState = Storage::NonPremiumDelayState{
		.limitedUntil = limitedUntil,
		.recoveryUntil = recoveryUntil,
		.penalty = _serverPenalty.load(std::memory_order_relaxed),
	};
	const auto recoveryRequestLimit = serverRecovering
		? std::min(
			_loader->smartStreamingRequestLimit(),
			Storage::NonPremiumRequestLimit(
				serverState,
				now,
				profile.smartInitialRequestLimit,
				profile.smartMinimumRequestLimit,
				profile.smartMaximumRequestLimit))
		: 0;
	const auto smartRequestLimit = std::clamp(
		_loader->smartStreamingRequestLimit(),
		profile.smartMinimumRequestLimit,
		profile.smartMaximumRequestLimit);
	const auto seekPrefetchWindowStart = _seekPrefetchWindowStart;
	const auto seekPrefetchWindowTill = _seekPrefetchWindowTill;
	const auto seekPrefetchActive = smartNonPremium
		&& seekPrefetchWindowStart >= 0
		&& seekPrefetchWindowTill > seekPrefetchWindowStart
		&& int64(offset) >= seekPrefetchWindowStart
		&& int64(offset) < seekPrefetchWindowTill;
	const auto smartPreloadCap = IsHighBitratePlaybackRate(smartPlaybackRate)
		? kSmartMaxPreloadParts
		: kPartsInSlice;
	const auto smartTargetPreloadParts = smartNonPremium
		? SmartPreloadPartsForBufferMs(
			smartPlaybackRate,
			recoveryBuffer,
			kPartSize,
			profile.smartMinimumPreload,
			smartPreloadCap)
		: 0;
	const auto seekPrefetchTill = _seekPrefetchBackgroundActive
		? seekPrefetchWindowTill
		: std::max(
			_seekPrefetchUrgentWindowTill,
			int64(offset));
	const auto seekPrefetchParts = seekPrefetchActive
		? int(std::min<int64>(
			(seekPrefetchTill - int64(offset) + kPartSize - 1)
				/ kPartSize,
			smartPreloadCap))
		: 0;
	// Steady bitrate floor: keep adaptive/recovery depth from collapsing
	// under the parts needed for recoveryBuffer, even when recovery
	// percent has tapered to 0.
	const auto steadyBitrateFloor = (smartNonPremium
		&& !serverLimited
		&& smartTargetPreloadParts > 0)
		? smartTargetPreloadParts
		: 0;
	const auto stagedSmartPreloadParts = (seekPrefetchActive
		&& !_seekPrefetchBackgroundActive)
		? 0
		: std::max(smartTargetPreloadParts, steadyBitrateFloor);
	const auto localRecoveryPreloadParts = std::max(
		stagedSmartPreloadParts,
		seekPrefetchParts);
	const auto adaptivePreloadPercent = _adaptivePreloadPercent.load(
		std::memory_order_relaxed);
	const auto preloadPercent = std::max(
		adaptivePreloadPercent,
		recoveryPreloadPercent);
	const auto preloadCeiling = smartNonPremium
		? smartPreloadCap
		: int(kLoadFromRemoteMax) * 2;
	auto preloadParts = std::clamp(
		(preloadBase * preloadPercent) / 100,
		preloadMinimum,
		preloadCeiling);
	preloadParts = std::max(preloadParts, localRecoveryPreloadParts);
	if (serverLimited) {
		preloadParts = std::min(
			preloadParts,
			std::max(profile.smartMinimumPreload, seekPrefetchParts));
	} else if (serverRecovering) {
		preloadParts = std::min(
			preloadParts,
			std::max(
				profile.nonPremiumPreloadLimit,
				localRecoveryPreloadParts));
	}
	// Consumption-aware cap: once we know how fast video is being read,
	// avoid preloading past roughly the next N seconds of playback. This
	// keeps Burst mode from spending bandwidth on parts the user will not
	// reach within the already-banked buffer horizon.
	const auto playbackBytesPerSecond = smartNonPremium
		? double(smartPlaybackRate)
		: _consumptionBytesPerSec;
	if (boost > 0 && playbackBytesPerSecond > 0.0) {
		const auto targetSecondsAhead = smartNonPremium ? 20.0 : 30.0;
		const auto neededBytes = playbackBytesPerSecond
			* targetSecondsAhead;
		const auto neededParts = int(std::min<double>(
			(neededBytes + double(kPartSize - 1)) / double(kPartSize),
			double(preloadCeiling)));
		// Never clamp below the bitrate floor while Smart is catching up.
		const auto floorParts = std::max(
			preloadMinimum,
			localRecoveryPreloadParts);
		preloadParts = std::clamp(
			preloadParts,
			floorParts,
			std::max(floorParts, neededParts));
	}
	// Grow the active seek keep-window with steady preload so cancelOutside
	// does not kill next-slice catch-up after urgent is done.
	if (seekPrefetchActive
		&& _seekPrefetchBackgroundActive
		&& smartNonPremium
		&& preloadParts > 0) {
		_seekPrefetchWindowTill = SmartKeepWindowTill(
			int64(offset),
			_seekPrefetchWindowTill,
			preloadParts,
			kPartSize,
			size());
	}
	auto requestsLimit = std::clamp(
		(limitBase * _adaptiveLimitPercent.load(
			std::memory_order_relaxed)) / 100,
		limitMinimum,
		int(kLoadFromRemoteMax));
	if (serverLimited) {
		requestsLimit = profile.smartMinimumRequests;
	} else if (serverRecovering) {
		requestsLimit = std::min(
			requestsLimit,
			recoveryRequestLimit);
	} else if (smartNonPremium) {
		requestsLimit = std::min(
			requestsLimit,
			smartRequestLimit);
	}
	// While catching up outside server limited/recovery, use the full Smart
	// DC budget. Never raise above recovery clamps (FLOOD-safe).
	if (smartNonPremium
		&& !serverLimited
		&& !serverRecovering
		&& (underPlayback || bufferPressure)) {
		requestsLimit = std::max(requestsLimit, smartRequestLimit);
	}
	const auto serverPhase = serverLimited
		? 1
		: (serverRecovering ? 2 : 0);
	const auto previousRecoveryPreloadPercent
		= _smartPreloadRecoveryLoggedPercent.exchange(
			recoveryPreloadPercent,
			std::memory_order_relaxed);
	if (previousRecoveryPreloadPercent != recoveryPreloadPercent) {
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: preload recovery "
			"previous=%1 floor=%2 pressure=%3 remaining=%4 "
			"adaptive=%5 preload=%6 serverPhase=%7 targetParts=%8 "
			"bufferMs=%9 seekParts=%10 seekLocal=%11.")
			.arg(previousRecoveryPreloadPercent)
			.arg(recoveryPreloadPercent)
			.arg(bufferPressure ? 1 : 0)
			.arg(qlonglong(preloadRecoveryRemaining))
			.arg(adaptivePreloadPercent)
			.arg(preloadParts)
			.arg(serverPhase)
			.arg(smartTargetPreloadParts)
			.arg(qlonglong(recoveryBuffer))
			.arg(seekPrefetchParts)
			.arg(seekLocalRecovery ? 1 : 0));
	}
	// Sparse catch-up snapshot: transitions or ≥2s while still catching up.
	if (smartNonPremium) {
		const auto catchUp = underPlayback || bufferPressure;
		const auto due = (catchUp != _smartCatchupLogged)
			|| (catchUp
				&& (now >= _smartCatchupLogLastTime
					+ kSmartCatchupLogMinInterval)
				&& (preloadParts != _smartCatchupLogPreload
					|| requestsLimit != _smartCatchupLogRequests));
		if (due) {
			_smartCatchupLogged = catchUp;
			_smartCatchupLogLastTime = now;
			_smartCatchupLogPreload = preloadParts;
			_smartCatchupLogRequests = requestsLimit;
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Smart catch-up "
				"active=%1 underPlayback=%2 pressure=%3 seekLocal=%4 "
				"throughput=%5 playback=%6 preload=%7 requests=%8 "
				"targetParts=%9 windowTill=%10 loading=%11 "
				"serverPhase=%12.")
				.arg(catchUp ? 1 : 0)
				.arg(underPlayback ? 1 : 0)
				.arg(bufferPressure ? 1 : 0)
				.arg(seekLocalRecovery ? 1 : 0)
				.arg(streamThroughput)
				.arg(smartPlaybackRate)
				.arg(preloadParts)
				.arg(requestsLimit)
				.arg(smartTargetPreloadParts)
				.arg(qlonglong(_seekPrefetchWindowTill))
				.arg(_loadingOffsets.empty() ? 0 : 1)
				.arg(serverPhase));
		}
	}
	const auto serverRequests = [&] {
		if (serverLimited || !smartNonPremium) {
			return 0;
		}
		return serverRecovering
			? recoveryRequestLimit
			: smartRequestLimit;
	}();
	const auto previousServerPhase = _serverLimitPhase.exchange(
		serverPhase,
		std::memory_order_relaxed);
	const auto previousServerRequests = _serverLimitRequests.exchange(
		serverRequests,
		std::memory_order_relaxed);
	if (previousServerPhase != serverPhase
		|| previousServerRequests != serverRequests) {
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: server phase=%1 "
			"dc=%2 previous=%3 preload=%4 queueLimit=%5 "
			"dispatchLimit=%6 penalty=%7.")
			.arg(serverPhase)
			.arg(_serverDcId.load(std::memory_order_relaxed))
			.arg(previousServerPhase)
			.arg(preloadParts)
			.arg(requestsLimit)
			.arg(serverRequests)
			.arg(serverState.penalty));
	}
	auto result = _slices.fill(offset, buffer, preloadParts, requestsLimit);
	auto remoteRequests = 0;
	for (const auto requestOffset : result.offsetsFromLoader.values()) {
		(void)requestOffset;
		++remoteRequests;
	}
	if (seekPrefetchActive && !_seekPrefetchBackgroundActive) {
		const auto urgentTill = _seekPrefetchUrgentWindowTill;
		const auto urgentStart = _seekPrefetchUrgentWindowStart;
		auto urgentHits = 0;
		auto urgentParts = 0;
		if (urgentStart >= 0 && urgentTill > urgentStart) {
			for (auto part = urgentStart; part < urgentTill; part += kPartSize) {
				++urgentParts;
				if (_slices.hasPart(uint32(part))) {
					++urgentHits;
				}
			}
		}
		const auto urgentReady = SmartSeekUrgentWindowReady(
			urgentHits,
			urgentParts,
			int64(offset),
			urgentTill);
		if (urgentReady) {
			_seekPrefetchBackgroundActive = true;
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Reader seek prefetch "
				"phase=background offset=%1 source=%2 urgentTill=%3 "
				"windowTill=%4 remoteRequests=%5 urgentHits=%6 "
				"urgentParts=%7.")
				.arg(qulonglong(offset))
				.arg((result.state == FillState::Success)
					? u"cache"_q
					: u"remote"_q)
				.arg(qlonglong(urgentTill))
				.arg(qlonglong(_seekPrefetchWindowTill))
				.arg(remoteRequests)
				.arg(urgentHits)
				.arg(urgentParts));
		}
	}
	if (result.state != FillState::Success) {
		VIDEO_PLAYBACK_VERBOSE_LOG(("Video Playback: Reader fill waiting offset=%1 buffer=%2 state=%3 boost=%4 preloadBase=%5 preload=%6 limitBase=%7 limit=%8 adaptivePreload=%9 adaptiveLimit=%10 playbackBps=%11 remoteRequests=%12 loadingActive=%13 headerBytes=%14 size=%15.")
			.arg(qulonglong(offset))
			.arg(qlonglong(buffer.size()))
			.arg(int(result.state))
			.arg(boost)
			.arg(preloadBase)
			.arg(preloadParts)
			.arg(limitBase)
			.arg(requestsLimit)
			.arg(_adaptivePreloadPercent.load(std::memory_order_relaxed))
			.arg(_adaptiveLimitPercent.load(std::memory_order_relaxed))
			.arg(playbackBytesPerSecond, 0, 'f', 0)
			.arg(remoteRequests)
			.arg(_loadingOffsets.empty() ? 0 : 1)
			.arg(_slices.headerSize())
			.arg(qlonglong(size())));
	}
	if (result.state != FillState::Success && _slices.headerWontBeFilled()) {
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Reader header limit hit at offset=%1 size=%2 headerBytes=%3 boost=%4 preload=%5 limit=%6.")
			.arg(qulonglong(offset))
			.arg(qlonglong(size()))
			.arg(_slices.headerSize())
			.arg(boost)
			.arg(preloadParts)
			.arg(requestsLimit));
		_streamingError = Error::NotStreamable;
		return FillState::Failed;
	}

	for (const auto sliceNumber : result.sliceNumbersFromCache.values()) {
		readFromCache(sliceNumber);
	}

	if (_cacheHelper && result.toCache.number >= 0) {
		// If we put to cache the header (number == 0) that means we're in
		// HeaderMode::Good and really are putting the first slice to cache.
		Assert(result.toCache.number > 0 || _slices.isGoodHeader());

		const auto index = std::max(result.toCache.number, 1) - 1;
		cancelLoadInRange(index * kInSlice, (index + 1) * kInSlice);
		putToCache(std::move(result.toCache));
	}
	auto checkPriority = true;
	consumePendingTailPrefetch();
	if (StreamingSeekCancelEnabled() && !_loadingOffsets.empty()) {
		auto minOff = std::numeric_limits<uint32>::max();
		auto maxOff = uint32(0);
		auto hasAny = false;
		for (const auto off : result.offsetsFromLoader.values()) {
			hasAny = true;
			if (off < minOff) {
				minOff = off;
			}
			if (off > maxOff) {
				maxOff = off;
			}
		}
		if (hasAny) {
			constexpr auto kGuardParts = int64(2);
			constexpr auto kUintMax
				= std::numeric_limits<uint32>::max();
			const auto guard = kGuardParts * kPartSize;
			const auto startBelow = int64(minOff) - guard;
			const auto keepTill = SmartKeepWindowTill(
				int64(offset),
				seekPrefetchActive
					? seekPrefetchWindowTill
					: (int64(maxOff) + kPartSize + guard),
				preloadParts,
				kPartSize,
				size());
			const auto windowStart = seekPrefetchActive
				? uint32(seekPrefetchWindowStart)
				: (startBelow > 0)
				? uint32(startBelow)
				: uint32(0);
			const auto windowTill = (keepTill >= int64(kUintMax))
				? kUintMax
				: uint32(std::max<int64>(0, keepTill));
			if (windowStart < windowTill) {
				cancelLoadOutsideWindow(windowStart, windowTill);
			}
		}
	}
	for (const auto offset : result.offsetsFromLoader.values()) {
		if (checkPriority) {
			checkLoadWillBeFirst(offset);
			checkPriority = false;
		}
		loadAtOffset(offset);
	}
	return result.state;
}

void Reader::cancelLoadInRange(uint32 from, uint32 till) {
	Expects(from < till);

	for (const auto offset : _loadingOffsets.takeInRange(from, till)) {
		if (!_downloaderOffsetsRequested.contains(offset)) {
			_seekCancellationOffsets.remove(offset);
			_loader->cancel(offset);
		}
	}
}

void Reader::noteDualKeepRead(int64 offset, int64 span) {
	if (!smartStreamingEnabled() || offset < 0) {
		return;
	}
	const auto guard = std::max<int64>(
		StreamingSeekCancelGuardBytes(),
		int64(2) * 1024 * 1024);
	SmartNoteDualKeepOffset(
		_dualKeep,
		offset,
		guard,
		std::max(span, int64(kPartSize)),
		size());
}

void Reader::cancelLoadOutsideWindow(
		uint32 windowStart,
		uint32 windowTill,
		bool force) {
	Expects(windowStart < windowTill);

	const auto preserveSent = smartStreamingEnabled();
	const auto now = crl::now();
	const auto serverLimited = (DownloadBoostLevel() == 6)
		&& !_premiumSession
		&& (now < _serverLimitedUntil.load(std::memory_order_relaxed));
	const auto seekRecovery = preserveSent
		&& (now < _smartSeekRecoveryUntil.load(std::memory_order_acquire));
	const auto underPlayback = preserveSent
		&& SmartIsUnderPlayback(
			_loader->smartStreamingPlaybackRate(),
			_streamThroughputBytesPerSecond.load(std::memory_order_relaxed));
	// Only skip cancel during *steady* under-playback. Never skip when:
	// - force (new seek window install), or
	// - still inside seek recovery (old parts must yield to the new position).
	// bufferPressure alone used to skip cancel and made frequent seeks worse.
	if (!force && underPlayback && !seekRecovery) {
		if (!_smartUnderPlaybackSkipLogged
			|| now >= _smartUnderPlaybackSkipLogLastTime
				+ kSmartCatchupLogMinInterval) {
			_smartUnderPlaybackSkipLogged = true;
			_smartUnderPlaybackSkipLogLastTime = now;
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Reader cancel outside "
				"window skipped by under-playback start=%1 till=%2 "
				"throughput=%3 playback=%4 boost=%5.")
				.arg(qulonglong(windowStart))
				.arg(qulonglong(windowTill))
				.arg(_streamThroughputBytesPerSecond.load(
					std::memory_order_relaxed))
				.arg(_loader->smartStreamingPlaybackRate())
				.arg(DownloadBoostLevel()));
		}
		return;
	}
	if (!underPlayback) {
		_smartUnderPlaybackSkipLogged = false;
	}
	if (!force
		&& !preserveSent
		&& (serverLimited
			|| _speedIsThrottled.load(std::memory_order_relaxed))) {
		VIDEO_PLAYBACK_VERBOSE_LOG(("Video Playback: Reader cancel outside window skipped by throttle start=%1 till=%2 boost=%3.")
			.arg(qulonglong(windowStart))
			.arg(qulonglong(windowTill))
			.arg(DownloadBoostLevel()));
		return;
	}

	auto cancelled = 0;
	auto selective = 0;
	auto pinned = 0;
	auto dualKept = 0;
	const auto protectDual = preserveSent && !force;
	const auto cancelOne = [&](int64 offset) {
		if (_pinnedTailOffsets.contains(offset)) {
			if (!preserveSent) {
				_loadingOffsets.add(offset);
			}
			++pinned;
			return;
		}
		// Keep both A/V envelopes alive unless this is a forced seek install.
		if (protectDual && SmartOffsetInDualKeep(offset, _dualKeep)) {
			if (preserveSent) {
				_loadingOffsets.add(offset);
			}
			++dualKept;
			return;
		}
		if (!_downloaderOffsetsRequested.contains(uint32(offset))) {
			if (preserveSent) {
				if (_seekCancellationOffsets.emplace(offset).second) {
					_loader->cancelForSeek(offset);
					++selective;
				}
			} else {
				_seekCancellationOffsets.remove(offset);
				_loader->cancel(offset);
				++cancelled;
			}
		}
	};
	const auto offsetsInRange = [&](int64 from, int64 till) {
		return preserveSent
			? _loadingOffsets.valuesInRange(from, till)
			: _loadingOffsets.takeInRange(from, till);
	};
	if (windowStart > 0) {
		for (const auto off : offsetsInRange(0, windowStart)) {
			cancelOne(off);
		}
	}
	constexpr auto kMax = std::numeric_limits<uint32>::max();
	if (windowTill < kMax) {
		for (const auto off : offsetsInRange(windowTill, kMax)) {
			cancelOne(off);
		}
	}
	if (cancelled || selective || pinned || dualKept) {
		// Always rate-limit force DEBUG (including seekRecovery).
		const auto logForceDebug = force
			&& (now >= _smartForceCancelLogLastTime
				+ kSmartCatchupLogMinInterval);
		if (logForceDebug) {
			_smartForceCancelLogLastTime = now;
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Reader cancel outside "
				"window force=%1 start=%2 till=%3 cancelled=%4 selective=%5 "
				"pinned=%6 dualKept=%7 seekRecovery=%8 boost=%9.")
				.arg(1)
				.arg(qulonglong(windowStart))
				.arg(qulonglong(windowTill))
				.arg(cancelled)
				.arg(selective)
				.arg(pinned)
				.arg(dualKept)
				.arg(seekRecovery ? 1 : 0)
				.arg(DownloadBoostLevel()));
		} else {
			VIDEO_PLAYBACK_VERBOSE_LOG(("Video Playback: Reader cancel outside window force=%1 start=%2 till=%3 cancelled=%4 selective=%5 pinned=%6 dualKept=%7 boost=%8.")
				.arg(force ? 1 : 0)
				.arg(qulonglong(windowStart))
				.arg(qulonglong(windowTill))
				.arg(cancelled)
				.arg(selective)
				.arg(pinned)
				.arg(dualKept)
				.arg(DownloadBoostLevel()));
		}
	}
}

void Reader::consumePendingSeekPrefetch() {
	const auto generation = _seekPrefetchGeneration.load(
		std::memory_order_acquire);
	if (!generation || generation == _seekPrefetchConsumedGeneration) {
		return;
	}
	_seekPrefetchConsumedGeneration = generation;
	const auto clear = [&] {
		_seekPrefetchWindowStart = -1;
		_seekPrefetchWindowTill = -1;
		_seekPrefetchUrgentWindowStart = -1;
		_seekPrefetchUrgentWindowTill = -1;
		_seekPrefetchBackgroundActive = false;
	};
	const auto amount = _seekPrefetchBytes.load(std::memory_order_relaxed);
	const auto requestedOffset = _seekPrefetchOffset.load(
		std::memory_order_relaxed);
	const auto urgentAmount = _seekPrefetchUrgentBytes.load(
		std::memory_order_relaxed);
	const auto requestedUrgentOffset = _seekPrefetchUrgentOffset.load(
		std::memory_order_relaxed);
	const auto fileSize = size();
	if (amount <= 0
		|| urgentAmount <= 0
		|| requestedOffset < 0
		|| requestedOffset >= fileSize
		|| requestedUrgentOffset < 0
		|| requestedUrgentOffset >= fileSize) {
		clear();
		return;
	}
	const auto alignWindow = [&](int64 offset, int64 bytes) {
		const auto start = (offset / kPartSize) * kPartSize;
		const auto till = offset + std::min(bytes, fileSize - offset);
		const auto tillParts = (till / kPartSize)
			+ ((till % kPartSize) ? 1 : 0);
		return std::pair(start, std::min(fileSize, tillParts * kPartSize));
	};
	const auto background = alignWindow(requestedOffset, amount);
	const auto urgent = alignWindow(requestedUrgentOffset, urgentAmount);
	const auto windowStart = std::min(background.first, urgent.first);
	const auto windowTill = std::max(background.second, urgent.second);
	if (windowStart >= windowTill || urgent.first >= urgent.second) {
		clear();
		return;
	}
	const auto now = crl::now();
	const auto serverRecovering = now < _serverRecoveryUntil.load(
		std::memory_order_relaxed);
	// New seek position: drop dual A/V envelopes from the previous place,
	// then force-cancel outside the new window.
	SmartClearDualKeep(_dualKeep);
	if (StreamingSeekCancelEnabled()) {
		cancelLoadOutsideWindow(
			uint32(windowStart),
			uint32(windowTill),
			true);
	}
	_seekPrefetchWindowStart = windowStart;
	_seekPrefetchWindowTill = windowTill;
	_seekPrefetchUrgentWindowStart = urgent.first;
	_seekPrefetchUrgentWindowTill = urgent.second;
	const auto countHits = [&](int64 start, int64 till) {
		auto result = 0;
		for (auto offset = start; offset < till; offset += kPartSize) {
			if (_slices.hasPart(uint32(offset))) {
				++result;
			}
		}
		return result;
	};
	const auto urgentParts = int(
		(urgent.second - urgent.first + kPartSize - 1) / kPartSize);
	const auto urgentHits = countHits(urgent.first, urgent.second);
	_seekPrefetchBackgroundActive = (urgentHits == urgentParts);
	const auto targetParts = int(
		(windowTill - windowStart + kPartSize - 1) / kPartSize);
	const auto targetHits = countHits(windowStart, windowTill);
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Reader seek prefetch window "
		"start=%1 bytes=%2 targetBytes=%3 targetParts=%4 cacheHits=%5 "
		"urgentStart=%6 urgentBytes=%7 urgentParts=%8 urgentHits=%9 "
		"phase=%10 playback=%11 size=%12 boost=%13 bufferMs=%14 "
		"recovering=%15.")
		.arg(qlonglong(windowStart))
		.arg(qlonglong(windowTill - windowStart))
		.arg(qlonglong(amount))
		.arg(targetParts)
		.arg(targetHits)
		.arg(qlonglong(urgent.first))
		.arg(qlonglong(urgent.second - urgent.first))
		.arg(urgentParts)
		.arg(urgentHits)
		.arg(_seekPrefetchBackgroundActive
			? u"background"_q
			: u"urgent"_q)
		.arg(_loader->smartStreamingPlaybackRate())
		.arg(qlonglong(fileSize))
		.arg(DownloadBoostLevel())
		.arg(qlonglong(smartStreamingBackgroundBuffer()))
		.arg(serverRecovering ? 1 : 0));
}

void Reader::consumePendingTailPrefetch() {
	const auto tail = _pendingTailPrefetchBytes.exchange(
		0,
		std::memory_order_acq_rel);
	if (tail <= 0) {
		return;
	}
	// Do not compete with first-frame urgent window after a seek.
	if (_seekPrefetchWindowStart >= 0
		&& !_seekPrefetchBackgroundActive) {
		_pendingTailPrefetchBytes.store(
			std::max<int64>(
				_pendingTailPrefetchBytes.load(std::memory_order_relaxed),
				tail),
			std::memory_order_release);
		return;
	}
	const auto fileSize = size();
	// Skip tiny files: FFmpeg will read them end-to-end anyway, and for
	// files fully embedded in the header slice the moov is already there.
	if (fileSize <= kMaxOnlyInHeader || fileSize <= tail) {
		return;
	}
	const auto clamped = std::min(tail, fileSize);
	const auto tailStart = fileSize - clamped;
	const auto alignedStart = (tailStart / kPartSize) * kPartSize;
	auto requested = 0;
	for (auto off = alignedStart; off < fileSize; off += kPartSize) {
		_pinnedTailOffsets.emplace(off);
		loadAtOffset(uint32(off));
		++requested;
	}
	if (requested) {
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Reader tail prefetch start=%1 bytes=%2 requested=%3 size=%4 boost=%5.")
			.arg(qlonglong(alignedStart))
			.arg(qlonglong(clamped))
			.arg(requested)
			.arg(qlonglong(fileSize))
			.arg(DownloadBoostLevel()));
	}
}

void Reader::cancelStreamingLoads(bool preserveSent) {
	enqueueDownloaderOffsets();
	preserveSent = preserveSent && smartStreamingEnabled();
	if (!preserveSent) {
		_pinnedTailOffsets.clear();
	}
	auto cancelled = 0;
	auto selective = 0;
	auto keptForDownloader = 0;
	auto keptPinned = 0;
	const auto offsets = preserveSent
		? _loadingOffsets.valuesInRange(
			0,
			std::numeric_limits<int64>::max())
		: _loadingOffsets.takeInRange(
			0,
			std::numeric_limits<int64>::max());
	for (const auto offset : offsets) {
		const auto downloaderNeedsOffset
			= _attachedDownloader
			&& (_downloaderOffsetsRequested.contains(offset)
			|| (ranges::find(_offsetsForDownloader, uint32(offset))
				!= end(_offsetsForDownloader)));
		if (downloaderNeedsOffset) {
			++keptForDownloader;
		} else if (preserveSent && _pinnedTailOffsets.contains(offset)) {
			++keptPinned;
		} else if (preserveSent) {
			if (_seekCancellationOffsets.emplace(offset).second) {
				_loader->cancelForSeek(offset);
				++selective;
			}
		} else {
			_loader->cancel(offset);
			++cancelled;
		}
	}
	if (cancelled || selective || keptForDownloader || keptPinned) {
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Reader cancel streaming "
			"cancelled=%1 selective=%2 downloader=%3 pinned=%4 "
			"active=%5.")
			.arg(cancelled)
			.arg(selective)
			.arg(keptForDownloader)
			.arg(keptPinned)
			.arg(_streamingActive.load(std::memory_order_acquire) ? 1 : 0));
	}
}

void Reader::checkLoadWillBeFirst(uint32 offset) {
	if (_loadingOffsets.front().value_or(offset) != offset) {
		_loadingOffsets.resetPriorities();
		_loader->resetPriorities();
	}
}

bool Reader::processCacheResults() {
	if (!_cacheHelper) {
		return false;
	}

	QMutexLocker lock(&_cacheHelper->mutex);
	auto loaded = base::take(_cacheHelper->results);
	auto sizes = base::take(_cacheHelper->sizes);
	lock.unlock();

	for (auto &[sliceNumber, cachedParts] : _downloaderReadCache) {
		if (!cachedParts) {
			const auto i = loaded.find(sliceNumber);
			if (i != end(loaded)) {
				cachedParts = i->second;
			}
		}
	}

	if (_streamingError) {
		return false;
	}
	for (auto &[sliceNumber, result] : loaded) {
		_slices.processCacheResult(sliceNumber, std::move(result));
	}
	if (!sizes.empty()) {
		_slices.processCachedSizes(sizes);
	}
	if (!loaded.empty()
		&& (loaded.front().first == 0)
		&& _slices.isGoodHeader()) {
		Assert(loaded.size() > 1);
		Assert((loaded.begin() + 1)->first == 1);
	}
	return !loaded.empty();
}

bool Reader::processLoadedParts() {
	if (_streamingError) {
		return false;
	}

	auto loaded = _loadedParts.take();
	auto queuedCancelled = 0;
	auto sentCompleted = 0;
	for (auto &part : loaded) {
		if (part.cancelled) {
			_loadingOffsets.remove(part.offset);
			_seekCancellationOffsets.remove(part.offset);
			++queuedCancelled;
			continue;
		}
		const auto cancellationRequested
			= _seekCancellationOffsets.remove(part.offset);
		if (!part.valid(size())) {
			_streamingError = Error::LoadFailed;
			return false;
		} else if (!_loadingOffsets.remove(part.offset)) {
			continue;
		}
		if (cancellationRequested) {
			++sentCompleted;
		}
		_slices.processPart(
			part.offset,
			std::move(part.bytes));
	}
	if (queuedCancelled || sentCompleted) {
		_seekCancelLogQueued += queuedCancelled;
		_seekCancelLogSentCompleted += sentCompleted;
		const auto now = crl::now();
		const auto pending = int(_seekCancellationOffsets.size());
		const auto due = (_seekCancelLogLastTime == 0)
			|| (now >= _seekCancelLogLastTime + kSmartCancelLogMinInterval)
			|| (pending == 0);
		if (due) {
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Reader seek cancel "
				"queued=%1 sentCompleted=%2 pending=%3.")
				.arg(_seekCancelLogQueued)
				.arg(_seekCancelLogSentCompleted)
				.arg(pending));
			_seekCancelLogQueued = 0;
			_seekCancelLogSentCompleted = 0;
			_seekCancelLogLastTime = now;
		} else {
			VIDEO_PLAYBACK_VERBOSE_LOG(("Video Playback: Reader seek cancel "
				"queued=%1 sentCompleted=%2 pending=%3.")
				.arg(queuedCancelled)
				.arg(sentCompleted)
				.arg(pending));
		}
	}
	return !loaded.empty();
}

bool Reader::checkForSomethingMoreReceived() {
	const auto result1 = processCacheResults();
	const auto result2 = processLoadedParts();
	return result1 || result2;
}

void Reader::loadAtOffset(uint32 offset) {
	if (_loadingOffsets.add(offset)) {
		_loader->load(offset);
	}
}

void Reader::finalizeCache() {
	if (!_cacheHelper) {
		return;
	}
	Assert(_cache != nullptr);
	auto toCache = _slices.unloadToCache();
	while (toCache.number >= 0) {
		putToCache(std::move(toCache));
		toCache = _slices.unloadToCache();
	}
	_cache->sync();
}

Reader::~Reader() {
	finalizeCache();
}

QByteArray SerializeComplexPartsMap(
		const base::flat_map<uint32, QByteArray> &parts) {
	auto result = QByteArray();
	const auto count = parts.size();
	const auto intSize = sizeof(int32);
	result.reserve(count * kPartSize + 2 * intSize * (count + 1));
	const auto appendInt = [&](int value) {
		auto serialized = int32(value);
		result.append(
			reinterpret_cast<const char*>(&serialized),
			intSize);
	};
	appendInt(count);
	for (const auto &[offset, part] : parts) {
		appendInt(offset);
		appendInt(part.size());
		result.append(part);
	}
	return result;
}

} // namespace Streaming
} // namespace Media
