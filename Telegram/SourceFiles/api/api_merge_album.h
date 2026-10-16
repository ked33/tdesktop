/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_common.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "data/data_types.h"

#include <memory>

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Api {

inline constexpr auto kMergeAlbumToastDuration = crl::time(4000);

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

class SourceLinkTracker final
	: public std::enable_shared_from_this<SourceLinkTracker> {
public:
	SourceLinkTracker(
		not_null<Main::Session*> session,
		const std::vector<not_null<HistoryItem*>> &sources,
		std::shared_ptr<SelectedAction> progress);
	void add(
		SendAction action,
		const MessageIdsList &sources,
		const QVector<MTPlong> &randomIds,
		const MTPUpdates &updates,
		bool merged = false);
	void finish(bool deleteSources = true);

private:
	struct Target {
		SendAction action;
		FullMsgId message;
		MessageIdsList sources;
	};
	void appendRetainedLinks();

	const not_null<Main::Session*> _session;
	const std::shared_ptr<SelectedAction> _progress;
	base::flat_map<FullMsgId, QString> _sources;
	base::flat_set<FullMsgId> _copied;
	base::flat_set<FullMsgId> _deleted;
	std::vector<Target> _targets;

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

[[nodiscard]] TextWithEntities DedupeMergeText(const TextWithEntities &text);
[[nodiscard]] TextWithEntities SummarizeMergeCaptions(
	const std::vector<TextWithEntities> &captions);
[[nodiscard]] QString SourceMessageLink(not_null<HistoryItem*> item);

void SendMergedAlbums(
	SendAction action,
	const std::vector<not_null<HistoryItem*>> &items,
	Fn<void(MergeAlbumResult)> done,
	std::shared_ptr<SourceLinkTracker> sourceLinks = nullptr);

void CleanupMergedSources(
	not_null<Main::Session*> session,
	const MessageIdsList &ids,
	std::shared_ptr<SelectedAction> progress = nullptr,
	Fn<void(const MessageIdsList &)> deleted = nullptr);

} // namespace Api
