/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_common.h"
#include "base/bytes.h"

#include <array>
#include <memory>
#include <optional>

namespace Media::Streaming {

class Reader;

struct SeekPrefetchRange {
	int64 offset = -1;
	int64 amount = 0;
};

struct SeekPrefetchRequest {
	static constexpr auto kCriticalRangeLimit = 2;

	uint64 generation = 0;
	int64 offset = -1;
	int64 amount = 0;
	int64 fallbackUrgentOffset = -1;
	std::array<SeekPrefetchRange, kCriticalRangeLimit> criticalRanges;
	int criticalRangeCount = 0;
};

struct SeekPrefetchProgress {
	uint64 generation = 0;
	crl::time requestedAt = 0;
	crl::time criticalReadyAt = 0;
	int criticalParts = 0;
	int criticalCacheHits = 0;
};

class FileSource {
public:
	enum class FillState : uchar {
		Success,
		WaitingRemote,
		Failed,
	};

	[[nodiscard]] virtual int64 size() const = 0;
	[[nodiscard]] virtual bool isRemoteLoader() const = 0;
	[[nodiscard]] virtual bool smartStreamingEnabled() const {
		return false;
	}
	[[nodiscard]] virtual crl::time smartStreamingRecoveryBuffer() const {
		return 0;
	}
	[[nodiscard]] virtual FillState fill(
		int64 offset,
		bytes::span buffer,
		not_null<crl::semaphore*> notify) = 0;
	virtual void prefetch(SeekPrefetchRequest) {
	}
	[[nodiscard]] virtual SeekPrefetchProgress seekPrefetchProgress(
			uint64) const {
		return {};
	}
	[[nodiscard]] virtual std::optional<Error> streamingError() const = 0;
	virtual void headerDone() = 0;
	[[nodiscard]] virtual int headerSize() const = 0;
	[[nodiscard]] virtual bool fullInCache() const = 0;
	virtual void startSleep(not_null<crl::semaphore*> wake) = 0;
	virtual void stopSleep() = 0;
	virtual void stopStreamingAsync() = 0;
	virtual void tryRemoveLoaderAsync() = 0;
	virtual void startStreaming() = 0;
	virtual void stopStreaming(bool stillActive = false) = 0;
	virtual void continueStreamingForSoftSeek() {
	}
	virtual void primeSeekPrefetch(SeekPrefetchRequest) {
	}
	virtual void setLoaderPriority(int priority) = 0;
	virtual void setSmartStreamingBufferPressure(bool) {
	}
	virtual void setSmartStreamingPlaybackRate(int) {
	}
	virtual void notifySmartStreamingSeek() {
	}
	[[nodiscard]] virtual rpl::producer<SpeedEstimate> speedEstimate() const = 0;

	virtual ~FileSource() = default;
};

[[nodiscard]] std::shared_ptr<FileSource> MakeFileSource(
	std::shared_ptr<Reader> reader);

} // namespace Media::Streaming
