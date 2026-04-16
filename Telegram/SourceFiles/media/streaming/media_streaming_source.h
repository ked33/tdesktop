/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_common.h"
#include "base/bytes.h"

#include <memory>
#include <optional>

namespace Media::Streaming {

class Reader;

class FileSource {
public:
	enum class FillState : uchar {
		Success,
		WaitingRemote,
		Failed,
	};

	[[nodiscard]] virtual int64 size() const = 0;
	[[nodiscard]] virtual bool isRemoteLoader() const = 0;
	[[nodiscard]] virtual FillState fill(
		int64 offset,
		bytes::span buffer,
		not_null<crl::semaphore*> notify) = 0;
	virtual void prefetch(int64 offset, int64 amount) {
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
	virtual void setLoaderPriority(int priority) = 0;
	[[nodiscard]] virtual rpl::producer<SpeedEstimate> speedEstimate() const = 0;

	virtual ~FileSource() = default;
};

[[nodiscard]] std::shared_ptr<FileSource> MakeFileSource(
	std::shared_ptr<Reader> reader);

} // namespace Media::Streaming
