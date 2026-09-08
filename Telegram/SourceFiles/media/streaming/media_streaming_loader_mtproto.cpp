/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_loader_mtproto.h"

#include "apiwrap.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_debug.h"
#include "media/streaming/media_streaming_diagnostics.h"
#include "settings.h"
#include "storage/cache/storage_cache_types.h"
#include "storage/streamed_file_downloader.h"

#include <algorithm>
#include <limits>

namespace Media {
namespace Streaming {
namespace {

constexpr auto kCheckStatsInterval = crl::time(1000);
constexpr auto kInitialStatsWait = 5 * crl::time(1000);

} // namespace

LoaderMtproto::LoaderMtproto(
	not_null<Storage::DownloadManagerMtproto*> owner,
	const StorageFileLocation &location,
	int64 size,
	Data::FileOrigin origin)
: DownloadMtprotoTask(owner, location, origin)
, _owner(owner)
, _size(size)
, _smartRequestLimit(owner->nonPremiumRequestLimit(location.dcId()))
, _api(&api().instance())
, _statsTimer([=] { checkStats(); }) {
	const auto dc = dcId();
	owner->nonPremiumDelayUpdates(
	) | rpl::filter([=](const auto &entry) {
		return (entry.first == dc);
	}) | rpl::on_next([=](const auto &entry) {
		const auto state = nonPremiumDelayState();
		_serverDelays.fire({
			.waitMs = entry.second.appliedWaitMs,
			.dcId = int(dc),
			.limitedUntil = state.limitedUntil,
			.recoveryUntil = state.recoveryUntil,
			.penalty = state.penalty,
		});
	}, _lifetime);
	owner->nonPremiumRequestLimitUpdates(
	) | rpl::filter([=](const auto &entry) {
		return (entry.first == dc);
	}) | rpl::on_next([=](const auto &entry) {
		_smartRequestLimit.store(entry.second, std::memory_order_relaxed);
	}, _lifetime);
}

LoaderMtproto::~LoaderMtproto() {
	if (_diagnostics) {
		_diagnostics->cancelAll();
		_diagnostics->report("loader-destroyed");
	}
}

Storage::Cache::Key LoaderMtproto::baseCacheKey() const {
	return v::get<StorageFileLocation>(
		location().data
	).bigFileBaseCacheKey();
}

int64 LoaderMtproto::size() const {
	return _size;
}

void LoaderMtproto::setDiagnostics(
		std::shared_ptr<TransferDiagnostics> diagnostics) {
	_diagnostics = std::move(diagnostics);
}

void LoaderMtproto::setStreamingReadRange(int64 offset, int64 amount) {
	const auto lock = std::lock_guard(_readStallMutex);
	_readStall.setRead(offset, amount, crl::now());
}

void LoaderMtproto::load(int64 offset) {
	crl::on_main(this, [=] {
		if (_downloader) {
			auto bytes = _downloader->readLoadedPart(offset);
			if (!bytes.isEmpty()) {
				cancelForOffset(offset);
				if (_diagnostics) {
					_diagnostics->reused(bytes.size());
				}
				_parts.fire({ offset, std::move(bytes) });
				return;
			}
		}
		if (haveSentRequestForOffset(offset)) {
			return;
		} else if (_requested.add(offset)) {
			if (_diagnostics) {
				_diagnostics->queued(offset);
			}
			addToQueueWithPriority();
		}
	});
}

void LoaderMtproto::addToQueueWithPriority() {
	addToQueue(_priority);
}

void LoaderMtproto::stop() {
	crl::on_main(this, [=] {
		_smartBufferPressureGeneration.fetch_add(
			1,
			std::memory_order_release);
		_smartPlaybackRateGeneration.fetch_add(
			1,
			std::memory_order_release);
		_smartBufferPressure.store(false, std::memory_order_release);
		_smartPlaybackRate.store(0, std::memory_order_release);
		_owner->setSmartStreamingBufferPressure(this, false);
		_owner->setSmartStreamingPlaybackRate(this, 0);
		if (_diagnostics) {
			_diagnostics->cancelAll();
		}
		cancelAllRequests();
		_requested.clear();
		clearStats();
		removeFromQueue();
	});
}

void LoaderMtproto::tryRemoveFromQueue() {
	crl::on_main(this, [=] {
		if (_requested.empty() && !haveSentRequests()) {
			removeFromQueue();
		}
	});
}

void LoaderMtproto::cancel(int64 offset) {
	crl::on_main(this, [=] {
		cancelForOffset(offset);
	});
}

void LoaderMtproto::cancelForSeek(int64 offset) {
	crl::on_main(this, [=] {
		if (haveSentRequestForOffset(offset)) {
			if (_diagnostics) {
				_diagnostics->retainedForSeek(offset);
			}
			return;
		} else if (_requested.remove(offset)) {
			if (_diagnostics) {
				_diagnostics->cancelled(offset, false);
			}
			_parts.fire({
				.offset = offset,
				.cancelled = true,
			});
		}
	});
}

void LoaderMtproto::cancelForOffset(int64 offset) {
	if (haveSentRequestForOffset(offset)) {
		finishStats(offset, 0);
		if (_diagnostics) {
			_diagnostics->cancelled(offset, true);
		}
		cancelRequestForOffset(offset);
		if (!_requested.empty()) {
			addToQueueWithPriority();
		}
	} else if (_requested.remove(offset)) {
		if (_diagnostics) {
			_diagnostics->cancelled(offset, false);
		}
	}
}

void LoaderMtproto::attachDownloader(
		not_null<Storage::StreamedFileDownloader*> downloader) {
	_downloader = downloader;
}

void LoaderMtproto::clearAttachedDownloader() {
	_downloader = nullptr;
}

void LoaderMtproto::resetPriorities() {
	crl::on_main(this, [=] {
		_requested.resetPriorities();
	});
}

void LoaderMtproto::setPriority(int priority) {
	if (_priority == priority) {
		return;
	}
	_priority = priority;
	if (haveSentRequests()) {
		addToQueueWithPriority();
	}
}

bool LoaderMtproto::readyToRequest() const {
	return !_requested.empty();
}

int64 LoaderMtproto::takeNextRequestOffset() {
	const auto offset = _requested.take();
	Assert(offset.has_value());

	const auto time = crl::now();
	if (!_firstRequestStart) {
		_firstRequestStart = time;
	}
	_stats.push_back({ .start = time, .offset = *offset });
	if (!_statsTimer.isActive()) {
		_statsTimer.callOnce(kCheckStatsInterval);
	}
	if (_diagnostics) {
		_diagnostics->dispatched(*offset);
	}

	Ensures(offset.has_value());
	return *offset;
}

bool LoaderMtproto::feedPart(int64 offset, const QByteArray &bytes) {
	if (!bytes.isEmpty()) {
		_lastStatsProgress = crl::now();
		const auto lock = std::lock_guard(_readStallMutex);
		_readStall.progress(offset, bytes.size(), _lastStatsProgress);
	}
	if (_diagnostics) {
		_diagnostics->received(offset, bytes.size());
	}
	finishStats(offset, bytes.size());
	_parts.fire({ offset, bytes });
	return true;
}

void LoaderMtproto::finishStats(int64 offset, int64 received) {
	const auto time = crl::now();
	for (auto &entry : _stats) {
		if (entry.offset == offset && !entry.end) {
			entry.end = std::max(time, entry.start + 1);
			entry.received = received;
			if (!_statsTimer.isActive()) {
				_statsTimer.callOnce(kCheckStatsInterval);
			}
			break;
		}
	}
}

void LoaderMtproto::clearStats() {
	_statsTimer.cancel();
	_stats.clear();
	_firstRequestStart = 0;
	_lastStatsProgress = 0;
	_retryLatencyMs = 0;
	_retryJitterMs = 0;
	setStreamingReadRange(-1, 0);
}

void LoaderMtproto::cancelOnFail() {
	clearStats();
	_parts.fire({ LoadedPart::kFailedOffset });
}

rpl::producer<LoadedPart> LoaderMtproto::parts() const {
	return _parts.events();
}

rpl::producer<SpeedEstimate> LoaderMtproto::speedEstimate() const {
	return _speedEstimate.events();
}

rpl::producer<ServerDelay> LoaderMtproto::serverDelays() const {
	return _serverDelays.events();
}

ServerDelay LoaderMtproto::serverDelayState() const {
	const auto state = nonPremiumDelayState();
	return {
		.dcId = int(dcId()),
		.limitedUntil = state.limitedUntil,
		.recoveryUntil = state.recoveryUntil,
		.penalty = state.penalty,
	};
}

bool LoaderMtproto::premiumSession() const {
	return api().session().premium();
}

void LoaderMtproto::setSmartStreamingBufferPressure(bool pressure) {
	if (_smartBufferPressure.exchange(
			pressure,
			std::memory_order_acq_rel) == pressure) {
		return;
	}
	const auto generation = _smartBufferPressureGeneration.fetch_add(
		1,
		std::memory_order_acq_rel) + 1;
	crl::on_main(this, [=] {
		if (_smartBufferPressureGeneration.load(std::memory_order_acquire)
				!= generation
			|| _smartBufferPressure.load(std::memory_order_acquire)
				!= pressure) {
			return;
		}
		_owner->setSmartStreamingBufferPressure(this, pressure);
	});
}

void LoaderMtproto::setSmartStreamingPlaybackRate(int bytesPerSecond) {
	bytesPerSecond = std::max(bytesPerSecond, 0);
	if (_smartPlaybackRate.exchange(
			bytesPerSecond,
			std::memory_order_acq_rel) == bytesPerSecond) {
		return;
	}
	const auto generation = _smartPlaybackRateGeneration.fetch_add(
		1,
		std::memory_order_acq_rel) + 1;
	crl::on_main(this, [=] {
		if (_smartPlaybackRateGeneration.load(std::memory_order_acquire)
				!= generation
			|| _smartPlaybackRate.load(std::memory_order_acquire)
				!= bytesPerSecond) {
			return;
		}
		_owner->setSmartStreamingPlaybackRate(this, bytesPerSecond);
	});
}

void LoaderMtproto::notifySmartStreamingSeek() {
	crl::on_main(this, [=] {
		_owner->notifySmartStreamingSeek(this);
	});
}

int LoaderMtproto::smartStreamingRequestLimit() const {
	return _smartRequestLimit.load(std::memory_order_relaxed);
}

int LoaderMtproto::smartStreamingPlaybackRate() const {
	return _smartPlaybackRate.load(std::memory_order_relaxed);
}

void LoaderMtproto::checkStats() {
	const auto time = crl::now();
	const auto from = time - kInitialStatsWait;
	std::erase_if(_stats, [=](const StatsEntry &entry) {
		return entry.end && entry.end <= from;
	});
	if (_stats.empty()) {
		_speedEstimate.fire({ .unreliable = true });
		return;
	}
	_statsTimer.callOnce(kCheckStatsInterval);
	// Count duration for which at least one request was in progress.
	// This is the time we should consider for download speed.
	// We don't count time when no requests were in progress.
	auto durationCountedTill = from;
	auto duration = crl::time(0);
	auto received = int64(0);
	auto latencyTotal = int64(0);
	auto latencyCount = 0;
	auto activeSince = time;
	for (const auto &entry : _stats) {
		if (!entry.end) {
			activeSince = std::min(activeSince, entry.start);
		}
		if (entry.start > durationCountedTill) {
			durationCountedTill = entry.start;
		}
		const auto till = entry.end ? std::min(entry.end, time) : time;
		if (till > durationCountedTill) {
			duration += (till - durationCountedTill);
			durationCountedTill = till;
		}
		if (entry.received > 0) {
			received += entry.received;
			latencyTotal += std::max(entry.end - entry.start, crl::time(1));
			++latencyCount;
		}
	}
	if (duration) {
		const auto latency = latencyCount
			? crl::time(latencyTotal / latencyCount)
			: crl::time(0);
		auto jitterTotal = int64(0);
		for (const auto &entry : _stats) {
			if (entry.received > 0) {
				const auto sample = entry.end - entry.start;
				jitterTotal += (sample >= latency)
					? (sample - latency)
					: (latency - sample);
			}
		}
		const auto noProgress = time - std::max(activeSince, _lastStatsProgress);
		const auto readStalled = [&] {
			const auto lock = std::lock_guard(_readStallMutex);
			return _readStall.waitingFor(time) >= ReadStallPolicy::kStallTimeout;
		}();
		const auto jitter = latencyCount
			? int(std::clamp(
				crl::time(jitterTotal / latencyCount),
				crl::time(0),
				crl::time(std::numeric_limits<int>::max())))
			: 0;
		const auto latencyMs = int(std::clamp(
			latency,
			crl::time(0),
			crl::time(std::numeric_limits<int>::max())));
		if (latencyCount) {
			_retryLatencyMs = latencyMs;
			_retryJitterMs = jitter;
		}
		checkReadRetry(time, _retryLatencyMs, _retryJitterMs);
		_speedEstimate.fire({
			.bytesPerSecond = int(std::clamp(
				int64(received * 1000 / duration),
				int64(0),
				int64(64 * 1024 * 1024))),
			.latencyMs = latencyMs,
			.jitterMs = jitter,
			.noProgressMs = int(std::clamp(
				noProgress,
				crl::time(0),
				crl::time(std::numeric_limits<int>::max()))),
			.unreliable = received < 3 * Storage::kDownloadPartSize
				|| time < _firstRequestStart + kInitialStatsWait
				|| readStalled
				|| noProgress >= ReadStallPolicy::kStallTimeout,
			.stalled = readStalled || noProgress >= ReadStallPolicy::kStallTimeout,
		});
	}
}

void LoaderMtproto::checkReadRetry(crl::time now, int latencyMs, int jitterMs) {
	if (GetEnhancedInt(u"net_download_speed_boost"_q) != 6 || premiumSession()) {
		return;
	}
	const auto lock = std::lock_guard(_readStallMutex);
	const auto i = std::find_if(
		_stats.begin(),
		_stats.end(),
		[&](const auto &entry) {
			return !entry.end && _readStall.contains(entry.offset, kPartSize);
		});
	if (i == _stats.end()
		|| !_readStall.retryReady(now, i->start, latencyMs, jitterMs)) {
		return;
	}
	const auto offset = i->offset;
	const auto requestAge = now - i->start;
	if (!retryRequestForOffset(
			offset,
			ReadStallPolicy::RetryDelay(latencyMs, jitterMs))) {
		return;
	}
	finishStats(offset, 0);
	_stats.push_back({ .start = now, .offset = offset });
	_readStall.retried(now);
	if (_diagnostics) {
		_diagnostics->cancelled(offset, true);
		_diagnostics->dispatched(offset);
	}
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: critical read retry "
		"offset=%1 requestMs=%2 readWaitMs=%3 reader_id=%4.")
		.arg(qlonglong(offset))
		.arg(qlonglong(requestAge))
		.arg(qlonglong(_readStall.waitingFor(now)))
		.arg(qulonglong(_diagnostics ? _diagnostics->id() : 0)));
}

} // namespace Streaming
} // namespace Media
