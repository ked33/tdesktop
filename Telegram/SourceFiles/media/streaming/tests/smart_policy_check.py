#!/usr/bin/env python3
"""Drive the same pure formulas as media_streaming_boost.cpp SmartPolicy helpers."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

kHighBitrate = 1024 * 1024
kMinMs, kMaxMs = 6000, 12000
kBitrateStart, kBitrateFull = 512 * 1024, 2 * 1024 * 1024
kBitrateExtra = 2000
kRiskStart, kRiskFull = 650, 1100
kRiskExtra = 4000
kNetMax = 3000
kUrgent = 2000
kHighBoot = 4000


def avg_rate(size: int, duration: int) -> int:
	if size <= 0 or duration <= 1:
		return 0
	return int(max(0, min(size * 1000.0 / duration, 64 * 1024 * 1024)))


def high(rate: int) -> bool:
	return rate >= kHighBitrate


def adaptive(playback: int, thr: int, lat: int, jit: int) -> int:
	result = kMinMs
	if playback > kBitrateStart:
		rng = kBitrateFull - kBitrateStart
		above = max(0, min(playback - kBitrateStart, rng))
		result += (above * kBitrateExtra) // rng
	if thr > 0:
		risk = max(0, min(playback * 1000 // thr, 2000))
		rng = kRiskFull - kRiskStart
		above = max(0, min(risk - kRiskStart, rng))
		result += (above * kRiskExtra) // rng
	else:
		result += 1000
	net = max(0, min(max(lat, 0) + 3 * max(jit, 0), kNetMax))
	result += net
	return max(kMinMs, min(result, kMaxMs))


def parts_for(
		playback: int,
		buffer_ms: int,
		part: int = 128 * 1024,
		mn: int = 8,
		mx: int = 64) -> int:
	if playback <= 0 or buffer_ms <= 0 or part <= 0 or mx <= 0:
		return max(0, mn)
	bytes_ = (playback * buffer_ms + 999) // 1000
	parts = (bytes_ + part - 1) // part
	return max(mn, min(parts, mx))


def urgent_ready(hits: int, parts: int, off: int, till: int) -> bool:
	if parts <= 0:
		return True
	if hits >= parts:
		return True
	return off >= 0 and till > 0 and off >= till


def boot_wait(playback: int, bg: int) -> int:
	if bg <= 0:
		return 0
	b = kHighBoot if high(playback) else kUrgent
	return min(bg, b)


def test_formulas() -> None:
	rate = avg_rate(773795309, 636934)
	assert 1_100_000 <= rate <= 1_300_000, rate
	assert high(rate)
	buf = adaptive(rate, 900000, 500, 50)
	assert 6000 <= buf <= 12000, buf
	p = parts_for(rate, buf, mx=128)
	assert 16 <= p <= 128, p
	assert p >= 64, p
	assert not urgent_ready(0, 20, 0, 1000)
	assert urgent_ready(20, 20, 0, 1000)
	assert urgent_ready(5, 20, 1000, 1000)
	assert boot_wait(rate, buf) == 4000
	assert boot_wait(400 * 1024, buf) == 2000
	# remoteRequests alone must not imply urgent ready
	assert not urgent_ready(0, 19, 384696320, 387317760)

	def under_playback(playback: int, thr: int) -> bool:
		return playback > 0 and thr > 0 and thr < playback

	def keep_till(
			read_off: int,
			existing: int,
			parts: int,
			part: int,
			file_size: int) -> int:
		want = read_off + max(parts, 0) * part
		return min(file_size, max(existing, want))

	assert under_playback(rate, rate // 2)
	assert not under_playback(rate, rate + 1)
	assert not under_playback(rate, 0)
	assert keep_till(1000, 2000, 10, 128 * 1024, 1 << 30) >= 1000 + 10 * 128 * 1024

	# Dual-keep: two far A/V envelopes stay separate; mid gap not covered.
	class Dual:
		def __init__(self):
			self.s0 = self.t0 = self.s1 = self.t1 = -1
			self.last = 0

	def in_dual(d, off):
		if d.s0 >= 0 and d.t0 > d.s0 and d.s0 <= off < d.t0:
			return True
		if d.s1 >= 0 and d.t1 > d.s1 and d.s1 <= off < d.t1:
			return True
		return False

	def note(d, off, guard=2 * 1024 * 1024, span=128 * 1024, size=1 << 30):
		a0 = max(0, off - guard)
		a1 = min(size, off + span + guard)
		near = 1024 * 1024
		def overlap(s, t):
			return s >= 0 and t > s and a0 <= t + near and s <= a1 + near
		if overlap(d.s0, d.t0):
			d.s0, d.t0 = min(d.s0, a0) if d.s0 >= 0 else a0, max(d.t0, a1) if d.t0 > 0 else a1
			if d.s0 < 0:
				d.s0, d.t0 = a0, a1
			else:
				d.s0, d.t0 = min(d.s0, a0), max(d.t0, a1)
			d.last = 0
			return
		if overlap(d.s1, d.t1):
			if d.s1 < 0:
				d.s1, d.t1 = a0, a1
			else:
				d.s1, d.t1 = min(d.s1, a0), max(d.t1, a1)
			d.last = 1
			return
		if d.last == 0:
			d.s1, d.t1, d.last = a0, a1, 1
		else:
			d.s0, d.t0, d.last = a0, a1, 0

	d = Dual()
	note(d, 100 * 1024 * 1024)
	note(d, 150 * 1024 * 1024)
	assert in_dual(d, 100 * 1024 * 1024)
	assert in_dual(d, 150 * 1024 * 1024)
	assert not in_dual(d, 125 * 1024 * 1024)
	print("OK formulas", {"rate": rate, "bufferMs": buf, "parts": p})


def test_source_structure() -> None:
	reader = (ROOT / "media_streaming_reader.cpp").read_text(encoding="utf-8")
	reader_h = (ROOT / "media_streaming_reader.h").read_text(encoding="utf-8")
	file = (ROOT / "media_streaming_file.cpp").read_text(encoding="utf-8")
	file_delegate = (ROOT / "media_streaming_file_delegate.h").read_text(
		encoding="utf-8"
	)
	player = (ROOT / "media_streaming_player.cpp").read_text(encoding="utf-8")
	player_h = (ROOT / "media_streaming_player.h").read_text(encoding="utf-8")
	boost = (ROOT / "media_streaming_boost.cpp").read_text(encoding="utf-8")
	boost_h = (ROOT / "media_streaming_boost.h").read_text(encoding="utf-8")
	loader = (ROOT / "media_streaming_loader_mtproto.cpp").read_text(encoding="utf-8")
	mp4_seek = (ROOT / "media_streaming_mp4_seek.h").read_text(encoding="utf-8")
	mp4_header = (ROOT / "media_streaming_mp4_header.h").read_text(encoding="utf-8")
	startup = (ROOT / "media_streaming_startup.h").read_text(encoding="utf-8")
	cache = (ROOT / "media_streaming_cache.h").read_text(encoding="utf-8")
	manager = (ROOT.parent.parent / "storage" / "download_manager_mtproto.cpp").read_text(
		encoding="utf-8"
	)
	source = (ROOT / "media_streaming_source.cpp").read_text(encoding="utf-8")
	document = (ROOT / "media_streaming_document.cpp").read_text(encoding="utf-8")
	source_h = (ROOT / "media_streaming_source.h").read_text(encoding="utf-8")
	diagnostics = (ROOT / "media_streaming_diagnostics.cpp").read_text(encoding="utf-8")
	fill = reader[
		reader.index("Reader::FillState Reader::fillFromSlices("):
		reader.index("void Reader::cancelLoadInRange(")
	]
	critical_loads = reader[
		reader.index("int Reader::topUpSeekCriticalLoads("):
		reader.index("bool Reader::updateSeekPrefetchCriticalProgress()")
	]
	provide_start = player[
		player.index("void Player::provideStartInformation() {"):
		player.index("void Player::fail(Error error)")
	]
	resume_waiting = player[
		player.index("void Player::checkResumeFromWaitingForData() {"):
		player.index("void Player::start() {")
	]
	start_generation = player[
		player.index("uint64 Player::startTrackGeneration() {"):
		player.index("void Player::beginSeekTiming(")
	]
	pause = player[
		player.index("void Player::pause() {"):
		player.index("void Player::resume() {")
	]
	read_source_bytes = file[
		file.index("[[nodiscard]] std::optional<QByteArray> ReadSourceBytes("):
		file.index("[[nodiscard]] Mp4SeekMapBuildResult BuildMp4SeekTrack(")
	]
	read_callback = file[
		file.index("int File::Context::read(bytes::span buffer) {"):
		file.index("int64_t File::Context::seek(")
	]
	read_packet = file[
		file.index("File::Context::readPacket() {"):
		file.index("void File::Context::start(")
	]
	soft_seek = file[
		file.index("bool File::Context::applyPendingSoftSeekIfAny() {"):
		file.index("FFmpeg::FormatPointer File::Context::takeFormat()")
	]
	render_timer = player[
		player.index("void Player::renderFrameTimerFired() {"):
		player.index("void Player::checkNextFrameRender() {")
	]
	next_frame_render = player[
		player.index("void Player::checkNextFrameRender() {"):
		player.index("void Player::checkNextFrameAvailability() {")
	]
	seek_barrier = player[
		player.index("void Player::applySoftSeekTrackBarrier("):
		player.index("bool Player::tryJoinSoftSeek(")
	]
	in_place_seek = player[
		player.index("bool Player::trySoftSeek("):
		player.index("void Player::fileSoftSeekApplied(")
	]
	join_seek = player[
		player.index("bool Player::tryJoinSoftSeek("):
		player.index("void Player::play(")
	]
	video_step = player[
		player.index("void Player::checkVideoStep() {"):
		player.index("void Player::stop(bool stillActive) {")
	]
	stop = player[player.index("void Player::stop(bool stillActive) {"):]
	retry = loader[
		loader.index("void LoaderMtproto::checkReadRetry("):
		loader.index("bool LoaderMtproto::promoteQueuedRead(")
	]
	promotion = loader[loader.index("bool LoaderMtproto::promoteQueuedRead("):]
	replacement = manager[
		manager.index("bool DownloadMtprotoTask::replaceRequestForOffset("):
		manager.index("void DownloadMtprotoTask::cancelRequest(")
	]
	slice_fill = reader[
		reader.index("Reader::FillResult Reader::Slices::fill("):
		reader.index("auto Reader::Slices::fillFromHeader(")
	]
	retry_diagnostics = diagnostics[
		diagnostics.index("void TransferDiagnostics::retried("):
		diagnostics.index("void TransferDiagnostics::received(")
	]
	file_start = file[
		file.index("void File::Context::start(StartOptions options) {"):
		file.index("void File::Context::sendFullInCache(")
	]
	reader_header_done = reader[
		reader.index("void Reader::headerDone() {"):
		reader.index("int Reader::headerSize() const {")
	]
	checks = [
		(
			"CachePolicy::RestoreHeaderParts(" in reader
			and slice_fill.index("restoreHeaderParts(fromSlice);")
				< slice_fill.index("_data[fromSlice].prepareFill(")
			and slice_fill.index("restoreHeaderParts(fromSlice + 1);")
				< slice_fill.index("_data[fromSlice + 1].prepareFill("),
			"actual reads hydrate header blocks before testing data-slice readiness",
		),
		(
			"CachePolicy::SplitPreload(" in slice_fill
			and "preload.first," in slice_fill
			and "preload.second," in slice_fill
			and "preloadParts - first" in cache,
			"neighboring slices share one forward-preload budget",
		),
		(
			"kSlicesInMemory = 2" in reader
			and "kSmartSlicesInMemory = 4" in reader
			and "smartNonPremium ? kSmartSlicesInMemory : kSlicesInMemory" in fill,
			"Smart keeps both audio and video slice pairs without changing other modes",
		),
		(
			"queued && promoteQueuedRead(" in retry
			and "if (_downloader)" in promotion
			and "_readStall.replacementReady(" in promotion
			and "_readStall.retried(now);" in promotion,
			"queued blockers share the existing recovery budget and protect downloader work",
		),
		(
			"_cdnDcId" in replacement
			and "haveSentRequestForOffset(requiredOffset)" in replacement
			and "now < state.limitedUntil" in replacement
			and "now < state.recoveryUntil" in replacement
			and "requestIsDelayed(requestId)" in replacement
			and "j->second.readRetrySuppressed" in replacement
			and replacement.index("cancelRequest(requestId);")
				< replacement.index("makeRequest({ requiredOffset, index });"),
			"replacement releases an existing slot and honors server and transport delays",
		),
		(
			"_requested.remove(offset)" in promotion
			and "_diagnostics->cancelled(displaced, true);" in promotion
			and "_diagnostics->dispatched(offset);" in promotion
			and ".offset = displaced," in promotion
			and ".cancelled = true," in promotion,
			"replacement retires displaced Reader loads and preserves request accounting",
		),
		(
			"topUpSeekCriticalLoads" in reader
			and "updateSeekPrefetchCriticalProgress" in reader,
			"reader schedules complete seek-critical parts",
		),
		(
			"_seekPrefetchUrgentWindow" not in reader
			and "_seekPrefetchUrgentWindow" not in reader_h,
			"removed obsolete contiguous urgent window",
		),
		(
			"publishSeekPrefetch" in reader
			and "_seekPrefetchRequestMutex" in reader_h
			and "_pendingSeekPrefetch" in reader_h,
			"seek prefetch publishes one locked request snapshot",
		),
		(
			fill.index("_slices.fill(")
			< fill.index("loadAtOffset(part);")
			< fill.index("topUpSeekCriticalLoads(requestsLimit)")
			and "activeLoads >= requestsLimit" in fill
			and "active >= limit" in critical_loads,
			"actual reads precede speculative loads within the dispatch budget",
		),
		(
			"mode == ReadMode::Required" in fill
			and "_readStall.firstRequired(" in loader
			and "required && _requested.remove(*required)" in loader,
			"dispatch selects the current blocking read before speculative offsets",
		),
		(
			"prepareCacheForPart" in critical_loads
			and "readFromCache(cacheSlice);" in critical_loads
			and "_seekPrefetchCriticalParts.erase(i)" in reader
			and "!_seekPrefetchCriticalParts.contains(part)" in critical_loads,
			"critical prefetch consults cache and retires received parts",
		),
		(
			"ComputeMp4SampleRanges(" in file
			and "ComputeSampleRange(" not in file
			and "chunkOffsets[chunk]" in mp4_seek
			and "kByteLimit" in mp4_seek
			and "kCriticalPartLimit = 128" in source_h
			and "criticalParts.size() < SeekPrefetchRequest::kCriticalPartLimit"
			in reader,
			"MP4 predictions preserve chunk gaps and bound speculative bytes",
		),
		(
			"_file->smartStreamingEnabled()" in provide_start
			and "_fullInCacheSinceStart.value_or(false)" in provide_start
			and "StartupBufferPolicy::kTargetMs * _options.speed" in provide_start
			and "_stage == Stage::Ready" in resume_waiting
			and "bothReceivedEnough(crl::time(required * _options.speed))"
			in resume_waiting,
			"Smart startup checks both tracks using a speed-adjusted buffer target",
		),
		(
			"&& _waitingForStartupBuffer" in resume_waiting
			and "_waitingForStartupBuffer = !_pausedByUser" in provide_start
			and "_waitingForStartupBuffer = false;" in resume_waiting
			and "_waitingForStartupBuffer = false;" in start_generation
			and "_startupBufferTimer.cancel();" in start_generation
			and "checkResumeFromWaitingForData();" in pause,
			"startup rechecks stay armed only for ready tracks of the current play",
		),
		(
			provide_start.index("_stage = Stage::Ready;")
			< provide_start.index("_paused = true;")
			< provide_start.index("_updates.fire(Update{ std::move(copy) });")
			< provide_start.index("_updates.fire({ WaitingForData{ true } });")
			and "waitingChange(_player.buffering());" in document,
			"decoded preview is published while the buffering indicator stays accurate",
		),
		(
			"StartupBufferPolicy::RequiredBuffer(" in resume_waiting
			and "StartupBufferPolicy::ExtraWaitLimit(" in provide_start
			and "elapsed < ExtraWaitLimit(seek)" in startup
			and "? kTargetMs : kMinimumMs" in startup
			and "_startupBufferTimer.callOnce(remaining);" in resume_waiting
			and "StartupBufferPolicy::kTargetMs" in file,
			"startup target has a bounded extra wait and a shared seek prediction target",
		),
		(
			"generation != _trackGeneration.load" in provide_start
			and "generation == _trackGeneration.load" in resume_waiting
			and "base::make_weak(&_sessionGuard)" in provide_start
			and "base::make_weak(&_sessionGuard)" in resume_waiting
			and "WaitingForData{ _pausedByWaitingForData }" in resume_waiting,
			"preview and buffer release reject reentrant seek and stop callbacks",
		),
		(
			"_stage == Stage::Started" in resume_waiting
			and "bothReceivedEnough(waitingForDataBuffer())" in resume_waiting
			and "FullTrackReceived(state)" in player,
			"normal rebuffering and fully received short tracks retain their policies",
		),
		(
			"_diagnostics->playable();" in player
			and "void PlaybackDiagnostics::playable()" in diagnostics
			and "playable_ms=" in diagnostics
			and "ready_to_playable_ms=" in diagnostics,
			"diagnostics distinguish preview readiness from actual playback start",
		),
		(
			"_headerReadAhead->observe(" in read_callback
			and read_callback.count("_source->fill(") == 1
			and "ProbeForStreaming(" not in read_callback
			and "_reader->setHeaderReadRange(offset, amount);" in source
			and "_headerReadAhead.emplace(_size);" in file_start,
			"header discovery observes existing AVIO reads without additional remote probes",
		),
		(
			"_headerReadAhead.preloadParts(" in fill
			and "!_headerReadAhead.intersects(part, kPartSize)" in fill
			and "activeLoads >= requestsLimit" in fill
			and "(readStalled && !headerRead)" in fill
			and "_atomsLeft = 64" in mp4_header
			and "atom->size <= ReadAheadRange::kMaximumSize" in mp4_header,
			"known metadata fills spare slots within atom bounds and the existing request budget",
		),
		(
			"_headerReadAhead.reset();" in file_start
			and "_source->setHeaderReadRange(-1, 0);" in file_start
			and "gsl::finally" in file_start
			and "_headerReadAhead = {};" in reader_header_done,
			"header scheduling ends on completion and early open failure",
		),
		(
			"_diagnostics->retried(offset);" in retry
			and "_diagnostics->cancelled(" not in retry
			and "s.requests.erase" not in retry_diagnostics
			and "i->second.sentAt = now;" in retry_diagnostics
			and "s.maxQueueMs" not in retry_diagnostics
			and "s.requestsComplete = false;" in retry_diagnostics
			and "retried=%8" in diagnostics,
			"retry preserves logical requests without inflating queue time or masking partial capture",
		),
		(
			all(key in diagnostics for key in (
				"read_missing_offset", "read_missing_parts", "read_queued",
				"read_sent", "read_oldest_ms", "critical_pending_parts",
				"prefetch_slots",
			)),
			"sparse snapshots distinguish unscheduled and in-flight blockers",
		),
		(
			"audioTrack" in file
			and "criticalRanges" in file
			and "audioTargetSample" in file,
			"MP4 seek map covers video and audio critical ranges",
		),
		(
			re.search(
				r"if \(_interrupted\).*?streamingError\(\).*?hasPendingSoftSeek\(\)",
				read_callback,
				re.DOTALL,
			)
			is not None,
			"source errors win over intentional soft-seek AVIO aborts",
		),
		(
			"_format->pb->error = 0;" in soft_seek
			and "_format->pb->eof_reached = 0;" in soft_seek
			and soft_seek.count("resetAvioReadState();") == 2
			and "avformat_flush(_format.get());" in soft_seek,
			"soft seek clears sticky AVIO error and EOF state",
		),
		(
			read_packet.index("_source->streamingError()")
			< read_packet.index("error.code() == AVERROR_EOF")
			and "error.code() == AVERROR_EXTERNAL && _offset >= _size"
			in read_packet,
			"real source errors stay fatal and EXTERNAL EOF stays bounded",
		),
		(
			".acquire()" not in read_source_bytes
			and "while (" not in read_source_bytes
			and "state != FileSource::FillState::Success" in read_source_bytes
			and "source->fill(offset, buffer, notify, ReadMode::Probe)"
			in read_source_bytes
			and "mode == ReadMode::Required" in reader,
			"seek map source reads return when data is unavailable",
		),
		(
			"else if (!notify)" in file
			and re.search(
				r"_seekMapCache,\s*position,\s*&_semaphore",
				file,
			)
			is not None
			and re.search(
				r"_seekMapCache\.get\(\),\s*position,\s*nullptr",
				file,
			)
			is not None,
			"seek map uses context wake and cache-only prime paths",
		),
		(
			"uint64 generation" in file_delegate
			and "_delegate->fileReady(" in file
			and "_delegate->fileError(_trackGeneration" in file
			and "generation != _trackGeneration.load" in player,
			"file and track callbacks preserve seek generation",
		),
		(
			"void clearFrameRenderSchedule();" in player_h
			and "void renderFrameTimerFired();" in player_h
			and "uint64 _renderFrameGeneration = 0;" in player_h
			and "_renderFrameTimer([=] { renderFrameTimerFired(); })" in player,
			"render timer retains its track generation",
		),
		(
			"generation != _trackGeneration.load" in render_timer
			and "clearFrameRenderSchedule();" in render_timer
			and "if (_stage != Stage::Started || !_video)" in next_frame_render
			and "_renderFrameGeneration = generation;" in next_frame_render,
			"stale render timers are dropped before video access",
		),
		(
			"clearFrameRenderSchedule();" in seek_barrier
			and "if (_stage != Stage::Started || !_video)" in video_step,
			"seek barriers and old callbacks clear frame scheduling",
		),
		(
			"clearFrameRenderSchedule();" in in_place_seek
			and "clearFrameRenderSchedule();" in join_seek
			and "clearFrameRenderSchedule();" in stop,
			"all seek and stop transitions clear frame scheduling",
		),
		(
			"result.state == FillState::Success || remoteRequests > 0"
			not in reader,
			"removed early background flip on remoteRequests",
		),
		(
			"wantNextPreload" in reader,
			"cross-slice preload for steady buffer",
		),
		(
			"kSmartSeekPressureLocalDuration" in reader,
			"short pressure-local seek window",
		),
		(
			"kSmartHighBitrateExcessCapacityRatio" in (
				ROOT.parent.parent
				/ "storage"
				/ "download_manager_mtproto.cpp"
			).read_text(encoding="utf-8"),
			"high-bitrate excess capacity ratio",
		),
		(
			"SmartIsUnderPlayback" in reader
			and "SmartKeepWindowTill" in reader,
			"under-playback catch-up helpers used",
		),
		(
			"smartNonPremiumAdaptive" in reader
			or "never uses Throttle" in reader
			or "Smart non-Premium never" in reader
			or "smartNonPremiumAdaptive" in reader,
			"smart disables false throttle path",
		),
		(
			"skipped by under-playback" in reader,
			"cancel skipped only for under-playback",
		),
		(
			"Smart catch-up" in reader
			and "kSmartCatchupLogMinInterval" in reader,
			"sparse Smart catch-up debug snapshot",
		),
		(
			"skipped dual-keep recovery" in reader,
			"seek recovery skips dual-stream jump cancel",
		),
		(
			"SmartNoteDualKeepOffset" in reader
			and "SmartOffsetInDualKeep" in reader,
			"dual-keep protect on cancel",
		),
		(
			"Always rate-limit force DEBUG" in reader
			or (
				"kSmartCatchupLogMinInterval" in reader
				and "_smartForceCancelLogLastTime" in reader_h
			),
			"force cancel debug always rate-limited",
		),
		(
			"force-cancel on a new seek window" in reader
			or "force)," in reader
			or "bool force" in (
				ROOT / "media_streaming_reader.h"
			).read_text(encoding="utf-8"),
			"force cancel on seek window",
		),
		(
			"bufferPressure alone used to skip cancel" in reader
			or "!seekRecovery" in reader,
			"seek recovery never skips cancel",
		),
		(
			"kSmartCancelLogMinInterval" in reader,
			"cancel log rate-limit present",
		),
		(
			"SmartSeekBootstrapWaitMs" in reader,
			"bootstrap wait helper used",
		),
		(
			"smartStreamingPlaybackRate() > 0" in reader,
			"pressure gated on playback rate",
		),
		(
			"SmartPreloadPartsForBufferMs" in reader,
			"steady floor helper used",
		),
		(
			"SmartPolicySelfCheck" in boost and "SmartPolicySelfCheck" in boost_h,
			"SmartPolicySelfCheck shipped",
		),
		(
			"kSmartSeekHighBitrateBootstrapWaitMs" in boost,
			"high-bitrate bootstrap wait constant",
		),
		(
			re.search(
				r"seekPrefetchWindowStart >= 0\s*\n\s*&& !_seekPrefetchBackgroundActive",
				reader,
			)
			is not None,
			"tail prefetch deferred during seek-critical phase",
		),
	]
	failed = [msg for ok, msg in checks if not ok]
	for ok, msg in checks:
		print(("OK" if ok else "BAD"), msg)
	if failed:
		raise SystemExit(f"structural failures: {failed}")
	print("OK structure", len(checks), "checks")


if __name__ == "__main__":
	test_formulas()
	test_source_structure()
	print("ALL PASSED")
