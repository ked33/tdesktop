/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_source.h"

#include "media/streaming/media_streaming_reader.h"

namespace Media::Streaming {
namespace {

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

} // namespace

std::shared_ptr<FileSource> MakeFileSource(std::shared_ptr<Reader> reader) {
	return std::make_shared<ReaderFileSource>(std::move(reader));
}

} // namespace Media::Streaming
