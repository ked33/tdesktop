/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_loader_mtproto.h"

#include "apiwrap.h"
#include "main/main_session.h"
#include "storage/streamed_file_downloader.h"
#include "storage/cache/storage_cache_types.h"

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

Storage::Cache::Key LoaderMtproto::baseCacheKey() const {
	return v::get<StorageFileLocation>(
		location().data
	).bigFileBaseCacheKey();
}

int64 LoaderMtproto::size() const {
	return _size;
}

void LoaderMtproto::load(int64 offset) {
	crl::on_main(this, [=] {
		if (_downloader) {
			auto bytes = _downloader->readLoadedPart(offset);
			if (!bytes.isEmpty()) {
				cancelForOffset(offset);
				_parts.fire({ offset, std::move(bytes) });
				return;
			}
		}
		if (haveSentRequestForOffset(offset)) {
			return;
		} else if (_requested.add(offset)) {
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
		cancelAllRequests();
		_requested.clear();
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
			return;
		} else if (_requested.remove(offset)) {
			_parts.fire({
				.offset = offset,
				.cancelled = true,
			});
		}
	});
}

void LoaderMtproto::cancelForOffset(int64 offset) {
	if (haveSentRequestForOffset(offset)) {
		cancelRequestForOffset(offset);
		if (!_requested.empty()) {
			addToQueueWithPriority();
		}
	} else {
		_requested.remove(offset);
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
	_stats.push_back({ .start = crl::now(), .offset = *offset });

	Ensures(offset.has_value());
	return *offset;
}

bool LoaderMtproto::feedPart(int64 offset, const QByteArray &bytes) {
	const auto time = crl::now();
	for (auto &entry : _stats) {
		if (entry.offset == offset && entry.start < time) {
			entry.end = time;
			if (!_statsTimer.isActive()) {
				const auto checkAt = std::max(
					time + kCheckStatsInterval,
					_firstRequestStart + kInitialStatsWait);
				_statsTimer.callOnce(checkAt - time);
			}
			break;
		}
	}
	_parts.fire({ offset, bytes });
	return true;
}

void LoaderMtproto::cancelOnFail() {
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
	{ // Erase all stats entries that are too old.
		for (auto i = begin(_stats); i != end(_stats);) {
			if (i->start >= from) {
				break;
			} else if (i->end && i->end < from) {
				i = _stats.erase(i);
			} else {
				++i;
			}
		}
	}
	if (_stats.empty()) {
		return;
	}
	// Count duration for which at least one request was in progress.
	// This is the time we should consider for download speed.
	// We don't count time when no requests were in progress.
	auto durationCountedTill = _stats.front().start;
	auto duration = crl::time(0);
	auto received = int64(0);
	auto latencyTotal = int64(0);
	auto latencyCount = 0;
	for (const auto &entry : _stats) {
		if (entry.start > durationCountedTill) {
			durationCountedTill = entry.start;
		}
		const auto till = entry.end ? entry.end : time;
		if (till > durationCountedTill) {
			duration += (till - durationCountedTill);
			durationCountedTill = till;
		}
		if (entry.end) {
			received += Storage::kDownloadPartSize;
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
			if (entry.end) {
				const auto sample = entry.end - entry.start;
				jitterTotal += (sample >= latency)
					? (sample - latency)
					: (latency - sample);
			}
		}
		_speedEstimate.fire({
			.bytesPerSecond = int(std::clamp(
				int64(received * 1000 / duration),
				int64(0),
				int64(64 * 1024 * 1024))),
			.latencyMs = int(std::clamp(
				latency,
				crl::time(0),
				crl::time(std::numeric_limits<int>::max()))),
			.jitterMs = latencyCount
				? int(std::clamp(
					crl::time(jitterTotal / latencyCount),
					crl::time(0),
					crl::time(std::numeric_limits<int>::max())))
				: 0,
			.unreliable = (received < 3 * Storage::kDownloadPartSize),
		});
	}
}

} // namespace Streaming
} // namespace Media
