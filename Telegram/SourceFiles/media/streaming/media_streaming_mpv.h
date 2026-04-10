/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class DocumentData;
class HistoryItem;

namespace Media::Streaming::Mpv {

enum class OpenResult {
	Success,
	Unsupported,
	PlayerNotFound,
	Failed,
};

[[nodiscard]] bool CanOpenVideoMessageInMpv(
	HistoryItem *item,
	DocumentData *document);
[[nodiscard]] OpenResult OpenVideoMessageInMpv(
	HistoryItem *item,
	DocumentData *document);
[[nodiscard]] OpenResult OpenVideoMessageInMpvLocalTemp(
	HistoryItem *item,
	DocumentData *document);

} // namespace Media::Streaming::Mpv
