#include "storage/download_diagnostics.h"

#include "logs.h"

#include <algorithm>

namespace Storage {
namespace {

constexpr auto kCheckInterval = crl::time(15000);
constexpr auto kReportInterval = crl::time(60000);
constexpr auto kTraceLimit = 2048;
constexpr auto kDetailLimit = 24;
constexpr auto kBurstLimit = 6;
constexpr auto kSummaryLimit = 4;

[[nodiscard]] uint64 NextDiagnosticId() {
	static auto next = uint64(0);
	return ++next;
}

[[nodiscard]] bool PermitLog(bool summary, crl::time now) {
	static auto window = crl::time(0);
	static auto details = 0;
	static auto summaries = 0;
	static auto burstAt = crl::time(0);
	static auto burst = 0;
	if (!window || now >= window + kReportInterval) {
		window = now;
		details = summaries = 0;
	}
	if (!burstAt || now >= burstAt + kCheckInterval) {
		burstAt = now;
		burst = 0;
	}
	auto &count = summary ? summaries : details;
	if (count >= (summary ? kSummaryLimit : kDetailLimit)
		|| (!summary && burst >= kBurstLimit)) {
		return false;
	}
	++count;
	if (!summary) {
		++burst;
	}
	return true;
}

[[nodiscard]] const char *StageName(DownloadTrace::Stage stage) {
	using Stage = DownloadTrace::Stage;
	switch (stage) {
	case Stage::Created: return "created";
	case Stage::CacheLookup: return "cache_lookup";
	case Stage::CacheDecodeQueued: return "cache_decode_queued";
	case Stage::CacheDecode: return "cache_decode";
	case Stage::CacheDelivery: return "cache_delivery";
	case Stage::Queued: return "queued";
	case Stage::Request: return "request";
	case Stage::Receiving: return "receiving";
	case Stage::Reference: return "reference_refresh";
	case Stage::Retry: return "retry_wait";
	case Stage::Idle: return "idle";
	case Stage::Complete: return "complete";
	case Stage::Cancelled: return "cancelled";
	case Stage::Failed: return "failed";
	}
	Unexpected("Download trace stage.");
}

} // namespace

void DownloadTrace::setStage(Stage value) {
	auto previous = stage.load(std::memory_order_relaxed);
	while (previous != Stage::Complete
		&& previous != Stage::Cancelled
		&& previous != Stage::Failed) {
		if (stage.compare_exchange_weak(
				previous,
				value,
				std::memory_order_relaxed)) {
			return;
		}
	}
}

void DownloadTrace::finish(Stage value) {
	if (!terminal()) {
		finishedAt = crl::now();
		setStage(value);
	}
}

void DownloadTrace::setError(QString value) {
	error = value.left(80)
		.replace(u'\r', u' ')
		.replace(u'\n', u' ')
		.replace(u'\t', u' ');
}

bool DownloadTrace::terminal() const {
	const auto value = stage.load(std::memory_order_relaxed);
	return value == Stage::Complete
		|| value == Stage::Cancelled
		|| value == Stage::Failed;
}

QString DownloadTrace::snapshot(crl::time now) const {
	const auto until = finishedAt ? finishedAt : now;
	return u"id=%1 dc=%2 kind=%3 tag=%4 auto=%5 stream=%6 stage=%7 "
		"age_ms=%8 idle_ms=%9 bytes=%10 sent=%11 received=%12 "
		"pending=%13 deferred=%14 oldest_ms=%15 request=%16 priority=%17 "
		"refs=%18 refreshed=%19 ref_wait_ms=%20 retries=%21 retry_ms=%22 "
		"error=%23 first_send_ms=%24 first_byte_ms=%25 "
		"cache_ms=%26 ref_ms=%27 scope=%28"_q
		.arg(qulonglong(id)).arg(dc).arg(kind).arg(cacheTag)
		.arg(automatic).arg(streaming)
		.arg(StageName(stage.load(std::memory_order_relaxed)))
		.arg(qlonglong(until - created)).arg(qlonglong(until - progressAt))
		.arg(qlonglong(bytes)).arg(sent).arg(received).arg(pending)
		.arg(deferred).arg(qlonglong(oldestSent ? now - oldestSent : 0))
		.arg(requestId).arg(priority).arg(references).arg(refreshed)
		.arg(qlonglong(referenceAt ? now - referenceAt : 0)).arg(retries)
		.arg(qlonglong(std::max(retryAt - now, crl::time(0))))
		.arg(error)
		.arg(qlonglong(firstSent ? firstSent - created : -1))
		.arg(qlonglong(firstReceived ? firstReceived - created : -1))
		.arg(qlonglong(cacheMs)).arg(qlonglong(referenceMs))
		.arg(qulonglong(scope));
}

DownloadDiagnostics::DownloadDiagnostics(Fn<QString(int)> queueSnapshot)
: _id(NextDiagnosticId())
, _queueSnapshot(std::move(queueSnapshot))
, _timer([=] { check(); }) {
}

std::shared_ptr<DownloadTrace> DownloadDiagnostics::create() {
	auto result = std::make_shared<DownloadTrace>();
	result->id = NextDiagnosticId();
	result->scope = _id;
	watch(result);
	return result;
}

void DownloadDiagnostics::watch(const std::shared_ptr<DownloadTrace> &trace) {
	if (_traces.contains(trace->id)) {
		return;
	}
	if (_traces.size() < kTraceLimit) {
		_traces.emplace(trace->id, trace);
		++_started;
	} else {
		++_untracked;
	}
	if (!_timer.isActive()) {
		_timer.callEach(kCheckInterval);
	}
}

bool DownloadDiagnostics::report(
		const DownloadTrace &trace,
		const char *event,
		crl::time now) {
	if (!PermitLog(false, now)) {
		++_suppressed;
		return false;
	}
	LOG(("Media load: event=%1 %2 queue={%3}")
		.arg(event)
		.arg(trace.snapshot(now))
		.arg(_queueSnapshot(trace.dc)));
	return true;
}

void DownloadDiagnostics::check() {
	const auto now = crl::now();
	auto stalled = 0;
	for (auto i = _traces.begin(); i != _traces.end();) {
		const auto &trace = *i->second;
		const auto stage = trace.stage.load(std::memory_order_relaxed);
		if (stage == DownloadTrace::Stage::Idle && !trace.pending && !trace.deferred) {
			i = _traces.erase(i);
			continue;
		}
		if (trace.terminal()) {
			if (stage == DownloadTrace::Stage::Complete) {
				++_completed;
				auto &sample = _successSamples[{
					trace.dc,
					trace.kind,
					trace.streaming,
				}];
				if (trace.reportedAt || !sample || now >= sample + kReportInterval) {
					if (report(trace, "complete", now)) {
						sample = now;
					}
				}
			} else if (stage == DownloadTrace::Stage::Failed) {
				++_failed;
				report(trace, "failed", now);
			} else {
				++_cancelled;
				if (trace.reportedAt) {
					report(trace, "cancelled", now);
				}
			}
			i = _traces.erase(i);
			continue;
		}
		const auto buffered = trace.streaming
			&& stage == DownloadTrace::Stage::Receiving
			&& !trace.pending
			&& !trace.deferred;
		if (stage != DownloadTrace::Stage::Idle
			&& !buffered
			&& now >= trace.progressAt + kCheckInterval) {
			++stalled;
			if (!trace.reportedAt || now >= trace.reportedAt + kReportInterval) {
				if (report(trace, "stalled", now)) {
					i->second->reportedAt = now;
				}
			}
		} else if (trace.streaming && trace.received
			&& now < trace.progressAt + kCheckInterval) {
			auto &sample = _successSamples[{ trace.dc, trace.kind, true }];
			if ((!sample || now >= sample + kReportInterval)
				&& report(trace, "progress", now)) {
				sample = now;
			}
		}
		++i;
	}
	if (!_summaryAt || now >= _summaryAt + kReportInterval) {
		_summaryAt = now;
		if (PermitLog(true, now)) {
			LOG(("Media load summary: active=%1 stalled=%2 started=%3 "
				"completed=%4 failed=%5 cancelled=%6 untracked=%7 "
				"suppressed=%8 scope=%9")
				.arg(_traces.size()).arg(stalled).arg(_started).arg(_completed)
				.arg(_failed).arg(_cancelled).arg(_untracked).arg(_suppressed)
				.arg(qulonglong(_id)));
			_started = _completed = _failed = _cancelled = 0;
			_untracked = _suppressed = 0;
		}
	}
	if (_traces.empty() && !_started && !_completed && !_failed && !_cancelled
		&& !_untracked && !_suppressed) {
		_timer.cancel();
	}
}

} // namespace Storage
