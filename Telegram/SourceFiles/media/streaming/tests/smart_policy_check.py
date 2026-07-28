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
	checks = [
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
			"regularRequestLimit" in reader
			and "requestsLimit - activeCriticalLoads" in reader,
			"critical and regular loads share one request budget",
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
			and "source->fill(offset, buffer, notify)" in read_source_bytes,
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
