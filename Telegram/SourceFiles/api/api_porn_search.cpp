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
#include "settings.h"

namespace Api {
namespace {

constexpr auto kDialogsPerPage = 100;
constexpr auto kChannelsPerRequest = 100;
constexpr auto kMessagesPerPage = 50;
constexpr auto kCachedQueries = 10;
constexpr auto kCachedMessages = 20000;

} // namespace

PornSearch::PornSearch(not_null<ApiWrap*> api)
: _session(&api->session())
, _api(&api->instance())
, _timer([=] { pump(); })
, _cache(kCachedQueries, kCachedMessages) {
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
	rpl::merge(
		_session->changes().realtimePeerUpdates(Flag::UnavailableReason),
		_session->changes().realtimePeerUpdates(Flag::ChannelAmIn),
		_session->changes().realtimePeerUpdates(Flag::Migration)
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		invalidateMessages(update.peer->id);
	}, _lifetime);
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
		if (update.flags & (Flag::UnavailableReason
			| Flag::ChannelAmIn | Flag::Migration)) {
			_folderFailed.erase(channel->id);
		}
		peerChanged(channel);
		_metadataDirty = true;
		schedule();
	}, _lifetime);
	_session->changes().historyUpdates(
		Data::HistoryUpdate::Flag::Folder
	) | rpl::on_next([=](const Data::HistoryUpdate &update) {
		if (const auto channel = update.history->peer->asChannel()) {
			if (channel->amIn() && update.history->folderKnown()) {
				const auto folder = update.history->folder() ? 1 : 0;
				updateFolder(channel->id, folder);
			}
		}
	}, _lifetime);
	using MessageFlag = Data::MessageUpdate::Flag;
	rpl::merge(
		_session->changes().realtimeMessageUpdates(MessageFlag::NewAdded),
		_session->changes().realtimeMessageUpdates(MessageFlag::NewMaybeAdded),
		_session->changes().realtimeMessageUpdates(MessageFlag::Destroyed)
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		invalidateMessages(update.item->history()->peer->id);
	}, _lifetime);
	_session->data().channelDifferenceTooLong(
	) | rpl::on_next([=](not_null<ChannelData*> channel) {
		invalidateMessages(channel->id);
	}, _lifetime);
}

PornSearch::~PornSearch() = default;

PornSearch::QueryId PornSearch::start(
		PornSearchRequest request,
		Callback done) {
	const auto id = ++_nextQuery;
	auto cached = _cache.take(request);
	const auto before = cached
		? cached->before
		: std::numeric_limits<TimeId>::max();
	_queries.emplace(id, Query{
		.request = std::move(request),
		.done = std::move(done),
		.cached = std::move(cached),
		.before = before,
	});
	adoptLoadedFolders();
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
	for (auto &[peer, source] : i->second.sources.entries()) {
		if (const auto task = base::take(source.task)) {
			cancelTask(task);
			source.retry = true;
		}
	}
	cacheQuery(i->second);
	_queries.erase(i);
	if (!folderNeeded(0)) {
		cancelCatalogTasks();
	}
	if (_queries.empty() && !_preloading) {
		_timer.cancel();
	} else {
		schedule();
	}
}

void PornSearch::cacheQuery(Query &query) {
	if (!query.sources.ready() && !query.cached) {
		return;
	}
	auto cached = query.sources.ready()
		? CachedQuery{ std::move(query.sources.entries()), query.before }
		: std::move(*query.cached);
	auto count = std::size_t(0);
	for (const auto &[peer, source] : cached.sources) {
		count += source.messages.size();
	}
	if (_cache.put(query.request, std::move(cached), count)) {
		LOG(("Search Info: supplemental cache retained %1 queries, %2 message IDs.")
			.arg(_cache.size())
			.arg(_cache.cost()));
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
	for (auto &[peer, source] : i->second.sources.entries()) {
		source.retry |= source.failed;
		source.failed = false;
	}
	if (!i->second.sources.ready()) {
		for (auto &folder : _folders) {
			folder.dialogsFailed = folder.pinnedFailed = false;
		}
		_metadataFailed.clear();
		_folderPending.insert(begin(_folderFailed), end(_folderFailed));
		_folderFailed.clear();
		refreshMetadata();
	}
	i->second.dirty = true;
	schedule();
}

void PornSearch::preload() {
	_preloading = true;
	_preloadArchive = GetEnhancedBool("search_main_and_archive");
	adoptLoadedFolders();
	refreshMetadata();
	schedule();
}

void PornSearch::adoptLoadedFolders() {
	if (!_adoptLoadedFolders) {
		return;
	}
	for (auto folder = 0; folder != int(_folders.size()); ++folder) {
		auto &state = _folders[folder];
		const auto local = folder
			? _session->data().folderLoaded(Data::Folder::kId)
			: nullptr;
		if ((state.dialogsDone && state.pinnedDone)
			|| (folder && !local)
			|| !_session->data().chatsListLoaded(local)) {
			continue;
		}
		cancelTask(state.dialogsTask);
		cancelTask(state.pinnedTask);
		state = {};
		state.dialogsDone = state.pinnedDone = true;
		const auto remember = [&](not_null<PeerData*> peer) {
			const auto channel = peer->asChannel();
			if (!channel || !channel->amIn()) {
				return;
			}
			const auto history = _session->data().historyLoaded(peer->id);
			if (history && history->folderKnown()) {
				if (int(history->folder() != nullptr) == folder) {
					updateFolder(peer->id, folder);
				}
			} else if (channel->isLoaded()
				&& channel->hasPornRestriction()
				&& !_catalog.contains(peer->id)
				&& !_folderFailed.contains(peer->id)) {
				_folderPending.insert(peer->id);
			}
		};
		_session->data().enumerateGroups(remember);
		_session->data().enumerateBroadcasts([&](not_null<ChannelData*> channel) {
			remember(channel);
		});
		checkFolder(folder);
		LOG(("Search Info: supplemental catalog reused loaded folder %1, "
			"%2 channels.").arg(folder).arg(state.seen.size()));
	}
	refreshMetadata();
}

void PornSearch::peerChanged(not_null<ChannelData*> channel) {
	if (!channel->isLoaded()) {
		return;
	}
	if (!channel->amIn()) {
		forgetPeer(channel->id);
		return;
	}
	const auto history = _session->data().historyLoaded(channel->id);
	if (history && history->folderKnown()) {
		updateFolder(channel->id, history->folder() ? 1 : 0);
	} else if (!channel->hasPornRestriction()) {
		_folderPending.erase(channel->id);
		_folderFailed.erase(channel->id);
	} else if (!_catalog.contains(channel->id)
		&& !_folderFailed.contains(channel->id)) {
		_folderPending.insert(channel->id);
	}
}

void PornSearch::updateFolder(PeerId peer, int folder) {
	if (!peerIsChannel(peer) || folder < 0 || folder >= int(_folders.size())) {
		return;
	}
	_catalog[peer] = folder;
	_metadataDirty = true;
	_folders[folder].seen.insert(peer);
	_folders[1 - folder].seen.erase(peer);
	_folderPending.erase(peer);
	_folderFailed.erase(peer);
	schedule();
}

void PornSearch::forgetPeer(PeerId peer) {
	_catalog.erase(peer);
	for (auto &folder : _folders) {
		folder.seen.erase(peer);
	}
	_metadataPending.erase(peer);
	_metadataFailed.erase(peer);
	_folderPending.erase(peer);
	_folderFailed.erase(peer);
}

void PornSearch::invalidate() {
	cancelCatalogTasks();
	_folders = {};
	_catalog.clear();
	_metadataFailed.clear();
	_folderPending.clear();
	_folderFailed.clear();
	_cache.clear();
	_adoptLoadedFolders = false;
	refreshMetadata();
	for (auto &[id, query] : _queries) {
		query.cached.reset();
		for (auto &[peer, source] : query.sources.entries()) {
			source.changed = true;
		}
		if (!query.sources.ready()) {
			query.dirty = true;
		}
	}
	LOG(("Search Info: supplemental chat catalog invalidated; "
		"confirmed search sources remain fixed."));
	preload();
}

void PornSearch::invalidateMessages(PeerId peer) {
	if (!peer) {
		return;
	}
	const auto mark = [&](auto &sources) {
		PornSearchPolicy::InvalidateSources(sources, peer);
	};
	for (auto &[id, query] : _queries) {
		mark(query.sources.entries());
		if (query.cached) {
			mark(query.cached->sources);
		}
	}
	_cache.forEach([&](CachedQuery &query) {
		mark(query.sources);
	});
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
	return (_preloading && (!folder || _preloadArchive))
		|| ranges::any_of(_queries, [&](const auto &entry) {
			return !entry.second.sources.ready()
				&& (!folder || entry.second.request.fromArchive);
		});
}

bool PornSearch::metadataNeeded(PeerId peer) const {
	const auto i = _catalog.find(peer);
	return i != end(_catalog) && folderNeeded(i->second);
}

void PornSearch::schedule() {
	if (!_queries.empty() || _preloading) {
		_timer.callOnce(0);
	}
}

void PornSearch::reconcile() {
	for (auto &[id, query] : _queries) {
		if (query.sources.ready()) {
			continue;
		}
		if (query.sources.prepare(catalogStatus(query.request).complete, [&] {
			auto sources = collectSources(query.request);
			if (query.cached) {
				const auto reused = PornSearchPolicy::RestoreSources(
					sources,
					std::move(query.cached->sources),
					[&](FullMsgId id) {
						return _session->data().message(id) != nullptr;
					});
				query.cached.reset();
				LOG(("Search Info: supplemental query %1 reused %2/%3 "
					"cached sources; changed or unloaded sources will be searched.")
					.arg(id)
					.arg(reused)
					.arg(sources.size()));
			}
			return sources;
		})) {
			query.dirty = true;
			LOG(("Search Info: supplemental query %1 confirmed %2 marked chats; "
				"starting with fixed sources.")
				.arg(id)
				.arg(resultFor(query).total));
		}
	}
}

PornSearch::CatalogStatus PornSearch::catalogStatus(
		const PornSearchRequest &request) const {
	auto result = CatalogStatus{ .complete = true };
	for (auto folder = 0; folder != int(_folders.size()); ++folder) {
		if (folder && !request.fromArchive) {
			continue;
		}
		const auto &state = _folders[folder];
		result.complete &= state.dialogsDone && state.pinnedDone;
		result.scanning |= (!state.dialogsDone && !state.dialogsFailed)
			|| (!state.pinnedDone && !state.pinnedFailed);
		result.failed += int(state.dialogsFailed) + int(state.pinnedFailed);
	}
	const auto relevant = [&](PeerId peer) {
		const auto i = _catalog.find(peer);
		return i != end(_catalog) && (!i->second || request.fromArchive);
	};
	result.scanning |= ranges::any_of(_metadataPending, relevant);
	result.failed += int(ranges::count_if(_metadataFailed, relevant));
	result.scanning |= !_folderPending.empty();
	result.failed += int(_folderFailed.size());
	result.complete &= !result.scanning && !result.failed;
	return result;
}

std::map<PeerId, PornSearch::Source> PornSearch::collectSources(
		const PornSearchRequest &request) const {
	auto result = std::map<PeerId, Source>();
	for (const auto &[peer, folder] : _catalog) {
		if (folder && !request.fromArchive) {
			continue;
		}
		const auto channel = _session->data().channelLoaded(peerToChannel(peer));
		if (!channel
			|| !channel->amIn()
			|| !channel->hasPornRestriction()
			|| (request.filter == PornSearchFilter::Groups
				&& !channel->isMegagroup())
			|| (request.filter == PornSearchFilter::Channels
				&& !channel->isBroadcast())) {
			continue;
		}
		result.emplace(peer, Source{ .channel = peer });
		if (const auto migrated = channel->migrateFrom()) {
			result.emplace(migrated->id, Source{ .channel = peer });
		}
	}
	return result;
}

void PornSearch::pump() {
	_requestGate.setInterval(EnhancedSettings::SearchPornRequestInterval());
	if (_metadataDirty) {
		refreshMetadata();
	}
	reconcile();
	if (_preloading) {
		const auto status = catalogStatus({ .fromArchive = _preloadArchive });
		if (!status.scanning) {
			_preloading = false;
			LOG(("Search Info: supplemental catalog preparation finished; "
				"%1 channels, %2 failures.")
				.arg(_catalog.size())
				.arg(status.failed));
		}
	}
	if (_queries.empty() && !_preloading) {
		return;
	}
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
		&& (!_queries.empty() || _preloading)) {
		const auto now = crl::now();
		if (!_requestGate.canStart(now, _tasks.size(), limit)) {
			_timer.callOnce(_requestGate.delay(now));
			break;
		}
		if (sendReadySearch(true)) {
			continue;
		}
		auto unknown = std::vector<PeerId>();
		if (folderNeeded(0)) {
			for (const auto peer : _folderPending) {
				const auto running = ranges::any_of(_tasks, [&](const auto &entry) {
					return entry.second.type == TaskType::PeerDialogs
						&& ranges::contains(entry.second.peers, peer);
				});
				if (!running) {
					unknown.push_back(peer);
					if (unknown.size() == kChannelsPerRequest) {
						break;
					}
				}
			}
		}
		if (!unknown.empty()) {
			sendPeerDialogs(std::move(unknown));
			continue;
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
		if (!sent && !sendReadySearch(false)) {
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
		for (const auto &[peer, source] : query->second.sources.entries()) {
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
			updateFolder(peer, folder);
		}
	}
	refreshMetadata();
}

void PornSearch::refreshMetadata() {
	_metadataDirty = false;
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

void PornSearch::sendPeerDialogs(std::vector<PeerId> peers) {
	_requestGate.started(crl::now());
	const auto taskId = ++_nextTask;
	auto dialogs = QVector<MTPInputDialogPeer>();
	for (const auto peer : peers) {
		dialogs.push_back(MTP_inputDialogPeer(_session->data().peer(peer)->input()));
	}
	_tasks.emplace(taskId, Task{
		.type = TaskType::PeerDialogs,
		.peers = peers,
	});
	_tasks[taskId].requestId = _api.request(MTPmessages_GetPeerDialogs(
		MTP_vector<MTPInputDialogPeer>(std::move(dialogs))
	)).done([=](const MTPmessages_PeerDialogs &result) {
		if (!_tasks.erase(taskId)) {
			return;
		}
		const auto &data = result.data();
		_session->data().processUsers(data.vusers());
		_session->data().processChats(data.vchats());
		for (const auto &value : data.vdialogs().v) {
			if (value.type() == mtpc_dialog) {
				const auto &dialog = value.c_dialog();
				updateFolder(
					peerFromMTP(dialog.vpeer()),
					dialog.vfolder_id().value_or_empty());
			}
		}
		for (const auto peer : peers) {
			if (_folderPending.erase(peer)) {
				const auto channel = _session->data().channel(peerToChannel(peer));
				if (channel->amIn()) {
					_folderFailed.insert(peer);
				} else {
					forgetPeer(peer);
				}
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
	auto &source = query.sources.entries().at(peerId);
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
	auto &source = query.sources.entries().at(peerId);
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
			: (task.type == TaskType::PeerDialogs)
			? u"messages.getPeerDialogs"_q
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
	case TaskType::PeerDialogs:
		if (!flood) {
			for (const auto peer : task.peers) {
				if (_folderPending.erase(peer)) {
					_folderFailed.insert(peer);
				}
			}
		}
		break;
	case TaskType::Metadata:
		if (!flood) {
			_metadataFailed.insert(begin(task.peers), end(task.peers));
			refreshMetadata();
		}
		break;
	case TaskType::Search: {
		auto &source = _queries.at(task.query).sources.entries().at(task.peer);
		source.task = 0;
		if (flood) {
			source.retry = true;
		} else {
			source.started = true;
			source.exhausted = MTP::IgnoreError(error)
				|| error.type() == u"SEARCH_QUERY_EMPTY"_q;
			source.changed |= source.exhausted;
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
	result.totalKnown = query.sources.ready();
	if (!result.totalKnown) {
		const auto status = catalogStatus(query.request);
		result.scanning = status.scanning;
		result.failed = status.failed;
	}
	auto progress = std::map<PeerId, bool>();
	for (const auto &[peer, source] : query.sources.entries()) {
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
