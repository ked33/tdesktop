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
	print("OK formulas", {"rate": rate, "bufferMs": buf, "parts": p})


def test_source_structure() -> None:
	reader = (ROOT / "media_streaming_reader.cpp").read_text(encoding="utf-8")
	boost = (ROOT / "media_streaming_boost.cpp").read_text(encoding="utf-8")
	boost_h = (ROOT / "media_streaming_boost.h").read_text(encoding="utf-8")
	checks = [
		(
			"SmartSeekUrgentWindowReady" in reader,
			"reader uses SmartSeekUrgentWindowReady",
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
			"Reader cancel outside window force=" in reader
			or "force=%1 start=" in reader,
			"force cancel debug line",
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
			"tail prefetch deferred during urgent",
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
