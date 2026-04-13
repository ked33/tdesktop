/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_source.h"

#include "media/streaming/media_streaming_debug.h"
#include "media/streaming/media_streaming_loader.h"
#include "media/streaming/media_streaming_reader.h"
#include "storage/cache/storage_cache_types.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/thread_safe_wrap.h"
#include "base/weak_ptr.h"

#include <algorithm>
#include <atomic>
#include <limits>

namespace Media::Streaming {
namespace {

constexpr auto kPreloadPartsAhead = 8;
constexpr auto kVerboseFillLogs = 24;
constexpr auto kVerbosePartLogs = 24;

class ReaderFileSource final : public FileSource {
public:
	explicit ReaderFileSource(std::shared_ptr<Reader> reader)
	: _reader(std::move(reader)) {
	}

	[[nodiscard]] int64 size() const override {
		return _reader->size();
	}

	[[nodiscard]] bool isRemoteLoader() const override {
		return _reader->isRemoteLoader();
	}

	[[nodiscard]] FillState fill(
			int64 offset,
			bytes::span buffer,
			not_null<crl::semaphore*> notify) override {
		switch (_reader->fill(offset, buffer, notify)) {
		case Reader::FillState::Success:
			return FillState::Success;
		case Reader::FillState::Failed:
			return FillState::Failed;
		case Reader::FillState::WaitingCache:
		case Reader::FillState::WaitingRemote:
			return FillState::WaitingRemote;
		}
		Unexpected("Unknown reader fill state.");
		return FillState::Failed;
	}

	[[nodiscard]] std::optional<Error> streamingError() const override {
		return _reader->streamingError();
	}

	void headerDone() override {
		_reader->headerDone();
	}

	[[nodiscard]] int headerSize() const override {
		return _reader->headerSize();
	}

	[[nodiscard]] bool fullInCache() const override {
		return _reader->fullInCache();
	}

	void startSleep(not_null<crl::semaphore*> wake) override {
		_reader->startSleep(wake);
	}

	void stopSleep() override {
		_reader->stopSleep();
	}

	void stopStreamingAsync() override {
		_reader->stopStreamingAsync();
	}

	void tryRemoveLoaderAsync() override {
		_reader->tryRemoveLoaderAsync();
	}

	void startStreaming() override {
		_reader->startStreaming();
	}

	void stopStreaming(bool stillActive) override {
		_reader->stopStreaming(stillActive);
	}

	void setLoaderPriority(int priority) override {
		_reader->setLoaderPriority(priority);
	}

	[[nodiscard]] rpl::producer<SpeedEstimate> speedEstimate() const override {
		return _reader->speedEstimate();
	}

private:
	const std::shared_ptr<Reader> _reader;
};

class DirectFileSource final : public FileSource, public base::has_weak_ptr {
public:
	explicit DirectFileSource(std::unique_ptr<Loader> loader)
	: _loader(std::move(loader))
	, _size(_loader ? _loader->size() : 0)
	, _remoteLoader(_loader ? _loader->baseCacheKey().valid() : false)
	, _totalParts((_size + Loader::kPartSize - 1) / Loader::kPartSize)
	, _fullInCache(!_remoteLoader) {
		Expects(_loader != nullptr);
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Direct AVIO source created size=%1 remote=%2 totalParts=%3.")
			.arg(qlonglong(_size))
			.arg(_remoteLoader ? 1 : 0)
			.arg(qlonglong(_totalParts)));

		_loader->parts(
		) | rpl::on_next([=](LoadedPart &&part) {
			if ((_debugPartNotifications < kVerbosePartLogs)
				|| !(_debugPartNotifications % 32)
				|| (part.offset == LoadedPart::kFailedOffset)) {
				VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Direct AVIO source part notification offset=%1 bytes=%2 failed=%3.")
					.arg(qlonglong(part.offset))
					.arg(qlonglong(part.bytes.size()))
					.arg(part.offset == LoadedPart::kFailedOffset ? 1 : 0));
			}
			++_debugPartNotifications;
			_loadedParts.emplace(std::move(part));
			if (const auto waiting = _waiting.load(std::memory_order_acquire)) {
				_waiting.store(nullptr, std::memory_order_release);
				waiting->release();
			}
		}, _lifetime);
	}

	[[nodiscard]] int64 size() const override {
		return _size;
	}

	[[nodiscard]] bool isRemoteLoader() const override {
		return _remoteLoader;
	}

	[[nodiscard]] FillState fill(
			int64 offset,
			bytes::span buffer,
			not_null<crl::semaphore*> notify) override {
		Expects(offset >= 0);
		Expects(offset + buffer.size() <= _size);

		if ((_debugFillCalls < kVerboseFillLogs) || !(_debugFillCalls % 32)) {
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Direct AVIO fill enter offset=%1 amount=%2 contiguous=%3 loadedParts=%4 queued=%5 waitingCount=%6.")
				.arg(qlonglong(offset))
				.arg(qlonglong(buffer.size()))
				.arg(qlonglong(_contiguousLoadedTill))
				.arg(qlonglong(_loadedPartCount))
				.arg(qlonglong(_loadingOffsets.size()))
				.arg(_debugWaitReturns));
		}
		++_debugFillCalls;

		const auto startWaiting = [&] {
			_waiting.store(notify.get(), std::memory_order_release);
		};
		const auto clearWaiting = [&] {
			_waiting.store(nullptr, std::memory_order_release);
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

		[[maybe_unused]] const auto loaded = processLoadedParts();
		if (_streamingError) {
			return FillState::Failed;
		}

		auto last = fillFromLoaded(offset, buffer);
		while (last != FillState::Success) {
			startWaiting();
			++_debugWaitReturns;
			if ((_debugWaitReturns <= kVerboseFillLogs)
				|| !(_debugWaitReturns % 32)) {
				VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Direct AVIO fill waiting offset=%1 amount=%2 waitCount=%3 contiguous=%4 queued=%5 lastMissing=%6.")
					.arg(qlonglong(offset))
					.arg(qlonglong(buffer.size()))
					.arg(_debugWaitReturns)
					.arg(qlonglong(_contiguousLoadedTill))
					.arg(qlonglong(_loadingOffsets.size()))
					.arg(qlonglong(_lastMissingPartOffset)));
			}
			if (!processLoadedParts()) {
				break;
			}
			if (_streamingError) {
				return failed();
			}
			last = fillFromLoaded(offset, buffer);
		}
		if (last == FillState::Success
			&& ((_debugSuccessfulFills < kVerboseFillLogs)
				|| !(_debugSuccessfulFills % 32))) {
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Direct AVIO fill success offset=%1 amount=%2 contiguous=%3 loadedParts=%4 queued=%5.")
				.arg(qlonglong(offset))
				.arg(qlonglong(buffer.size()))
				.arg(qlonglong(_contiguousLoadedTill))
				.arg(qlonglong(_loadedPartCount))
				.arg(qlonglong(_loadingOffsets.size())));
		}
		if (last == FillState::Success) {
			++_debugSuccessfulFills;
		}
		return _streamingError ? failed() : (last == FillState::Success ? done() : last);
	}

	[[nodiscard]] std::optional<Error> streamingError() const override {
		return _streamingError;
	}

	void headerDone() override {
		if (_headerFinalized) {
			return;
		}
		_headerFinalized = true;
		_headerSize = int(std::clamp(
			_contiguousLoadedTill,
			int64(0),
			int64(std::numeric_limits<int>::max())));
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Direct AVIO header finalized size=%1 contiguous=%2 remote=%3.")
			.arg(qlonglong(_size))
			.arg(_headerSize)
			.arg(_remoteLoader ? 1 : 0));
	}

	[[nodiscard]] int headerSize() const override {
		return _headerSize;
	}

	[[nodiscard]] bool fullInCache() const override {
		return _fullInCache;
	}

	void startSleep(not_null<crl::semaphore*> wake) override {
		_sleeping.store(wake.get(), std::memory_order_release);
	}

	void stopSleep() override {
		_sleeping.store(nullptr, std::memory_order_release);
	}

	void stopStreamingAsync() override {
		_stopStreamingAsync = true;
		crl::on_main(this, [=] {
			if (_stopStreamingAsync) {
				stopStreaming(false);
			}
		});
	}

	void tryRemoveLoaderAsync() override {
		_loader->tryRemoveFromQueue();
	}

	void startStreaming() override {
		_streamingActive = true;
		refreshLoaderPriority();
	}

	void stopStreaming(bool stillActive) override {
		_stopStreamingAsync = false;
		_waiting.store(nullptr, std::memory_order_release);
		_sleeping.store(nullptr, std::memory_order_release);
		if (stillActive) {
			cancelOutstandingLoads(base::flat_set<int64>());
			return;
		}
		_streamingActive = false;
		refreshLoaderPriority();
		cancelOutstandingLoads(base::flat_set<int64>());
		_loader->stop();
	}

	void setLoaderPriority(int priority) override {
		if (_realPriority == priority) {
			return;
		}
		_realPriority = priority;
		refreshLoaderPriority();
	}

	[[nodiscard]] rpl::producer<SpeedEstimate> speedEstimate() const override {
		return _loader->speedEstimate();
	}

private:
	[[nodiscard]] FillState fillFromLoaded(int64 offset, bytes::span buffer) {
		queueRequiredOffsets(offset, buffer.size());
		const auto till = offset + buffer.size();
		auto cursor = offset;
		auto out = buffer;
		while (cursor < till) {
			const auto partOffset = AlignOffset(cursor);
			const auto i = _parts.find(partOffset);
			if (i == end(_parts)) {
				_lastMissingPartOffset = partOffset;
				return _streamingError ? FillState::Failed : FillState::WaitingRemote;
			}
			const auto partBytes = bytes::make_span(i->second);
			const auto from = int(cursor - partOffset);
			if (from < 0 || from >= partBytes.size()) {
				return _streamingError ? FillState::Failed : FillState::WaitingRemote;
			}
			const auto copy = std::min<int64>(partBytes.size() - from, till - cursor);
			bytes::copy(out.subspan(0, copy), partBytes.subspan(from, copy));
			out = out.subspan(copy);
			cursor += copy;
		}
		return FillState::Success;
	}

	[[nodiscard]] bool processLoadedParts() {
		if (_streamingError) {
			return false;
		}
		auto loaded = _loadedParts.take();
		auto changed = false;
		for (auto &part : loaded) {
			if (!part.valid(_size)) {
				_streamingError = Error::LoadFailed;
				VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Direct AVIO source load failed size=%1 partOffset=%2 partBytes=%3.")
					.arg(qlonglong(_size))
					.arg(qlonglong(part.offset))
					.arg(qlonglong(part.bytes.size())));
				return false;
			}
			const auto inserted = _parts.emplace(part.offset, std::move(part.bytes));
			_loadingOffsets.remove(part.offset);
			if (!inserted.second) {
				continue;
			}
			++_loadedPartCount;
			changed = true;
			updateContiguousLoadedTill();
			if ((_debugLoadedParts < kVerbosePartLogs) || !(_debugLoadedParts % 32)) {
				VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Direct AVIO part loaded offset=%1 bytes=%2 contiguous=%3 loadedParts=%4 queued=%5.")
					.arg(qlonglong(part.offset))
					.arg(qlonglong(inserted.first->second.size()))
					.arg(qlonglong(_contiguousLoadedTill))
					.arg(qlonglong(_loadedPartCount))
					.arg(qlonglong(_loadingOffsets.size())));
			}
			++_debugLoadedParts;
		}
		if (_loadedPartCount >= _totalParts) {
			_fullInCache = true;
		}
		return changed;
	}

	void updateContiguousLoadedTill() {
		while (_contiguousLoadedTill < _size) {
			const auto i = _parts.find(_contiguousLoadedTill);
			if (i == end(_parts)) {
				break;
			}
			_contiguousLoadedTill += i->second.size();
		}
	}

	void queueRequiredOffsets(int64 offset, int64 amount) {
		const auto start = AlignOffset(offset);
		const auto preload = std::max<int64>(
			amount,
			kPreloadPartsAhead * Loader::kPartSize);
		const auto till = std::min(
			_size,
			AlignOffset(offset + preload + Loader::kPartSize - 1)
				+ Loader::kPartSize);
		auto needed = base::flat_set<int64>();
		auto added = 0;
		auto firstAdded = int64(-1);
		auto lastAdded = int64(-1);
		for (auto part = start; part < till; part += Loader::kPartSize) {
			needed.emplace(part);
			if (_parts.contains(part) || _loadingOffsets.contains(part)) {
				continue;
			}
			if (_loadingOffsets.empty() || *_loadingOffsets.begin() != part) {
				_loader->resetPriorities();
			}
			_loadingOffsets.emplace(part);
			_loader->load(part);
			++added;
			if (firstAdded < 0) {
				firstAdded = part;
			}
			lastAdded = part;
		}
		if (((_debugQueueCalls < kVerboseFillLogs) || !(_debugQueueCalls % 32))
			&& (added > 0 || _loadingOffsets.empty())) {
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Direct AVIO queue request offset=%1 amount=%2 rangeStart=%3 rangeTill=%4 added=%5 firstAdded=%6 lastAdded=%7 queued=%8 contiguous=%9.")
				.arg(qlonglong(offset))
				.arg(qlonglong(amount))
				.arg(qlonglong(start))
				.arg(qlonglong(till))
				.arg(added)
				.arg(qlonglong(firstAdded))
				.arg(qlonglong(lastAdded))
				.arg(qlonglong(_loadingOffsets.size()))
				.arg(qlonglong(_contiguousLoadedTill)));
		}
		++_debugQueueCalls;
		cancelOutstandingLoads(needed);
	}

	void cancelOutstandingLoads(const base::flat_set<int64> &keep) {
		for (auto i = begin(_loadingOffsets); i != end(_loadingOffsets);) {
			if (keep.contains(*i)) {
				++i;
				continue;
			}
			_loader->cancel(*i);
			i = _loadingOffsets.erase(i);
		}
	}

	void refreshLoaderPriority() {
		_loader->setPriority(_streamingActive ? _realPriority : 0);
	}

	[[nodiscard]] static int64 AlignOffset(int64 offset) {
		return (offset / Loader::kPartSize) * Loader::kPartSize;
	}

	const std::unique_ptr<Loader> _loader;
	const int64 _size = 0;
	const bool _remoteLoader = false;
	const int _totalParts = 0;

	base::thread_safe_queue<LoadedPart, std::vector> _loadedParts;
	std::atomic<crl::semaphore*> _waiting = nullptr;
	std::atomic<crl::semaphore*> _sleeping = nullptr;
	std::atomic<bool> _stopStreamingAsync = false;

	base::flat_map<int64, QByteArray> _parts;
	base::flat_set<int64> _loadingOffsets;
	std::optional<Error> _streamingError;

	int _realPriority = 1;
	bool _streamingActive = false;
	bool _fullInCache = false;
	bool _headerFinalized = false;
	int _headerSize = 0;
	int _loadedPartCount = 0;
	int64 _contiguousLoadedTill = 0;
	int64 _lastMissingPartOffset = -1;
	int _debugFillCalls = 0;
	int _debugSuccessfulFills = 0;
	int _debugWaitReturns = 0;
	int _debugQueueCalls = 0;
	int _debugLoadedParts = 0;
	int _debugPartNotifications = 0;

	rpl::lifetime _lifetime;
};

} // namespace

std::shared_ptr<FileSource> MakeFileSource(std::shared_ptr<Reader> reader) {
	return std::make_shared<ReaderFileSource>(std::move(reader));
}

std::shared_ptr<FileSource> MakeDirectFileSource(std::unique_ptr<Loader> loader) {
	return std::make_shared<DirectFileSource>(std::move(loader));
}

} // namespace Media::Streaming
