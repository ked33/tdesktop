/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_mpv.h"

class DocumentData;
class HistoryItem;

namespace Media::Streaming::MpvSpecial {

[[nodiscard]] bool CanOpenVideoMessageInMpvSpecial(
	HistoryItem *item,
	DocumentData *document);
[[nodiscard]] Mpv::OpenResult OpenVideoMessageInMpvSpecial(
	HistoryItem *item,
	DocumentData *document);

} // namespace Media::Streaming::MpvSpecial
