/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_mpv_index.h"

#include "base/bytes.h"
#include "media/streaming/media_streaming_debug.h"
#include "media/streaming/media_streaming_mp4_fragment.h"
#include "media/streaming/media_streaming_mp4_index.h"
#include "media/streaming/media_streaming_mpv_http.h"
#include "media/streaming/media_streaming_reader.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace Media::Streaming::Mpv {

struct PreparedIndex::State {
	struct Seek {
		std::shared_ptr<const std::vector<Mp4::HeaderPatch>> patches;
		double start = 0.;
	};

	std::atomic<IndexState> status = IndexState::Unknown;
	crl::semaphore ready;
	mutable std::mutex mutex;
	std::condition_variable changed;
	std::optional<Mp4::IndexCache> cache;
	std::map<std::uint64_t, Seek> seeks;
	std::atomic<std::uint64_t> revision = 0;
	std::int64_t positionMs = 0;
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
	{
		const auto lock = std::lock_guard(_state->mutex);
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: MPV index cancelled "
			"revision=%1 target_ms=%2.")
			.arg(qulonglong(_state->revision.load()))
			.arg(qlonglong(_state->positionMs)));
		_state->cache.reset();
		_state->seeks.clear();
	}
	_state->changed.notify_all();
}

std::uint64_t PreparedIndex::request(std::int64_t positionMs) {
	const auto lock = std::lock_guard(_state->mutex);
	if (state() != IndexState::OnDemand || positionMs < 0) {
		return 0;
	}
	if (_state->revision.load() && _state->positionMs == positionMs) {
		return _state->revision.load();
	}
	_state->positionMs = positionMs;
	const auto revision = ++_state->revision;
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: MPV index requested "
		"revision=%1 target_ms=%2.")
		.arg(qulonglong(revision)).arg(qlonglong(positionMs)));
	_state->changed.notify_all();
	return revision;
}

bool PreparedIndex::ready(std::uint64_t revision) const {
	return bool(patches(revision));
}

double PreparedIndex::seekStart(std::uint64_t revision) const {
	const auto lock = std::lock_guard(_state->mutex);
	const auto i = _state->seeks.find(revision);
	return (i != _state->seeks.end()) ? i->second.start : -1.;
}

void PreparedIndex::settle() {
	const auto lock = std::lock_guard(_state->mutex);
	if (state() == IndexState::OnDemand) {
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: MPV index settled "
			"revision=%1 target_ms=%2.")
			.arg(qulonglong(_state->revision.load()))
			.arg(qlonglong(_state->positionMs)));
		_state->positionMs = -1;
		++_state->revision;
		_state->changed.notify_all();
	}
}

auto PreparedIndex::patches(std::uint64_t revision) const
-> std::shared_ptr<const std::vector<Mp4::HeaderPatch>> {
	const auto lock = std::lock_guard(_state->mutex);
	const auto i = _state->seeks.find(revision);
	return (i != _state->seeks.end())
		? i->second.patches
		: nullptr;
}

auto PreparedIndex::waitForPatches(
		std::uint64_t revision,
		const std::function<bool()> &cancelled,
		const std::function<bool()> &wait) const
-> std::shared_ptr<const std::vector<Mp4::HeaderPatch>> {
	if (!revision) {
		return nullptr;
	}
	while (!cancelled()) {
		{
			const auto lock = std::lock_guard(_state->mutex);
			const auto i = _state->seeks.find(revision);
			if (i != _state->seeks.end()) {
				return i->second.patches;
			} else if (_state->revision.load() != revision
				|| _state->positionMs < 0
				|| state() == IndexState::Unavailable) {
				return nullptr;
			} else if (state() == IndexState::Ready) {
				return std::make_shared<const std::vector<Mp4::HeaderPatch>>();
			}
		}
		if (!wait()) {
			break;
		}
	}
	return nullptr;
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

void PreparedIndex::start(
		std::shared_ptr<Reader> reader,
		std::int64_t durationMs) {
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
	std::thread(
		&PreparedIndex::Prepare,
		_state,
		std::move(reader),
		durationMs).detach();
}

void PreparedIndex::Prepare(
		std::shared_ptr<State> state,
		std::shared_ptr<Reader> reader,
		std::int64_t durationMs) {
	reader->headerDone();
	auto revision = std::uint64_t(0);
	auto reads = std::uint64_t(0);
	auto bytes = std::uint64_t(0);
	const auto cancelled = [&] {
		return state->status.load() == IndexState::Unavailable
			|| (revision && state->revision.load() != revision);
	};
	const auto read = [&](std::int64_t offset, std::span<char> buffer) {
		++reads;
		bytes += buffer.size();
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
	};
	auto fragments = Mp4::FragmentIndex::Create(reader->size(), durationMs, read);
	if (fragments) {
		auto expected = IndexState::Preparing;
		if (state->status.compare_exchange_strong(expected, IndexState::OnDemand)) {
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: MPV index on-demand ready "
				"reads=%1 bytes=%2.")
				.arg(qulonglong(reads)).arg(qulonglong(bytes)));
		}
	}
	while (fragments && state->status.load() == IndexState::OnDemand) {
		auto positionMs = std::int64_t(0);
		{
			auto lock = std::unique_lock(state->mutex);
			state->changed.wait(lock, [&] {
				return state->status.load() != IndexState::OnDemand
					|| state->revision.load() != revision;
			});
			if (state->status.load() != IndexState::OnDemand) {
				break;
			}
			revision = state->revision.load();
			positionMs = state->positionMs;
		}
		if (positionMs < 0) {
			continue;
		}
		reads = bytes = 0;
		const auto started = std::chrono::steady_clock::now();
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: MPV index seek started "
			"revision=%1 target_ms=%2.")
			.arg(qulonglong(revision)).arg(qlonglong(positionMs)));
		auto seek = fragments->seek(positionMs, read);
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started).count();
		if (cancelled()) {
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: MPV index seek cancelled "
				"revision=%1 target_ms=%2 current_revision=%3 state=%4 "
				"reads=%5 bytes=%6 elapsed_ms=%7.")
				.arg(qulonglong(revision)).arg(qlonglong(positionMs))
				.arg(qulonglong(state->revision.load()))
				.arg(int(state->status.load()))
				.arg(qulonglong(reads)).arg(qulonglong(bytes))
				.arg(qlonglong(elapsed)));
			continue;
		} else if (!seek) {
			auto expected = IndexState::OnDemand;
			if (state->status.compare_exchange_strong(expected, IndexState::Preparing)) {
				VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: MPV index fallback "
					"target_ms=%1 reads=%2 bytes=%3.")
					.arg(qlonglong(positionMs))
					.arg(qulonglong(reads)).arg(qulonglong(bytes)));
			}
			break;
		}
		const auto lock = std::lock_guard(state->mutex);
		if (!cancelled()) {
			state->seeks.emplace(
				revision,
				State::Seek{
					std::make_shared<const std::vector<Mp4::HeaderPatch>>(
						std::move(seek->patches)),
					seek->start,
				});
			while (state->seeks.size() > 8) {
				state->seeks.erase(state->seeks.begin());
			}
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: MPV index seek ready "
				"revision=%1 target_ms=%2 reads=%3 bytes=%4 elapsed_ms=%5.")
				.arg(qulonglong(revision)).arg(qlonglong(positionMs))
				.arg(qulonglong(reads)).arg(qulonglong(bytes)).arg(qlonglong(elapsed)));
		} else {
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: MPV index seek cancelled "
				"revision=%1 target_ms=%2 current_revision=%3 phase=publish.")
				.arg(qulonglong(revision)).arg(qlonglong(positionMs))
				.arg(qulonglong(state->revision.load())));
		}
	}
	revision = 0;
	auto cache = (state->status.load() == IndexState::Preparing)
		? Mp4::BuildIndexCache(reader->size(), read)
		: std::nullopt;
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

IndexControl ControlIndex(std::weak_ptr<PreparedIndex> index) {
	auto result = IndexControl{
		.state = [=] {
			const auto strong = index.lock();
			return strong ? strong->state() : IndexState::Unavailable;
		},
		.request = [=](std::int64_t positionMs) {
			const auto strong = index.lock();
			return strong ? strong->request(positionMs) : 0;
		},
		.ready = [=](std::uint64_t revision) {
			const auto strong = index.lock();
			return strong && strong->ready(revision);
		},
		.seekStart = [=](std::uint64_t revision) {
			const auto strong = index.lock();
			return strong ? strong->seekStart(revision) : -1.;
		},
		.settled = [=] {
			if (const auto strong = index.lock()) {
				strong->settle();
			}
		},
		.abandoned = [=] {
			if (const auto strong = index.lock()) {
				strong->cancel();
			}
		},
	};
	if (PlaybackDebugLogsEnabled()) {
		result.diagnostic = [](const QString &message) {
			VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: MPV controller %1")
				.arg(message));
		};
	}
	return result;
}

} // namespace Media::Streaming::Mpv
