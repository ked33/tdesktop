/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_diagnostics.h"

#include "base/timer.h"
#include "media/streaming/media_streaming_debug.h"
#include "media/streaming/media_streaming_loader.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>

namespace Media::Streaming {
namespace {

constexpr auto kTrackingLimit = 4096;
constexpr auto kReportInterval = crl::time(5000);
constexpr auto kWaitingReportInterval = crl::time(2000);

// Settings are published on the main thread: workers must not read the
// mutable enhanced-settings QHash on every fragment. The low bit enables
// collection; the remaining bits identify an uninterrupted capture. A
// toggle invalidates pending timings and range totals across all readers.
std::atomic<uint64> CaptureEpoch = 0;

[[nodiscard]] bool CaptureEnabled() {
	return (CaptureEpoch.load(std::memory_order_acquire) & 1) != 0;
}

[[nodiscard]] uint64 NextId() {
	static auto next = std::atomic<uint64>(0);
	return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

class ByteRanges final {
public:
	void add(int64 from, int64 length) {
		if (!_complete || length <= 0) {
			return;
		}
		auto till = from + length;
		auto i = _ranges.lower_bound(from);
		if (i != _ranges.begin() && std::prev(i)->second >= from) {
			--i;
		}
		while (i != _ranges.end() && i->first <= till) {
			from = std::min(from, i->first);
			till = std::max(till, i->second);
			_bytes -= i->second - i->first;
			i = _ranges.erase(i);
		}
		_ranges.emplace(from, till);
		_bytes += till - from;
		if (_ranges.size() > kTrackingLimit) {
			_complete = false;
			_ranges.clear();
		}
	}

	[[nodiscard]] int64 size() const {
		return _complete ? _bytes : -1;
	}

	[[nodiscard]] int64 intersection(const ByteRanges &other) const {
		if (!_complete || !other._complete) {
			return -1;
		}
		auto result = int64(0);
		auto i = _ranges.begin();
		auto j = other._ranges.begin();
		while (i != _ranges.end() && j != other._ranges.end()) {
			result += std::max<int64>(
				0,
				std::min(i->second, j->second) - std::max(i->first, j->first));
			if (i->second < j->second) {
				++i;
			} else {
				++j;
			}
		}
		return result;
	}

private:
	std::map<int64, int64> _ranges;
	int64 _bytes = 0;
	bool _complete = true;

};

struct RequestTiming {
	crl::time queuedAt = 0;
	crl::time sentAt = 0;
	bool retainedForSeek = false;
};

struct DemuxSeekTiming {
	uint64 generation = 0;
	crl::time position = 0;
	crl::time startedAt = 0;
	int firstPackets = 0;
	int targetPackets = 0;
};

struct TransferState {
	crl::time startedAt = crl::now();
	bool partial = false;
	bool requestsComplete = true;
	ByteRanges received;
	ByteRanges supplied;
	int64 payloadBytes = 0;
	int64 suppliedBytes = 0;
	int64 reusedBytes = 0;
	int64 cacheLoadedBytes = 0;
	int64 retainedPayloadBytes = 0;
	int64 dispatchedCount = 0;
	int64 receivedCount = 0;
	int64 cancelledQueued = 0;
	int64 cancelledSent = 0;
	int64 untrackedCompletions = 0;
	crl::time maxQueueMs = 0;
	crl::time maxRequestMs = 0;
	std::map<int64, RequestTiming> requests;
	int64 waitingOffset = -1;
	int64 waitingAmount = 0;
	crl::time waitingSince = 0;
	bool waitingCache = false;
	crl::time cacheWaitMs = 0;
	crl::time remoteWaitMs = 0;
	bool pressureRequested = false;
	bool pressureLocal = false;
	bool pressureForwarded = false;
	crl::time pressureSince = 0;
	int preloadParts = 0;
	int requestLimit = 0;
	int playbackRate = 0;
	int64 firstMissing = -1;
	int missingParts = 0;
	int criticalPendingParts = 0;
	int prefetchSlots = 0;
	ServerDelay server;
	SpeedEstimate speed = { .unreliable = true };
	std::optional<DemuxSeekTiming> demuxSeek;
	crl::time lastBridgeReport = 0;
	int64 bridgeRequests = 0;
	int64 bridgeWritten = 0;
	int64 bridgeBackgroundRead = 0;
	crl::time bridgeMaxFirstChunkMs = -1;
	int64 bridgeTotalRequests = 0;
	int64 bridgeTotalWritten = 0;
	int64 bridgeTotalBackgroundRead = 0;
	int64 bridgeFailed = 0;
	int64 bridgeDisconnected = 0;
	int64 bridgeSuperseded = 0;
};

[[nodiscard]] crl::time Buffered(const TrackState &state) {
	return (state.position == kTimeUnknown
		|| state.receivedTill == kTimeUnknown
		|| (state.duration != kTimeUnknown && state.position >= state.duration))
		? crl::time(-1)
		: std::max(crl::time(0), state.receivedTill - state.position);
}

[[nodiscard]] crl::time KnownMinimum(crl::time a, crl::time b) {
	return (a < 0) ? b : (b < 0) ? a : std::min(a, b);
}

} // namespace

void RefreshPlaybackDiagnosticsSettings() {
	const auto enabled = PlaybackDebugLogsEnabled();
	const auto epoch = CaptureEpoch.load(std::memory_order_relaxed);
	if (bool(epoch & 1) != enabled) {
		CaptureEpoch.store((epoch + 2) ^ 1, std::memory_order_release);
	}
}

struct TransferDiagnostics::Impl {
	Impl(int64 size, int dcId) : size(size), dcId(dcId) {
		RefreshPlaybackDiagnosticsSettings();
		epoch = CaptureEpoch.load(std::memory_order_acquire);
		if (epoch & 1) {
			state.emplace();
		}
	}

	TransferState &current(uint64 captureEpoch) {
		if (epoch != captureEpoch || !state) {
			state.emplace();
			state->partial = true;
			state->requestsComplete = false;
			++capture;
			epoch = captureEpoch;
		}
		return *state;
	}

	template <typename Callback>
	void update(Callback &&callback) {
		if (CaptureEnabled()) {
			const auto lock = std::lock_guard(mutex);
			const auto captureEpoch = CaptureEpoch.load(std::memory_order_acquire);
			if (captureEpoch & 1) {
				callback(current(captureEpoch));
			}
		}
	}

	const uint64 id = NextId();
	const int64 size = 0;
	const int dcId = 0;
	std::mutex mutex;
	std::atomic<uint64> bridgePlayId = 0;
	std::optional<TransferState> state;
	uint64 epoch = 0;
	uint64 capture = 1;
};

TransferDiagnostics::TransferDiagnostics(int64 size, int dcId)
: _impl(std::make_unique<Impl>(size, dcId)) {
}

TransferDiagnostics::~TransferDiagnostics() = default;

uint64 TransferDiagnostics::id() const {
	return _impl->id;
}

std::pair<int64, int64> TransferDiagnostics::byteTotals() const {
	const auto lock = std::lock_guard(_impl->mutex);
	const auto epoch = CaptureEpoch.load(std::memory_order_acquire);
	if (!(epoch & 1) || epoch != _impl->epoch || !_impl->state) {
		return { -1, -1 };
	}
	return { _impl->state->payloadBytes, _impl->state->suppliedBytes };
}

void TransferDiagnostics::demuxSeekStarted(
		uint64 generation,
		crl::time position) {
	_impl->update([&](TransferState &s) {
		s.demuxSeek = DemuxSeekTiming{
			.generation = generation,
			.position = position,
			.startedAt = crl::now(),
		};
	});
}

void TransferDiagnostics::demuxPacket(
		uint64 generation,
		bool video,
		crl::time position,
		int64 offset,
		int size,
		bool keyframe) {
	_impl->update([&](TransferState &s) {
		if (!s.demuxSeek || s.demuxSeek->generation != generation) {
			return;
		}
		auto &seek = *s.demuxSeek;
		const auto bit = video ? 1 : 2;
		const auto first = !(seek.firstPackets & bit);
		const auto target = position != kTimeUnknown
			&& position >= seek.position
			&& !(seek.targetPackets & bit);
		if (!first && !target) {
			return;
		}
		seek.firstPackets |= bit;
		if (target) {
			seek.targetPackets |= bit;
		}
		LOG((u"Video Playback: demux_packet reader_id=%1 seek_gen=%2 "
			"track=%3 target_ms=%4 position_ms=%5 offset=%6 size=%7 "
			"keyframe=%8 first=%9 target_reached=%10 elapsed_ms=%11"_q)
			.arg(qulonglong(id()))
			.arg(qulonglong(generation))
			.arg(video ? u"video"_q : u"audio"_q)
			.arg(qlonglong(seek.position))
			.arg(qlonglong(position == kTimeUnknown ? -1 : position))
			.arg(qlonglong(offset))
			.arg(size)
			.arg(keyframe ? 1 : 0)
			.arg(first ? 1 : 0)
			.arg(target ? 1 : 0)
			.arg(qlonglong(crl::now() - seek.startedAt)));
	});
}

void TransferDiagnostics::queued(int64 offset) {
	_impl->update([&](TransferState &s) {
		if (s.requests.size() < kTrackingLimit) {
			s.requests.emplace(offset, RequestTiming{ .queuedAt = crl::now() });
		} else {
			s.requestsComplete = false;
		}
	});
}

void TransferDiagnostics::dispatched(int64 offset) {
	_impl->update([&](TransferState &s) {
		++s.dispatchedCount;
		const auto i = s.requests.find(offset);
		if (i != s.requests.end()) {
			i->second.sentAt = crl::now();
			s.maxQueueMs = std::max(
				s.maxQueueMs,
				i->second.sentAt - i->second.queuedAt);
		} else {
			s.requestsComplete = false;
		}
	});
}

void TransferDiagnostics::received(int64 offset, int64 bytes) {
	_impl->update([&](TransferState &s) {
		s.payloadBytes += bytes;
		++s.receivedCount;
		s.received.add(offset, bytes);
		const auto i = s.requests.find(offset);
		if (i != s.requests.end()) {
			if (i->second.sentAt) {
				s.maxRequestMs = std::max(
					s.maxRequestMs,
					crl::now() - i->second.sentAt);
			}
			if (i->second.retainedForSeek) {
				s.retainedPayloadBytes += bytes;
			}
			s.requests.erase(i);
		} else {
			++s.untrackedCompletions;
			s.requestsComplete = false;
		}
	});
}

void TransferDiagnostics::reused(int64 bytes) {
	_impl->update([&](TransferState &s) { s.reusedBytes += bytes; });
}

void TransferDiagnostics::cancelled(int64 offset, bool sent) {
	_impl->update([&](TransferState &s) {
		++(sent ? s.cancelledSent : s.cancelledQueued);
		s.requests.erase(offset);
	});
}

void TransferDiagnostics::retainedForSeek(int64 offset) {
	_impl->update([&](TransferState &s) {
		const auto i = s.requests.find(offset);
		if (i != s.requests.end()) {
			i->second.retainedForSeek = true;
		}
	});
}

void TransferDiagnostics::cancelAll() {
	_impl->update([&](TransferState &s) {
		for (const auto &[offset, request] : s.requests) {
			++(request.sentAt ? s.cancelledSent : s.cancelledQueued);
		}
		s.requests.clear();
	});
}

void TransferDiagnostics::read(
		int64 offset,
		int64 bytes,
		bool success,
		bool cacheWait) {
	_impl->update([&](TransferState &s) {
		const auto now = crl::now();
		if (s.waitingSince
			&& (success
				|| s.waitingOffset != offset
				|| s.waitingAmount != bytes
				|| s.waitingCache != cacheWait)) {
			(s.waitingCache ? s.cacheWaitMs : s.remoteWaitMs)
				+= now - s.waitingSince;
			s.waitingSince = 0;
		}
		if (success) {
			s.suppliedBytes += bytes;
			s.supplied.add(offset, bytes);
			s.waitingAmount = 0;
		} else if (!s.waitingSince) {
			s.waitingOffset = offset;
			s.waitingAmount = bytes;
			s.waitingSince = now;
			s.waitingCache = cacheWait;
		}
	});
}

void TransferDiagnostics::readStopped() {
	read(0, 0, true, false);
}

void TransferDiagnostics::cacheLoaded(
		const base::flat_map<uint32, QByteArray> &parts) {
	_impl->update([&](TransferState &s) {
		for (const auto &part : parts) {
			s.cacheLoadedBytes += part.second.size();
		}
	});
}

void TransferDiagnostics::pressureRequested(bool pressure) {
	_impl->update([&](TransferState &s) { s.pressureRequested = pressure; });
}

void TransferDiagnostics::pressureForwarded(bool local, bool forwarded) {
	_impl->update([&](TransferState &s) {
		if (s.pressureLocal != local) {
			s.pressureSince = local ? crl::now() : 0;
		}
		s.pressureLocal = local;
		s.pressureForwarded = forwarded;
	});
}

void TransferDiagnostics::policy(
		int preloadParts,
		int requestLimit,
		int playbackRate) {
	_impl->update([&](TransferState &s) {
		s.preloadParts = preloadParts;
		s.requestLimit = requestLimit;
		s.playbackRate = playbackRate;
	});
}

void TransferDiagnostics::serverDelay(ServerDelay delay) {
	_impl->update([&](TransferState &s) { s.server = delay; });
}

void TransferDiagnostics::readPlan(
		int64 firstMissing,
		int missingParts,
		int criticalPendingParts,
		int prefetchSlots) {
	_impl->update([&](TransferState &s) {
		s.firstMissing = firstMissing;
		s.missingParts = missingParts;
		s.criticalPendingParts = criticalPendingParts;
		s.prefetchSlots = prefetchSlots;
	});
}

void TransferDiagnostics::speed(SpeedEstimate estimate) {
	_impl->update([&](TransferState &s) { s.speed = estimate; });
}

QString TransferDiagnostics::snapshot(crl::time now) {
	if (!CaptureEnabled()) {
		return {};
	}
	const auto lock = std::lock_guard(_impl->mutex);
	const auto epoch = CaptureEpoch.load(std::memory_order_acquire);
	if (!(epoch & 1)) {
		return {};
	}
	const auto &s = _impl->current(epoch);
	now = std::max(now, crl::now());
	const auto unique = s.received.size();
	const auto suppliedRemote = s.received.intersection(s.supplied);
	auto queued = 0;
	auto sent = 0;
	auto oldestQueued = crl::time(0);
	auto oldestSent = crl::time(0);
	auto readQueued = 0;
	auto readSent = 0;
	auto readOldest = crl::time(0);
	for (const auto &[offset, request] : s.requests) {
		if (request.sentAt) {
			++sent;
			oldestSent = std::max(oldestSent, now - request.sentAt);
		} else {
			++queued;
			oldestQueued = std::max(oldestQueued, now - request.queuedAt);
		}
		if (s.waitingSince
			&& !s.waitingCache
			&& offset < s.waitingOffset + s.waitingAmount
			&& offset + Loader::kPartSize > s.waitingOffset) {
			++(request.sentAt ? readSent : readQueued);
			readOldest = std::max(
				readOldest,
				now - (request.sentAt ? request.sentAt : request.queuedAt));
		}
	}
	const auto waitMs = s.waitingSince ? now - s.waitingSince : 0;
	return (u"reader_id=%1 reader_capture=%2 reader_partial=%3 dc=%4 "
		"size=%5 reader_elapsed_ms=%6 "
		"remote_payload_bytes=%7 remote_unique_bytes=%8 duplicate_payload_bytes=%9 "
		"supplied_bytes=%10 supplied_unique_bytes=%11 remote_unread_bytes=%12 "
		"downloader_reused_bytes=%13 cache_loaded_bytes=%14 seek_retained_bytes=%15 "_q
	).arg(qulonglong(id()))
		.arg(qulonglong(_impl->capture))
		.arg(s.partial ? 1 : 0)
		.arg(_impl->dcId)
		.arg(qlonglong(_impl->size))
		.arg(qlonglong(now - s.startedAt))
		.arg(qlonglong(s.payloadBytes))
		.arg(qlonglong(unique))
		.arg(qlonglong(unique >= 0 ? s.payloadBytes - unique : -1))
		.arg(qlonglong(s.suppliedBytes))
		.arg(qlonglong(s.supplied.size()))
		.arg(qlonglong(suppliedRemote >= 0 ? unique - suppliedRemote : -1))
		.arg(qlonglong(s.reusedBytes))
		.arg(qlonglong(s.cacheLoadedBytes))
		.arg(qlonglong(s.retainedPayloadBytes))
		+ (u"queued=%1 sent=%2 requests_complete=%3 dispatched=%4 received=%5 "
			"cancelled_queued=%6 cancelled_sent=%7 untracked_completions=%8 "
			"oldest_queue_ms=%9 oldest_sent_ms=%10 max_queue_ms=%11 max_request_ms=%12 "
			"cache_wait_ms=%13 remote_wait_ms=%14 read_wait_ms=%15 "
			"read_offset=%16 "_q
		).arg(s.requestsComplete ? queued : -1)
			.arg(s.requestsComplete ? sent : -1)
			.arg(s.requestsComplete ? 1 : 0)
			.arg(qlonglong(s.dispatchedCount))
			.arg(qlonglong(s.receivedCount))
			.arg(qlonglong(s.requestsComplete ? s.cancelledQueued : -1))
			.arg(qlonglong(s.requestsComplete ? s.cancelledSent : -1))
			.arg(qlonglong(s.untrackedCompletions))
			.arg(qlonglong(s.requestsComplete ? oldestQueued : -1))
			.arg(qlonglong(s.requestsComplete ? oldestSent : -1))
			.arg(qlonglong(s.requestsComplete ? s.maxQueueMs : -1))
			.arg(qlonglong(s.requestsComplete ? s.maxRequestMs : -1))
			.arg(qlonglong(s.cacheWaitMs + (s.waitingCache ? waitMs : 0)))
			.arg(qlonglong(s.remoteWaitMs + (s.waitingCache ? 0 : waitMs)))
			.arg(qlonglong(waitMs))
			.arg(qlonglong(s.waitingOffset))
		+ (u"read_missing_offset=%1 read_missing_parts=%2 read_queued=%3 "
			"read_sent=%4 read_oldest_ms=%5 critical_pending_parts=%6 "
			"prefetch_slots=%7 "_q
		).arg(qlonglong(s.waitingSince ? s.firstMissing : -1))
			.arg(s.waitingSince ? s.missingParts : 0)
			.arg(s.requestsComplete ? readQueued : -1)
			.arg(s.requestsComplete ? readSent : -1)
			.arg(qlonglong(s.requestsComplete ? readOldest : -1))
			.arg(s.criticalPendingParts)
			.arg(s.prefetchSlots)
		+ (u"preload_parts=%1 request_limit=%2 playback_bps=%3 speed_bps=%4 "
			"latency_ms=%5 jitter_ms=%6 speed_unreliable=%7 "
			"pressure_requested=%8 pressure_local=%9 pressure_forwarded=%10 "
			"pressure_age_ms=%11 limited_remaining_ms=%12 recovery_remaining_ms=%13 "
			"no_progress_ms=%14 read_stalled=%15"_q
		).arg(s.preloadParts)
			.arg(s.requestLimit)
			.arg(s.playbackRate)
			.arg(s.speed.bytesPerSecond)
			.arg(s.speed.latencyMs)
			.arg(s.speed.jitterMs)
			.arg(s.speed.unreliable ? 1 : 0)
			.arg(s.pressureRequested ? 1 : 0)
			.arg(s.pressureLocal ? 1 : 0)
			.arg(s.pressureForwarded ? 1 : 0)
			.arg(qlonglong(s.pressureSince ? now - s.pressureSince : 0))
			.arg(qlonglong(std::max(crl::time(0), s.server.limitedUntil - now)))
			.arg(qlonglong(std::max(crl::time(0), s.server.recoveryUntil - now)))
			.arg(s.speed.noProgressMs)
			.arg(s.speed.stalled ? 1 : 0)
		+ (u" bridge_play_id=%1 http_completed=%2 http_written_bytes=%3 "
			"http_background_read_bytes=%4 http_failed=%5 http_disconnected=%6 "
			"http_superseded=%7"_q)
			.arg(qulonglong(_impl->bridgePlayId.load(std::memory_order_relaxed)))
			.arg(qlonglong(s.bridgeTotalRequests))
			.arg(qlonglong(s.bridgeTotalWritten))
			.arg(qlonglong(s.bridgeTotalBackgroundRead))
			.arg(qlonglong(s.bridgeFailed))
			.arg(qlonglong(s.bridgeDisconnected))
			.arg(qlonglong(s.bridgeSuperseded));
}

void TransferDiagnostics::report(const char *reason) {
	const auto details = snapshot(crl::now());
	if (!details.isEmpty()) {
		LOG((u"Video Playback: transfer event=%1 %2"_q
			).arg(QString::fromLatin1(reason), details));
	}
}

void TransferDiagnostics::bridgeOpened(uint64 documentId, bool special) {
	_impl->bridgePlayId.store(id(), std::memory_order_relaxed);
	VIDEO_PLAYBACK_DEBUG_LOG((u"Video Playback: bridge_open play_id=%1 "
		"doc=%2 backend=%3 %4"_q)
		.arg(qulonglong(id()))
		.arg(qulonglong(documentId))
		.arg(special ? u"mpv-special"_q : u"mpv"_q)
		.arg(snapshot(crl::now())));
}

namespace {

struct PresentationTiming {
	uint64 sequence = 0;
	uint64 generation = 0;
	crl::time startedAt = crl::now();
	crl::time readyAt = 0;
	crl::time position = 0;
	crl::time pausedMs = 0;
	crl::time pauseSince = 0;
	std::pair<int64, int64> initialBytes = { -1, -1 };
	bool seek = false;
};

struct PlaybackState {
	crl::time startedAt = crl::now();
	crl::time lastReport = startedAt;
	std::optional<PresentationTiming> pending;
	TrackState audio;
	TrackState video;
	float64 speed = 1.;
	bool paused = false;
	bool waiting = false;
	bool hasVideo = false;
	bool presented = false;
	bool partial = false;
	crl::time framePosition = kTimeUnknown;
	crl::time stallSince = 0;
	crl::time stallMs = 0;
	crl::time maxStallMs = 0;
	crl::time maxSeekMs = 0;
	int64 stalls = 0;
	int64 seeksShown = 0;
	int64 seeksAborted = 0;
};

} // namespace

struct PlaybackDiagnostics::Impl {
	Impl(std::shared_ptr<TransferDiagnostics> transfer, bool remote)
	: transfer(std::move(transfer))
	, remote(remote)
	, timer([=] { tick(); }) {
		RefreshPlaybackDiagnosticsSettings();
	}

	[[nodiscard]] bool enabled() {
		const auto current = CaptureEpoch.load(std::memory_order_acquire);
		if (epoch != current) {
			if (state) {
				timer.cancel();
				state.reset();
			}
			epoch = current;
		}
		return (current & 1) != 0;
	}

	void ensureState(bool partial) {
		if (!state) {
			state.emplace();
			state->partial = partial;
			++capture;
			timer.callEach(1000);
			LOG((u"Video Playback: playback_begin play_id=%1 capture=%2 "
				"backend=native remote=%3 partial=%4 reader_id=%5 boost=%6"_q
				).arg(qulonglong(id))
				.arg(qulonglong(capture))
				.arg(remote ? 1 : 0)
				.arg(partial ? 1 : 0)
				.arg(qulonglong(transfer ? transfer->id() : 0))
				.arg(PlaybackDebugBoostLevel()));
		}
	}

	void closeStall(const char *reason) {
		if (!state || !state->stallSince) {
			return;
		}
		auto &s = *state;
		const auto elapsed = crl::now() - s.stallSince;
		s.stallMs += elapsed;
		s.maxStallMs = std::max(s.maxStallMs, elapsed);
		s.stallSince = 0;
		LOG((u"Video Playback: stall_end play_id=%1 capture=%2 reason=%3 "
			"wait_ms=%4 total_stalls=%5 total_stall_ms=%6"_q
			).arg(qulonglong(id))
			.arg(qulonglong(capture))
			.arg(QString::fromLatin1(reason))
			.arg(qlonglong(elapsed))
			.arg(qlonglong(s.stalls))
			.arg(qlonglong(s.stallMs)));
	}

	void complete(const char *outcome, crl::time position = kTimeUnknown) {
		if (!state || !state->pending) {
			return;
		}
		auto &s = *state;
		const auto timing = *s.pending;
		s.pending.reset();
		const auto now = crl::now();
		const auto paused = timing.pausedMs
			+ (timing.pauseSince ? now - timing.pauseSince : 0);
		const auto elapsed = now - timing.startedAt;
		const auto totals = transfer
			? transfer->byteTotals()
			: std::pair<int64, int64>{ -1, -1 };
		const auto delta = [](int64 initial, int64 current) {
			return (initial >= 0 && current >= initial) ? current - initial : -1;
		};
		const auto shown = (position != kTimeUnknown);
		if (timing.seek) {
			if (shown) {
				++s.seeksShown;
				s.maxSeekMs = std::max(s.maxSeekMs, elapsed - paused);
			} else {
				++s.seeksAborted;
			}
		}
		LOG((u"Video Playback: presentation play_id=%1 capture=%2 request=%3 "
			"seek_gen=%4 kind=%5 outcome=%6 target_ms=%7 shown_position_ms=%8 "
			"elapsed_ms=%9 ready_ms=%10 user_pause_ms=%11 active_ms=%12 "
			"downloaded_bytes=%13 read_bytes=%14"_q
			).arg(qulonglong(id))
			.arg(qulonglong(capture))
			.arg(qulonglong(timing.sequence))
			.arg(qulonglong(timing.generation))
			.arg(timing.seek ? u"seek"_q : u"start"_q)
			.arg(QString::fromLatin1(outcome))
			.arg(qlonglong(timing.position))
			.arg(qlonglong(shown ? position : -1))
			.arg(qlonglong(elapsed))
			.arg(qlonglong(timing.readyAt ? timing.readyAt - timing.startedAt : -1))
			.arg(qlonglong(paused))
			.arg(qlonglong(elapsed - paused))
			.arg(qlonglong(delta(timing.initialBytes.first, totals.first)))
			.arg(qlonglong(delta(timing.initialBytes.second, totals.second))));
	}

	void report(const char *event) {
		const auto now = crl::now();
		const auto &s = *state;
		const auto audio = Buffered(s.audio);
		const auto video = Buffered(s.video);
		const auto buffered = KnownMinimum(audio, video);
		const auto stall = s.stallSince ? now - s.stallSince : 0;
		LOG((u"Video Playback: playback_%1 play_id=%2 capture=%3 partial=%4 "
			"elapsed_ms=%5 audio_ms=%6 video_ms=%7 speed=%8 paused=%9 waiting=%10 "
			"audio_buffer_ms=%11 video_buffer_ms=%12 buffer_wall_ms=%13 "
			"pending_ms=%14 stalls=%15 stall_ms=%16 max_stall_ms=%17 "
			"seeks_shown=%18 seeks_aborted=%19 max_seek_active_ms=%20 %21"_q
			).arg(QString::fromLatin1(event))
			.arg(qulonglong(id))
			.arg(qulonglong(capture))
			.arg(s.partial ? 1 : 0)
			.arg(qlonglong(now - s.startedAt))
			.arg(qlonglong(s.audio.position == kTimeUnknown ? -1 : s.audio.position))
			.arg(qlonglong(s.video.position == kTimeUnknown ? -1 : s.video.position))
			.arg(s.speed, 0, 'f', 2)
			.arg(s.paused ? 1 : 0)
			.arg(s.waiting ? 1 : 0)
			.arg(qlonglong(audio))
			.arg(qlonglong(video))
			.arg(qlonglong(buffered >= 0 ? buffered / s.speed : -1))
			.arg(qlonglong(s.pending ? now - s.pending->startedAt : 0))
			.arg(qlonglong(s.stalls))
			.arg(qlonglong(s.stallMs + stall))
			.arg(qlonglong(std::max(s.maxStallMs, stall)))
			.arg(qlonglong(s.seeksShown))
			.arg(qlonglong(s.seeksAborted))
			.arg(qlonglong(s.maxSeekMs))
			.arg(transfer ? transfer->snapshot(now) : QString()));
	}

	void tick() {
		if (!enabled() || !state) {
			return;
		}
		const auto now = crl::now();
		const auto interval = (state->pending || state->stallSince)
			? kWaitingReportInterval
			: kReportInterval;
		if (now - state->lastReport >= interval) {
			state->lastReport = now;
			report("snapshot");
		}
	}

	const uint64 id = NextId();
	const std::shared_ptr<TransferDiagnostics> transfer;
	const bool remote = false;
	base::Timer timer;
	std::optional<PlaybackState> state;
	uint64 capture = 0;
	uint64 sequence = 0;
	uint64 epoch = 0;
	bool active = false;
};

PlaybackDiagnostics::PlaybackDiagnostics(
	std::shared_ptr<TransferDiagnostics> transfer,
	bool remote)
: _impl(std::make_unique<Impl>(std::move(transfer), remote)) {
}

PlaybackDiagnostics::~PlaybackDiagnostics() = default;

uint64 PlaybackDiagnostics::id() const {
	return _impl->id;
}

void PlaybackDiagnostics::requested(
		const PlaybackOptions &options,
		bool seek,
		crl::time startedAt) {
	const auto wasActive = _impl->active;
	_impl->active = true;
	if (!_impl->enabled()) {
		return;
	}
	_impl->ensureState(wasActive);
	_impl->closeStall("request");
	_impl->complete("superseded");
	auto &s = *_impl->state;
	s.framePosition = kTimeUnknown;
	s.audio = TrackState();
	s.video = TrackState();
	s.paused = false;
	s.waiting = false;
	s.presented = false;
	s.speed = SupportsSpeedControl() ? options.speed : 1.;
	s.pending = PresentationTiming{
		.sequence = ++_impl->sequence,
		.startedAt = startedAt,
		.position = options.seekable ? options.position : 0,
		.initialBytes = _impl->transfer
			? _impl->transfer->byteTotals()
			: std::pair<int64, int64>{ -1, -1 },
		.seek = seek,
	};
	LOG((u"Video Playback: presentation_request play_id=%1 capture=%2 "
		"request=%3 kind=%4 target_ms=%5 speed=%6"_q)
		.arg(qulonglong(id()))
		.arg(qulonglong(_impl->capture))
		.arg(qulonglong(s.pending->sequence))
		.arg(seek ? u"seek"_q : u"start"_q)
		.arg(qlonglong(s.pending->position))
		.arg(s.speed, 0, 'f', 2));
}

void PlaybackDiagnostics::generation(uint64 generation) {
	if (_impl->enabled() && _impl->state && _impl->state->pending) {
		_impl->state->pending->generation = generation;
	}
}

void PlaybackDiagnostics::ready(const Information &information) {
	if (!_impl->enabled() || !_impl->active) {
		return;
	}
	_impl->ensureState(true);
	auto &s = *_impl->state;
	s.hasVideo = !information.video.size.isEmpty();
	s.audio = information.audio.state;
	s.video = information.video.state;
	if (s.pending) {
		s.pending->readyAt = crl::now();
	}
}

void PlaybackDiagnostics::sample(
		const Information &information,
		float64 speed,
		bool pausedByUser,
		bool waiting) {
	if (!_impl->enabled() || !_impl->active) {
		return;
	}
	_impl->ensureState(true);
	auto &s = *_impl->state;
	const auto now = crl::now();
	s.audio = information.audio.state;
	s.video = information.video.state;
	if (s.paused != pausedByUser || s.speed != speed) {
		LOG((u"Video Playback: playback_control play_id=%1 capture=%2 "
			"paused=%3 speed=%4"_q).arg(qulonglong(id()))
			.arg(qulonglong(_impl->capture))
			.arg(pausedByUser ? 1 : 0)
			.arg(speed, 0, 'f', 2));
	}
	if (s.pending && pausedByUser != s.paused) {
		if (pausedByUser) {
			s.pending->pauseSince = now;
		} else if (s.pending->pauseSince) {
			s.pending->pausedMs += now - s.pending->pauseSince;
			s.pending->pauseSince = 0;
		}
	}
	s.paused = pausedByUser;
	s.speed = speed;
	s.waiting = waiting;
	if (s.presented && waiting && !pausedByUser && !s.pending) {
		if (!s.stallSince) {
			s.stallSince = now;
			++s.stalls;
			_impl->report("stall_begin");
		}
	} else {
		_impl->closeStall(pausedByUser ? "user-paused" : "ready");
	}
}

void PlaybackDiagnostics::frameDisplayed(
		crl::time position,
		bool waitForShown) {
	if (!_impl->enabled() || !_impl->active) {
		return;
	}
	_impl->ensureState(true);
	auto &s = *_impl->state;
	s.framePosition = position;
	if (!waitForShown) {
		s.presented = true;
		_impl->complete("render", position);
		s.framePosition = kTimeUnknown;
	}
}

void PlaybackDiagnostics::frameShown() {
	if (!_impl->enabled() || !_impl->state) {
		return;
	}
	auto &s = *_impl->state;
	if (s.framePosition != kTimeUnknown) {
		s.presented = true;
		_impl->complete("ui-shown", s.framePosition);
		s.framePosition = kTimeUnknown;
	}
}

void PlaybackDiagnostics::audioProgress(crl::time position) {
	if (position != kTimeUnknown
		&& _impl->enabled()
		&& _impl->state
		&& !_impl->state->hasVideo
		&& (!_impl->state->pending || _impl->state->pending->readyAt)) {
		_impl->state->presented = true;
		_impl->complete("audio-progress", position);
	}
}

void PlaybackDiagnostics::finish(const char *reason) {
	_impl->active = false;
	if (_impl->enabled() && _impl->state) {
		_impl->closeStall(reason);
		_impl->complete(reason);
		_impl->report(reason);
		_impl->state.reset();
	}
	_impl->timer.cancel();
}

struct BridgeRequestDiagnostics::Impl {
	Impl(
		std::shared_ptr<TransferDiagnostics> transfer,
		bool special,
		int64 offset,
		int64 length)
	: root(std::move(transfer))
	, reader(root)
	, special(special)
	, offset(offset)
	, length(length) {
	}

	[[nodiscard]] bool enabled() const {
		return (epoch & 1)
			&& epoch == CaptureEpoch.load(std::memory_order_acquire);
	}

	const std::shared_ptr<TransferDiagnostics> root;
	std::shared_ptr<TransferDiagnostics> reader;
	const bool special = false;
	const int64 offset = 0;
	const int64 length = 0;
	const crl::time startedAt = crl::now();
	const uint64 epoch = CaptureEpoch.load(std::memory_order_acquire);
	uint64 generation = 0;
	uint64 smartGeneration = 0;
	crl::time lastReportAt = startedAt;
	crl::time readStartedAt = 0;
	crl::time readLockedAt = 0;
	crl::time mutexMs = 0;
	crl::time fillMs = 0;
	crl::time maxFillMs = 0;
	crl::time firstChunkMs = -1;
	int64 written = 0;
	int64 backgroundRead = 0;
	int64 reportedWritten = 0;
	int64 reportedBackgroundRead = 0;
	bool firstChunkReported = false;
	const char *outcome = "setup-failed";
};

BridgeRequestDiagnostics::BridgeRequestDiagnostics(
	std::shared_ptr<TransferDiagnostics> transfer,
	bool special,
	int64 offset,
	int64 length)
: _impl(CaptureEnabled()
	? std::make_unique<Impl>(std::move(transfer), special, offset, length)
	: nullptr) {
}

BridgeRequestDiagnostics::~BridgeRequestDiagnostics() {
	report(true);
}

void BridgeRequestDiagnostics::report(bool completed) {
	if (!_impl || !_impl->enabled()) {
		return;
	}
	auto &r = *_impl;
	const auto now = crl::now();
	r.lastReportAt = now;
	auto message = QString();
	r.root->_impl->update([&](TransferState &s) {
		if (completed) {
			++s.bridgeRequests;
			++s.bridgeTotalRequests;
			const auto outcome = std::string_view(r.outcome);
			s.bridgeFailed += (outcome == "setup-failed" || outcome == "read-failed");
			s.bridgeDisconnected += (outcome == "client-disconnected");
			s.bridgeSuperseded += (outcome == "superseded");
		}
		const auto written = r.written - r.reportedWritten;
		const auto backgroundRead = r.backgroundRead - r.reportedBackgroundRead;
		s.bridgeWritten += written;
		s.bridgeTotalWritten += written;
		s.bridgeBackgroundRead += backgroundRead;
		s.bridgeTotalBackgroundRead += backgroundRead;
		r.reportedWritten = r.written;
		r.reportedBackgroundRead = r.backgroundRead;
		if (!r.firstChunkReported && r.firstChunkMs >= 0) {
			s.bridgeMaxFirstChunkMs = std::max(s.bridgeMaxFirstChunkMs, r.firstChunkMs);
			r.firstChunkReported = true;
		}
		const auto interval = (r.fillMs >= 1000 || r.mutexMs >= 1000)
			? kWaitingReportInterval
			: kReportInterval;
		if (s.lastBridgeReport && now - s.lastBridgeReport < interval) {
			return;
		}
		s.lastBridgeReport = now;
		message = (u"Video Playback: bridge_summary play_id=%1 reader_id=%2 "
			"backend=%3 seek_gen=%4 requested_offset=%5 requested_length=%6 outcome=%7 "
			"request_ms=%8 first_chunk_ms=%9 mutex_ms=%10 fill_ms=%11 max_fill_ms=%12 "
			"written_bytes=%13 background_read_bytes=%14 window_requests=%15 "
			"window_written_bytes=%16 window_background_read_bytes=%17 "
			"window_max_first_chunk_ms=%18 smart_gen=%19 completed=%20"_q
		).arg(qulonglong(r.root->id()))
			.arg(qulonglong(r.reader->id()))
			.arg(r.special ? u"mpv-special"_q : u"mpv"_q)
			.arg(qulonglong(r.generation))
			.arg(qlonglong(r.offset))
			.arg(qlonglong(r.length))
			.arg(QString::fromLatin1(r.outcome))
			.arg(qlonglong(now - r.startedAt))
			.arg(qlonglong(r.firstChunkMs))
			.arg(qlonglong(r.mutexMs))
			.arg(qlonglong(r.fillMs))
			.arg(qlonglong(r.maxFillMs))
			.arg(qlonglong(r.written))
			.arg(qlonglong(r.backgroundRead))
			.arg(qlonglong(s.bridgeRequests))
			.arg(qlonglong(s.bridgeWritten))
			.arg(qlonglong(s.bridgeBackgroundRead))
			.arg(qlonglong(s.bridgeMaxFirstChunkMs))
			.arg(qulonglong(r.smartGeneration))
			.arg(completed ? 1 : 0);
		s.bridgeRequests = 0;
		s.bridgeWritten = 0;
		s.bridgeBackgroundRead = 0;
		s.bridgeMaxFirstChunkMs = -1;
	});
	if (!message.isEmpty()) {
		LOG((message));
		r.root->report("bridge");
		if (r.reader != r.root) {
			r.reader->report("bridge-reader");
		}
	}
}

void BridgeRequestDiagnostics::useReader(
		std::shared_ptr<TransferDiagnostics> transfer,
		uint64 generation,
		uint64 smartGeneration) {
	if (_impl && _impl->enabled()) {
		_impl->reader = std::move(transfer);
		_impl->generation = generation;
		_impl->smartGeneration = smartGeneration;
		_impl->reader->_impl->bridgePlayId.store(
			_impl->root->id(),
			std::memory_order_relaxed);
	}
}

void BridgeRequestDiagnostics::readStarted() {
	if (_impl && _impl->enabled()) {
		_impl->readStartedAt = crl::now();
	}
}

void BridgeRequestDiagnostics::readLocked() {
	if (_impl && _impl->enabled()) {
		_impl->readLockedAt = crl::now();
		_impl->mutexMs += _impl->readLockedAt - _impl->readStartedAt;
	}
}

void BridgeRequestDiagnostics::readFinished() {
	if (_impl && _impl->enabled() && _impl->readLockedAt) {
		const auto elapsed = crl::now() - _impl->readLockedAt;
		_impl->fillMs += elapsed;
		_impl->maxFillMs = std::max(_impl->maxFillMs, elapsed);
		_impl->readLockedAt = 0;
	}
}

void BridgeRequestDiagnostics::wrote(int64 bytes) {
	if (_impl && _impl->enabled()) {
		const auto now = crl::now();
		_impl->written += bytes;
		_impl->outcome = "streaming";
		if (_impl->firstChunkMs < 0) {
			_impl->firstChunkMs = now - _impl->startedAt;
			report(false);
		} else if (now - _impl->lastReportAt >= kReportInterval) {
			report(false);
		}
	}
}

void BridgeRequestDiagnostics::backgroundRead(int64 bytes) {
	if (_impl && _impl->enabled()) {
		_impl->backgroundRead += bytes;
	}
}

void BridgeRequestDiagnostics::outcome(const char *reason) {
	if (_impl && _impl->enabled()) {
		_impl->outcome = reason;
	}
}

} // namespace Media::Streaming
