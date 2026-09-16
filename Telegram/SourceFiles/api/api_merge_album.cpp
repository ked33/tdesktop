/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_merge_album.h"

#include "api/api_sending.h"
#include "api/api_text_entities.h"
#include "apiwrap.h"
#include "base/random.h"
#include "data/business/data_shortcut_messages.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "main/main_session.h"
#include "mtproto/mtproto_config.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/text/text_entity.h"

namespace Api {
namespace {

struct MergeAlbumMedia {
	PhotoData *photo = nullptr;
	DocumentData *document = nullptr;
	Data::FileOrigin origin;
	TextWithEntities caption;
	FullMsgId sourceId;
};

struct MergeSendRequest {
	MergeAlbumMedia media;
	not_null<HistoryItem*> localItem;
	FullMsgId localId;
	uint64 randomId = 0;
	TextWithEntities caption;
};

struct MergeRefreshItem {
	PhotoData *photo = nullptr;
	DocumentData *document = nullptr;
	Data::FileOrigin origin;
	QByteArray usedFileReference;
};

[[nodiscard]] QByteArray MediaFileReference(const MergeAlbumMedia &media) {
	if (media.photo) {
		return media.photo->fileReference();
	}
	return media.document ? media.document->fileReference() : QByteArray();
}

[[nodiscard]] MTPInputMedia PrepareMergeInputMedia(
		const MergeAlbumMedia &media,
		bool spoiler) {
	if (media.photo) {
		using Flag = MTPDinputMediaPhoto::Flag;
		return MTP_inputMediaPhoto(
			MTP_flags(spoiler ? Flag::f_spoiler : Flag(0)),
			media.photo->mtpInput(),
			MTPint(),
			MTPInputDocument());
	}
	using Flag = MTPDinputMediaDocument::Flag;
	return MTP_inputMediaDocument(
		MTP_flags(spoiler ? Flag::f_spoiler : Flag(0)),
		media.document->mtpInput(),
		MTPInputPhoto(),
		MTPint(),
		MTPint(),
		MTPstring());
}

[[nodiscard]] TextWithEntities CaptionForItem(
		not_null<HistoryItem*> item,
		int limit) {
	auto caption = item->originalText();
	TextUtilities::Trim(caption);
	if (caption.text.isEmpty() || caption.text.size() > limit) {
		return {};
	}
	return caption;
}

[[nodiscard]] std::vector<MergeAlbumMedia> CollectMergeMedia(
		const std::vector<not_null<HistoryItem*>> &items,
		int captionLimit) {
	auto result = std::vector<MergeAlbumMedia>();
	result.reserve(items.size());
	for (const auto &item : items) {
		const auto kind = ClassifyMergeAlbumKind(item);
		if (kind == MergeAlbumKind::Skip) {
			continue;
		}
		const auto media = item->media();
		auto entry = MergeAlbumMedia{
			.origin = item->fullId(),
			.caption = CaptionForItem(item, captionLimit),
			.sourceId = item->fullId(),
		};
		if (const auto photo = media->photo()) {
			entry.photo = photo;
		} else {
			entry.document = media->document();
		}
		if (entry.photo || entry.document) {
			result.push_back(std::move(entry));
		}
	}
	return result;
}

void SendMergeGroup(
		SendAction action,
		std::vector<MergeAlbumMedia> items,
		Fn<void(QString)> done) {
	Expects(!items.empty());

	const auto history = action.history;
	const auto peer = history->peer;
	const auto session = &history->session();
	const auto api = &session->api();
	const auto multi = (items.size() > 1);
	const auto groupId = multi ? base::RandomValue<uint64>() : uint64(0);

	auto flags = NewMessageFlags(peer);
	if (action.replyTo) {
		flags |= MessageFlag::HasReplyInfo;
	}
	FillMessagePostFlags(action, peer, flags);
	if (action.options.scheduled) {
		flags |= MessageFlag::IsOrWasScheduled;
	}
	if (action.options.shortcutId) {
		flags |= MessageFlag::ShortcutMessage;
	}
	if (action.options.invertCaption) {
		flags |= MessageFlag::InvertMedia;
	}

	auto batchStarsPaid = 0;
	auto remainingStarsApproved = action.options.starsApproved;
	auto requests = std::vector<MergeSendRequest>();
	requests.reserve(items.size());
	for (auto i = 0; i != int(items.size()); ++i) {
		auto itemCaption = (i == 0)
			? items[i].caption
			: TextWithEntities();
		const auto newId = FullMsgId(
			peer->id,
			session->data().nextLocalMessageId());
		const auto randomId = base::RandomValue<uint64>();
		const auto messageStarsPaid = std::min(
			peer->starsPerMessageChecked(),
			remainingStarsApproved);
		remainingStarsApproved -= messageStarsPaid;
		batchStarsPaid += messageStarsPaid;
		session->data().registerMessageRandomId(randomId, newId);
		auto fields = HistoryItemCommonFields{
			.id = newId.msg,
			.flags = flags,
			.from = NewMessageFromId(action),
			.replyTo = action.replyTo,
			.date = NewMessageDate(action.options),
			.scheduleRepeatPeriod = action.options.scheduleRepeatPeriod,
			.shortcutId = action.options.shortcutId,
			.starsPaid = messageStarsPaid,
			.postAuthor = NewMessagePostAuthor(action),
			.groupedId = groupId,
			.effectId = action.options.effectId,
			.suggest = HistoryMessageSuggestInfo(action.options),
			.mediaSpoiler = action.options.mediaSpoiler,
		};
		const auto localItem = items[i].photo
			? history->addNewLocalMessage(
				std::move(fields),
				items[i].photo,
				itemCaption)
			: history->addNewLocalMessage(
				std::move(fields),
				items[i].document,
				itemCaption);
		requests.push_back({
			.media = std::move(items[i]),
			.localItem = localItem,
			.localId = newId,
			.randomId = randomId,
			.caption = std::move(itemCaption),
		});
	}

	const auto failRequest = [=](const MTP::Error &error) {
		for (const auto &item : requests) {
			api->sendMessageFail(error, peer, item.randomId, item.localId);
		}
		if (done) {
			done(error.type());
		}
	};

	auto refreshItems = std::vector<MergeRefreshItem>();
	refreshItems.reserve(requests.size());
	for (const auto &item : requests) {
		if (!item.media.origin) {
			continue;
		}
		refreshItems.push_back({
			.photo = item.media.photo,
			.document = item.media.document,
			.origin = item.media.origin,
			.usedFileReference = MediaFileReference(item.media),
		});
	}

	const auto performRequest = [=](const auto &repeatRequest, bool refreshed)
			-> void {
		const auto sendAs = action.options.sendAs;
		const auto retryOrFail = [=](
				const MTP::Error &error,
				const MTP::Response &response) {
			if (refreshed
				|| (error.code() != 400)
				|| !error.type().startsWith(u"FILE_REFERENCE_"_q)
				|| refreshItems.empty()) {
				failRequest(error);
				return;
			}
			const auto changed = std::make_shared<bool>(false);
			const auto left = std::make_shared<int>(int(refreshItems.size()));
			for (const auto &refresh : refreshItems) {
				api->refreshFileReference(refresh.origin, [=](const auto &) {
					const auto now = refresh.photo
						? refresh.photo->fileReference()
						: refresh.document->fileReference();
					*changed = *changed || (now != refresh.usedFileReference);
					if (!--*left) {
						if (*changed) {
							repeatRequest(repeatRequest, true);
						} else {
							failRequest(error);
						}
					}
				});
			}
		};
		if (!multi) {
			const auto &item = requests.front();
			const auto inputMedia = PrepareMergeInputMedia(
				item.media,
				action.options.mediaSpoiler);
			const auto entities = EntitiesToMTP(
				session,
				item.caption.entities,
				ConvertOption::SkipLocal);
			auto sendFlags = MTPmessages_SendMedia::Flags(0);
			if (action.replyTo) {
				sendFlags |= MTPmessages_SendMedia::Flag::f_reply_to;
			}
			if (ShouldSendSilent(peer, action.options)) {
				sendFlags |= MTPmessages_SendMedia::Flag::f_silent;
			}
			if (!entities.v.isEmpty()) {
				sendFlags |= MTPmessages_SendMedia::Flag::f_entities;
			}
			if (action.options.scheduled) {
				sendFlags |= MTPmessages_SendMedia::Flag::f_schedule_date;
				if (action.options.scheduleRepeatPeriod) {
					sendFlags |= MTPmessages_SendMedia::Flag::f_schedule_repeat_period;
				}
			}
			if (action.options.shortcutId) {
				sendFlags |= MTPmessages_SendMedia::Flag::f_quick_reply_shortcut;
			}
			if (sendAs) {
				sendFlags |= MTPmessages_SendMedia::Flag::f_send_as;
			}
			if (action.options.effectId) {
				sendFlags |= MTPmessages_SendMedia::Flag::f_effect;
			}
			if (action.options.suggest) {
				sendFlags |= MTPmessages_SendMedia::Flag::f_suggested_post;
			}
			if (action.options.invertCaption) {
				sendFlags |= MTPmessages_SendMedia::Flag::f_invert_media;
			}
			if (batchStarsPaid) {
				sendFlags |= MTPmessages_SendMedia::Flag::f_allow_paid_stars;
			}
			auto &histories = history->owner().histories();
			histories.sendPreparedMessage(
				history,
				action.replyTo,
				item.randomId,
				Data::Histories::PrepareMessage<MTPmessages_SendMedia>(
					MTP_flags(sendFlags),
					peer->input(),
					Data::Histories::ReplyToPlaceholder(),
					inputMedia,
					MTP_string(item.caption.text),
					MTP_long(item.randomId),
					MTPReplyMarkup(),
					entities,
					MTP_int(action.options.scheduled),
					MTP_int(action.options.scheduleRepeatPeriod),
					(sendAs ? sendAs->input() : MTP_inputPeerEmpty()),
					Data::ShortcutIdToMTP(session, action.options.shortcutId),
					MTP_long(action.options.effectId),
					MTP_long(batchStarsPaid),
					SuggestToMTP(action.options.suggest)
				), [=](const MTPUpdates &result, const MTP::Response &response) {
					if (done) {
						done(QString());
					}
				}, retryOrFail);
			return;
		}

		using Flag = MTPmessages_SendMultiMedia::Flag;
		const auto sendFlags = Flag(0)
			| (action.replyTo ? Flag::f_reply_to : Flag(0))
			| (ShouldSendSilent(peer, action.options)
				? Flag::f_silent
				: Flag(0))
			| (action.options.scheduled ? Flag::f_schedule_date : Flag(0))
			| (sendAs ? Flag::f_send_as : Flag(0))
			| (action.options.shortcutId
				? Flag::f_quick_reply_shortcut
				: Flag(0))
			| (action.options.effectId ? Flag::f_effect : Flag(0))
			| (action.options.invertCaption ? Flag::f_invert_media : Flag(0))
			| (batchStarsPaid ? Flag::f_allow_paid_stars : Flag(0));
		auto media = QVector<MTPInputSingleMedia>();
		media.reserve(int(requests.size()));
		for (const auto &item : requests) {
			const auto entities = EntitiesToMTP(
				session,
				item.caption.entities,
				ConvertOption::SkipLocal);
			using SingleFlag = MTPDinputSingleMedia::Flag;
			media.push_back(MTP_inputSingleMedia(
				MTP_flags(!entities.v.isEmpty()
					? SingleFlag::f_entities
					: SingleFlag(0)),
				PrepareMergeInputMedia(item.media, action.options.mediaSpoiler),
				MTP_long(item.randomId),
				MTP_string(item.caption.text),
				entities));
		}
		auto &histories = history->owner().histories();
		histories.sendPreparedMessage(
			history,
			action.replyTo,
			uint64(0),
			Data::Histories::PrepareMessage<MTPmessages_SendMultiMedia>(
				MTP_flags(sendFlags),
				peer->input(),
				Data::Histories::ReplyToPlaceholder(),
				MTP_vector<MTPInputSingleMedia>(media),
				MTP_int(action.options.scheduled),
				(sendAs ? sendAs->input() : MTP_inputPeerEmpty()),
				Data::ShortcutIdToMTP(session, action.options.shortcutId),
				MTP_long(action.options.effectId),
				MTP_long(batchStarsPaid)
			), [=](const MTPUpdates &result, const MTP::Response &response) {
				if (done) {
					done(QString());
				}
			}, retryOrFail);
	};
	performRequest(performRequest, false);
}

} // namespace

MergeAlbumKind ClassifyMergeAlbumKind(not_null<HistoryItem*> item) {
	if (item->forbidsSaving()) {
		return MergeAlbumKind::Skip;
	}
	const auto media = item->media();
	if (!media || !media->canBeGrouped() || media->ttlSeconds()) {
		return MergeAlbumKind::Skip;
	}
	if (media->photo()) {
		return MergeAlbumKind::PhotoVideo;
	}
	const auto document = media->document();
	if (!document
		|| document->sticker()
		|| document->isVideoMessage()
		|| document->isVoiceMessage()
		|| document->isGifv()) {
		return MergeAlbumKind::Skip;
	} else if (document->isVideoFile()) {
		return MergeAlbumKind::PhotoVideo;
	} else if (document->isAnimation()) {
		return MergeAlbumKind::Skip;
	} else if (document->isSong() || document->isAudioFile()) {
		return MergeAlbumKind::Music;
	}
	return MergeAlbumKind::File;
}

std::vector<MergeAlbumGroup> PackAlbumGroups(
		const std::vector<MergeAlbumKind> &kinds,
		int maxItems) {
	Expects(maxItems > 0);

	auto result = std::vector<MergeAlbumGroup>();
	auto from = -1;
	auto kind = MergeAlbumKind::Skip;
	const auto flush = [&](int till) {
		if (from >= 0 && till > from) {
			result.push_back({
				.kind = kind,
				.from = from,
				.till = till,
			});
		}
		from = -1;
		kind = MergeAlbumKind::Skip;
	};
	for (auto i = 0; i != int(kinds.size()); ++i) {
		const auto current = kinds[i];
		if (current == MergeAlbumKind::Skip) {
			flush(i);
			continue;
		} else if (from < 0) {
			from = i;
			kind = current;
			continue;
		} else if (current != kind || (i - from) == maxItems) {
			flush(i);
			from = i;
			kind = current;
		}
	}
	flush(int(kinds.size()));
	return result;
}

void SendMergedAlbums(
		SendAction action,
		const std::vector<not_null<HistoryItem*>> &items,
		Fn<void(MergeAlbumResult)> done) {
	action.clearDraft = false;
	const auto session = &action.history->session();
	const auto captionLimit = session->serverConfig().captionLengthMax;
	auto media = CollectMergeMedia(items, captionLimit);
	auto result = MergeAlbumResult{
		.skipped = int(items.size()) - int(media.size()),
	};
	if (media.empty()) {
		if (done) {
			done(std::move(result));
		}
		return;
	}

	auto kinds = std::vector<MergeAlbumKind>();
	kinds.reserve(media.size());
	for (const auto &item : items) {
		const auto kind = ClassifyMergeAlbumKind(item);
		if (kind != MergeAlbumKind::Skip) {
			kinds.push_back(kind);
		}
	}
	Expects(kinds.size() == media.size());
	const auto groups = PackAlbumGroups(kinds, Ui::MaxAlbumItems());
	if (groups.empty()) {
		if (done) {
			done(std::move(result));
		}
		return;
	}

	struct State {
		SendAction action;
		std::vector<MergeAlbumMedia> media;
		std::vector<MergeAlbumGroup> groups;
		MergeAlbumResult result;
		int index = 0;
		Fn<void(MergeAlbumResult)> done;
	};
	const auto state = std::make_shared<State>(State{
		.action = action,
		.media = std::move(media),
		.groups = groups,
		.result = std::move(result),
		.index = 0,
		.done = std::move(done),
	});
	state->action.history->session().api().sendAction(state->action);

	const auto sendNext = [=](const auto &self) -> void {
		if (state->index >= int(state->groups.size())) {
			state->action.history->session().api().finishForwarding(
				state->action);
			if (state->done) {
				state->done(std::move(state->result));
			}
			return;
		}
		const auto group = state->groups[state->index++];
		auto batch = std::vector<MergeAlbumMedia>();
		batch.reserve(group.till - group.from);
		for (auto i = group.from; i != group.till; ++i) {
			batch.push_back(state->media[i]);
		}
		const auto sentIds = [&] {
			auto ids = MessageIdsList();
			ids.reserve(batch.size());
			for (const auto &entry : batch) {
				ids.push_back(entry.sourceId);
			}
			return ids;
		}();
		const auto count = int(batch.size());
		SendMergeGroup(state->action, std::move(batch), [=](QString error) {
			if (!error.isEmpty()) {
				state->result.error = error;
				state->action.history->session().api().finishForwarding(
					state->action);
				if (state->done) {
					state->done(std::move(state->result));
				}
				return;
			}
			state->result.sentMedia += count;
			if (count > 1) {
				++state->result.albumCount;
			}
			state->result.sentSourceIds.insert(
				state->result.sentSourceIds.end(),
				sentIds.begin(),
				sentIds.end());
			self(self);
		});
	};
	sendNext(sendNext);
}

} // namespace Api
