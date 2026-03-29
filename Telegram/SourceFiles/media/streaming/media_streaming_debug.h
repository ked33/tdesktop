#pragma once

#include "media/streaming/media_streaming_common.h"
#include "logs.h"
#include "settings.h"

#include <QtCore/QString>

#include <optional>

namespace Media::Streaming {

[[nodiscard]] inline bool PlaybackDebugLogsEnabled() {
	return GetEnhancedBool("mpv_streaming_debug_logs");
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
