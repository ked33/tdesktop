/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"
#include "media/streaming/media_streaming_common.h"

#include <memory>
#include <utility>

namespace Media::Streaming {

void RefreshPlaybackDiagnosticsSettings();

class TransferDiagnostics final {
public:
	explicit TransferDiagnostics(int64 size, int dcId);
	~TransferDiagnostics();

	[[nodiscard]] uint64 id() const;
	[[nodiscard]] std::pair<int64, int64> byteTotals() const;
	void demuxSeekStarted(uint64 generation, crl::time position);
	void demuxPacket(
		uint64 generation,
		bool video,
		crl::time position,
		int64 offset,
		int size,
		bool keyframe);
	void queued(int64 offset);
	void dispatched(int64 offset);
	void received(int64 offset, int64 bytes);
	void reused(int64 bytes);
	void cancelled(int64 offset, bool sent);
	void retainedForSeek(int64 offset);
	void cancelAll();
	void read(int64 offset, int64 bytes, bool success, bool cacheWait);
	void readStopped();
	void cacheLoaded(const base::flat_map<uint32, QByteArray> &parts);
	void pressureRequested(bool pressure);
	void pressureForwarded(bool local, bool forwarded);
	void policy(int preloadParts, int requestLimit, int playbackRate);
	void readPlan(
		int64 firstMissing,
		int missingParts,
		int criticalPendingParts,
		int prefetchSlots);
	void serverDelay(ServerDelay delay);
	void speed(SpeedEstimate estimate);
	[[nodiscard]] QString snapshot(crl::time now);
	void report(const char *reason);
	void bridgeOpened(uint64 documentId, bool special);

private:
	friend class BridgeRequestDiagnostics;
	struct Impl;
	const std::unique_ptr<Impl> _impl;

};

class PlaybackDiagnostics final {
public:
	explicit PlaybackDiagnostics(
		std::shared_ptr<TransferDiagnostics> transfer,
		bool remote);
	~PlaybackDiagnostics();

	[[nodiscard]] uint64 id() const;
	void requested(
		const PlaybackOptions &options,
		bool seek,
		crl::time startedAt);
	void generation(uint64 generation);
	void ready(const Information &information);
	void sample(
		const Information &information,
		float64 speed,
		bool pausedByUser,
		bool waiting);
	void frameDisplayed(crl::time position, bool waitForShown);
	void frameShown();
	void audioProgress(crl::time position);
	void finish(const char *reason);

private:
	struct Impl;
	const std::unique_ptr<Impl> _impl;

};

class BridgeRequestDiagnostics final {
public:
	BridgeRequestDiagnostics(
		std::shared_ptr<TransferDiagnostics> transfer,
		bool special,
		int64 offset,
		int64 length);
	~BridgeRequestDiagnostics();

	void useReader(
		std::shared_ptr<TransferDiagnostics> transfer,
		uint64 generation,
		uint64 smartGeneration = 0);
	void readStarted();
	void readLocked();
	void readFinished();
	void wrote(int64 bytes);
	void backgroundRead(int64 bytes);
	void outcome(const char *reason);

private:
	void report(bool completed);

	struct Impl;
	const std::unique_ptr<Impl> _impl;

};

} // namespace Media::Streaming
