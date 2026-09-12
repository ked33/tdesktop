/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_porn_search_policy.h"
#include "base/timer.h"
#include "data/data_types.h"
#include "mtproto/sender.h"

#include <array>
#include <limits>
#include <map>
#include <set>

class ApiWrap;

namespace Main {
class Session;
} // namespace Main

namespace Api {

enum class PornSearchFilter {
	All,
	Groups,
	Channels,
};

struct PornSearchRequest {
	QString query;
	PornSearchFilter filter = PornSearchFilter::All;
	bool fromArchive = true;

	friend inline bool operator==(
		const PornSearchRequest &,
		const PornSearchRequest &) = default;
};

struct PornSearchResult {
	MessageIdsList messages;
	int searched = 0;
	int total = 0;
	int failed = 0;
	bool totalKnown = false;
	bool scanning = false;
	bool loading = false;
	bool waiting = false;
	bool more = false;
};

class PornSearch final {
public:
	using QueryId = uint64;
	using Callback = Fn<void(QueryId, PornSearchResult)>;

	explicit PornSearch(not_null<ApiWrap*> api);
	~PornSearch();

	[[nodiscard]] QueryId start(PornSearchRequest request, Callback done);
	void cancel(QueryId id);
	void loadMore(QueryId id, TimeId before);
	void retry(QueryId id);
	void invalidate();
	void applyFloodWait(const MTP::Error &error);
	[[nodiscard]] crl::time floodWaitRemaining() const;
	[[nodiscard]] crl::time takeFloodWaitNotice();

private:
	using TaskId = uint64;

	struct Source {
		PeerId channel;
		MsgId offsetId = 0;
		TimeId oldestDate = std::numeric_limits<TimeId>::max();
		std::set<FullMsgId> messages;
		TaskId task = 0;
		bool started = false;
		bool exhausted = false;
		bool failed = false;
		bool retry = false;
	};

	struct Query {
		PornSearchRequest request;
		Callback done;
		std::map<PeerId, Source> sources;
		TimeId before = std::numeric_limits<TimeId>::max();
		bool dirty = true;
	};

	struct Folder {
		PeerId offsetPeer;
		MsgId offsetId = 0;
		TimeId offsetDate = 0;
		std::set<PeerId> seen;
		TaskId dialogsTask = 0;
		TaskId pinnedTask = 0;
		bool dialogsDone = false;
		bool pinnedDone = false;
		bool dialogsFailed = false;
		bool pinnedFailed = false;
	};

	enum class TaskType {
		Dialogs,
		Pinned,
		Metadata,
		Search,
	};

	struct Task {
		TaskType type = TaskType::Dialogs;
		QueryId query = 0;
		PeerId peer;
		int folder = 0;
		std::vector<PeerId> peers;
		mtpRequestId requestId = 0;
	};

	void schedule();
	void pump();
	void publish();
	void reconcile();
	void checkFolder(int folder);
	void rememberDialogs(int folder, const QVector<MTPDialog> &dialogs);
	void refreshMetadata();
	void cancelTask(TaskId id);
	void cancelCatalogTasks();
	void sendDialogs(int folder, bool pinned);
	void sendMetadata(std::vector<PeerId> peers);
	void sendSearch(QueryId query, PeerId peer);
	[[nodiscard]] bool sendReadySearch(bool firstPage);
	void searchReceived(TaskId task, const MTPmessages_Messages &result);
	void taskFailed(TaskId task, const MTP::Error &error);
	[[nodiscard]] bool folderNeeded(int folder) const;
	[[nodiscard]] bool metadataNeeded(PeerId peer) const;
	[[nodiscard]] PornSearchResult resultFor(const Query &query) const;

	const not_null<Main::Session*> _session;
	MTP::Sender _api;
	base::Timer _timer;
	std::array<Folder, 2> _folders;
	std::map<PeerId, int> _catalog;
	std::set<PeerId> _metadataPending;
	std::set<PeerId> _metadataFailed;
	std::map<QueryId, Query> _queries;
	std::map<TaskId, Task> _tasks;
	QueryId _nextQuery = 0;
	QueryId _lastQuery = 0;
	TaskId _nextTask = 0;
	PornSearchPolicy::RequestGate _requestGate;
	rpl::lifetime _lifetime;

};

} // namespace Api
