/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "ui/userpic_view.h"

namespace Main {
class Session;
} // namespace Main

namespace Data {

class Thread;
class Forum;

class RecentPeers final : public base::has_weak_ptr {
public:
	explicit RecentPeers(not_null<Main::Session*> session);
	~RecentPeers();

	[[nodiscard]] const std::vector<not_null<PeerData*>> &list() const;
	[[nodiscard]] rpl::producer<> updates() const;

	void remove(not_null<PeerData*> peer);
	void bump(not_null<PeerData*> peer);
	void clear();

	[[nodiscard]] QByteArray serialize() const;
	void applyLocal(QByteArray serialized);

	[[nodiscard]] std::vector<not_null<Thread*>> collectChatOpenHistory();
	[[nodiscard]] rpl::producer<> chatOpenHistoryChanges() const;
	void chatOpenPush(not_null<Thread*> thread);
	void chatOpenRemove(not_null<Thread*> thread);
	void chatOpenRemove(PeerId peer, MsgId topicRootId, PeerId sublistPeer);
	void chatOpenKeepUserpics(
		base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView> userpics);

private:
	struct OpenEntry {
		PeerId peer = 0;
		MsgId topicRootId = 0;
		PeerId sublistPeer = 0;
		base::weak_ptr<Thread> thread;
		Forum *requestedForum = nullptr;
	};

	void readChatOpenHistory();
	void saveChatOpenHistory(bool flush = false);
	void trimChatOpenHistory();
	void resolveChatOpenEntry(OpenEntry &entry);
	void pruneChatOpenUserpics();

	const not_null<Main::Session*> _session;

	std::vector<not_null<PeerData*>> _list;
	// Weak: a ForumTopic or SavedSublist can be destroyed without an
	// applyTopicDeleted() / applySublistDeleted() call.
	std::vector<OpenEntry> _opens;
	bool _opensRead = false;
	base::flat_map<
		not_null<PeerData*>,
		Ui::PeerUserpicView> _chatOpenUserpicsCache;

	rpl::event_stream<> _updates;
	rpl::event_stream<> _chatOpenUpdates;
	rpl::lifetime _lifetime;

};

} // namespace Data
