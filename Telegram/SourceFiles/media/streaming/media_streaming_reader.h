/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_common.h"
#include "media/streaming/media_streaming_loader.h"
#include "base/bytes.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "base/thread_safe_wrap.h"

namespace Storage {
class StreamedFileDownloader;
} // namespace Storage

namespace Storage {
namespace Cache {
struct Key;
class Database;
} // namespace Cache
} // namespace Storage

namespace Media {
namespace Streaming {

class Loader;
struct LoadedPart;
enum class Error;

class Reader final : public base::has_weak_ptr {
public:
	enum class FillState : uchar {
		Success,
		WaitingCache,
		WaitingRemote,
		Failed,
	};

	// Main thread.
	explicit Reader(
		std::unique_ptr<Loader> loader,
		Storage::Cache::Database *cache = nullptr);

	void setLoaderPriority(int priority);

	// Any thread.
	[[nodiscard]] int64 size() const;
	[[nodiscard]] bool isRemoteLoader() const;
	[[nodiscard]] bool smartStreamingEnabled() const;
	[[nodiscard]] crl::time smartStreamingRecoveryBuffer() const;

	// Single thread.
	[[nodiscard]] FillState fill(
		int64 offset,
		bytes::span buffer,
		not_null<crl::semaphore*> notify);
	void prefetch(int64 offset, int64 amount, int64 urgentOffset);
	[[nodiscard]] std::optional<Error> streamingError() const;
	void headerDone();
	[[nodiscard]] int headerSize() const;
	[[nodiscard]] bool fullInCache() const;

	// Thread safe.
	void startSleep(not_null<crl::semaphore*> wake);
	void wakeFromSleep();
	void stopSleep();
	void stopStreamingAsync();
	void tryRemoveLoaderAsync();
	void requestTailPrefetch(int64 bytes);
	void setSmartStreamingBufferPressure(bool pressure);
	void setSmartStreamingPlaybackRate(int bytesPerSecond);
	void notifySmartStreamingSeek();

	// Main thread.
	void startStreaming();
	void stopStreaming(bool stillActive = false);
	[[nodiscard]] rpl::producer<LoadedPart> partsForDownloader() const;
	void loadForDownloader(
		not_null<Storage::StreamedFileDownloader*> downloader,
		int64 offset);
	void doneForDownloader(int64 offset);
	void cancelForDownloader(
		not_null<Storage::StreamedFileDownloader*> downloader);
	void continueDownloaderFromMainThread();
	[[nodiscard]] rpl::producer<SpeedEstimate> speedEstimate() const;

	~Reader();

private:
	static constexpr auto kLoadFromRemoteMax = 32;

	struct CacheHelper;

	// FileSize: Right now any file size fits 32 bit.

	using PartsMap = base::flat_map<uint32, QByteArray>;

	template <int Size>
	class StackIntVector {
	public:
		bool add(uint32 value);
		auto values() const;

	private:
		std::array<uint32, Size> _storage = { uint32(-1) };

	};

	struct SerializedSlice {
		int number = -1;
		QByteArray data;
	};
	struct FillResult {
		static constexpr auto kReadFromCacheMax = 2;

		StackIntVector<kReadFromCacheMax> sliceNumbersFromCache;
		StackIntVector<kLoadFromRemoteMax> offsetsFromLoader;
		SerializedSlice toCache;
		FillState state = FillState::WaitingRemote;
	};
	struct Slice {
		enum class Flag : uchar {
			LoadingFromCache = 0x01,
			LoadedFromCache = 0x02,
			ChangedSinceCache = 0x04,
			FullInCache = 0x08,
		};
		friend constexpr inline bool is_flag_type(Flag) { return true; }
		using Flags = base::flags<Flag>;

		struct PrepareFillResult {
			StackIntVector<kLoadFromRemoteMax> offsetsFromLoader;
			PartsMap::const_iterator start;
			PartsMap::const_iterator finish;
			bool ready = true;
		};

		void processCacheData(PartsMap &&data);
		void addPart(uint32 offset, QByteArray bytes);
		PrepareFillResult prepareFill(
			uint32 from,
			uint32 till,
			int preloadParts,
			int requestsLimit);

		// Get up to the runtime remote-load limit in from-till range.
		StackIntVector<kLoadFromRemoteMax> offsetsFromLoader(
			uint32 from,
			uint32 till,
			int requestsLimit) const;

		PartsMap parts;
		Flags flags;

	};

	class Slices {
	public:
		Slices(uint32 size, bool useCache);

		void headerDone(bool fromCache);
		[[nodiscard]] int headerSize() const;
		[[nodiscard]] bool fullInCache() const;
		[[nodiscard]] bool headerWontBeFilled() const;
		[[nodiscard]] bool headerModeUnknown() const;
		[[nodiscard]] bool isFullInHeader() const;
		[[nodiscard]] bool isGoodHeader() const;
		[[nodiscard]] bool waitingForHeaderCache() const;

		[[nodiscard]] int requestSliceSizesCount() const;

		void processCacheResult(int sliceNumber, PartsMap &&result);
		void processCachedSizes(const std::vector<int> &sizes);
		void processPart(uint32 offset, QByteArray &&bytes);

		[[nodiscard]] FillResult fill(
			uint32 offset,
			bytes::span buffer,
			int preloadParts,
			int requestsLimit);
		[[nodiscard]] SerializedSlice unloadToCache();

		[[nodiscard]] QByteArray partForDownloader(uint32 offset) const;
		[[nodiscard]] bool hasPart(uint32 offset) const;
		[[nodiscard]] bool readCacheForDownloaderRequired(uint32 offset);

	private:
		enum class HeaderMode {
			Unknown,
			Small,
			Good,
			Full,
			NoCache,
		};

		void applyHeaderCacheData();
		[[nodiscard]] int maxSliceSize(int sliceNumber) const;
		[[nodiscard]] SerializedSlice serializeAndUnloadSlice(
			int sliceNumber);
		[[nodiscard]] SerializedSlice serializeAndUnloadUnused();
		[[nodiscard]] QByteArray serializeComplexSlice(
			const Slice &slice) const;
		[[nodiscard]] QByteArray serializeAndUnloadFirstSliceNoHeader();
		void markSliceUsed(int sliceIndex);
		[[nodiscard]] bool computeIsGoodHeader() const;
		[[nodiscard]] FillResult fillFromHeader(
			uint32 offset,
			bytes::span buffer,
			int preloadParts,
			int requestsLimit);
		void unloadSlice(Slice &slice) const;
		void checkSliceFullLoaded(int sliceNumber);
		[[nodiscard]] bool checkFullInCache() const;

		std::vector<Slice> _data;
		Slice _header;
		std::deque<int> _usedSlices;
		uint32 _size = 0;
		HeaderMode _headerMode = HeaderMode::Unknown;
		bool _fullInCache = false;

	};

	// 0 is for headerData, slice index = sliceNumber - 1.
	// returns false if asked for a known-empty downloader slice cache.
	void readFromCache(int sliceNumber);
	[[nodiscard]] bool readFromCacheForDownloader(int sliceNumber);
	bool processCacheResults();
	void putToCache(SerializedSlice &&data);

	void cancelLoadInRange(uint32 from, uint32 till);
	void cancelLoadOutsideWindow(uint32 windowStart, uint32 windowTill);
	[[nodiscard]] crl::time smartStreamingBackgroundBuffer() const;
	void syncSmartStreamingBufferPressure(crl::time now);
	void consumePendingSeekPrefetch();
	void consumePendingTailPrefetch();
	void cancelStreamingLoads(bool preserveSent = false);
	void loadAtOffset(uint32 offset);
	void checkLoadWillBeFirst(uint32 offset);
	bool processLoadedParts();

	bool checkForSomethingMoreReceived();

	FillState fillFromSlices(uint32 offset, bytes::span buffer);

	void finalizeCache();

	void processDownloaderRequests();
	void checkCacheResultsForDownloader();
	void pruneDownloaderCache(uint32 minimalOffset);
	void pruneDoneDownloaderRequests();
	void sendDownloaderRequests();
	[[nodiscard]] bool downloaderWaitForCachedSlice(uint32 offset);
	void enqueueDownloaderOffsets();
	void checkForDownloaderChange(int checkItemsCount);
	void checkForDownloaderReadyOffsets();

	void refreshLoaderPriority();

	static std::shared_ptr<CacheHelper> InitCacheHelper(
		Storage::Cache::Key baseKey);

	const std::unique_ptr<Loader> _loader;
	const bool _premiumSession = true;
	Storage::Cache::Database * const _cache = nullptr;

	// shared_ptr is used to be able to have weak_ptr.
	const std::shared_ptr<CacheHelper> _cacheHelper;

	base::thread_safe_queue<LoadedPart, std::vector> _loadedParts;
	std::atomic<crl::semaphore*> _waiting = nullptr;
	std::atomic<crl::semaphore*> _sleeping = nullptr;
	std::atomic<bool> _stopStreamingAsync = false;
	std::atomic<int64> _pendingTailPrefetchBytes = 0;
	std::atomic<int64> _seekPrefetchOffset = -1;
	std::atomic<int64> _seekPrefetchBytes = 0;
	std::atomic<int64> _seekPrefetchUrgentOffset = -1;
	std::atomic<int64> _seekPrefetchUrgentBytes = 0;
	std::atomic<uint64> _seekPrefetchGeneration = 0;
	int64 _seekPrefetchWindowStart = -1;
	int64 _seekPrefetchWindowTill = -1;
	int64 _seekPrefetchUrgentWindowStart = -1;
	int64 _seekPrefetchUrgentWindowTill = -1;
	uint64 _seekPrefetchConsumedGeneration = 0;
	bool _seekPrefetchBackgroundActive = false;

	// Adaptive scheduling driven by speedEstimate (main thread updates,
	// streaming thread reads). 100 = static baseline.
	enum class SpeedState : uchar { Normal, Burst, Throttle };
	SpeedState _speedState = SpeedState::Normal;
	double _burstSpeedEma = 0.0;
	bool _burstSpeedInitialized = false;
	int _throttleConfirmCount = 0;
	std::atomic<int> _adaptivePreloadPercent = 100;
	std::atomic<int> _adaptiveLimitPercent = 100;
	std::atomic<bool> _speedIsThrottled = false;
	std::atomic<int> _streamThroughputBytesPerSecond = 0;
	std::atomic<int> _streamLatencyMs = 0;
	std::atomic<int> _streamJitterMs = 0;
	std::atomic<int> _smartBufferTargetLoggedMs = 0;
	std::atomic<bool> _smartBufferPressure = false;
	std::atomic<crl::time> _smartSeekRecoveryUntil = 0;
	std::atomic<crl::time> _smartSeekPressureLocalUntil = 0;
	std::atomic<crl::time> _smartPreloadRecoveryUntil = 0;
	std::atomic<int> _smartPreloadRecoveryLoggedPercent = 0;
	std::atomic<int> _serverObservedWaitMs = 0;
	std::atomic<crl::time> _serverLimitedUntil = 0;
	std::atomic<crl::time> _serverRecoveryUntil = 0;
	std::atomic<int> _serverDcId = 0;
	std::atomic<int> _serverPenalty = 0;
	std::atomic<int> _serverLimitPhase = 0;
	std::atomic<int> _serverLimitRequests = 0;

	// Playback-consumption estimator (streaming thread only). Updated from
	// Reader::fill() by sampling forward offset advancement, then read in
	// fillFromSlices() to cap Burst-mode preload to "enough for the next
	// N seconds" instead of spending bandwidth on parts that will not be
	// consumed within the banked-buffer horizon.
	crl::time _consumptionLastTime = 0;
	int64 _consumptionLastOffset = -1;
	double _consumptionBytesPerSec = 0.0;
	std::atomic<int> _seekCancelGeneration = 0;
	int _seekCancelObservedGeneration = 0;
	int64 _seekCancelLastOffset = -1;
	bool _seekCancelEnabledLastFill = false;

	PriorityQueue _loadingOffsets;
	base::flat_set<int64> _seekCancellationOffsets;
	base::flat_set<int64> _pinnedTailOffsets;
	int _seekCancelLogQueued = 0;
	int _seekCancelLogSentCompleted = 0;
	crl::time _seekCancelLogLastTime = 0;

	Slices _slices;

	// Even if streaming had failed, the Reader can work for the downloader.
	std::optional<Error> _streamingError;

	// In case streaming is active both main and streaming threads have work.
	// In case only downloader is active, all work is done on main thread.

	// Main thread.
	Storage::StreamedFileDownloader *_attachedDownloader = nullptr;
	rpl::event_stream<LoadedPart> _partsForDownloader;
	int _realPriority = 1;
	std::atomic<bool> _streamingActive = false;

	// Streaming thread.
	std::deque<uint32> _offsetsForDownloader;
	base::flat_set<uint32> _downloaderOffsetsRequested;
	base::flat_map<uint32, std::optional<PartsMap>> _downloaderReadCache;

	// Communication from main thread to streaming thread.
	// Streaming thread to main thread communicates using crl::on_main.
	base::thread_safe_queue<uint32> _downloaderOffsetRequests;
	base::thread_safe_queue<uint32> _downloaderOffsetAcks;

	rpl::lifetime _lifetime;

};

[[nodiscard]] QByteArray SerializeComplexPartsMap(
	const base::flat_map<uint32, QByteArray> &parts);

} // namespace Streaming
} // namespace Media
