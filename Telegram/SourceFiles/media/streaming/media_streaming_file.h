/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_common.h"
#include "media/streaming/media_streaming_source.h"
#include "media/streaming/media_streaming_utility.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "base/bytes.h"
#include "base/weak_ptr.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace Media {
namespace Streaming {

class FileDelegate;
class Mp4SeekMapCache;
class Reader;

struct StartOptions {
	crl::time position = 0;
	crl::time durationOverride = 0;
	bool seekable = true;
	bool hwAllow = false;
	bool sequentialOpen = false;
};

// Survives soft-seek context rebuild so we can seek before re-opening codecs.
struct SoftSeekStreamCache {
	int videoIndex = -1;
	int audioIndex = -1;
	crl::time videoDuration = 0;
	crl::time audioDuration = 0;
	AVRational videoTimeBase = { 0, 1 };
	AVRational audioTimeBase = { 0, 1 };

	[[nodiscard]] bool usable() const {
		return (videoIndex >= 0) || (audioIndex >= 0);
	}
};

class File final {
public:
	explicit File(std::shared_ptr<FileSource> source);
	explicit File(std::shared_ptr<Reader> reader);

	File(const File &other) = delete;
	File &operator=(const File &other) = delete;

	void start(not_null<FileDelegate*> delegate, StartOptions options);
	[[nodiscard]] bool canSoftSeek() const;
	[[nodiscard]] bool canInPlaceSoftSeek() const;
	[[nodiscard]] uint64_t requestInPlaceSoftSeek(StartOptions options);
	void releaseSoftSeekTrackBarrier(uint64_t generation);
	[[nodiscard]] FFmpeg::FormatPointer detachFormatForSoftSeek(
		crl::time position = 0);
	void resumeSoftSeek(
		not_null<FileDelegate*> delegate,
		FFmpeg::FormatPointer format,
		StartOptions options);
	void wake();
	void stop(bool stillActive = false);

	[[nodiscard]] bool isRemoteLoader() const;
	[[nodiscard]] bool smartStreamingEnabled() const;
	[[nodiscard]] crl::time smartStreamingRecoveryBuffer() const;
	void setLoaderPriority(int priority);
	void setSmartStreamingBufferPressure(bool pressure);
	void setSmartStreamingPlaybackRate(int bytesPerSecond);
	void notifySmartStreamingSeek();

	[[nodiscard]] int64 size() const;
	[[nodiscard]] rpl::producer<SpeedEstimate> speedEstimate() const;

	~File();

private:
	class Context final : public base::has_weak_ptr {
	public:
		Context(
			not_null<FileDelegate*> delegate,
			not_null<FileSource*> source,
			not_null<Mp4SeekMapCache*> seekMapCache);
		~Context();

		void start(StartOptions options);
		void startWithFormat(
			FFmpeg::FormatPointer format,
			StartOptions options);
		void readNextPacket();
		[[nodiscard]] bool readyForSoftSeek() const;
		[[nodiscard]] bool canInPlaceSoftSeek() const;
		[[nodiscard]] uint64_t requestSoftSeek(StartOptions options);
		void releaseSoftSeekTrackBarrier(uint64_t generation);
		[[nodiscard]] bool hasPendingSoftSeek() const;
		[[nodiscard]] bool applyPendingSoftSeekIfAny();
		[[nodiscard]] FFmpeg::FormatPointer takeFormat();
		[[nodiscard]] SoftSeekStreamCache streamCache() const;
		void setStreamCache(const SoftSeekStreamCache &cache);

		void interrupt();
		void wake();
		void waitWhileIdleAtEnd();
		[[nodiscard]] bool interrupted() const;
		[[nodiscard]] bool failed() const;
		[[nodiscard]] bool finished() const;

		void stopStreamingAsync();

	private:
		enum class SleepPolicy {
			Allowed,
			Disallowed,
		};
		static int Read(void *opaque, uint8_t *buffer, int bufferSize);
		static int64_t Seek(void *opaque, int64_t offset, int whence);

		[[nodiscard]] int read(bytes::span buffer);
		[[nodiscard]] int64_t seek(int64_t offset, int whence);
		void prefetchAroundOffset(
			int64 offset,
			crl::time position,
			bool mapped);

		[[nodiscard]] bool unroll() const;
		void logError(QLatin1String method);
		void logError(QLatin1String method, FFmpeg::AvErrorWrap error);
		void logFatal(QLatin1String method);
		void logFatal(QLatin1String method, FFmpeg::AvErrorWrap error);
		void fail(Error error);

		[[nodiscard]] Stream initStream(
			not_null<AVFormatContext *> format,
			AVMediaType type,
			Mode mode,
			StartOptions options,
			int preferredIndex = -1);
		void seekToPosition(
			not_null<AVFormatContext *> format,
			const Stream &stream,
			StartOptions options,
			crl::time position);
		void rememberStreams(const Stream &video, const Stream &audio);
		bool seekUsingCache(
			not_null<AVFormatContext*> format,
			StartOptions options);

		// TODO base::expected.
		[[nodiscard]] auto readPacket()
		-> std::variant<FFmpeg::Packet, FFmpeg::AvErrorWrap>;
		void processQueuedPackets(SleepPolicy policy);

		void handleEndOfFile();
		void sendFullInCache(bool force = false);
		[[nodiscard]] bool reopenCodecsAfterSoftSeek(StartOptions options);

		const not_null<FileDelegate*> _delegate;
		const not_null<FileSource*> _source;
		const not_null<Mp4SeekMapCache*> _seekMapCache;

		base::flat_map<int, std::vector<FFmpeg::Packet>> _queuedPackets;
		int64 _offset = 0;
		int64 _size = 0;
		crl::time _pendingSeekPrefetchPosition = 0;
		bool _failed = false;
		bool _readTillEnd = false;
		int _debugReadCalls = 0;
		int _debugWaitingCount = 0;
		std::optional<bool> _fullInCache;
		crl::semaphore _semaphore;
		std::atomic<bool> _interrupted = false;
		std::atomic<bool> _avioAbortForSoftSeek = false;
		SoftSeekStreamCache _streamCache;

		std::mutex _softSeekMutex;
		StartOptions _softSeekOptions;
		std::atomic<uint64_t> _softSeekRequestGen = 0;
		std::atomic<uint64_t> _softSeekAppliedGen = 0;
		std::atomic<uint64_t> _softSeekBarrierGen = 0;
		uint64_t _softSeekHandledGen = 0;
		crl::semaphore _softSeekBarrier;
		crl::time _softSeekRequestStarted = 0;

		FFmpeg::FormatPointer _format;

	};

	void primeSoftSeekPrefetch(crl::time position);

	std::optional<Context> _context;
	std::shared_ptr<FileSource> _source;
	std::unique_ptr<Mp4SeekMapCache> _seekMapCache;
	SoftSeekStreamCache _streamCache;
	std::thread _thread;

};

} // namespace Streaming
} // namespace Media
