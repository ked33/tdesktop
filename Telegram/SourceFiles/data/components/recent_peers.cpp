/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/components/recent_peers.h"

#include "core/version.h"
#include "core/enhanced_settings.h"
#include "data/data_changes.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_saved_messages.h"
#include "data/data_saved_sublist.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "main/main_session.h"
#include "storage/serialize_common.h"
#include "storage/serialize_peer.h"
#include "storage/storage_account.h"

#include <crl/crl_on_main.h>

namespace Data {
namespace {

constexpr auto kLimit = 48;
constexpr auto kOpenHistoryVersion = quint32(1);
constexpr auto kMaxOpenHistoryBytes = 4 * 1024 * 1024;
constexpr auto kOpenHistoryKey = std::string_view("recent_open_chats");

} // namespace

RecentPeers::RecentPeers(not_null<Main::Session*> session)
: _session(session) {
	EnhancedSettings::RecentChatsLimitChanges(
	) | rpl::on_next([=] {
		readChatOpenHistory();
		trimChatOpenHistory();
	}, _lifetime);
}

RecentPeers::~RecentPeers() {
	invalidate_weak_ptrs(this);
}

const std::vector<not_null<PeerData*>> &RecentPeers::list() const {
	_session->local().readSearchSuggestions();

	return _list;
}

rpl::producer<> RecentPeers::updates() const {
	return _updates.events();
}

void RecentPeers::remove(not_null<PeerData*> peer) {
	const auto i = ranges::find(_list, peer);
	if (i != end(_list)) {
		_list.erase(i);
		_updates.fire({});
	}
	_session->local().writeSearchSuggestionsDelayed();
}

void RecentPeers::bump(not_null<PeerData*> peer) {
	_session->local().readSearchSuggestions();

	if (!_list.empty() && _list.front() == peer) {
		return;
	}
	auto i = ranges::find(_list, peer);
	if (i == end(_list)) {
		_list.push_back(peer);
		i = end(_list) - 1;
	}
	ranges::rotate(begin(_list), i, i + 1);
	_updates.fire({});

	_session->local().writeSearchSuggestionsDelayed();
}

void RecentPeers::clear() {
	_session->local().readSearchSuggestions();

	_list.clear();
	_updates.fire({});

	_session->local().writeSearchSuggestionsDelayed();
}

QByteArray RecentPeers::serialize() const {
	_session->local().readSearchSuggestions();

	if (_list.empty()) {
		return {};
	}
	auto size = 2 * sizeof(quint32); // AppVersion, count
	const auto count = std::min(int(_list.size()), kLimit);
	auto &&list = _list | ranges::views::take(count);
	for (const auto &peer : list) {
		size += Serialize::peerSize(peer);
	}
	auto stream = Serialize::ByteArrayWriter(size);
	stream
		<< quint32(AppVersion)
		<< quint32(count);
	for (const auto &peer : list) {
		Serialize::writePeer(stream, peer);
	}
	return std::move(stream).result();
}

void RecentPeers::applyLocal(QByteArray serialized) {
	_list.clear();
	if (serialized.isEmpty()) {
		DEBUG_LOG(("Suggestions: Bad RecentPeers local, empty."));
		return;
	}
	auto stream = Serialize::ByteArrayReader(serialized);
	auto streamAppVersion = quint32();
	auto count = quint32();
	stream >> streamAppVersion >> count;
	if (!stream.ok()) {
		DEBUG_LOG(("Suggestions: Bad RecentPeers local, not ok."));
		return;
	}
	DEBUG_LOG(("Suggestions: "
		"Start RecentPeers read, count: %1, version: %2."
		).arg(count
		).arg(streamAppVersion));
	_list.reserve(count);
	for (auto i = 0; i != int(count); ++i) {
		const auto streamPosition = stream.underlying().device()->pos();
		const auto peer = Serialize::readPeer(
			_session,
			streamAppVersion,
			stream);
		if (stream.ok() && peer) {
			_list.push_back(peer);
		} else {
			_list.clear();
			DEBUG_LOG(("Suggestions: Failed RecentPeers reading %1 / %2."
				).arg(i + 1
				).arg(count));
			DEBUG_LOG(("Failed bytes: %1.").arg(
				QString::fromUtf8(serialized.mid(streamPosition).toHex())));
			return;
		}
	}
	DEBUG_LOG(
		("Suggestions: RecentPeers read OK, count: %1").arg(_list.size()));
}

std::vector<not_null<Thread*>> RecentPeers::collectChatOpenHistory() {
	readChatOpenHistory();
	auto result = std::vector<not_null<Thread*>>();
	result.reserve(_opens.size());
	for (auto &entry : _opens) {
		if (!entry.thread) {
			resolveChatOpenEntry(entry);
		}
		if (const auto thread = entry.thread.get()) {
			result.push_back(thread);
		}
	}
	return result;
}

rpl::producer<> RecentPeers::chatOpenHistoryChanges() const {
	return _chatOpenUpdates.events();
}

void RecentPeers::readChatOpenHistory() {
	if (std::exchange(_opensRead, true)) {
		return;
	}
	_session->changes().peerUpdates(
		PeerUpdate::Flag::FullInfo | PeerUpdate::Flag::Name
	) | rpl::on_next([=](const PeerUpdate &update) {
		if (ranges::any_of(_opens, [&](const OpenEntry &entry) {
			return entry.peer == update.peer->id && !entry.thread;
		})) {
			_chatOpenUpdates.fire({});
		}
	}, _lifetime);
	const auto bytes = _session->local().readPref<QByteArray>(kOpenHistoryKey);
	if (bytes.isEmpty() || bytes.size() > kMaxOpenHistoryBytes) {
		return;
	}
	auto stream = Serialize::ByteArrayReader(bytes);
	auto version = quint32();
	auto appVersion = quint32();
	auto count = quint32();
	auto account = quint64();
	stream >> version >> appVersion >> account >> count;
	if (!stream.ok()
		|| version != kOpenHistoryVersion
		|| appVersion > quint32(AppVersion)
		|| account != SerializePeerId(_session->userPeerId())
		|| count > EnhancedSettings::kRecentChatsLimitMaximum) {
		return;
	}
	auto entries = std::vector<OpenEntry>();
	entries.reserve(count);
	for (auto i = quint32(); i != count; ++i) {
		const auto peer = Serialize::readPeer(_session, appVersion, stream);
		auto rootId = qint64();
		auto sublistId = quint64();
		stream >> rootId >> sublistId;
		const auto sublistPeer = sublistId
			? Serialize::readPeer(_session, appVersion, stream)
			: nullptr;
		if (!stream.ok() || !peer) {
			return;
		}
		const auto root = MsgId(rootId);
		const auto sublist = DeserializePeerId(sublistId);
		if ((rootId && !IsServerMsgId(root))
			|| (rootId && sublistId)
			|| (sublistId && (!sublistPeer || sublistPeer->id != sublist))) {
			continue;
		}
		const auto duplicate = ranges::any_of(entries, [&](const OpenEntry &e) {
			return e.peer == peer->id
				&& e.topicRootId == root
				&& e.sublistPeer == sublist;
		});
		if (!duplicate) {
			entries.push_back({ peer->id, root, sublist, {} });
		}
	}
	if (!stream.atEnd()) {
		return;
	}
	_opens = std::move(entries);
	trimChatOpenHistory();
}

void RecentPeers::saveChatOpenHistory(bool flush) {
	auto &local = _session->local();
	if (_opens.empty()) {
		local.clearPref(kOpenHistoryKey);
	} else {
		auto stream = Serialize::ByteArrayWriter();
		stream
			<< kOpenHistoryVersion
			<< quint32(AppVersion)
			<< SerializePeerId(_session->userPeerId())
			<< quint32(_opens.size());
		for (const auto &entry : _opens) {
			Serialize::writePeer(stream, _session->data().peer(entry.peer));
			stream << qint64(entry.topicRootId.bare)
				<< (entry.sublistPeer ? SerializePeerId(entry.sublistPeer) : quint64(0));
			if (entry.sublistPeer) {
				Serialize::writePeer(stream, _session->data().peer(entry.sublistPeer));
			}
		}
		local.writePref<QByteArray>(kOpenHistoryKey, std::move(stream).result());
	}
	if (flush) {
		local.flushPrefs();
	}
}

void RecentPeers::trimChatOpenHistory() {
	const auto limit = EnhancedSettings::RecentChatsLimit();
	if (int(_opens.size()) <= limit) {
		return;
	}
	_opens.resize(limit);
	pruneChatOpenUserpics();
	saveChatOpenHistory(true);
	_chatOpenUpdates.fire({});
}

void RecentPeers::resolveChatOpenEntry(OpenEntry &entry) {
	const auto peer = _session->data().peerLoaded(entry.peer);
	if (!peer) {
		return;
	}
	if (entry.topicRootId) {
		const auto forum = peer->forum();
		if (!forum) {
			entry.requestedForum = nullptr;
			return;
		}
		if (const auto topic = forum->topicFor(entry.topicRootId)) {
			entry.thread = base::make_weak(topic);
		} else if (entry.requestedForum != forum) {
			entry.requestedForum = forum;
			const auto rootId = entry.topicRootId;
			forum->requestTopic(rootId, crl::guard(this, [=] {
				const auto i = ranges::find_if(_opens, [&](const OpenEntry &e) {
					return e.peer == peer->id && e.topicRootId == rootId;
				});
				if (i == end(_opens)) {
					return;
				}
				i->requestedForum = nullptr;
				const auto forum = peer->forum();
				if (forum && forum->topicDeleted(rootId)) {
					chatOpenRemove(peer->id, rootId, PeerId());
				} else if (const auto topic = forum ? forum->topicFor(rootId) : nullptr) {
					i->thread = base::make_weak(topic);
					_chatOpenUpdates.fire({});
				}
			}));
		}
	} else if (entry.sublistPeer) {
		const auto sublistPeer = _session->data().peerLoaded(entry.sublistPeer);
		const auto parent = peer->isSelf()
			? &_session->data().savedMessages()
			: peer->monoforum();
		if (parent && sublistPeer) {
			entry.thread = base::make_weak(parent->sublist(sublistPeer));
		}
	} else {
		entry.thread = base::make_weak(_session->data().history(peer));
	}
}

void RecentPeers::chatOpenPush(not_null<Thread*> thread) {
	readChatOpenHistory();
	const auto peer = thread->owningHistory()->peer->id;
	const auto root = thread->topicRootId();
	const auto sublist = thread->monoforumPeerId();
	if (root && !IsServerMsgId(root)) {
		return;
	}
	const auto i = ranges::find_if(_opens, [&](const OpenEntry &entry) {
		return entry.peer == peer
			&& entry.topicRootId == root
			&& entry.sublistPeer == sublist;
	});
	if (i != end(_opens)) {
		i->thread = base::make_weak(thread);
		if (i == begin(_opens)) {
			saveChatOpenHistory();
			_chatOpenUpdates.fire({});
			return;
		}
		ranges::rotate(begin(_opens), i, i + 1);
	} else {
		_opens.insert(begin(_opens), { peer, root, sublist, base::make_weak(thread) });
	}
	const auto limit = EnhancedSettings::RecentChatsLimit();
	if (int(_opens.size()) > limit) {
		_opens.resize(limit);
	}
	pruneChatOpenUserpics();
	saveChatOpenHistory();
	_chatOpenUpdates.fire({});
}

void RecentPeers::chatOpenRemove(not_null<Thread*> thread) {
	chatOpenRemove(
		thread->owningHistory()->peer->id,
		thread->topicRootId(),
		thread->monoforumPeerId());
}

void RecentPeers::chatOpenRemove(PeerId peer, MsgId root, PeerId sublist) {
	readChatOpenHistory();
	const auto removed = ranges::remove_if(_opens, [&](const OpenEntry &entry) {
		return entry.peer == peer
			&& entry.topicRootId == root
			&& entry.sublistPeer == sublist;
	});
	if (removed == end(_opens)) {
		return;
	}
	_opens.erase(removed, end(_opens));
	pruneChatOpenUserpics();
	saveChatOpenHistory(true);
	_chatOpenUpdates.fire({});
}

void RecentPeers::pruneChatOpenUserpics() {
	for (auto i = begin(_chatOpenUserpicsCache); i != end(_chatOpenUserpicsCache);) {
		const auto peer = i->first->id;
		if (ranges::any_of(_opens, [&](const OpenEntry &entry) {
			return entry.peer == peer || entry.sublistPeer == peer;
		})) {
			++i;
		} else {
			i = _chatOpenUserpicsCache.erase(i);
		}
	}
}

void RecentPeers::chatOpenKeepUserpics(
		base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView> userpics) {
	_chatOpenUserpicsCache = std::move(userpics);
	pruneChatOpenUserpics();
}

} // namespace Data
