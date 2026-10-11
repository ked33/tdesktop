/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/download_manager_mtproto.h"

#include "apiwrap.h"
#include "base/openssl_help.h"
#include "core/enhanced_settings.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_boost.h"
#include "mtproto/facade.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/mtproto_response.h"
#include "settings.h"
#include "ui/image/image_location.h"

#include <algorithm>

namespace Storage {
namespace {

constexpr auto kKillSessionTimeout = 15 * crl::time(1000);
constexpr auto kThumbnailServerFailureLimit = 3;
constexpr auto kMaxTrackedSessionRemoves = 64;
constexpr auto kRetryAddSessionTimeout = 8 * crl::time(1000);
constexpr auto kRetryAddSessionSuccesses = 3;
constexpr auto kMaxTrackedSuccesses = kRetryAddSessionSuccesses
	* kMaxTrackedSessionRemoves;
constexpr auto kRemoveSessionAfterTimeouts = 4;
constexpr auto kResetDownloadPrioritiesTimeout = crl::time(200);
constexpr auto kBadRequestDurationThreshold = 8 * crl::time(1000);
constexpr auto kNonPremiumDelayCoalesceTolerance = crl::time(1500);
constexpr auto kSmartSampleBusyDuration = 5 * crl::time(1000);
constexpr auto kSmartRateLogInterval = 5 * crl::time(1000);
constexpr auto kSmartSampleMaximumRequests = 64;
constexpr auto kSmartMeasurementMaxAge = 2 * 60 * crl::time(1000);
constexpr auto kSmartLimitChangeCooldown = 30 * crl::time(1000);
constexpr auto kSmartPressureDuration = 3 * crl::time(1000);
constexpr auto kSmartSeekFreezeDuration = 15 * crl::time(1000);
constexpr auto kSmartProbeDuration = 15 * crl::time(1000);
constexpr auto kSmartExcessCapacityRatio = 2.25;
// High-bitrate (~1MB/s+) streams need more headroom before cutting
// concurrency; brief catch-up bursts otherwise thrash 9↔8 and drain buffer.
constexpr auto kSmartHighBitrateExcessCapacityRatio = 3.5;
constexpr auto kSmartPostPressureHoldDuration = 20 * crl::time(1000);
constexpr auto kSmartHighLatencyCapacityRatio = 1.5;
constexpr auto kSmartHighLatencyThreshold = 3 * crl::time(1000);
constexpr auto kSmartProbeMaximumLatency = 4 * crl::time(1000);
constexpr auto kSmartProbeMinimumGain = 1.08;
constexpr auto kSmartProbeMaximumLatencyRatio = 1.35;
constexpr auto kSmartMaximumMeasuredThroughput = 64 * 1024 * 1024;

[[nodiscard]] PeerId AvatarPeerId(const MTPInputPeer &peer, UserId self) {
	return peer.match([&](const MTPDinputPeerSelf &) {
		return peerFromUser(self);
	}, [](const MTPDinputPeerUser &data) {
		return peerFromUser(data.vuser_id());
	}, [](const MTPDinputPeerChat &data) {
		return peerFromChat(data.vchat_id());
	}, [](const MTPDinputPeerChannel &data) {
		return peerFromChannel(data.vchannel_id());
	}, [](const MTPDinputPeerUserFromMessage &data) {
		return peerFromUser(data.vuser_id());
	}, [](const MTPDinputPeerChannelFromMessage &data) {
		return peerFromChannel(data.vchannel_id());
	}, [](const MTPDinputPeerEmpty &) {
		return PeerId();
	});
}

[[nodiscard]] QString AvatarPeerType(const Data::Session &data, PeerId id) {
	if (!id) {
		return u"unknown"_q;
	} else if (peerIsUser(id)) {
		if (const auto user = data.userLoaded(peerToUser(id))) {
			return user->isBot()
				? u"bot"_q
				: user->isContact() ? u"contact"_q : u"user"_q;
		}
		return u"user_unknown"_q;
	} else if (peerIsChat(id)) {
		return u"group"_q;
	} else if (peerIsChannel(id)) {
		if (const auto channel = data.channelLoaded(peerToChannel(id))) {
			if (channel->isMegagroup()) {
				return u"supergroup"_q;
			} else if (channel->isBroadcast()) {
				return u"channel"_q;
			}
		}
		return u"channel_unknown"_q;
	}
	return u"unknown"_q;
}

[[nodiscard]] QString SmartRequestLimitReasonString(
		NonPremiumRequestLimitReason reason) {
	switch (reason) {
	case NonPremiumRequestLimitReason::ManualRateTarget:
		return u"manual_rate_target"_q;
	case NonPremiumRequestLimitReason::BufferPressure:
		return u"buffer_pressure"_q;
	case NonPremiumRequestLimitReason::ExcessCapacity:
		return u"excess_capacity"_q;
	case NonPremiumRequestLimitReason::ProbeNoGain:
		return u"probe_no_gain"_q;
	case NonPremiumRequestLimitReason::HighLatency:
		return u"high_latency"_q;
	case NonPremiumRequestLimitReason::ServerLimit:
		return u"server_limit"_q;
	}
	Unexpected("NonPremiumRequestLimitReason value.");
}

[[nodiscard]] bool SmartPlaybackDebugLogsEnabled() {
	return GetEnhancedBool("online_playback_debug_logs")
		|| GetEnhancedBool("mpv_streaming_debug_logs");
}

[[nodiscard]] int DownloadBoostLevel() {
	const auto boost = GetEnhancedInt("net_download_speed_boost");
	return std::clamp(boost, 0, 6);
}

[[nodiscard]] const Media::Streaming::BoostProfile &SmartProfile() {
	return Media::Streaming::BoostProfileFor(6);
}

[[nodiscard]] int StartWaitedInSession() {
	return Media::Streaming::BoostProfileFor(
		DownloadBoostLevel()).startWaitedParts * kDownloadPartSize;
}

[[nodiscard]] int MaxWaitedInSession() {
	return Media::Streaming::BoostProfileFor(
		DownloadBoostLevel()).maxWaitedParts * kDownloadPartSize;
}

[[nodiscard]] int StartSessionsCount() {
	return Media::Streaming::BoostProfileFor(
		DownloadBoostLevel()).startSessions;
}

[[nodiscard]] int MaxSessionsCount() {
	return Media::Streaming::BoostProfileFor(
		DownloadBoostLevel()).maxSessions;
}

// Each (session remove by timeouts) we wait for time:
// kRetryAddSessionTimeout * max(removesCount, kMaxTrackedSessionRemoves)
// and for successes in all remaining sessions:
// kRetryAddSessionSuccesses * max(removesCount, kMaxTrackedSessionRemoves)

} // namespace

void DownloadManagerMtproto::Queue::enqueue(
		not_null<Task*> task,
		int priority) {
	const auto position = ranges::find_if(_tasks, [&](const Enqueued &task) {
		return task.priority <= priority;
	}) - begin(_tasks);
	const auto now = ranges::find(_tasks, task, &Enqueued::task);
	const auto i = [&] {
		if (now != end(_tasks)) {
			(now->priority = priority);
			return now;
		}
		_tasks.push_back({ task, priority });
		return end(_tasks) - 1;
	}();
	const auto j = begin(_tasks) + position;
	if (j < i) {
		std::rotate(j, i, i + 1);
	} else if (j > i + 1) {
		std::rotate(i, i + 1, j);
	}
}

void DownloadManagerMtproto::Queue::remove(not_null<Task*> task) {
	_tasks.erase(ranges::remove(_tasks, task, &Enqueued::task), end(_tasks));
}

void DownloadManagerMtproto::Queue::resetGeneration() {
	const auto from = ranges::find(_tasks, 0, &Enqueued::priority);
	for (auto &task : ranges::make_subrange(from, end(_tasks))) {
		if (task.priority) {
			Assert(task.priority == -1);
			break;
		}
		task.priority = -1;
	}
}

bool DownloadManagerMtproto::Queue::empty() const {
	return _tasks.empty();
}

auto DownloadManagerMtproto::Queue::nextTask(
		bool onlyHighestPriority,
		bool allowPreviews) const
-> Task* {
	const auto allowed = [&](const Enqueued &enqueued) {
		return allowPreviews || !enqueued.task->isMediaPreview();
	};
	const auto from = ranges::find_if(_tasks, allowed);
	if (from == end(_tasks)) {
		return nullptr;
	}
	const auto highestPriority = from->priority;
	const auto notHighestPriority = [&](const Enqueued &enqueued) {
		return (enqueued.priority != highestPriority);
	};
	const auto till = (onlyHighestPriority && highestPriority > 0)
		? ranges::find_if(
			ranges::make_subrange(from, end(_tasks)),
			notHighestPriority)
		: end(_tasks);
	const auto readyToRequest = [&](const Enqueued &enqueued) {
		return allowed(enqueued) && enqueued.task->readyToRequest();
	};
	const auto first = ranges::find_if(
		ranges::make_subrange(from, till),
		readyToRequest);
	return (first != till) ? first->task.get() : nullptr;
}

void DownloadManagerMtproto::Queue::removeSession(int index) {
	for (const auto &enqueued : _tasks) {
		enqueued.task->removeSession(index);
	}
}

QString DownloadManagerMtproto::Queue::diagnosticSnapshot(crl::time now) const {
	const auto top = _tasks.empty() ? 0 : _tasks.front().priority;
	auto ready = 0;
	auto topReady = 0;
	for (const auto &entry : _tasks) {
		if (entry.task->readyToRequest()) {
			++ready;
			if (entry.priority == top) {
				++topReady;
			}
		}
	}
	return u"tasks=%1 ready=%2 top_priority=%3 top_ready=%4 head={%5}"_q
		.arg(_tasks.size()).arg(ready).arg(top).arg(topReady)
		.arg(_tasks.empty()
			? QString()
			: _tasks.front().task->diagnosticSnapshot(now));
}

QString DownloadManagerMtproto::diagnosticSnapshot(int dcId) const {
	const auto now = crl::now();
	const auto balance = _balanceData.find(dcId);
	const auto queue = _queues.find(dcId);
	const auto deferred = _deferredTasks.find(dcId);
	const auto delay = nonPremiumDelayState(dcId);
	const auto requested = (balance == _balanceData.end())
		? 0
		: balance->second.totalRequested;
	const auto previewLimits = EnhancedSettings::MediaPreviewDownloadLimits();
	const auto resolvedPreviewLimits = previewLimits.resolved();
	auto previews = (balance == _balanceData.end())
		? MediaPreviewRequestLimiter()
		: balance->second.previews;
	previews.configure(previewLimits, now);
	const auto previewWait = previews.delay(now);
	const auto allowPreviews = previews.hasCapacity() && !previewWait;
	const auto hasDeferred = (deferred != _deferredTasks.end())
		&& !deferred->second.empty();
	const auto deferredReady = hasDeferred
		&& ranges::any_of(deferred->second, [&](const auto task) {
			return allowPreviews || !task->isMediaPreview();
		});
	const auto previewBlocked = !allowPreviews
		&& ((hasDeferred && !deferredReady)
			|| (queue != _queues.end()
				&& queue->second.nextTask(false)
				&& !queue->second.nextTask(false, false)));
	auto capacity = 0;
	auto slots = 0;
	if (balance != _balanceData.end()) {
		for (const auto &session : balance->second.sessions) {
			capacity += std::max(0, session.maxWaitedAmount - session.requested);
			slots += (session.requested + kDownloadPartSize <= session.maxWaitedAmount);
		}
	}
	auto limit = -1;
	if (smartNonPremiumEnabled()) {
		limit = nonPremiumRequestLimit(dcId);
		if (delay.recoveryUntil > now) {
			const auto &profile = SmartProfile();
			limit = std::min(limit, NonPremiumRequestLimit(
				delay,
				now,
				profile.smartInitialRequestLimit,
				profile.smartMinimumRequestLimit,
				profile.smartMaximumRequestLimit));
		}
	}
	const auto reason = (now < delay.limitedUntil)
		? "server_wait"
		: (limit >= 0 && requested + kDownloadPartSize > limit * kDownloadPartSize)
		? "smart_limit"
		: !slots
		? "session_capacity"
		: deferredReady
		? "deferred_pending"
		: (queue != _queues.end()
			&& queue->second.nextTask(requested > 0, allowPreviews))
		? "sendable"
		: previewBlocked
		? "preview_limit"
		: (queue != _queues.end() && queue->second.nextTask(false, allowPreviews))
		? "priority"
		: "no_ready_task";
	return u"requested_bytes=%1 free_bytes=%2 deferred_tasks=%3 "
		"limit_wait_ms=%4 recovery_ms=%5 smart_limit=%6 %7 "
		"gate=%8 rate_timer_ms=%9 preview_active=%10 preview_limit=%11 "
		"preview_rps=%12 preview_burst=%13 preview_wait_ms=%14"_q
		.arg(requested).arg(capacity)
		.arg(deferred == _deferredTasks.end() ? 0 : deferred->second.size())
		.arg(qlonglong(std::max(delay.limitedUntil - now, crl::time(0))))
		.arg(qlonglong(std::max(delay.recoveryUntil - now, crl::time(0))))
		.arg(limit)
		.arg(queue == _queues.end()
			? u"tasks=0"_q
			: queue->second.diagnosticSnapshot(now))
		.arg(reason)
		.arg(qlonglong(_downloadRateTimer.isActive()
			? _downloadRateTimer.remainingTime()
			: -1))
		.arg(previews.active())
		.arg(resolvedPreviewLimits.concurrent)
		.arg(resolvedPreviewLimits.requestsPerSecond)
		.arg(resolvedPreviewLimits.burst)
		.arg(qlonglong(previewWait));
}

DownloadManagerMtproto::DcSessionBalanceData::DcSessionBalanceData()
: maxWaitedAmount(StartWaitedInSession()) {
}

DownloadManagerMtproto::DcBalanceData::DcBalanceData()
: sessions(StartSessionsCount()) {
}

DownloadManagerMtproto::DownloadManagerMtproto(not_null<ApiWrap*> api)
: _api(api)
, _diagnostics([=](int dc) { return diagnosticSnapshot(dc); })
, _nonPremiumDelayTimer([=] { checkNonPremiumDelayState(); })
, _downloadRateTimer([=] { checkSendNext(); })
, _resetGenerationTimer([=] { resetGeneration(); })
, _killSessionsTimer([=] { killSessions(); }) {
	EnhancedSettings::MediaPreviewDownloadLimitsChanges(
	) | rpl::on_next([=] {
		scheduleDownloadCheck(1);
	}, _lifetime);
	_api->instance().restartsByTimeout(
	) | rpl::filter([](MTP::ShiftedDcId shiftedDcId) {
		return MTP::isDownloadDcId(shiftedDcId);
	}) | rpl::on_next([=](MTP::ShiftedDcId shiftedDcId) {
		sessionTimedOut(
			MTP::BareDcId(shiftedDcId),
			MTP::GetDcIdShift(shiftedDcId));
	}, _lifetime);
}

DownloadManagerMtproto::~DownloadManagerMtproto() {
	killSessions();
}

bool DownloadManagerMtproto::smartNonPremiumEnabled() const {
	return (DownloadBoostLevel() == 6) && !_api->session().premium();
}

auto DownloadManagerMtproto::smartRequestState(
	MTP::DcId dcId,
	crl::time now)
-> SmartRequestState & {
	auto &state = _smartRequestStates[dcId];
	if (!state.created) {
		state.target = SmartProfile().smartInitialRequestLimit;
		state.created = now;
		state.lastChange = now;
		state.sampleLastUpdate = now;
	}
	return state;
}

auto DownloadManagerMtproto::smartDemandSummary(MTP::DcId dcId) const
-> SmartDemandSummary {
	auto result = SmartDemandSummary();
	for (const auto &[task, demand] : _smartStreamingDemands) {
		if (demand.dcId != dcId || !demand.active) {
			continue;
		}
		result.streaming = true;
		result.readyRequests = result.readyRequests || task->readyToRequest();
		result.readWaiting = result.readWaiting
			|| (demand.readWaiting && task->readyToRequest());
		result.pacingBytesPerSecond = int(std::min(
			int64(result.pacingBytesPerSecond) + demand.pacingBytesPerSecond,
			int64(std::numeric_limits<int>::max())));
		const auto playback = int64(result.playbackBytesPerSecond)
			+ demand.playbackBytesPerSecond;
		result.playbackBytesPerSecond = int(std::min(
			playback,
			int64(std::numeric_limits<int>::max())));
		result.seekUntil = std::max(result.seekUntil, demand.seekUntil);
		if (!demand.bufferPressure) {
			continue;
		}
		result.bufferPressure = true;
		if (!result.pressureSince
			|| demand.pressureSince < result.pressureSince) {
			result.pressureSince = demand.pressureSince;
		}
	}
	return result;
}

void DownloadManagerMtproto::setSmartStreamingActive(
		not_null<Task*> task,
		bool active) {
	auto &demand = _smartStreamingDemands[task];
	demand.dcId = task->dcId();
	if (demand.active != active) {
		scheduleDownloadCheck(1);
	}
	demand.active = active;
}

void DownloadManagerMtproto::setSmartStreamingReadWaiting(
		not_null<Task*> task,
		bool waiting) {
	if (!smartNonPremiumEnabled() || !SmartProfile().adaptivePacingEnabled()) {
		return;
	}
	auto &demand = _smartStreamingDemands[task];
	demand.dcId = task->dcId();
	if (demand.readWaiting != waiting) {
		demand.readWaiting = waiting;
		scheduleDownloadCheck(1);
	}
}

void DownloadManagerMtproto::setSmartStreamingBufferPressure(
		not_null<Task*> task,
		bool pressure) {
	const auto now = crl::now();
	auto &demand = _smartStreamingDemands[task];
	demand.dcId = task->dcId();
	if (demand.bufferPressure == pressure) {
		return;
	}
	demand.bufferPressure = pressure;
	demand.pressureSince = pressure ? now : 0;
	if (smartNonPremiumEnabled()) {
		if (SmartProfile().adaptivePacingEnabled()) {
			scheduleDownloadCheck(1);
		}
		evaluateSmartRequestLimit(demand.dcId, now);
	}
}

void DownloadManagerMtproto::setSmartStreamingPlaybackRate(
		not_null<Task*> task,
		int bytesPerSecond) {
	const auto now = crl::now();
	auto &demand = _smartStreamingDemands[task];
	demand.dcId = task->dcId();
	demand.playbackBytesPerSecond = std::max(bytesPerSecond, 0);
	if (bytesPerSecond > 0) {
		demand.pacingBytesPerSecond = bytesPerSecond;
	}
	if (smartNonPremiumEnabled()) {
		evaluateSmartRequestLimit(demand.dcId, now);
	}
}

void DownloadManagerMtproto::notifySmartStreamingSeek(
		not_null<Task*> task) {
	const auto now = crl::now();
	auto &demand = _smartStreamingDemands[task];
	demand.dcId = task->dcId();
	demand.seekUntil = std::max(
		demand.seekUntil,
		now + kSmartSeekFreezeDuration);
	if (smartNonPremiumEnabled()) {
		auto &state = smartRequestState(demand.dcId, now);
		state.sampleLastUpdate = now;
		state.sampleBusyDuration = 0;
		state.sampleBytes = 0;
		state.sampleLatency = 0;
		state.sampleRequests = 0;
	}
}

int DownloadManagerMtproto::nonPremiumRequestLimit(MTP::DcId dcId) const {
	const auto i = _smartRequestStates.find(dcId);
	return (i == end(_smartRequestStates))
		? SmartProfile().smartInitialRequestLimit
		: i->second.target;
}

void DownloadManagerMtproto::trackSmartRequestActivity(
		MTP::DcId dcId,
		crl::time now) {
	if (!smartNonPremiumEnabled()) {
		const auto i = _smartRequestStates.find(dcId);
		if (i != end(_smartRequestStates)) {
			i->second.sampleLastUpdate = 0;
			i->second.sampleBusyDuration = 0;
			i->second.sampleBytes = 0;
			i->second.sampleLatency = 0;
			i->second.sampleRequests = 0;
		}
		return;
	}
	auto &state = smartRequestState(dcId, now);
	if (state.sampleLastUpdate
		&& now > state.sampleLastUpdate + kSmartMeasurementMaxAge) {
		state.sampleBusyDuration = 0;
		state.sampleBytes = 0;
		state.sampleLatency = 0;
		state.sampleRequests = 0;
		state.throughputEma = 0.;
		state.latencyEma = 0.;
		state.lastThroughput = 0.;
		state.lastLatency = 0.;
		state.probeActive = false;
		state.probePreviousTarget = 0;
	}
	const auto i = _balanceData.find(dcId);
	const auto busy = (i != end(_balanceData))
		&& (i->second.totalRequested > 0);
	if (state.sampleLastUpdate && busy) {
		state.sampleBusyDuration += now - state.sampleLastUpdate;
	}
	state.sampleLastUpdate = now;
}

void DownloadManagerMtproto::recordSmartRequestSuccess(
		MTP::DcId dcId,
		crl::time duration,
		crl::time now) {
	if (!smartNonPremiumEnabled()) {
		return;
	}
	auto &state = smartRequestState(dcId, now);
	state.sampleBytes += kDownloadPartSize;
	state.sampleLatency += std::max(duration, crl::time(1));
	++state.sampleRequests;
	if (state.sampleBusyDuration < kSmartSampleBusyDuration
		&& state.sampleRequests < kSmartSampleMaximumRequests) {
		return;
	} else if (!state.sampleBusyDuration || !state.sampleRequests) {
		return;
	}
	const auto throughput = std::min(
		double(state.sampleBytes) * 1000.
			/ double(state.sampleBusyDuration),
		double(kSmartMaximumMeasuredThroughput));
	const auto latency = double(state.sampleLatency)
		/ double(state.sampleRequests);
	constexpr auto kAlpha = 0.4;
	state.lastThroughput = throughput;
	state.lastLatency = latency;
	state.throughputEma = state.throughputEma
		? (state.throughputEma * (1. - kAlpha)) + (throughput * kAlpha)
		: throughput;
	state.latencyEma = state.latencyEma
		? (state.latencyEma * (1. - kAlpha)) + (latency * kAlpha)
		: latency;
	state.sampleLastUpdate = now;
	state.sampleBusyDuration = 0;
	state.sampleBytes = 0;
	state.sampleLatency = 0;
	state.sampleRequests = 0;
	evaluateSmartRequestLimit(dcId, now);
}

void DownloadManagerMtproto::evaluateSmartRequestLimit(
		MTP::DcId dcId,
		crl::time now) {
	if (!smartNonPremiumEnabled()) {
		return;
	}
	auto &state = smartRequestState(dcId, now);
	const auto delay = nonPremiumDelayState(dcId);
	if (now < delay.recoveryUntil) {
		return;
	}
	const auto demand = smartDemandSummary(dcId);
	const auto &profile = SmartProfile();
	const auto manual = profile.manualPacingEnabled();
	const auto targetRate = manual
		? DownloadRateLimiter::Target(0, profile.smartDownloadMaxKiBps, false, true)
		: demand.playbackBytesPerSecond;
	const auto needsCapacity = manual
		? (demand.readyRequests && state.throughputEma < targetRate * 0.95)
		: demand.bufferPressure;
	if (manual && needsCapacity) {
		if (!state.belowManualTargetSince) {
			state.belowManualTargetSince = now;
		}
	} else {
		state.belowManualTargetSince = 0;
	}
	if ((manual && !demand.readyRequests)
		|| (!manual && !demand.bufferPressure && !targetRate)) {
		state.probeActive = false;
		state.probePreviousTarget = 0;
		return;
	}
	if (state.probeActive) {
		if (now < state.probeStarted + kSmartProbeDuration) {
			return;
		}
		const auto noGain = state.probeThroughput > 0.
			&& state.lastThroughput
				< state.probeThroughput * kSmartProbeMinimumGain;
		const auto latencyWorse = state.probeLatency > 0.
			&& state.lastLatency
				> state.probeLatency * kSmartProbeMaximumLatencyRatio;
		const auto previous = state.probePreviousTarget;
		state.probeActive = false;
		state.probePreviousTarget = 0;
		if (latencyWorse || (needsCapacity && noGain)) {
			updateSmartRequestLimit(
				dcId,
				previous,
				NonPremiumRequestLimitReason::ProbeNoGain,
				now);
		}
		return;
	}
	if (now < state.lastChange + kSmartLimitChangeCooldown) {
		return;
	}
	const auto playback = double(targetRate);
	if (demand.bufferPressure) {
		state.lastPressureAt = now;
	}
	if (!needsCapacity) {
		if (now < demand.seekUntil) {
			return;
		}
		// After pressure clears, hold concurrency so high-bitrate catch-up
		// is not immediately undone by a single high-throughput sample.
		if (state.lastPressureAt
			&& now < state.lastPressureAt + kSmartPostPressureHoldDuration) {
			return;
		}
		const auto excessRatio
			= Media::Streaming::IsHighBitratePlaybackRate(
				demand.playbackBytesPerSecond)
			? kSmartHighBitrateExcessCapacityRatio
			: kSmartExcessCapacityRatio;
		if (state.target > profile.smartCapacityMinimumRequestLimit
			&& playback > 0.
			&& state.throughputEma > playback * excessRatio) {
			updateSmartRequestLimit(
				dcId,
				state.target - 1,
				NonPremiumRequestLimitReason::ExcessCapacity,
				now);
		} else if (state.target > profile.smartCapacityMinimumRequestLimit
			&& playback > 0.
			&& state.latencyEma > kSmartHighLatencyThreshold
			&& state.throughputEma
				> playback * kSmartHighLatencyCapacityRatio) {
			updateSmartRequestLimit(
				dcId,
				state.target - 1,
				NonPremiumRequestLimitReason::HighLatency,
				now);
		}
		return;
	}
	const auto pressureSince = manual
		? state.belowManualTargetSince
		: demand.pressureSince;
	if (!pressureSince
		|| now < state.pacedAt + kSmartSampleBusyDuration
		|| now < pressureSince + kSmartPressureDuration
		|| state.target >= profile.smartMaximumRequestLimit
		|| (state.lastLatency > 0.
			&& state.lastLatency > kSmartProbeMaximumLatency)) {
		return;
	}
	state.probeActive = true;
	state.probePreviousTarget = state.target;
	state.probeStarted = now;
	state.probeThroughput = state.throughputEma;
	state.probeLatency = state.latencyEma;
	updateSmartRequestLimit(
		dcId,
		state.target + 1,
		manual
			? NonPremiumRequestLimitReason::ManualRateTarget
			: NonPremiumRequestLimitReason::BufferPressure,
		now);
}

void DownloadManagerMtproto::updateSmartRequestLimit(
		MTP::DcId dcId,
		int target,
		NonPremiumRequestLimitReason reason,
		crl::time now) {
	auto &state = smartRequestState(dcId, now);
	const auto &profile = SmartProfile();
	target = std::clamp(
		target,
		profile.smartMinimumRequestLimit,
		profile.smartMaximumRequestLimit);
	if (state.target == target) {
		return;
	}
	const auto previous = state.target;
	state.target = target;
	state.lastChange = now;
	_nonPremiumRequestLimitUpdates.fire_copy({ dcId, target });
	if (SmartPlaybackDebugLogsEnabled()) {
		const auto demand = smartDemandSummary(dcId);
		LOG(("Video Playback: smart target dc=%1 previous=%2 target=%3 "
			"reason=%4 throughput=%5 playback=%6 latency=%7 "
			"pressure=%8 seekFrozen=%9.")
			.arg(dcId)
			.arg(previous)
			.arg(target)
			.arg(SmartRequestLimitReasonString(reason))
			.arg(int(std::clamp(
				state.throughputEma,
				0.,
				double(std::numeric_limits<int>::max()))))
			.arg(demand.playbackBytesPerSecond)
			.arg(int(std::clamp(
				state.latencyEma,
				0.,
				double(std::numeric_limits<int>::max()))))
			.arg(demand.bufferPressure ? 1 : 0)
			.arg(demand.seekUntil > now ? 1 : 0));
	}
	if (target > previous) {
		const auto i = _queues.find(dcId);
		if (i != end(_queues)) {
			checkSendNext(dcId, i->second);
		}
	}
}

void DownloadManagerMtproto::applySmartServerLimit(
		MTP::DcId dcId,
		crl::time now) {
	if (!smartNonPremiumEnabled()) {
		return;
	}
	auto &state = smartRequestState(dcId, now);
	state.probeActive = false;
	state.probePreviousTarget = 0;
	const auto target = std::max(
		SmartProfile().smartMinimumRequestLimit,
		(state.target + 1) / 2);
	updateSmartRequestLimit(
		dcId,
		target,
		NonPremiumRequestLimitReason::ServerLimit,
		now);
	state.lastChange = now;
	state.sampleLastUpdate = now;
	state.sampleBusyDuration = 0;
	state.sampleBytes = 0;
	state.sampleLatency = 0;
	state.sampleRequests = 0;
	state.throughputEma = 0.;
	state.latencyEma = 0.;
	state.lastThroughput = 0.;
	state.lastLatency = 0.;
}

void DownloadManagerMtproto::notifyNonPremiumDelay(
		MTP::DcId dcId,
		DocumentId id,
		NonPremiumDelayInfo info) {
	const auto now = crl::now();
	auto &state = _nonPremiumDelayStates[dcId];
	const auto limitedUntil = now + std::max(info.appliedWaitMs, 1000);
	const auto newWindow = (state.limitedUntil <= now);
	const auto shouldNotify = newWindow
		|| (limitedUntil
			> state.limitedUntil + kNonPremiumDelayCoalesceTolerance);
	if (newWindow) {
		state.penalty = (state.recoveryUntil > now)
			? std::min(std::max(state.penalty, 1) + 1, 3)
			: 1;
		applySmartServerLimit(dcId, now);
	}
	state.limitedUntil = std::max(
		state.limitedUntil,
		limitedUntil);
	state.recoveryUntil = std::max(
		state.recoveryUntil,
		state.limitedUntil + NonPremiumRecoveryDuration(state.penalty));
	const auto balance = _balanceData.find(dcId);
	if (balance != end(_balanceData)) {
		balance->second.previews.suspend(state.limitedUntil, state.recoveryUntil);
	}
	if (smartNonPremiumEnabled()) {
		auto &limiter = smartRequestState(dcId, now).rateLimiter;
		if (limiter.rate() > 0 && newWindow) {
			limiter.penalize(state.limitedUntil, state.recoveryUntil);
		} else if (limiter.rate() > 0) {
			limiter.suspend(state.limitedUntil, state.recoveryUntil);
		}
	}
	scheduleNonPremiumDelayCheck();
	if (!shouldNotify) {
		return;
	}
	_nonPremiumDelayUpdates.fire_copy({ dcId, info });
	if (id) {
		_nonPremiumDelays.fire_copy({ id, info });
	}
}

void DownloadManagerMtproto::scheduleNonPremiumDelayCheck() {
	_nonPremiumDelayTimer.cancel();
	const auto now = crl::now();
	auto next = crl::time(0);
	for (const auto &[dcId, state] : _nonPremiumDelayStates) {
		(void)dcId;
		const auto changeAt = NonPremiumNextStateChange(state, now);
		if (changeAt && (!next || changeAt < next)) {
			next = changeAt;
		}
	}
	if (next) {
		_nonPremiumDelayTimer.callOnce(std::max(
			next - now,
			crl::time(1)));
	}
}

void DownloadManagerMtproto::checkNonPremiumDelayState() {
	_nonPremiumDelayTimer.cancel();
	checkSendNext();
	scheduleNonPremiumDelayCheck();
}

void DownloadManagerMtproto::notifyThumbnailRetry(
		const ThumbnailRetryInfo &info) {
	_thumbnailRetries.fire_copy(info);
}

void DownloadManagerMtproto::enqueue(not_null<Task*> task, int priority) {
	const auto dcId = task->dcId();
	auto &queue = _queues[dcId];
	queue.enqueue(task, priority);
	if (!_resetGenerationTimer.isActive()) {
		_resetGenerationTimer.callOnce(kResetDownloadPrioritiesTimeout);
	}
	checkSendNext(dcId, queue);
}

void DownloadManagerMtproto::remove(not_null<Task*> task) {
	const auto dcId = task->dcId();
	auto &queue = _queues[dcId];
	queue.remove(task);
	if (smartNonPremiumEnabled()) {
		evaluateSmartRequestLimit(dcId, crl::now());
	}
	checkSendNext(dcId, queue);
}

void DownloadManagerMtproto::removeStreamingDemand(not_null<Task*> task) {
	_smartStreamingDemands.remove(task);
}

void DownloadManagerMtproto::deferRequest(not_null<Task*> task) {
	const auto dcId = task->dcId();
	_deferredTasks[dcId].emplace(task);
	scheduleDownloadCheck(1);
}

void DownloadManagerMtproto::forgetDeferredRequests(not_null<Task*> task) {
	const auto i = _deferredTasks.find(task->dcId());
	if (i != end(_deferredTasks)) {
		i->second.remove(task);
	}
}

void DownloadManagerMtproto::scheduleDownloadCheck(crl::time delay) {
	if (!_downloadRateTimer.isActive()
		|| _downloadRateTimer.remainingTime() > delay) {
		_downloadRateTimer.callOnce(delay, Qt::PreciseTimer);
	}
}

crl::time DownloadManagerMtproto::downloadRateDelay(
		MTP::DcId dcId,
		crl::time now) {
	if (!smartNonPremiumEnabled()) {
		return 0;
	}
	const auto demand = smartDemandSummary(dcId);
	if (!demand.streaming) {
		return 0;
	}
	auto &state = smartRequestState(dcId, now);
	const auto &profile = SmartProfile();
	auto &limiter = state.rateLimiter;
	const auto adaptive = profile.adaptivePacingEnabled();
	const auto catchUp = adaptive
		&& (demand.bufferPressure || demand.readWaiting);
	limiter.configure(
		DownloadRateLimiter::Target(
			demand.pacingBytesPerSecond,
			profile.smartDownloadMaxKiBps,
			false,
			profile.manualPacingEnabled(),
			profile.smartDownloadTargetPercent),
		std::int64_t(profile.smartDownloadBurstParts) * kDownloadPartSize,
		now,
		adaptive ? DownloadRateLimiter::Target(
			demand.pacingBytesPerSecond,
			profile.smartDownloadMaxKiBps,
			true,
			false,
			profile.smartDownloadTargetPercent) : 0,
		catchUp);
	const auto delay = crl::time(limiter.delay(kDownloadPartSize, now));
	if (delay > 0) {
		state.pacedAt = now;
		scheduleDownloadCheck(delay);
	}
	if (SmartPlaybackDebugLogsEnabled()
		&& (!state.rateLogAt || now >= state.rateLogAt + kSmartRateLogInterval)) {
		state.rateLogAt = now;
		LOG(("Video Playback: download pacing dc=%1 rateBps=%2 "
			"ceilingBps=%3 burstParts=%4 delayMs=%5 playbackBps=%6 "
			"adaptive=%7 catchUp=%8 pressure=%9 readWaiting=%10 "
			"manual=%11 targetPercent=%12.")
			.arg(dcId)
			.arg(limiter.rate())
			.arg(limiter.ceiling())
			.arg(profile.smartDownloadBurstParts)
			.arg(delay)
			.arg(demand.playbackBytesPerSecond)
			.arg(adaptive)
			.arg(limiter.catchingUp())
			.arg(demand.bufferPressure)
			.arg(demand.readWaiting)
			.arg(profile.manualPacingEnabled())
			.arg(profile.smartDownloadTargetPercent));
	}
	return delay;
}

void DownloadManagerMtproto::resetGeneration() {
	_resetGenerationTimer.cancel();
	for (auto &[dcId, queue] : _queues) {
		queue.resetGeneration();
	}
}

void DownloadManagerMtproto::checkSendNext() {
	_downloadRateTimer.cancel();
	for (auto &[dcId, queue] : _queues) {
		if (queue.empty() && _deferredTasks[dcId].empty()) {
			continue;
		}
		checkSendNext(dcId, queue);
	}
}

void DownloadManagerMtproto::checkSendNext(MTP::DcId dcId, Queue &queue) {
	while (trySendNextPart(dcId, queue)) {
	}
}

void DownloadManagerMtproto::checkSendNextAfterSuccess(MTP::DcId dcId) {
	checkSendNext(dcId, _queues[dcId]);
}

bool DownloadManagerMtproto::trySendNextPart(MTP::DcId dcId, Queue &queue) {
	auto &balanceData = _balanceData[dcId];
	const auto delay = nonPremiumDelayState(dcId);
	const auto now = crl::now();
	if (now < delay.limitedUntil) {
		return false;
	}
	if (DownloadBoostLevel() == 6 && !_api->session().premium()) {
		const auto target = nonPremiumRequestLimit(dcId);
		const auto &profile = SmartProfile();
		const auto requestLimit = (delay.recoveryUntil > now)
			? std::min(target, NonPremiumRequestLimit(
				delay,
				now,
				profile.smartInitialRequestLimit,
				profile.smartMinimumRequestLimit,
				profile.smartMaximumRequestLimit))
			: target;
		const auto requested = balanceData.totalRequested
			+ kDownloadPartSize;
		if (requested > requestLimit * kDownloadPartSize) {
			return false;
		}
	}
	const auto &sessions = balanceData.sessions;
	const auto bestIndex = [&] {
		const auto proj = [](const DcSessionBalanceData &data) {
			return (data.requested < data.maxWaitedAmount)
				? data.requested
				: MaxWaitedInSession();
		};
		const auto j = ranges::min_element(sessions, ranges::less(), proj);
		return (j->requested + kDownloadPartSize <= j->maxWaitedAmount)
			? (j - begin(sessions))
			: -1;
	}();
	if (bestIndex < 0) {
		return false;
	}
	auto &previews = balanceData.previews;
	previews.configure(EnhancedSettings::MediaPreviewDownloadLimits(), now);
	const auto previewDelay = previews.delay(now);
	const auto allowPreviews = previews.hasCapacity() && !previewDelay;
	const auto onlyHighestPriority = (balanceData.totalRequested > 0);
	const auto &deferred = _deferredTasks[dcId];
	const auto firstDeferred = ranges::find_if(deferred, [&](const auto task) {
		return allowPreviews || !task->isMediaPreview();
	});
	const auto task = (firstDeferred != end(deferred))
		? firstDeferred->get()
		: queue.nextTask(onlyHighestPriority, allowPreviews);
	if (previewDelay > 0 && previews.hasCapacity()) {
		const auto waiting = queue.nextTask(false);
		const auto deferredPreview = ranges::any_of(deferred, [](const auto task) {
			return task->isMediaPreview();
		});
		if ((waiting && waiting->isMediaPreview()) || deferredPreview) {
			scheduleDownloadCheck(previewDelay);
		}
	}
	if (task) {
		const auto preview = task->isMediaPreview();
		if (!preview && downloadRateDelay(dcId, now) > 0) {
			return false;
		}
		if (!preview
			&& smartNonPremiumEnabled()
			&& smartDemandSummary(dcId).streaming) {
			smartRequestState(dcId, now).rateLimiter.consume(
				kDownloadPartSize,
				now);
		}
		task->loadPart(bestIndex);
		return true;
	}
	return false;
}

int DownloadManagerMtproto::changeRequestedAmount(
		MTP::DcId dcId,
		int index,
		int delta,
		bool preview) {
	const auto i = _balanceData.find(dcId);
	Assert(i != _balanceData.end());
	Assert(index < i->second.sessions.size());
	trackSmartRequestActivity(dcId, crl::now());
	const auto result = (i->second.sessions[index].requested += delta);
	i->second.totalRequested += delta;
	if (preview) {
		if (delta > 0) {
			i->second.previews.requestStarted(crl::now());
		} else {
			i->second.previews.requestFinished();
		}
	}
	const auto findNonEmptySession = [](const DcBalanceData &data) {
		using namespace rpl::mappers;
		return ranges::find_if(
			data.sessions,
			_1 > 0,
			&DcSessionBalanceData::requested);
	};
	if (delta > 0) {
		killSessionsCancel(dcId);
	} else if (findNonEmptySession(i->second) == end(i->second.sessions)) {
		killSessionsSchedule(dcId);
	}
	return result;
}

void DownloadManagerMtproto::requestSucceeded(
		MTP::DcId dcId,
		int index,
		int amountAtRequestStart,
		crl::time timeAtRequestStart) {
	using namespace rpl::mappers;

	const auto i = _balanceData.find(dcId);
	Assert(i != end(_balanceData));
	auto &dc = i->second;
	Assert(index < dc.sessions.size());
	auto &data = dc.sessions[index];
	const auto overloaded = (timeAtRequestStart <= dc.lastSessionRemove)
		|| (amountAtRequestStart > data.maxWaitedAmount);
	const auto parts = amountAtRequestStart / kDownloadPartSize;
	const auto now = crl::now();
	const auto duration = (now - timeAtRequestStart);
	DEBUG_LOG(("Download (%1,%2) request done, duration: %3, parts: %4%5"
		).arg(dcId
		).arg(index
		).arg(duration
		).arg(parts
		).arg(overloaded ? " (overloaded)" : ""));
	if (overloaded) {
		return;
	}
	recordSmartRequestSuccess(dcId, duration, now);

	if (duration >= kBadRequestDurationThreshold) {
		DEBUG_LOG(("Duration too large, signaling time out."));
		crl::on_main(this, [=] {
			sessionTimedOut(dcId, index);
		});
		return;
	}
	if (amountAtRequestStart == data.maxWaitedAmount
		&& data.maxWaitedAmount < MaxWaitedInSession()) {
		data.maxWaitedAmount = std::min(
			data.maxWaitedAmount + kDownloadPartSize,
			MaxWaitedInSession());
		DEBUG_LOG(("Download (%1,%2) increased max waited amount %3."
			).arg(dcId
			).arg(index
			).arg(data.maxWaitedAmount));
	}
	data.successes = std::min(data.successes + 1, kMaxTrackedSuccesses);
	const auto notEnough = ranges::any_of(
		dc.sessions,
		_1 < (dc.sessionRemoveTimes + 1) * kRetryAddSessionSuccesses,
		&DcSessionBalanceData::successes);
	if (notEnough) {
		return;
	}
	for (auto &session : dc.sessions) {
		session.successes = 0;
	}
	if (dc.timeouts > 0) {
		--dc.timeouts;
		return;
	} else if (dc.sessions.size() == MaxSessionsCount()) {
		return;
	}
	const auto delay = (dc.sessionRemoveTimes + 1) * kRetryAddSessionTimeout;
	if (dc.lastSessionRemove && now < dc.lastSessionRemove + delay) {
		return;
	}
	dc.sessions.emplace_back();
	DEBUG_LOG(("Download (%1,%2) adding, now sessions: %3"
		).arg(dcId
		).arg(dc.sessions.size() - 1
		).arg(dc.sessions.size()));
}

int DownloadManagerMtproto::chooseSessionIndex(MTP::DcId dcId) const {
	const auto i = _balanceData.find(dcId);
	Assert(i != end(_balanceData));
	const auto &sessions = i->second.sessions;
	const auto j = ranges::min_element(
		sessions,
		ranges::less(),
		&DcSessionBalanceData::requested);
	return (j - begin(sessions));
}

bool DownloadManagerMtproto::sessionHasCapacity(
		MTP::DcId dcId,
		int index) const {
	const auto i = _balanceData.find(dcId);
	if (i == end(_balanceData)
		|| index < 0
		|| index >= int(i->second.sessions.size())) {
		return false;
	}
	const auto &session = i->second.sessions[index];
	return session.requested + kDownloadPartSize <= session.maxWaitedAmount;
}

auto DownloadManagerMtproto::chooseAlternativeSessionIndex(
		MTP::DcId dcId,
		int currentIndex) const
-> std::optional<int> {
	const auto i = _balanceData.find(dcId);
	if (i == end(_balanceData)) {
		return std::nullopt;
	}
	const auto &sessions = i->second.sessions;
	auto result = std::optional<int>();
	for (auto index = 0; index != int(sessions.size()); ++index) {
		const auto &data = sessions[index];
		if (index == currentIndex
			|| data.requested + kDownloadPartSize > data.maxWaitedAmount) {
			continue;
		}
		if (!result || data.requested < sessions[*result].requested) {
			result = index;
		}
	}
	return result;
}

void DownloadManagerMtproto::sessionTimedOut(MTP::DcId dcId, int index) {
	const auto i = _balanceData.find(dcId);
	if (i == end(_balanceData)) {
		return;
	}
	auto &dc = i->second;
	if (index >= dc.sessions.size()) {
		return;
	}
	DEBUG_LOG(("Download (%1,%2) session timed-out.").arg(dcId).arg(index));
	for (auto &session : dc.sessions) {
		session.successes = 0;
	}
	if (dc.sessions.size() == StartSessionsCount()
		|| ++dc.timeouts < kRemoveSessionAfterTimeouts) {
		return;
	}
	dc.timeouts = 0;
	removeSession(dcId);
}

void DownloadManagerMtproto::removeSession(MTP::DcId dcId) {
	auto &dc = _balanceData[dcId];
	Assert(dc.sessions.size() > StartSessionsCount());
	const auto index = int(dc.sessions.size() - 1);
	DEBUG_LOG(("Download (%1,%2) removing, now sessions: %3"
		).arg(dcId
		).arg(index
		).arg(index));
	auto &queue = _queues[dcId];
	if (dc.sessionRemoveIndex == index) {
		dc.sessionRemoveTimes = std::min(
			dc.sessionRemoveTimes + 1,
			kMaxTrackedSessionRemoves);
	} else {
		dc.sessionRemoveIndex = index;
		dc.sessionRemoveTimes = 1;
	}
	auto &session = dc.sessions.back();

	// Make sure we don't send anything to that session while redirecting.
	session.requested += MaxWaitedInSession() * MaxSessionsCount();
	queue.removeSession(index);
	Assert(session.requested == MaxWaitedInSession() * MaxSessionsCount());

	dc.sessions.pop_back();
	api().instance().killSession(MTP::downloadDcId(dcId, index));

	dc.lastSessionRemove = crl::now();
}

void DownloadManagerMtproto::killSessionsSchedule(MTP::DcId dcId) {
	if (!_killSessionsWhen.contains(dcId)) {
		_killSessionsWhen.emplace(dcId, crl::now() + kKillSessionTimeout);
	}
	if (!_killSessionsTimer.isActive()) {
		_killSessionsTimer.callOnce(kKillSessionTimeout + 5);
	}
}

void DownloadManagerMtproto::killSessionsCancel(MTP::DcId dcId) {
	_killSessionsWhen.erase(dcId);
	if (_killSessionsWhen.empty()) {
		_killSessionsTimer.cancel();
	}
}

void DownloadManagerMtproto::killSessions() {
	const auto now = crl::now();
	auto left = kKillSessionTimeout;
	for (auto i = begin(_killSessionsWhen); i != end(_killSessionsWhen); ) {
		if (i->second <= now) {
			killSessions(i->first);
			i = _killSessionsWhen.erase(i);
		} else {
			if (i->second - now < left) {
				left = i->second - now;
			}
			++i;
		}
	}
	if (!_killSessionsWhen.empty()) {
		_killSessionsTimer.callOnce(left);
	}
}

void DownloadManagerMtproto::killSessions(MTP::DcId dcId) {
	const auto i = _balanceData.find(dcId);
	if (i != end(_balanceData)) {
		auto &dc = i->second;
		Assert(dc.totalRequested == 0);
		auto sessions = base::take(dc.sessions);
		const auto previews = dc.previews;
		dc = DcBalanceData();
		dc.previews = previews;
		for (auto j = 0; j != int(sessions.size()); ++j) {
			Assert(sessions[j].requested == 0);
			sessions[j] = DcSessionBalanceData();
			api().instance().stopSession(MTP::downloadDcId(dcId, j));
		}
		dc.sessions = base::take(sessions);
	}
}

DownloadMtprotoTask::DownloadMtprotoTask(
	not_null<DownloadManagerMtproto*> owner,
	const StorageFileLocation &location,
	Data::FileOrigin origin)
: _owner(owner)
, _dcId(location.dcId())
, _location({ location })
, _origin(origin) {
}

DownloadMtprotoTask::DownloadMtprotoTask(
	not_null<DownloadManagerMtproto*> owner,
	MTP::DcId dcId,
	const Location &location)
: _owner(owner)
, _dcId(dcId)
, _location(location) {
}

DownloadMtprotoTask::~DownloadMtprotoTask() {
	if (_downloadTrace) {
		_downloadTrace->finish(DownloadTrace::Stage::Cancelled);
	}
	cancelAllRequests();
	_owner->removeStreamingDemand(this);
	_owner->remove(this);
}

std::shared_ptr<DownloadTrace> DownloadMtprotoTask::downloadTrace() {
	if (!_downloadTrace) {
		_downloadTrace = _owner->diagnostics().create();
		_downloadTrace->dc = dcId();
		_downloadTrace->streaming = downloadSource().startsWith(u"streaming"_q);
		if (const auto location = std::get_if<StorageFileLocation>(&_location.data)) {
			using Type = StorageFileLocation::Type;
			switch (location->type()) {
			case Type::Photo: _downloadTrace->kind = u"photo"_q; break;
			case Type::PeerPhoto: _downloadTrace->kind = u"avatar"_q; break;
			case Type::Document:
				_downloadTrace->kind = location->isDocumentThumbnail()
					? u"document_thumbnail"_q
					: u"document"_q;
				break;
			case Type::StickerSetThumb:
				_downloadTrace->kind = u"sticker_set_thumbnail"_q;
				break;
			default: break;
			}
		}
	}
	return _downloadTrace;
}

QString DownloadMtprotoTask::diagnosticSnapshot(crl::time now) const {
	return _downloadTrace ? _downloadTrace->snapshot(now) : u"untracked"_q;
}

void DownloadMtprotoTask::updateDownloadTrace() {
	if (!_downloadTrace) {
		return;
	}
	_downloadTrace->pending = int(_sentRequests.size());
	_downloadTrace->deferred = int(_deferredRequests.size());
	_downloadTrace->oldestSent = 0;
	_downloadTrace->referenceAt = 0;
	for (const auto &[id, request] : _sentRequests) {
		if (!_downloadTrace->oldestSent || request.sent < _downloadTrace->oldestSent) {
			_downloadTrace->oldestSent = request.sent;
		}
		if (request.referenceAt && (!_downloadTrace->referenceAt
			|| request.referenceAt < _downloadTrace->referenceAt)) {
			_downloadTrace->referenceAt = request.referenceAt;
		}
	}
}

MTP::DcId DownloadMtprotoTask::dcId() const {
	return _dcId;
}

Data::FileOrigin DownloadMtprotoTask::fileOrigin() const {
	return _origin;
}

uint64 DownloadMtprotoTask::objectId() const {
	if (const auto v = std::get_if<StorageFileLocation>(&_location.data)) {
		return v->objectId();
	}
	return 0;
}

const DownloadMtprotoTask::Location &DownloadMtprotoTask::location() const {
	return _location;
}

bool DownloadMtprotoTask::isMediaPreview() const {
	const auto location = std::get_if<StorageFileLocation>(&_location.data);
	return location
		&& (location->type() == StorageFileLocation::Type::Photo
			|| location->isDocumentThumbnail());
}

void DownloadMtprotoTask::refreshFileReferenceFrom(
		const Data::UpdatedFileReferences &updates,
		int requestId,
		const QByteArray &current) {
	if (const auto v = std::get_if<StorageFileLocation>(&_location.data)) {
		v->refreshFileReference(updates);
		if (v->fileReference() == current) {
			if (_downloadTrace) {
				_downloadTrace->setError(u"reference_unchanged"_q);
				_downloadTrace->finish(DownloadTrace::Stage::Failed);
			}
			cancelOnFail();
			return;
		}
	} else {
		cancelOnFail();
		return;
	}
	if (_sentRequests.contains(requestId)) {
		if (_downloadTrace) {
			++_downloadTrace->refreshed;
			if (_downloadTrace->referenceAt) {
				_downloadTrace->referenceMs = crl::now() - _downloadTrace->referenceAt;
			}
			_downloadTrace->setStage(DownloadTrace::Stage::Queued);
		}
		makeRequest(finishSentRequest(
			requestId,
			FinishRequestReason::Redirect));
	}
}

void DownloadMtprotoTask::loadPart(int sessionIndex) {
	auto request = RequestData();
	if (_deferredRequests.empty()) {
		request = { takeNextRequestOffset(), sessionIndex };
	} else {
		const auto i = _deferredRequests.begin();
		request = i->second;
		if (!_owner->sessionHasCapacity(dcId(), request.sessionIndex)) {
			request.sessionIndex = sessionIndex;
		}
		_deferredRequests.erase(i);
		if (_deferredRequests.empty()) {
			_owner->forgetDeferredRequests(this);
		}
	}
	placeSentRequest(sendRequest(request), request);
}

void DownloadMtprotoTask::removeSession(int sessionIndex) {
	struct Redirect {
		mtpRequestId requestId = 0;
		int64 offset = 0;
	};
	auto redirect = std::vector<Redirect>();
	for (const auto &[requestId, requestData] : _sentRequests) {
		if (requestData.sessionIndex == sessionIndex) {
			redirect.reserve(_sentRequests.size());
			redirect.push_back({ requestId, requestData.offset });
		}
	}
	for (auto &[requestData, bytes] : _cdnUncheckedParts) {
		if (requestData.sessionIndex == sessionIndex) {
			const auto newIndex = _owner->chooseSessionIndex(dcId());
			Assert(newIndex < sessionIndex);
			requestData.sessionIndex = newIndex;
		}
	}
	for (const auto &[requestId, offset] : redirect) {
		const auto needMakeRequest = (requestId != _cdnHashesRequestId);
		cancelRequest(requestId);
		if (needMakeRequest) {
			const auto newIndex = _owner->chooseSessionIndex(dcId());
			Assert(newIndex < sessionIndex);
			makeRequest({ offset, newIndex });
		}
	}
}

mtpRequestId DownloadMtprotoTask::sendRequest(
		const RequestData &requestData) {
	const auto offset = requestData.offset;
	const auto limit = Storage::kDownloadPartSize;
	const auto shiftedDcId = MTP::downloadDcId(
		_cdnDcId ? _cdnDcId : dcId(),
		requestData.sessionIndex);
	if (_cdnDcId) {
		return api().request(MTPupload_GetCdnFile(
			MTP_bytes(_cdnToken),
			MTP_long(offset),
			MTP_int(limit)
		)).done([=](const MTPupload_CdnFile &result, mtpRequestId id) {
			cdnPartLoaded(result, id);
		}).fail([=](const MTP::Error &error, mtpRequestId id) {
			cdnPartFailed(error, id);
		}).toDC(shiftedDcId).send();
	}
	return v::match(_location.data, [&](const WebFileLocation &location) {
		return api().request(MTPupload_GetWebFile(
			MTP_inputWebFileLocation(
				MTP_bytes(location.url()),
				MTP_long(location.accessHash())),
			MTP_int(offset),
			MTP_int(limit)
		)).done([=](const MTPupload_WebFile &result, mtpRequestId id) {
			webPartLoaded(result, id);
		}).fail([=](const MTP::Error &error, mtpRequestId id) {
			partFailed(error, id);
		}).toDC(shiftedDcId).send();
	}, [&](const GeoPointLocation &location) {
		return api().request(MTPupload_GetWebFile(
			MTP_inputWebFileGeoPointLocation(
				MTP_inputGeoPoint(
					MTP_flags(0),
					MTP_double(location.lat),
					MTP_double(location.lon),
					MTP_int(0)), // accuracy_radius
				MTP_long(location.access),
				MTP_int(location.width),
				MTP_int(location.height),
				MTP_int(location.zoom),
				MTP_int(location.scale)),
			MTP_int(offset),
			MTP_int(limit)
		)).done([=](const MTPupload_WebFile &result, mtpRequestId id) {
			webPartLoaded(result, id);
		}).fail([=](const MTP::Error &error, mtpRequestId id) {
			partFailed(error, id);
		}).toDC(shiftedDcId).send();
	}, [&](const AudioAlbumThumbLocation &location) {
		using Flag = MTPDinputWebFileAudioAlbumThumbLocation::Flag;
		const auto owner = &api().session().data();
		return api().request(MTPupload_GetWebFile(
			MTP_inputWebFileAudioAlbumThumbLocation(
				MTP_flags(Flag::f_document | Flag::f_small),
				owner->document(location.documentId)->mtpInput(),
				MTPstring(),
				MTPstring()),
			MTP_int(offset),
			MTP_int(limit)
		)).done([=](const MTPupload_WebFile &result, mtpRequestId id) {
			webPartLoaded(result, id);
		}).fail([=](const MTP::Error &error, mtpRequestId id) {
			partFailed(error, id);
		}).toDC(shiftedDcId).send();
	}, [&](const StorageFileLocation &location) {
		const auto reference = location.fileReference();
		return api().request(MTPupload_GetFile(
			MTP_flags(MTPupload_GetFile::Flag::f_cdn_supported),
			location.tl(api().session().userId()),
			MTP_long(offset),
			MTP_int(limit)
		)).done([=](const MTPupload_File &result, mtpRequestId id) {
			normalPartLoaded(result, id);
		}).fail([=](const MTP::Error &error, mtpRequestId id) {
			normalPartFailed(reference, error, id);
		}).toDC(shiftedDcId).send();
	});
}

bool DownloadMtprotoTask::setWebFileSizeHook(int64 size) {
	return true;
}

void DownloadMtprotoTask::makeRequest(const RequestData &requestData) {
	if (!isMediaPreview()
		&& (DownloadBoostLevel() != 6 || api().session().premium())) {
		placeSentRequest(sendRequest(requestData), requestData);
		return;
	}
	_deferredRequests.emplace(requestData.offset, requestData);
	updateDownloadTrace();
	_owner->deferRequest(this);
}

void DownloadMtprotoTask::requestMoreCdnFileHashes() {
	if (_cdnHashesRequestId || _cdnUncheckedParts.empty()) {
		return;
	}

	const auto requestData = _cdnUncheckedParts.cbegin()->first;
	const auto shiftedDcId = MTP::downloadDcId(
		dcId(),
		requestData.sessionIndex);
	_cdnHashesRequestId = api().request(MTPupload_GetCdnFileHashes(
		MTP_bytes(_cdnToken),
		MTP_long(requestData.offset)
	)).done([=](const MTPVector<MTPFileHash> &result, mtpRequestId id) {
		getCdnFileHashesDone(result, id);
	}).fail([=](const MTP::Error &error, mtpRequestId id) {
		cdnPartFailed(error, id);
	}).toDC(shiftedDcId).send();
	placeSentRequest(_cdnHashesRequestId, requestData, false);
}

void DownloadMtprotoTask::normalPartLoaded(
		const MTPupload_File &result,
		mtpRequestId requestId) {
	const auto requestData = finishSentRequest(
		requestId,
		FinishRequestReason::Success);
	const auto owner = _owner;
	const auto dcId = this->dcId();
	result.match([&](const MTPDupload_fileCdnRedirect &data) {
		switchToCDN(requestData, data);
	}, [&](const MTPDupload_file &data) {
		partLoaded(requestData.offset, data.vbytes().v);
	});

	// 'this' may be deleted at this point.
	owner->checkSendNextAfterSuccess(dcId);
}

void DownloadMtprotoTask::webPartLoaded(
		const MTPupload_WebFile &result,
		mtpRequestId requestId) {
	const auto requestData = finishSentRequest(
		requestId,
		FinishRequestReason::Success);
	const auto owner = _owner;
	const auto dcId = this->dcId();
	result.match([&](const MTPDupload_webFile &data) {
		if (setWebFileSizeHook(data.vsize().v)) {
			partLoaded(requestData.offset, data.vbytes().v);
		}
	});

	// 'this' may be deleted at this point.
	owner->checkSendNextAfterSuccess(dcId);
}

void DownloadMtprotoTask::cdnPartLoaded(const MTPupload_CdnFile &result, mtpRequestId requestId) {
	result.match([&](const MTPDupload_cdnFileReuploadNeeded &data) {
		const auto requestData = finishSentRequest(
			requestId,
			FinishRequestReason::Redirect);
		const auto shiftedDcId = MTP::downloadDcId(
			dcId(),
			requestData.sessionIndex);
		const auto requestId = api().request(MTPupload_ReuploadCdnFile(
			MTP_bytes(_cdnToken),
			data.vrequest_token()
		)).done([=](const MTPVector<MTPFileHash> &result, mtpRequestId id) {
			reuploadDone(result, id);
		}).fail([=](const MTP::Error &error, mtpRequestId id) {
			cdnPartFailed(error, id);
		}).toDC(shiftedDcId).send();
		placeSentRequest(requestId, requestData, false);
	}, [&](const MTPDupload_cdnFile &data) {
		const auto requestData = finishSentRequest(
			requestId,
			FinishRequestReason::Success);
		const auto owner = _owner;
		const auto dcId = this->dcId();
		const auto guard = gsl::finally([=] {
			// 'this' may be deleted at this point.
			owner->checkSendNextAfterSuccess(dcId);
		});

		auto key = bytes::make_span(_cdnEncryptionKey);
		auto iv = bytes::make_span(_cdnEncryptionIV);
		Expects(key.size() == MTP::CTRState::KeySize);
		Expects(iv.size() == MTP::CTRState::IvecSize);

		auto state = MTP::CTRState();
		auto ivec = bytes::make_span(state.ivec);
		std::copy(iv.begin(), iv.end(), ivec.begin());

		auto counterOffset = static_cast<uint32>(requestData.offset >> 4);
		state.ivec[15] = static_cast<uchar>(counterOffset & 0xFF);
		state.ivec[14] = static_cast<uchar>((counterOffset >> 8) & 0xFF);
		state.ivec[13] = static_cast<uchar>((counterOffset >> 16) & 0xFF);
		state.ivec[12] = static_cast<uchar>((counterOffset >> 24) & 0xFF);

		auto decryptInPlace = data.vbytes().v;
		auto buffer = bytes::make_detached_span(decryptInPlace);
		MTP::aesCtrEncrypt(buffer, key.data(), &state);

		switch (checkCdnFileHash(requestData.offset, buffer)) {
		case CheckCdnHashResult::NoHash: {
			_cdnUncheckedParts.emplace(requestData, decryptInPlace);
			requestMoreCdnFileHashes();
		} return;

		case CheckCdnHashResult::Invalid: {
			LOG(("API Error: Wrong cdnFileHash for offset %1."
				).arg(requestData.offset));
			cancelOnFail();
		} return;

		case CheckCdnHashResult::Good: {
			partLoaded(requestData.offset, decryptInPlace);
		} return;
		}
		Unexpected("Result of checkCdnFileHash()");
	});
}

DownloadMtprotoTask::CheckCdnHashResult DownloadMtprotoTask::checkCdnFileHash(
		int64 offset,
		bytes::const_span buffer) {
	const auto cdnFileHashIt = _cdnFileHashes.find(offset);
	if (cdnFileHashIt == _cdnFileHashes.cend()) {
		return CheckCdnHashResult::NoHash;
	}
	const auto realHash = openssl::Sha256(buffer);
	const auto receivedHash = bytes::make_span(cdnFileHashIt->second.hash);
	if (bytes::compare(realHash, receivedHash)) {
		return CheckCdnHashResult::Invalid;
	}
	return CheckCdnHashResult::Good;
}

void DownloadMtprotoTask::reuploadDone(
		const MTPVector<MTPFileHash> &result,
		mtpRequestId requestId) {
	const auto requestData = finishSentRequest(
		requestId,
		FinishRequestReason::Redirect);
	addCdnHashes(result.v);
	makeRequest(requestData);
}

void DownloadMtprotoTask::getCdnFileHashesDone(
		const MTPVector<MTPFileHash> &result,
		mtpRequestId requestId) {
	Expects(_cdnHashesRequestId == requestId);

	const auto requestData = finishSentRequest(
		requestId,
		FinishRequestReason::Redirect);
	addCdnHashes(result.v);
	auto someMoreChecked = false;
	for (auto i = _cdnUncheckedParts.begin(); i != _cdnUncheckedParts.cend();) {
		const auto uncheckedData = i->first;
		const auto uncheckedBytes = bytes::make_span(i->second);

		switch (checkCdnFileHash(uncheckedData.offset, uncheckedBytes)) {
		case CheckCdnHashResult::NoHash: {
			++i;
		} break;

		case CheckCdnHashResult::Invalid: {
			LOG(("API Error: Wrong cdnFileHash for offset %1."
				).arg(uncheckedData.offset));
			cancelOnFail();
			return;
		} break;

		case CheckCdnHashResult::Good: {
			someMoreChecked = true;
			const auto goodOffset = uncheckedData.offset;
			const auto goodBytes = std::move(i->second);
			const auto weak = base::make_weak(this);
			i = _cdnUncheckedParts.erase(i);
			recordReceivedPart(goodBytes.size());
			if (!feedPart(goodOffset, goodBytes) || !weak) {
				return;
			}
		} break;

		default: Unexpected("Result of checkCdnFileHash()");
		}
	}
	if (!someMoreChecked) {
		LOG(("API Error: "
			"Could not find cdnFileHash for offset %1 "
			"after getCdnFileHashes request."
			).arg(requestData.offset));
		cancelOnFail();
		return;
	}
	requestMoreCdnFileHashes();
}

void DownloadMtprotoTask::placeSentRequest(
		mtpRequestId requestId,
		const RequestData &requestData,
		bool content) {
	if (_sentRequests.empty()) {
		subscribeToTransferLimits();
	}

	const auto preview = content && isMediaPreview();
	const auto amount = _owner->changeRequestedAmount(
		dcId(),
		requestData.sessionIndex,
		Storage::kDownloadPartSize,
		preview);
	const auto &[i, ok1] = _sentRequests.emplace(requestId, requestData);
	const auto &[j, ok2] = _requestByOffset.emplace(
		requestData.offset,
		requestId);

	i->second.requestedInSession = amount;
	i->second.previewCounted = preview;
	i->second.sent = crl::now();
	i->second.referenceAt = 0;
	const auto trace = downloadTrace();
	++trace->sent;
	if (!trace->firstSent) {
		trace->firstSent = i->second.sent;
	}
	trace->requestId = requestId;
	if (!trace->received) {
		trace->setStage(DownloadTrace::Stage::Request);
	}
	updateDownloadTrace();

	Ensures(ok1 && ok2);
}

void DownloadMtprotoTask::subscribeToTransferLimits() {
	if (_transferLimitSubscription) {
		return;
	}
	_owner->api().instance().requestErrors(
	) | rpl::on_next([=](const MTP::RequestRetryInfo &info) {
		logAvatarFailure(info);
	}, _transferLimitSubscription);
	_owner->api().instance().requestRetries(
	) | rpl::on_next([=](const MTP::RequestRetryInfo &info) {
		handleRequestRetry(info);
	}, _transferLimitSubscription);
	_owner->api().instance().transferLimits(
	) | rpl::on_next([=](const MTP::TransferLimitInfo &info) {
		if (info.logDetails && !info.upload) {
			logTransferLimitSource(info);
		}
	}, _transferLimitSubscription);
	_owner->api().instance().nonPremiumDelayedRequests(
	) | rpl::on_next([=](const auto &data) {
		if (!_sentRequests.contains(data.first)) {
			return;
		}
		const auto location = std::get_if<StorageFileLocation>(&_location.data);
		if (!location) {
			return;
		}
		const auto type = location->type();
		if (type == StorageFileLocation::Type::Document
			|| type == StorageFileLocation::Type::Photo) {
			_owner->notifyNonPremiumDelay(
				dcId(),
				(type == StorageFileLocation::Type::Document)
					? location->objectId()
					: DocumentId(0),
				data.second);
		}
	}, _transferLimitSubscription);
}

void DownloadMtprotoTask::handleRequestRetry(
		const MTP::RequestRetryInfo &info) {
	if (!_sentRequests.contains(info.requestId)) {
		return;
	}
	if (_downloadTrace) {
		++_downloadTrace->retries;
		_downloadTrace->requestId = info.requestId;
		_downloadTrace->retryAt = info.retryAt;
		_downloadTrace->setError(info.type);
		_downloadTrace->setStage(DownloadTrace::Stage::Retry);
	}
	const auto location = std::get_if<StorageFileLocation>(&_location.data);
	if (!location
		|| !location->isDocumentThumbnail()
		|| _thumbnailFailureQueued) {
		return;
	}
	if (info.code >= 500) {
		++_thumbnailServerFailures;
	}
	if (!_thumbnailServerFailures) {
		return;
	} else if (_thumbnailServerFailures < kThumbnailServerFailureLimit) {
		_owner->notifyThumbnailRetry({
			.task = this,
			.retryAt = info.retryAt,
			.attempt = _thumbnailServerFailures + 1,
			.total = kThumbnailServerFailureLimit,
		});
		return;
	}
	_thumbnailFailureQueued = true;
	const auto requestId = info.requestId;
	// WHY: MTProto restores the response handler after notifying retry observers.
	// Defer cancellation until that handler and its delayed retry are registered.
	crl::on_main(this, [=] {
		failThumbnailAfterRetries(requestId);
	});
}

void DownloadMtprotoTask::failThumbnailAfterRetries(mtpRequestId requestId) {
	_thumbnailFailureQueued = false;
	if (_thumbnailServerFailures < kThumbnailServerFailureLimit
		|| !_sentRequests.contains(requestId)) {
		return;
	}
	if (_downloadTrace) {
		_downloadTrace->setError(u"thumbnail_retry_exhausted:%1"_q.arg(
			_downloadTrace->error));
		_downloadTrace->finish(DownloadTrace::Stage::Failed);
	}
	cancelAllRequests();
	const auto weak = base::make_weak(this);
	removeFromQueue();
	if (weak) {
		cancelOnFail();
	}
}

void DownloadMtprotoTask::clearThumbnailRetry() {
	if (base::take(_thumbnailServerFailures)) {
		_owner->notifyThumbnailRetry({ .task = this });
	}
}

void DownloadMtprotoTask::logAvatarFailure(
		const MTP::RequestRetryInfo &info) {
	if (_avatarFailureLogged || !_sentRequests.contains(info.requestId)) {
		return;
	}
	const auto location = std::get_if<StorageFileLocation>(&_location.data);
	if (!location) {
		return;
	}
	const auto &session = api().session();
	auto peerId = PeerId();
	auto photoId = PhotoId(0);
	auto size = QString();
	if (location->type() == StorageFileLocation::Type::PeerPhoto) {
		const auto input = location->tl(session.userId());
		const auto &photo = input.c_inputPeerPhotoFileLocation();
		peerId = AvatarPeerId(photo.vpeer(), session.userId());
		photoId = photo.vphoto_id().v;
		size = photo.is_big() ? u"big"_q : u"small"_q;
	} else if (location->type() == StorageFileLocation::Type::Photo) {
		peerId = v::match(_origin.data, [](
				const Data::FileOriginUserPhoto &origin) {
			return peerFromUser(origin.userId);
		}, [](const Data::FileOriginFullUser &origin) {
			return peerFromUser(origin.userId);
		}, [](const Data::FileOriginPeerPhoto &origin) {
			return origin.peerId;
		}, [](const auto &) {
			return PeerId();
		});
		if (!peerId) {
			return;
		}
		photoId = location->objectId();
		size = u"profile"_q;
	} else {
		return;
	}
	const auto bareId = peerId.value & PeerId::kChatTypeMask;
	const auto chatId = peerIsChannel(peerId)
		? -qlonglong(1000000000000LL) - qlonglong(bareId)
		: peerIsChat(peerId) ? -qlonglong(bareId) : qlonglong(bareId);
	const auto error = info.type.left(80)
		.replace(u'\r', u' ')
		.replace(u'\n', u' ')
		.replace(u'\t', u' ');
	_avatarFailureLogged = true;
	LOG(("Avatar load failed: id=%1 request=%2 dc=%3 peer_type=%4 "
		"peer_id=%5 chat_id=%6 photo_id=%7 size=%8 code=%9 error=%10")
		.arg(qulonglong(_downloadTrace ? _downloadTrace->id : 0))
		.arg(info.requestId)
		.arg(dcId())
		.arg(AvatarPeerType(session.data(), peerId))
		.arg(qulonglong(bareId))
		.arg(chatId)
		.arg(qulonglong(photoId))
		.arg(size)
		.arg(info.code)
		.arg(error));
}

void DownloadMtprotoTask::logTransferLimitSource(
		const MTP::TransferLimitInfo &info) const {
	const auto i = _sentRequests.find(info.requestId);
	if (i == _sentRequests.end()) {
		return;
	}
	const auto resource = v::match(_location.data, [](
			const StorageFileLocation &location) {
		using Type = StorageFileLocation::Type;
		switch (location.type()) {
		case Type::Legacy: return u"legacy"_q;
		case Type::Encrypted: return u"encrypted"_q;
		case Type::Document:
			return location.isDocumentThumbnail()
				? u"document_thumbnail"_q
				: u"document"_q;
		case Type::Secure: return u"secure"_q;
		case Type::Takeout: return u"takeout"_q;
		case Type::Photo: return u"photo"_q;
		case Type::PeerPhoto: return u"avatar"_q;
		case Type::StickerSetThumb: return u"sticker_set_thumbnail"_q;
		case Type::GroupCallStream: return u"group_call_stream"_q;
		}
		Unexpected("StorageFileLocation type.");
	}, [](const WebFileLocation &) {
		return u"web_file"_q;
	}, [](const GeoPointLocation &) {
		return u"map_thumbnail"_q;
	}, [](const AudioAlbumThumbLocation &) {
		return u"audio_album_thumbnail"_q;
	});
	const auto origin = v::match(_origin.data, [](v::null_t) {
		return u"unspecified"_q;
	}, [](const Data::FileOriginMessage &) {
		return u"message"_q;
	}, [](const Data::FileOriginUserPhoto &) {
		return u"user_photo"_q;
	}, [](const Data::FileOriginFullUser &) {
		return u"user_profile"_q;
	}, [](const Data::FileOriginPeerPhoto &) {
		return u"peer_photo"_q;
	}, [](const Data::FileOriginStickerSet &) {
		return u"sticker_set"_q;
	}, [](const Data::FileOriginSavedGifs &) {
		return u"saved_gifs"_q;
	}, [](const Data::FileOriginWallpaper &) {
		return u"wallpaper"_q;
	}, [](const Data::FileOriginTheme &) {
		return u"theme"_q;
	}, [](const Data::FileOriginRingtones &) {
		return u"ringtones"_q;
	}, [](const Data::FileOriginPremiumPreviews &) {
		return u"premium_previews"_q;
	}, [](const Data::FileOriginWebPage &) {
		return u"web_page"_q;
	}, [](const Data::FileOriginCloudDraft &) {
		return u"cloud_draft"_q;
	}, [](const Data::FileOriginStory &) {
		return u"story"_q;
	});
	LOG(("Transfer limit source: request=%1 dc=%2 source=%3 resource=%4 "
		"origin=%5 object=%6 offset=%7 request_age_ms=%8")
		.arg(info.requestId)
		.arg(info.dcId)
		.arg(downloadSource())
		.arg(resource)
		.arg(origin)
		.arg(qulonglong(objectId()))
		.arg(qlonglong(i->second.offset))
		.arg(qlonglong(crl::now() - i->second.sent)));
}

auto DownloadMtprotoTask::finishSentRequest(
	mtpRequestId requestId,
	FinishRequestReason reason)
-> RequestData {
	auto it = _sentRequests.find(requestId);
	Assert(it != _sentRequests.cend());

	if (_cdnHashesRequestId == requestId) {
		_cdnHashesRequestId = 0;
	}
	const auto result = it->second;
	_owner->changeRequestedAmount(
		dcId(),
		result.sessionIndex,
		-Storage::kDownloadPartSize,
		result.previewCounted);
	_sentRequests.erase(it);
	updateDownloadTrace();
	const auto ok = _requestByOffset.remove(result.offset);

	if (_sentRequests.empty()) {
		_transferLimitSubscription.destroy();
	}

	if (reason == FinishRequestReason::Success) {
		_owner->requestSucceeded(
			dcId(),
			result.sessionIndex,
			result.requestedInSession,
			result.sent);
	}

	Ensures(ok);
	return result;
}

bool DownloadMtprotoTask::haveSentRequests() const {
	return !_sentRequests.empty()
		|| !_deferredRequests.empty()
		|| !_cdnUncheckedParts.empty();
}

bool DownloadMtprotoTask::haveSentRequestForOffset(int64 offset) const {
	return _requestByOffset.contains(offset)
		|| _deferredRequests.contains(offset)
		|| _cdnUncheckedParts.contains({ offset, 0 });
}

void DownloadMtprotoTask::cancelAllRequests() {
	_deferredRequests.clear();
	updateDownloadTrace();
	_owner->forgetDeferredRequests(this);
	while (!_sentRequests.empty()) {
		cancelRequest(_sentRequests.begin()->first);
	}
	_cdnUncheckedParts.clear();
	clearThumbnailRetry();
}

void DownloadMtprotoTask::cancelRequestForOffset(int64 offset) {
	_deferredRequests.remove(offset);
	updateDownloadTrace();
	if (_deferredRequests.empty()) {
		_owner->forgetDeferredRequests(this);
	}
	const auto i = _requestByOffset.find(offset);
	if (i != end(_requestByOffset)) {
		cancelRequest(i->second);
	}
	_cdnUncheckedParts.remove({ offset, 0 });
}

bool DownloadMtprotoTask::retryRequestForOffset(
		int64 offset,
		crl::time minimumAge) {
	const auto now = crl::now();
	const auto state = nonPremiumDelayState();
	if (_cdnDcId || now < state.limitedUntil || now < state.recoveryUntil) {
		return false;
	}
	const auto i = _requestByOffset.find(offset);
	if (i == end(_requestByOffset)) {
		return false;
	}
	const auto requestId = i->second;
	const auto j = _sentRequests.find(requestId);
	Assert(j != end(_sentRequests));
	if (api().instance().requestIsDelayed(requestId)) {
		j->second.readRetrySuppressed = true;
	}
	if (now - j->second.sent < minimumAge
		|| j->second.readRetrySuppressed) {
		return false;
	}
	const auto index = _owner->chooseAlternativeSessionIndex(
		dcId(),
		j->second.sessionIndex);
	if (!index) {
		return false;
	}
	cancelRequest(requestId);
	makeRequest({ offset, *index });
	return true;
}

bool DownloadMtprotoTask::replaceRequestForOffset(
		int64 previousOffset,
		int64 requiredOffset,
		crl::time minimumAge) {
	const auto now = crl::now();
	const auto state = nonPremiumDelayState();
	if (_cdnDcId
		|| requiredOffset < 0
		|| requiredOffset % kDownloadPartSize
		|| haveSentRequestForOffset(requiredOffset)
		|| now < state.limitedUntil
		|| now < state.recoveryUntil) {
		return false;
	}
	const auto i = _requestByOffset.find(previousOffset);
	if (i == end(_requestByOffset)) {
		return false;
	}
	const auto requestId = i->second;
	const auto j = _sentRequests.find(requestId);
	Assert(j != end(_sentRequests));
	if (api().instance().requestIsDelayed(requestId)) {
		j->second.readRetrySuppressed = true;
	}
	if (now - j->second.sent < minimumAge
		|| j->second.readRetrySuppressed) {
		return false;
	}
	const auto index = _owner->chooseAlternativeSessionIndex(
		dcId(),
		j->second.sessionIndex).value_or(j->second.sessionIndex);
	cancelRequest(requestId);
	makeRequest({ requiredOffset, index });
	return true;
}

void DownloadMtprotoTask::cancelRequest(mtpRequestId requestId) {
	const auto hashes = (_cdnHashesRequestId == requestId);
	api().request(requestId).cancel();
	[[maybe_unused]] const auto data = finishSentRequest(
		requestId,
		FinishRequestReason::Cancel);
	if (hashes && !_cdnUncheckedParts.empty()) {
		crl::on_main(this, [=] {
			requestMoreCdnFileHashes();
		});
	}
}

void DownloadMtprotoTask::addToQueue(int priority) {
	const auto trace = downloadTrace();
	trace->priority = priority;
	if (trace->stage.load(std::memory_order_relaxed) == DownloadTrace::Stage::Idle) {
		trace->progressAt = crl::now();
		_owner->diagnostics().watch(trace);
	}
	if (_sentRequests.empty()) {
		trace->setStage(DownloadTrace::Stage::Queued);
	}
	_owner->enqueue(this, priority);
}

void DownloadMtprotoTask::removeFromQueue() {
	if (_downloadTrace) {
		_downloadTrace->setStage(DownloadTrace::Stage::Idle);
	}
	_owner->remove(this);
}

void DownloadMtprotoTask::partLoaded(
		int64 offset,
		const QByteArray &bytes) {
	recordReceivedPart(bytes.size());
	feedPart(offset, bytes);
}

void DownloadMtprotoTask::recordReceivedPart(int64 size) {
	clearThumbnailRetry();
	if (_downloadTrace) {
		_downloadTrace->bytes += size;
		++_downloadTrace->received;
		_downloadTrace->progressAt = crl::now();
		if (!_downloadTrace->firstReceived) {
			_downloadTrace->firstReceived = _downloadTrace->progressAt;
		}
		_downloadTrace->retryAt = 0;
		_downloadTrace->setStage(DownloadTrace::Stage::Receiving);
	}
}

bool DownloadMtprotoTask::normalPartFailed(
		QByteArray fileReference,
		const MTP::Error &error,
		mtpRequestId requestId) {
	const auto i = _sentRequests.find(requestId);
	if (_downloadTrace) {
		_downloadTrace->setError(error.type());
		_downloadTrace->requestId = requestId;
	}
	if (i != end(_sentRequests)) {
		i->second.readRetrySuppressed = true;
	}
	if (MTP::IsDefaultHandledError(error)) {
		return false;
	}
	if (error.code() == 400
		&& error.type().startsWith(u"FILE_REFERENCE_"_q)) {
		if (i != end(_sentRequests)) {
			i->second.referenceAt = crl::now();
		}
		if (_downloadTrace) {
			++_downloadTrace->references;
			_downloadTrace->setStage(DownloadTrace::Stage::Reference);
			updateDownloadTrace();
		}
		api().refreshFileReference(
			_origin,
			this,
			requestId,
			fileReference);
		return true;
	}
	return partFailed(error, requestId);
}

bool DownloadMtprotoTask::partFailed(
		const MTP::Error &error,
		mtpRequestId requestId) {
	if (_downloadTrace) {
		_downloadTrace->setError(error.type());
		_downloadTrace->requestId = requestId;
	}
	if (MTP::IsDefaultHandledError(error)) {
		return false;
	}
	if (_downloadTrace) {
		_downloadTrace->finish(DownloadTrace::Stage::Failed);
	}
	cancelOnFail();
	return true;
}

bool DownloadMtprotoTask::cdnPartFailed(
		const MTP::Error &error,
		mtpRequestId requestId) {
	if (MTP::IsDefaultHandledError(error)) {
		return false;
	}

	if (error.type() == u"FILE_TOKEN_INVALID"_q
		|| error.type() == u"REQUEST_TOKEN_INVALID"_q) {
		const auto requestData = finishSentRequest(
			requestId,
			FinishRequestReason::Redirect);
		changeCDNParams(
			requestData,
			0,
			QByteArray(),
			QByteArray(),
			QByteArray(),
			QVector<MTPFileHash>());
		return true;
	}
	return partFailed(error, requestId);
}

void DownloadMtprotoTask::switchToCDN(
		const RequestData &requestData,
		const MTPDupload_fileCdnRedirect &redirect) {
	changeCDNParams(
		requestData,
		redirect.vdc_id().v,
		redirect.vfile_token().v,
		redirect.vencryption_key().v,
		redirect.vencryption_iv().v,
		redirect.vfile_hashes().v);
}

void DownloadMtprotoTask::addCdnHashes(
		const QVector<MTPFileHash> &hashes) {
	for (const auto &hash : hashes) {
		hash.match([&](const MTPDfileHash &data) {
			_cdnFileHashes.emplace(
				data.voffset().v,
				CdnFileHash{ data.vlimit().v, data.vhash().v });
		});
	}
}

void DownloadMtprotoTask::changeCDNParams(
		const RequestData &requestData,
		MTP::DcId dcId,
		const QByteArray &token,
		const QByteArray &encryptionKey,
		const QByteArray &encryptionIV,
		const QVector<MTPFileHash> &hashes) {
	if (dcId != 0
		&& (encryptionKey.size() != MTP::CTRState::KeySize
			|| encryptionIV.size() != MTP::CTRState::IvecSize)) {
		LOG(("Message Error: Wrong key (%1) / iv (%2) size in CDN params"
			).arg(encryptionKey.size()
			).arg(encryptionIV.size()));
		cancelOnFail();
		return;
	}

	auto resendAllRequests = (_cdnDcId != dcId
		|| _cdnToken != token
		|| _cdnEncryptionKey != encryptionKey
		|| _cdnEncryptionIV != encryptionIV);
	_cdnDcId = dcId;
	_cdnToken = token;
	_cdnEncryptionKey = encryptionKey;
	_cdnEncryptionIV = encryptionIV;
	addCdnHashes(hashes);

	if (resendAllRequests && !_sentRequests.empty()) {
		auto resendRequests = std::vector<RequestData>();
		resendRequests.reserve(_sentRequests.size());
		while (!_sentRequests.empty()) {
			const auto requestId = _sentRequests.begin()->first;
			api().request(requestId).cancel();
			resendRequests.push_back(finishSentRequest(
				requestId,
				FinishRequestReason::Redirect));
		}
		for (const auto &requestData : resendRequests) {
			makeRequest(requestData);
		}
	}
	makeRequest(requestData);
}

} // namespace Storage
