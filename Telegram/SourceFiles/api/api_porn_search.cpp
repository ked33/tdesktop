/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_porn_search.h"

#include "apiwrap.h"
#include "core/enhanced_settings.h"
#include "data/data_channel.h"
#include "data/data_changes.h"
#include "data/data_chat.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"

namespace Api {
namespace {

constexpr auto kDialogsPerPage = 100;
constexpr auto kChannelsPerRequest = 100;
constexpr auto kMessagesPerPage = 50;

} // namespace

PornSearch::PornSearch(not_null<ApiWrap*> api)
: _session(&api->session())
, _api(&api->instance())
, _timer([=] { pump(); }) {
	const auto settingsChanged = [=] {
		LOG(("Search Info: supplemental concurrency %1, request interval %2 ms.")
			.arg(EnhancedSettings::SearchPornConcurrency())
			.arg(EnhancedSettings::SearchPornRequestInterval()));
		schedule();
	};
	EnhancedSettings::SearchPornConcurrencyChanges(
	) | rpl::on_next(settingsChanged, _lifetime);
	EnhancedSettings::SearchPornRequestIntervalChanges(
	) | rpl::on_next(settingsChanged, _lifetime);
	using Flag = Data::PeerUpdate::Flag;
	_session->changes().peerUpdates(
		Flag::UnavailableReason
		| Flag::ChannelAmIn
		| Flag::Migration
		| Flag::FullInfo
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		const auto channel = update.peer->asChannel();
		if (!channel) {
			return;
		}
		if (channel->amIn() && !_catalog.contains(channel->id)) {
			const auto history = _session->data().historyLoaded(channel->id);
			if (history && history->folderKnown()) {
				const auto folder = history->folder() ? 1 : 0;
				_catalog[channel->id] = folder;
				_folders[folder].seen.insert(channel->id);
			} else if (update.flags & Flag::ChannelAmIn) {
				invalidate();
			}
		}
		refreshMetadata();
		schedule();
	}, _lifetime);
	_session->changes().historyUpdates(
		Data::HistoryUpdate::Flag::Folder
	) | rpl::on_next([=](const Data::HistoryUpdate &update) {
		if (const auto channel = update.history->peer->asChannel()) {
			if (channel->amIn() && update.history->folderKnown()) {
				const auto folder = update.history->folder() ? 1 : 0;
				_catalog[channel->id] = folder;
				_folders[folder].seen.insert(channel->id);
				schedule();
			}
		}
	}, _lifetime);
}

PornSearch::~PornSearch() = default;

PornSearch::QueryId PornSearch::start(
		PornSearchRequest request,
		Callback done) {
	const auto id = ++_nextQuery;
	_queries.emplace(id, Query{ std::move(request), std::move(done) });
	LOG(("Search Info: supplemental query %1 started, "
		"concurrency %2, request interval %3 ms.")
		.arg(id)
		.arg(EnhancedSettings::SearchPornConcurrency())
		.arg(EnhancedSettings::SearchPornRequestInterval()));
	schedule();
	return id;
}

void PornSearch::cancel(QueryId id) {
	const auto i = _queries.find(id);
	if (i == end(_queries)) {
		return;
	}
	for (const auto &[peer, source] : i->second.sources) {
		cancelTask(source.task);
	}
	_queries.erase(i);
	if (_queries.empty()) {
		cancelCatalogTasks();
		_timer.cancel();
	} else {
		schedule();
	}
}

void PornSearch::loadMore(QueryId id, TimeId before) {
	const auto i = _queries.find(id);
	if (i != end(_queries) && before > 0 && before < i->second.before) {
		i->second.before = before;
		i->second.dirty = true;
		schedule();
	}
}

void PornSearch::retry(QueryId id) {
	const auto i = _queries.find(id);
	if (i == end(_queries)) {
		return;
	}
	for (auto &[peer, source] : i->second.sources) {
		source.retry |= source.failed;
		source.failed = false;
	}
	for (auto &folder : _folders) {
		folder.dialogsFailed = folder.pinnedFailed = false;
	}
	_metadataFailed.clear();
	refreshMetadata();
	i->second.dirty = true;
	schedule();
}

void PornSearch::invalidate() {
	cancelCatalogTasks();
	_folders = {};
	_metadataFailed.clear();
	refreshMetadata();
	for (auto &[id, query] : _queries) {
		query.dirty = true;
	}
	schedule();
}

void PornSearch::applyFloodWait(const MTP::Error &error) {
	_requestGate.pause(
		crl::now(),
		error.type().mid(error.type().lastIndexOf('_') + 1).toLongLong());
	LOG(("Search Warning: %1 (code %2); pausing search requests for %3 ms, "
		"supplemental concurrency %4, active %5, request interval %6 ms.")
		.arg(error.type())
		.arg(error.code())
		.arg(floodWaitRemaining())
		.arg(EnhancedSettings::SearchPornConcurrency())
		.arg(int(_tasks.size()))
		.arg(EnhancedSettings::SearchPornRequestInterval()));
	for (auto &[id, query] : _queries) {
		query.dirty = true;
	}
	schedule();
}

crl::time PornSearch::floodWaitRemaining() const {
	return _requestGate.pauseRemaining(crl::now());
}

crl::time PornSearch::takeFloodWaitNotice() {
	return _requestGate.takePauseNotice(crl::now());
}

void PornSearch::cancelTask(TaskId id) {
	const auto i = _tasks.find(id);
	if (i != end(_tasks)) {
		_api.request(i->second.requestId).cancel();
		_tasks.erase(i);
	}
}

void PornSearch::cancelCatalogTasks() {
	for (auto i = begin(_tasks); i != end(_tasks);) {
		if (i->second.type == TaskType::Search) {
			++i;
		} else {
			_api.request(i->second.requestId).cancel();
			i = _tasks.erase(i);
		}
	}
	for (auto &folder : _folders) {
		folder.dialogsTask = folder.pinnedTask = 0;
	}
}

bool PornSearch::folderNeeded(int folder) const {
	return !folder || ranges::any_of(_queries, [](const auto &entry) {
		return entry.second.request.fromArchive;
	});
}

bool PornSearch::metadataNeeded(PeerId peer) const {
	const auto i = _catalog.find(peer);
	return i != end(_catalog) && folderNeeded(i->second);
}

void PornSearch::schedule() {
	if (!_queries.empty()) {
		_timer.callOnce(0);
	}
}

void PornSearch::reconcile() {
	for (auto &[id, query] : _queries) {
		auto allowed = std::map<PeerId, PeerId>();
		for (const auto &[peer, folder] : _catalog) {
			if (folder && !query.request.fromArchive) {
				continue;
			}
			const auto channel = _session->data().channelLoaded(
				peerToChannel(peer));
			if (!channel
				|| !channel->amIn()
				|| !channel->hasPornRestriction()
				|| (query.request.filter == PornSearchFilter::Groups
					&& !channel->isMegagroup())
				|| (query.request.filter == PornSearchFilter::Channels
					&& !channel->isBroadcast())) {
				continue;
			}
			allowed.emplace(peer, peer);
			if (const auto migrated = channel->migrateFrom()) {
				allowed.emplace(migrated->id, peer);
			}
		}
		for (auto i = begin(query.sources); i != end(query.sources);) {
			if (!allowed.contains(i->first)) {
				cancelTask(i->second.task);
				i = query.sources.erase(i);
				query.dirty = true;
			} else {
				++i;
			}
		}
		for (const auto &[peer, channel] : allowed) {
			const auto [i, added] = query.sources.try_emplace(peer);
			if (added || i->second.channel != channel) {
				i->second.channel = channel;
				query.dirty = true;
			}
		}
	}
}

void PornSearch::pump() {
	_requestGate.setInterval(EnhancedSettings::SearchPornRequestInterval());
	reconcile();
	if (_requestGate.waiting(crl::now())) {
		_timer.callOnce(_requestGate.delay(crl::now()));
		publish();
		return;
	} else if (_requestGate.finishPause(crl::now())) {
		LOG(("Search Info: flood wait expired; supplemental search may resume."));
		for (auto &[id, query] : _queries) {
			query.dirty = true;
		}
	}
	const auto limit = EnhancedSettings::SearchPornConcurrency();
	while (_tasks.size() < std::size_t(limit)
		&& !_queries.empty()) {
		const auto now = crl::now();
		if (!_requestGate.canStart(now, _tasks.size(), limit)) {
			_timer.callOnce(_requestGate.delay(now));
			break;
		}
		auto metadata = std::vector<PeerId>();
		for (const auto peer : _metadataPending) {
			if (!metadataNeeded(peer)) {
				continue;
			}
			const auto running = ranges::any_of(_tasks, [&](const auto &task) {
				return ranges::contains(task.second.peers, peer);
			});
			if (!running) {
				metadata.push_back(peer);
				if (metadata.size() == kChannelsPerRequest) {
					break;
				}
			}
		}
		if (!metadata.empty()) {
			sendMetadata(std::move(metadata));
			continue;
		}
		auto sent = false;
		for (auto folder = 0; folder != int(_folders.size()); ++folder) {
			if (!folderNeeded(folder)) {
				continue;
			}
			const auto &state = _folders[folder];
			if (!state.pinnedDone && !state.pinnedFailed && !state.pinnedTask) {
				sendDialogs(folder, true);
				sent = true;
				break;
			} else if (!state.dialogsDone && !state.dialogsFailed
				&& !state.dialogsTask) {
				sendDialogs(folder, false);
				sent = true;
				break;
			}
		}
		if (!sent && !sendReadySearch(true) && !sendReadySearch(false)) {
			break;
		}
	}
	publish();
}

bool PornSearch::sendReadySearch(bool firstPage) {
	auto query = _queries.upper_bound(_lastQuery);
	for (auto n = _queries.size(); n != 0; --n) {
		if (query == end(_queries)) {
			query = begin(_queries);
		}
		for (const auto &[peer, source] : query->second.sources) {
			if (!source.task && !source.exhausted && !source.failed
				&& ((!source.started && firstPage)
					|| (!firstPage && (source.retry
						|| source.oldestDate >= query->second.before)))) {
				_lastQuery = query->first;
				sendSearch(query->first, peer);
				return true;
			}
		}
		++query;
	}
	return false;
}

void PornSearch::rememberDialogs(
		int folder,
		const QVector<MTPDialog> &dialogs) {
	for (const auto &value : dialogs) {
		if (value.type() != mtpc_dialog) {
			continue;
		}
		const auto peer = peerFromMTP(value.c_dialog().vpeer());
		if (peerIsChannel(peer)) {
			_catalog[peer] = folder;
			_folders[folder].seen.insert(peer);
		}
	}
	refreshMetadata();
}

void PornSearch::refreshMetadata() {
	_metadataPending.clear();
	for (const auto &[peer, folder] : _catalog) {
		const auto channel = _session->data().channel(peerToChannel(peer));
		if (channel->isLoaded()) {
			_metadataFailed.erase(peer);
		} else if (!_metadataFailed.contains(peer)) {
			if (channel->accessHash()) {
				_metadataPending.insert(peer);
			} else {
				_metadataFailed.insert(peer);
			}
		}
	}
}

void PornSearch::checkFolder(int folder) {
	const auto &state = _folders[folder];
	if (state.dialogsDone && state.pinnedDone) {
		for (auto i = begin(_catalog); i != end(_catalog);) {
			if (i->second == folder && !state.seen.contains(i->first)) {
				_metadataPending.erase(i->first);
				_metadataFailed.erase(i->first);
				i = _catalog.erase(i);
			} else {
				++i;
			}
		}
	}
	for (auto &[id, query] : _queries) {
		query.dirty = true;
	}
	schedule();
}

void PornSearch::sendDialogs(int folder, bool pinned) {
	_requestGate.started(crl::now());
	const auto taskId = ++_nextTask;
	_tasks.emplace(taskId, Task{
		.type = pinned ? TaskType::Pinned : TaskType::Dialogs,
		.folder = folder,
	});
	auto &state = _folders[folder];
	if (pinned) {
		state.pinnedTask = taskId;
		_tasks[taskId].requestId = _api.request(MTPmessages_GetPinnedDialogs(
			MTP_int(folder)
		)).done([=](const MTPmessages_PeerDialogs &result) {
			if (!_tasks.erase(taskId)) {
				return;
			}
			const auto &data = result.data();
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
			rememberDialogs(folder, data.vdialogs().v);
			_folders[folder].pinnedTask = 0;
			_folders[folder].pinnedDone = true;
			checkFolder(folder);
		}).fail([=](const MTP::Error &error) {
			taskFailed(taskId, error);
		}).handleFloodErrors().send();
		return;
	}
	state.dialogsTask = taskId;
	using Flag = MTPmessages_GetDialogs::Flag;
	_tasks[taskId].requestId = _api.request(MTPmessages_GetDialogs(
		MTP_flags(Flag::f_exclude_pinned | Flag::f_folder_id),
		MTP_int(folder),
		MTP_int(state.offsetDate),
		MTP_int(state.offsetId),
		(state.offsetPeer
			? _session->data().peer(state.offsetPeer)->input()
			: MTP_inputPeerEmpty()),
		MTP_int(kDialogsPerPage),
		MTP_long(0)
	)).done([=](const MTPmessages_Dialogs &result) {
		if (!_tasks.erase(taskId)) {
			return;
		}
		auto &state = _folders[folder];
		state.dialogsTask = 0;
		result.match([&](const MTPDmessages_dialogsNotModified &) {
			state.dialogsFailed = true;
		}, [&](const auto &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
			rememberDialogs(folder, data.vdialogs().v);
			if (result.type() == mtpc_messages_dialogs
				|| data.vdialogs().v.empty()) {
				state.dialogsDone = true;
				return;
			}
			for (const auto &value : ranges::views::reverse(data.vdialogs().v)) {
				if (value.type() != mtpc_dialog) {
					continue;
				}
				const auto &dialog = value.c_dialog();
				const auto peer = peerFromMTP(dialog.vpeer());
				const auto id = MsgId(dialog.vtop_message().v);
				for (const auto &message : data.vmessages().v) {
					const auto date = DateFromMessage(message);
					if (PeerFromMessage(message) == peer
						&& IdFromMessage(message) == id && date) {
						if (state.offsetPeer == peer && state.offsetId == id
							&& state.offsetDate == date) {
							state.dialogsFailed = true;
						} else {
							state.offsetPeer = peer;
							state.offsetId = id;
							state.offsetDate = date;
						}
						return;
					}
				}
			}
			state.dialogsFailed = true;
		});
		checkFolder(folder);
	}).fail([=](const MTP::Error &error) {
		taskFailed(taskId, error);
	}).handleFloodErrors().send();
}

void PornSearch::sendMetadata(std::vector<PeerId> peers) {
	_requestGate.started(crl::now());
	const auto taskId = ++_nextTask;
	auto channels = QVector<MTPInputChannel>();
	for (const auto peer : peers) {
		channels.push_back(_session->data().channel(
			peerToChannel(peer))->inputChannel());
	}
	_tasks.emplace(taskId, Task{
		.type = TaskType::Metadata,
		.peers = peers,
	});
	_tasks[taskId].requestId = _api.request(MTPchannels_GetChannels(
		MTP_vector<MTPInputChannel>(std::move(channels))
	)).done([=](const MTPmessages_Chats &result) {
		if (!_tasks.erase(taskId)) {
			return;
		}
		result.match([&](const auto &data) {
			_session->data().processChats(data.vchats());
		});
		for (const auto peer : peers) {
			if (!_session->data().channel(peerToChannel(peer))->isLoaded()) {
				_metadataFailed.insert(peer);
			}
		}
		refreshMetadata();
		for (auto &[id, query] : _queries) {
			query.dirty = true;
		}
		schedule();
	}).fail([=](const MTP::Error &error) {
		taskFailed(taskId, error);
	}).handleFloodErrors().send();
}

void PornSearch::sendSearch(QueryId queryId, PeerId peerId) {
	_requestGate.started(crl::now());
	auto &query = _queries.at(queryId);
	auto &source = query.sources.at(peerId);
	const auto peer = _session->data().peer(peerId);
	const auto taskId = ++_nextTask;
	source.task = taskId;
	source.retry = false;
	query.dirty = true;
	_tasks.emplace(taskId, Task{
		.type = TaskType::Search,
		.query = queryId,
		.peer = peerId,
	});
	_tasks[taskId].requestId = _api.request(MTPmessages_Search(
		MTP_flags(0),
		peer->input(),
		MTP_string(query.request.query),
		MTP_inputPeerEmpty(), // from_id
		MTP_inputPeerEmpty(), // saved_peer_id
		MTP_vector<MTPReaction>(),
		MTP_int(0), // top_msg_id
		MTP_inputMessagesFilterEmpty(),
		MTP_int(0), // min_date
		MTP_int(0), // max_date
		MTP_int(source.offsetId),
		MTP_int(0), // add_offset
		MTP_int(kMessagesPerPage),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		searchReceived(taskId, result);
	}).fail([=](const MTP::Error &error) {
		taskFailed(taskId, error);
	}).handleFloodErrors().send();
}

void PornSearch::searchReceived(
		TaskId taskId,
		const MTPmessages_Messages &result) {
	const auto task = _tasks.find(taskId);
	if (task == end(_tasks)) {
		return;
	}
	const auto queryId = task->second.query;
	const auto peerId = task->second.peer;
	_tasks.erase(task);
	auto &query = _queries.at(queryId);
	auto &source = query.sources.at(peerId);
	source.task = 0;
	source.started = true;
	query.dirty = true;
	const auto previous = source.offsetId;
	const auto exactCount = result.match(
		[](const MTPDmessages_messagesSlice &data) {
			return data.is_inexact() ? -1 : data.vcount().v;
		}, [](const MTPDmessages_channelMessages &data) {
			return data.is_inexact() ? -1 : data.vcount().v;
		}, [](const auto &) {
			return -1;
		});
	result.match([&](const MTPDmessages_messagesNotModified &) {
		source.failed = true;
	}, [&](const auto &data) {
		_session->data().processUsers(data.vusers());
		_session->data().processChats(data.vchats());
		const auto peer = _session->data().peer(peerId);
		peer->processTopics(data.vtopics());
		auto nextOffset = MsgId();
		for (const auto &message : data.vmessages().v) {
			if (PeerFromMessage(message) != peerId) {
				continue;
			}
			const auto id = IdFromMessage(message);
			if (id > 0) {
				nextOffset = nextOffset ? std::min(nextOffset, id) : id;
			}
			if (const auto date = DateFromMessage(message)) {
				source.oldestDate = std::min(source.oldestDate, date);
				const auto item = _session->data().addNewMessage(
					message,
					MessageFlags(),
					NewMessageType::Existing);
				source.messages.insert(item->fullId());
			}
		}
		if (data.vmessages().v.empty()
			|| result.type() == mtpc_messages_messages
			|| PornSearchPolicy::ExactCountReached(
				source.messages.size(),
				exactCount)) {
			source.exhausted = true;
		} else if (!PornSearchPolicy::PageAdvanced(
			previous.bare,
			nextOffset.bare)) {
			source.failed = true;
		}
		if (!source.failed && nextOffset) {
			source.offsetId = nextOffset;
		}
	});
	if (result.type() == mtpc_messages_channelMessages) {
		if (const auto channel = _session->data().peer(peerId)->asChannel()) {
			channel->ptsReceived(result.c_messages_channelMessages().vpts().v);
		}
	}
	schedule();
}

void PornSearch::taskFailed(TaskId taskId, const MTP::Error &error) {
	const auto i = _tasks.find(taskId);
	if (i == end(_tasks)) {
		return;
	}
	const auto task = i->second;
	_tasks.erase(i);
	if (!MTP::IgnoreError(error)) {
		const auto method = (task.type == TaskType::Dialogs)
			? u"messages.getDialogs"_q
			: (task.type == TaskType::Pinned)
			? u"messages.getPinnedDialogs"_q
			: (task.type == TaskType::Metadata)
			? u"channels.getChannels"_q
			: u"messages.search"_q;
		LOG(("Search Error: supplemental %1 request %2 failed, "
			"code %3, type %4.")
			.arg(method)
			.arg(task.requestId)
			.arg(error.code())
			.arg(error.type()));
	}
	const auto flood = MTP::IsFloodError(error);
	if (flood) {
		applyFloodWait(error);
	}
	switch (task.type) {
	case TaskType::Dialogs:
		_folders[task.folder].dialogsTask = 0;
		_folders[task.folder].dialogsFailed = !flood;
		break;
	case TaskType::Pinned:
		_folders[task.folder].pinnedTask = 0;
		_folders[task.folder].pinnedFailed = !flood;
		break;
	case TaskType::Metadata:
		if (!flood) {
			_metadataFailed.insert(begin(task.peers), end(task.peers));
			refreshMetadata();
		}
		break;
	case TaskType::Search: {
		auto &source = _queries.at(task.query).sources.at(task.peer);
		source.task = 0;
		if (flood) {
			source.retry = true;
		} else {
			source.started = true;
			source.exhausted = MTP::IgnoreError(error)
				|| error.type() == u"SEARCH_QUERY_EMPTY"_q;
			source.failed = !source.exhausted;
		}
	} break;
	}
	for (auto &[id, query] : _queries) {
		query.dirty = true;
	}
	schedule();
}

PornSearchResult PornSearch::resultFor(const Query &query) const {
	auto result = PornSearchResult();
	result.totalKnown = true;
	for (auto folder = 0; folder != int(_folders.size()); ++folder) {
		if (folder && !query.request.fromArchive) {
			continue;
		}
		const auto &state = _folders[folder];
		result.totalKnown &= state.dialogsDone && state.pinnedDone;
		result.scanning |= (!state.dialogsDone && !state.dialogsFailed)
			|| (!state.pinnedDone && !state.pinnedFailed);
		result.failed += int(state.dialogsFailed) + int(state.pinnedFailed);
	}
	const auto relevant = [&](PeerId peer) {
		const auto i = _catalog.find(peer);
		return i != end(_catalog)
			&& (!i->second || query.request.fromArchive);
	};
	result.scanning |= ranges::any_of(_metadataPending, relevant);
	result.failed += int(ranges::count_if(_metadataFailed, relevant));
	result.totalKnown &= !result.scanning && !result.failed;
	auto progress = std::map<PeerId, bool>();
	for (const auto &[peer, source] : query.sources) {
		progress.try_emplace(source.channel, true).first->second
			&= source.started;
		result.failed += int(source.failed);
		result.loading |= source.task != 0
			|| (!source.exhausted && !source.failed
				&& (!source.started || source.retry
					|| source.oldestDate >= query.before));
		result.more |= !source.exhausted;
		result.messages.insert(
			end(result.messages),
			begin(source.messages),
			end(source.messages));
	}
	result.total = int(progress.size());
	result.searched = int(ranges::count_if(progress, [](const auto &entry) {
		return entry.second;
	}));
	result.more |= !result.totalKnown;
	result.waiting = _requestGate.waiting(crl::now())
		&& (result.scanning || result.loading);
	return result;
}

void PornSearch::publish() {
	for (auto &[id, query] : _queries) {
		if (!base::take(query.dirty)) {
			continue;
		}
		crl::on_main(_session, [
			id,
			done = query.done,
			result = resultFor(query)
		]() mutable {
			done(id, std::move(result));
		});
	}
}

} // namespace Api
