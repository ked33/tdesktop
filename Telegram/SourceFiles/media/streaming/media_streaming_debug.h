#pragma once

#include "media/streaming/media_streaming_common.h"
#include "logs.h"
#include "settings.h"

#include <QtCore/QString>

#include <optional>

namespace Media::Streaming {

[[nodiscard]] inline bool PlaybackDebugLogsEnabled() {
	return GetEnhancedBool("online_playback_debug_logs")
		|| GetEnhancedBool("mpv_streaming_debug_logs");
}

[[nodiscard]] inline bool PlaybackVerboseDebugLogsEnabled() {
	return GetEnhancedBool("mpv_streaming_debug_logs");
}

[[nodiscard]] inline int PlaybackDebugBoostLevel() {
	const auto boost = GetEnhancedInt("net_download_speed_boost");
	return (boost < 0) ? 0 : (boost > 6) ? 6 : boost;
}

[[nodiscard]] inline QString PlaybackModeDebugString(Mode mode) {
	switch (mode) {
	case Mode::Both: return QStringLiteral("Both");
	case Mode::Audio: return QStringLiteral("Audio");
	case Mode::Video: return QStringLiteral("Video");
	case Mode::Inspection: return QStringLiteral("Inspection");
	}
	return QStringLiteral("Unknown(%1)").arg(int(mode));
}

[[nodiscard]] inline QString PlaybackFrameFormatDebugString(
		FrameFormat format) {
	switch (format) {
	case FrameFormat::None: return QStringLiteral("None");
	case FrameFormat::ARGB32: return QStringLiteral("ARGB32");
	case FrameFormat::YUV420: return QStringLiteral("YUV420");
	case FrameFormat::NV12: return QStringLiteral("NV12");
	}
	return QStringLiteral("Unknown(%1)").arg(int(format));
}

[[nodiscard]] inline QString PlaybackTrackStateDebugString(
		const TrackState &state) {
	return QStringLiteral("%1/%2/%3"
	).arg(qlonglong(state.position)
	).arg(qlonglong(state.receivedTill)
	).arg(qlonglong(state.duration));
}

[[nodiscard]] inline QString PlaybackErrorDebugString(
		std::optional<Error> error) {
	if (!error) {
		return QStringLiteral("none");
	}
	switch (*error) {
	case Error::OpenFailed: return QStringLiteral("OpenFailed");
	case Error::LoadFailed: return QStringLiteral("LoadFailed");
	case Error::InvalidData: return QStringLiteral("InvalidData");
	case Error::NotStreamable: return QStringLiteral("NotStreamable");
	}
	return QStringLiteral("Unknown(%1)").arg(int(*error));
}

[[nodiscard]] inline QString PlaybackErrorDebugString(Error error) {
	return PlaybackErrorDebugString(std::optional<Error>(error));
}

} // namespace Media::Streaming

#define VIDEO_PLAYBACK_DEBUG_LOG(expr) \
	do { \
		if (::Media::Streaming::PlaybackDebugLogsEnabled()) { \
			LOG(expr); \
		} \
	} while (false)

#define VIDEO_PLAYBACK_VERBOSE_LOG(expr) \
	do { \
		if (::Media::Streaming::PlaybackVerboseDebugLogsEnabled()) { \
			LOG(expr); \
		} \
	} while (false)
