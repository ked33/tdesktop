/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_mpv_index.h"

#include "base/bytes.h"
#include "media/streaming/media_streaming_mp4_index.h"
#include "media/streaming/media_streaming_mpv_http.h"
#include "media/streaming/media_streaming_reader.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace Media::Streaming::Mpv {

struct PreparedIndex::State {
	std::atomic<IndexState> status = IndexState::Unknown;
	crl::semaphore ready;
	mutable std::mutex mutex;
	std::optional<Mp4::IndexCache> cache;
};

PreparedIndex::PreparedIndex() : _state(std::make_shared<State>()) {
}

PreparedIndex::~PreparedIndex() {
	cancel();
}

IndexState PreparedIndex::state() const {
	return _state->status.load();
}

bool PreparedIndex::prepare() {
	auto expected = IndexState::Unknown;
	return _state->status.compare_exchange_strong(
		expected,
		IndexState::Preparing);
}

void PreparedIndex::cancel() {
	_state->status.store(IndexState::Unavailable);
	const auto lock = std::lock_guard(_state->mutex);
	_state->cache.reset();
}

std::size_t PreparedIndex::copy(
		std::int64_t offset,
		std::span<char> buffer) const {
	if (state() != IndexState::Ready) {
		return 0;
	}
	const auto lock = std::lock_guard(_state->mutex);
	return _state->cache ? _state->cache->copy(offset, buffer) : 0;
}

void PreparedIndex::start(std::shared_ptr<Reader> reader) {
	if (!reader) {
		cancel();
		return;
	} else if (state() != IndexState::Preparing) {
		reader->stopStreaming(false);
		return;
	}
	// Downloads only select the highest-priority queue while requests are
	// in flight. A lower priority would starve metadata behind continuous
	// media reads, including the seek that needs this index. Metadata mode
	// limits this reader to the blocks intersecting its small current read.
	reader->setLoaderPriority(2);
	std::thread(&PreparedIndex::Prepare, _state, std::move(reader)).detach();
}

void PreparedIndex::Prepare(
		std::shared_ptr<State> state,
		std::shared_ptr<Reader> reader) {
	reader->headerDone();
	const auto cancelled = [&] {
		return state->status.load() != IndexState::Preparing;
	};
	auto cache = Mp4::BuildIndexCache(
		reader->size(),
		[&](std::int64_t offset, std::span<char> buffer) {
			return Http::ReadChunk([&] {
				const auto result = reader->fill(
					offset,
					bytes::span(
						reinterpret_cast<bytes::type*>(buffer.data()),
						buffer.size()),
					&state->ready,
					ReadMode::Metadata);
				if (result == Reader::FillState::Success) {
					return Http::ReadResult::Success;
				} else if (result == Reader::FillState::Failed) {
					return Http::ReadResult::Failed;
				}
				return Http::ReadResult::Waiting;
			}, cancelled, [] {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				return true;
			}) == Http::ReadResult::Success;
		});
	crl::on_main([
		state,
		reader = std::move(reader),
		cache = std::move(cache)
	]() mutable {
		reader->stopStreaming(false);
		const auto lock = std::lock_guard(state->mutex);
		state->cache = std::move(cache);
		auto expected = IndexState::Preparing;
		if (!state->status.compare_exchange_strong(
			expected,
			state->cache ? IndexState::Ready : IndexState::Unavailable)) {
			state->cache.reset();
		}
	});
}

} // namespace Media::Streaming::Mpv
