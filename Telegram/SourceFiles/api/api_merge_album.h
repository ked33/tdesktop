/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_common.h"
#include "data/data_types.h"

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Api {

inline constexpr auto kMergeAlbumToastDuration = crl::time(3000);

enum class MergeAlbumKind : uchar {
	Skip,
	PhotoVideo,
	Music,
	File,
};

struct MergeAlbumGroup {
	MergeAlbumKind kind = MergeAlbumKind::Skip;
	int from = 0;
	int till = 0;
};

struct MergeAlbumResult {
	int albumCount = 0;
	int sentMedia = 0;
	int skipped = 0;
	QString error;
	MessageIdsList sentSourceIds;
};

struct MergeAlbumCleanup {
	int deleted = 0;
	int kept = 0;
};

[[nodiscard]] MergeAlbumKind ClassifyMergeAlbumKind(
	not_null<HistoryItem*> item);

[[nodiscard]] std::vector<MergeAlbumGroup> PackAlbumGroups(
	const std::vector<MergeAlbumKind> &kinds,
	int maxItems = 10);
[[nodiscard]] std::vector<MergeAlbumGroup> PackAlbumGroups(
	const std::vector<MergeAlbumKind> &kinds,
	const std::vector<PeerId> &sourcePeers,
	int maxItems = 10);

[[nodiscard]] QString DedupeMergeText(const QString &text);
[[nodiscard]] QString SummarizeMergeCaptions(
	const std::vector<QString> &captions);

void SendMergedAlbums(
	SendAction action,
	const std::vector<not_null<HistoryItem*>> &items,
	Fn<void(MergeAlbumResult)> done);

MergeAlbumCleanup CleanupMergedSources(
	not_null<Main::Session*> session,
	const MessageIdsList &ids);

} // namespace Api
