/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/download_manager_mtproto.h"

#include "apiwrap.h"
#include "base/openssl_help.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_boost.h"
#include "mtproto/facade.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/mtproto_response.h"
#include "settings.h"

#include <algorithm>

namespace Storage {
namespace {

constexpr auto kKillSessionTimeout = 15 * crl::time(1000);
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
constexpr auto kSmartSampleMaximumRequests = 64;
constexpr auto kSmartMeasurementMaxAge = 2 * 60 * crl::time(1000);
constexpr auto kSmartLimitChangeCooldown = 30 * crl::time(1000);
constexpr auto kSmartPressureDuration = 3 * crl::time(1000);
constexpr auto kSmartSeekFreezeDuration = 15 * crl::time(1000);
constexpr auto kSmartSeekPressureDuration = crl::time(1000);
constexpr auto kSmartProbeDuration = 15 * crl::time(1000);
constexpr auto kSmartExcessCapacityRatio = 2.25;
constexpr auto kSmartHighLatencyCapacityRatio = 1.5;
constexpr auto kSmartHighLatencyThreshold = 3 * crl::time(1000);
constexpr auto kSmartProbeMaximumLatency = 4 * crl::time(1000);
constexpr auto kSmartProbeMinimumGain = 1.08;
constexpr auto kSmartProbeMaximumLatencyRatio = 1.35;
constexpr auto kSmartMaximumMeasuredThroughput = 64 * 1024 * 1024;

[[nodiscard]] QString SmartRequestLimitReasonString(
		NonPremiumRequestLimitReason reason) {
	switch (reason) {
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

auto DownloadManagerMtproto::Queue::nextTask(bool onlyHighestPriority) const
-> Task* {
	if (_tasks.empty()) {
		return nullptr;
	}
	const auto highestPriority = _tasks.front().priority;
	const auto notHighestPriority = [&](const Enqueued &enqueued) {
		return (enqueued.priority != highestPriority);
	};
	const auto till = (onlyHighestPriority && highestPriority > 0)
		? ranges::find_if(_tasks, notHighestPriority)
		: end(_tasks);
	const auto readyToRequest = [&](const Enqueued &enqueued) {
		return enqueued.task->readyToRequest();
	};
	const auto first = ranges::find_if(
		ranges::make_subrange(begin(_tasks), till),
		readyToRequest);
	return (first != till) ? first->task.get() : nullptr;
}

void DownloadManagerMtproto::Queue::removeSession(int index) {
	for (const auto &enqueued : _tasks) {
		enqueued.task->removeSession(index);
	}
}

DownloadManagerMtproto::DcSessionBalanceData::DcSessionBalanceData()
: maxWaitedAmount(StartWaitedInSession()) {
}

DownloadManagerMtproto::DcBalanceData::DcBalanceData()
: sessions(StartSessionsCount()) {
}

DownloadManagerMtproto::DownloadManagerMtproto(not_null<ApiWrap*> api)
: _api(api)
, _nonPremiumDelayTimer([=] { checkNonPremiumDelayState(); })
, _resetGenerationTimer([=] { resetGeneration(); })
, _killSessionsTimer([=] { killSessions(); }) {
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
		(void)task;
		if (demand.dcId != dcId) {
			continue;
		}
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
		if (demand.bufferPressure) {
			evaluateSmartRequestLimit(demand.dcId, now);
		}
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
	const auto demand = smartDemandSummary(dcId);
	const auto seekPressure = demand.bufferPressure
		&& (now < demand.seekUntil);
	const auto sampleDuration = seekPressure
		? kSmartSeekPressureDuration
		: kSmartSampleBusyDuration;
	if (state.sampleBusyDuration < sampleDuration
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
	if (!demand.bufferPressure && !demand.playbackBytesPerSecond) {
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
		if (latencyWorse || (demand.bufferPressure && noGain)) {
			updateSmartRequestLimit(
				dcId,
				previous,
				NonPremiumRequestLimitReason::ProbeNoGain,
				now);
		}
		return;
	}
	const auto seekPressure = demand.bufferPressure
		&& (now < demand.seekUntil);
	if (now < state.lastChange + kSmartLimitChangeCooldown
		&& !seekPressure) {
		return;
	}
	const auto playback = double(demand.playbackBytesPerSecond);
	const auto &profile = SmartProfile();
	if (!demand.bufferPressure) {
		if (now < demand.seekUntil) {
			return;
		}
		if (state.target > profile.smartCapacityMinimumRequestLimit
			&& playback > 0.
			&& state.throughputEma
				> playback * kSmartExcessCapacityRatio) {
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
	const auto pressureDuration = seekPressure
		? kSmartSeekPressureDuration
		: kSmartPressureDuration;
	if (!demand.pressureSince
		|| now < demand.pressureSince + pressureDuration
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
		NonPremiumRequestLimitReason::BufferPressure,
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
	scheduleNonPremiumDelayCheck();
	if (!shouldNotify) {
		return;
	}
	_nonPremiumDelayUpdates.fire_copy({ dcId, info });
	_nonPremiumDelays.fire_copy({ id, info });
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
	_smartStreamingDemands.remove(task);
	if (smartNonPremiumEnabled()) {
		evaluateSmartRequestLimit(dcId, crl::now());
	}
	auto &queue = _queues[dcId];
	queue.remove(task);
	checkSendNext(dcId, queue);
}

void DownloadManagerMtproto::resetGeneration() {
	_resetGenerationTimer.cancel();
	for (auto &[dcId, queue] : _queues) {
		queue.resetGeneration();
	}
}

void DownloadManagerMtproto::checkSendNext() {
	for (auto &[dcId, queue] : _queues) {
		if (queue.empty()) {
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
	const auto onlyHighestPriority = (balanceData.totalRequested > 0);
	if (const auto task = queue.nextTask(onlyHighestPriority)) {
		task->loadPart(bestIndex);
		return true;
	}
	return false;
}

int DownloadManagerMtproto::changeRequestedAmount(
		MTP::DcId dcId,
		int index,
		int delta) {
	const auto i = _balanceData.find(dcId);
	Assert(i != _balanceData.end());
	Assert(index < i->second.sessions.size());
	trackSmartRequestActivity(dcId, crl::now());
	const auto result = (i->second.sessions[index].requested += delta);
	i->second.totalRequested += delta;
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
		dc = DcBalanceData();
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
	cancelAllRequests();
	_owner->remove(this);
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

void DownloadMtprotoTask::refreshFileReferenceFrom(
		const Data::UpdatedFileReferences &updates,
		int requestId,
		const QByteArray &current) {
	if (const auto v = std::get_if<StorageFileLocation>(&_location.data)) {
		v->refreshFileReference(updates);
		if (v->fileReference() == current) {
			cancelOnFail();
			return;
		}
	} else {
		cancelOnFail();
		return;
	}
	if (_sentRequests.contains(requestId)) {
		makeRequest(finishSentRequest(
			requestId,
			FinishRequestReason::Redirect));
	}
}

void DownloadMtprotoTask::loadPart(int sessionIndex) {
	makeRequest({ takeNextRequestOffset(), sessionIndex });
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
	placeSentRequest(sendRequest(requestData), requestData);
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
	placeSentRequest(_cdnHashesRequestId, requestData);
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
		placeSentRequest(requestId, requestData);
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
		const RequestData &requestData) {
	if (_sentRequests.empty()) {
		subscribeToNonPremiumLimit();
	}

	const auto amount = _owner->changeRequestedAmount(
		dcId(),
		requestData.sessionIndex,
		Storage::kDownloadPartSize);
	const auto &[i, ok1] = _sentRequests.emplace(requestId, requestData);
	const auto &[j, ok2] = _requestByOffset.emplace(
		requestData.offset,
		requestId);

	i->second.requestedInSession = amount;
	i->second.sent = crl::now();

	Ensures(ok1 && ok2);
}

void DownloadMtprotoTask::subscribeToNonPremiumLimit() {
	if (_nonPremiumLimitSubscription) {
		return;
	}
	_owner->api().instance().nonPremiumDelayedRequests(
	) | rpl::on_next([=](const auto &data) {
		if (_sentRequests.contains(data.first)) {
			if (const auto documentId = objectId()) {
				const auto type = v::get<StorageFileLocation>(
					_location.data).type();
				if (type == StorageFileLocation::Type::Document) {
					_owner->notifyNonPremiumDelay(
						dcId(),
						documentId,
						data.second);
				}
			}
		}
	}, _nonPremiumLimitSubscription);
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
		-Storage::kDownloadPartSize);
	_sentRequests.erase(it);
	const auto ok = _requestByOffset.remove(result.offset);

	if (_sentRequests.empty()) {
		_nonPremiumLimitSubscription.destroy();
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
	return !_sentRequests.empty() || !_cdnUncheckedParts.empty();
}

bool DownloadMtprotoTask::haveSentRequestForOffset(int64 offset) const {
	return _requestByOffset.contains(offset)
		|| _cdnUncheckedParts.contains({ offset, 0 });
}

void DownloadMtprotoTask::cancelAllRequests() {
	while (!_sentRequests.empty()) {
		cancelRequest(_sentRequests.begin()->first);
	}
	_cdnUncheckedParts.clear();
}

void DownloadMtprotoTask::cancelRequestForOffset(int64 offset) {
	const auto i = _requestByOffset.find(offset);
	if (i != end(_requestByOffset)) {
		cancelRequest(i->second);
	}
	_cdnUncheckedParts.remove({ offset, 0 });
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
	_owner->enqueue(this, priority);
}

void DownloadMtprotoTask::removeFromQueue() {
	_owner->remove(this);
}

void DownloadMtprotoTask::partLoaded(
		int64 offset,
		const QByteArray &bytes) {
	feedPart(offset, bytes);
}

bool DownloadMtprotoTask::normalPartFailed(
		QByteArray fileReference,
		const MTP::Error &error,
		mtpRequestId requestId) {
	if (MTP::IsDefaultHandledError(error)) {
		return false;
	}
	if (error.code() == 400
		&& error.type().startsWith(u"FILE_REFERENCE_"_q)) {
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
	if (MTP::IsDefaultHandledError(error)) {
		return false;
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
