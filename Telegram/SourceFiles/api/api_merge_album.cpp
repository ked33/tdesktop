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
#include "base/debug_log.h"
#include "base/random.h"
#include "data/business/data_shortcut_messages.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "main/main_session.h"
#include "mtproto/mtproto_config.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/text/text_entity.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QStringList>

namespace Api {
namespace {

constexpr auto kMergeMessageTextLimit = 4096;
constexpr auto kMinSentenceChars = 12;

struct MergeAlbumMedia {
	PhotoData *photo = nullptr;
	DocumentData *document = nullptr;
	Data::FileOrigin origin;
	QString caption;
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

[[nodiscard]] QString DescribeMergeMedia(const MergeAlbumMedia &media) {
	auto result = u"src=%1/%2"_q.arg(
		media.sourceId.peer.value
	).arg(media.sourceId.msg.bare);
	const auto ref = MediaFileReference(media);
	if (media.photo) {
		result += u" photo=%1 ref=%2"_q.arg(media.photo->id).arg(ref.size());
	} else if (media.document) {
		result += u" doc=%1 video=%2 ref=%3"_q.arg(
			media.document->id
		).arg(media.document->isVideoFile() ? 1 : 0).arg(ref.size());
	}
	return result;
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

[[nodiscard]] const QRegularExpression &MergeTagTokenRe() {
	static const auto re = QRegularExpression(
		QString::fromUtf8(
			R"((?:https?://|www\.)[^\s<>\[\]{}]+|t\.me/[^\s<>\[\]{}]+|#[^\s#@]+|@[A-Za-z0-9_]{4,32})"),
		QRegularExpression::CaseInsensitiveOption);
	return re;
}

[[nodiscard]] QString MergeTagKey(QString token) {
	token = token.toLower();
	const auto strip = u".,;:!?)/"_q;
	while (!token.isEmpty() && strip.contains(token.back())) {
		token.chop(1);
	}
	if (token.startsWith(u"http://"_q)) {
		token = u"https://"_q + token.mid(7);
	} else if (token.startsWith(u"www."_q)) {
		token = u"https://"_q + token;
	} else if (token.startsWith(u"t.me/"_q)) {
		token = u"https://"_q + token;
	}
	while (token.endsWith(u'/')) {
		token.chop(1);
	}
	return token;
}

[[nodiscard]] QString StripMergeTags(const QString &text) {
	auto result = text;
	result.replace(MergeTagTokenRe(), QString());
	return result;
}

[[nodiscard]] QString DedupeHashtagsAndMentions(const QString &text) {
	auto seen = QSet<QString>();
	auto result = QString();
	auto last = 0;
	for (auto it = MergeTagTokenRe().globalMatch(text); it.hasNext();) {
		const auto match = it.next();
		result += text.mid(last, match.capturedStart() - last);
		const auto token = match.captured(0);
		const auto key = MergeTagKey(token);
		if (!seen.contains(key)) {
			seen.insert(key);
			result += token;
		}
		last = match.capturedEnd();
	}
	result += text.mid(last);
	return result;
}

[[nodiscard]] QString LineFingerprint(const QString &line) {
	return StripMergeTags(line).simplified().toLower();
}

[[nodiscard]] int CompactLen(const QString &text) {
	auto compact = text;
	compact.remove(QRegularExpression(u"\\s+"_q));
	auto total = 0;
	for (const auto &ch : compact) {
		const auto code = ch.unicode();
		if (code >= 0x4E00 && code <= 0x9FFF) {
			total += 2;
		} else if (ch.isLetterOrNumber()) {
			total += 1;
		}
	}
	return total;
}

[[nodiscard]] bool IsSubstantialSentence(const QString &fingerprint) {
	return CompactLen(fingerprint) >= kMinSentenceChars;
}

[[nodiscard]] QString ParagraphFingerprint(const QStringList &lines) {
	auto parts = QStringList();
	for (const auto &line : lines) {
		const auto part = LineFingerprint(line);
		if (!part.isEmpty()) {
			parts.push_back(part);
		}
	}
	return parts.join(u'\n');
}

[[nodiscard]] QStringList DedupeLineRuns(QStringList lines) {
	auto kept = QStringList();
	auto seenLongLines = QSet<QString>();
	auto seenPairs = QSet<QString>();
	auto index = 0;
	while (index < lines.size()) {
		auto line = lines[index];
		while (!line.isEmpty() && line.back().isSpace()) {
			line.chop(1);
		}
		const auto fingerprint = LineFingerprint(line);
		if (fingerprint.isEmpty()) {
			if (!line.trimmed().isEmpty()) {
				kept.push_back(line);
			}
			++index;
			continue;
		}
		if (index + 1 < lines.size()) {
			const auto pair = fingerprint
				+ u'\n'
				+ LineFingerprint(lines[index + 1]);
			if (!QString(pair).remove(u'\n').isEmpty()
				&& seenPairs.contains(pair)) {
				index += 2;
				continue;
			}
		}
		if (IsSubstantialSentence(fingerprint)
			&& seenLongLines.contains(fingerprint)) {
			++index;
			continue;
		}
		kept.push_back(line);
		if (IsSubstantialSentence(fingerprint)) {
			seenLongLines.insert(fingerprint);
		}
		if (kept.size() >= 2) {
			seenPairs.insert(
				LineFingerprint(kept[kept.size() - 2])
				+ u'\n'
				+ LineFingerprint(kept.back()));
		}
		++index;
	}
	return kept;
}

[[nodiscard]] QString DedupeRepeatedParagraphs(const QString &text) {
	const auto paragraphs = text.split(
		QRegularExpression(u"\\n\\s*\\n"_q));
	auto seen = QSet<QString>();
	auto seenLineFps = QSet<QString>();
	auto kept = QStringList();
	for (const auto &paragraph : paragraphs) {
		auto rawLines = paragraph.split(u'\n');
		for (auto &line : rawLines) {
			while (!line.isEmpty() && line.back().isSpace()) {
				line.chop(1);
			}
		}
		const auto lines = DedupeLineRuns(rawLines);
		auto cleanedLines = QStringList();
		for (const auto &line : lines) {
			if (!line.trimmed().isEmpty()) {
				cleanedLines.push_back(line);
			}
		}
		const auto cleaned = cleanedLines.join(u'\n');
		if (cleaned.trimmed().isEmpty()) {
			continue;
		}
		const auto fingerprint = ParagraphFingerprint(cleaned.split(u'\n'));
		auto lineFps = QStringList();
		auto lineCount = 0;
		for (const auto &line : cleaned.split(u'\n')) {
			if (!line.trimmed().isEmpty()) {
				++lineCount;
			}
			const auto fp = LineFingerprint(line);
			if (!fp.isEmpty()) {
				lineFps.push_back(fp);
			}
		}
		const auto substantial = (lineCount >= 2)
			|| IsSubstantialSentence(fingerprint);
		if (seen.contains(fingerprint)) {
			continue;
		}
		if (!lineFps.isEmpty()) {
			auto allSeen = true;
			for (const auto &fp : lineFps) {
				if (!seenLineFps.contains(fp)) {
					allSeen = false;
					break;
				}
			}
			if (allSeen) {
				continue;
			}
		}
		if (substantial && !fingerprint.isEmpty()) {
			seen.insert(fingerprint);
		}
		for (const auto &fp : lineFps) {
			seenLineFps.insert(fp);
		}
		kept.push_back(cleaned);
	}
	return kept.join(u"\n\n"_q);
}

[[nodiscard]] std::vector<QString> SplitTextChunks(
		QString text,
		int limit) {
	text = text.trimmed();
	auto result = std::vector<QString>();
	if (text.isEmpty()) {
		return result;
	} else if (text.size() <= limit) {
		result.push_back(text);
		return result;
	}
	auto current = QString();
	const auto lines = text.split(u'\n');
	for (const auto &line : lines) {
		auto piece = line.isEmpty() ? u" "_q : line;
		auto candidate = current.isEmpty()
			? piece
			: (current + u'\n' + piece);
		if (candidate.size() <= limit) {
			current = std::move(candidate);
			continue;
		}
		if (!current.isEmpty()) {
			result.push_back(current);
			current = QString();
		}
		while (piece.size() > limit) {
			result.push_back(piece.left(limit));
			piece = piece.mid(limit);
		}
		current = piece;
	}
	if (!current.isEmpty()) {
		result.push_back(current);
	}
	return result;
}

[[nodiscard]] bool IsMergeAlbumTextItem(not_null<HistoryItem*> item) {
	if (item->forbidsSaving()
		|| item->isService()
		|| !item->isRegular()
		|| ClassifyMergeAlbumKind(item) != MergeAlbumKind::Skip) {
		return false;
	} else if (item->originalText().text.trimmed().isEmpty()) {
		return false;
	}
	const auto media = item->media();
	return !media || media->webpage();
}

[[nodiscard]] int FindNearestMediaIndex(
		const std::vector<int> &mediaItemIndices,
		int textItemIndex) {
	auto bestMedia = -1;
	auto bestDist = 0;
	for (auto i = 0; i != int(mediaItemIndices.size()); ++i) {
		const auto itemIndex = mediaItemIndices[i];
		const auto dist = (itemIndex > textItemIndex)
			? (itemIndex - textItemIndex)
			: (textItemIndex - itemIndex);
		if (bestMedia < 0 || dist <= bestDist) {
			bestDist = dist;
			bestMedia = i;
		}
	}
	return bestMedia;
}

struct MergeSlot {
	enum class Type : uchar {
		Skip,
		Media,
		Text,
	};

	Type type = Type::Skip;
	MergeAlbumKind kind = MergeAlbumKind::Skip;
	MergeAlbumMedia media;
	QString text;
	FullMsgId sourceId;
	int mediaIndex = -1;
	int sourceKey = -1;
};

struct MergePrepared {
	std::vector<MergeSlot> slots;
	int sourceCount = 0;
};

struct SourceCaptionTarget {
	QStringList parts;
	int packedGroup = -1;
};

[[nodiscard]] MergePrepared PrepareMergeSlots(
		const std::vector<not_null<HistoryItem*>> &items) {
	auto prepared = MergePrepared();
	prepared.slots.reserve(items.size());
	auto mediaItemIndices = std::vector<int>();
	auto lastGroupId = MessageGroupId();
	auto lastWasGrouped = false;
	for (auto i = 0; i != int(items.size()); ++i) {
		const auto item = items[i];
		const auto kind = ClassifyMergeAlbumKind(item);
		auto slot = MergeSlot{ .sourceId = item->fullId() };
		if (kind != MergeAlbumKind::Skip) {
			const auto media = item->media();
			slot.type = MergeSlot::Type::Media;
			slot.kind = kind;
			slot.media = {
				.origin = item->fullId(),
				.caption = item->originalText().text.trimmed(),
				.sourceId = item->fullId(),
			};
			if (const auto photo = media->photo()) {
				slot.media.photo = photo;
			} else {
				slot.media.document = media->document();
			}
			if (slot.media.photo || slot.media.document) {
				const auto groupId = item->groupId();
				if (!groupId
					|| !lastWasGrouped
					|| groupId != lastGroupId) {
					slot.sourceKey = prepared.sourceCount++;
					lastGroupId = groupId;
					lastWasGrouped = bool(groupId);
				} else {
					slot.sourceKey = prepared.sourceCount - 1;
				}
				slot.mediaIndex = int(mediaItemIndices.size());
				mediaItemIndices.push_back(i);
				prepared.slots.push_back(std::move(slot));
				continue;
			}
			slot = MergeSlot{ .sourceId = item->fullId() };
		}
		if (IsMergeAlbumTextItem(item)) {
			slot.type = MergeSlot::Type::Text;
			slot.text = item->originalText().text.trimmed();
		}
		prepared.slots.push_back(std::move(slot));
	}
	for (auto i = 0; i != int(prepared.slots.size()); ++i) {
		if (prepared.slots[i].type == MergeSlot::Type::Text) {
			prepared.slots[i].mediaIndex = FindNearestMediaIndex(
				mediaItemIndices,
				i);
		}
	}
	return prepared;
}

[[nodiscard]] std::vector<SourceCaptionTarget> AssignSourceCaptions(
		const std::vector<MergeSlot> &slots,
		const std::vector<MergeAlbumGroup> &groups,
		int sourceCount) {
	struct Range {
		int first = -1;
		int last = -1;
		QStringList parts;
	};
	auto ranges = std::vector<Range>(sourceCount);
	auto mediaSourceKeys = std::vector<int>();
	for (const auto &slot : slots) {
		if (slot.type != MergeSlot::Type::Media || slot.sourceKey < 0) {
			continue;
		}
		if (slot.mediaIndex >= int(mediaSourceKeys.size())) {
			mediaSourceKeys.resize(slot.mediaIndex + 1, -1);
		}
		mediaSourceKeys[slot.mediaIndex] = slot.sourceKey;
		auto &range = ranges[slot.sourceKey];
		if (range.first < 0) {
			range.first = slot.mediaIndex;
		}
		range.last = slot.mediaIndex;
		if (!slot.media.caption.isEmpty()) {
			range.parts.push_back(slot.media.caption);
		}
	}
	const auto groupOf = [&](int mediaIndex) {
		for (auto i = 0; i != int(groups.size()); ++i) {
			if (mediaIndex >= groups[i].from
				&& mediaIndex < groups[i].till) {
				return i;
			}
		}
		return -1;
	};
	auto result = std::vector<SourceCaptionTarget>(sourceCount);
	for (auto key = 0; key != sourceCount; ++key) {
		const auto &range = ranges[key];
		if (range.first < 0) {
			continue;
		}
		const auto firstGroup = groupOf(range.first);
		const auto lastGroup = groupOf(range.last);
		auto packed = firstGroup;
		if (firstGroup >= 0
			&& firstGroup != lastGroup
			&& groups[firstGroup].from < int(mediaSourceKeys.size())
			&& mediaSourceKeys[groups[firstGroup].from] != key) {
			packed = lastGroup;
		}
		result[key] = {
			.parts = range.parts,
			.packedGroup = packed,
		};
	}
	return result;
}

void SendTextChunks(
		SendAction action,
		const QString &text) {
	action.clearDraft = false;
	auto &api = action.history->session().api();
	for (const auto &chunk : SplitTextChunks(text, kMergeMessageTextLimit)) {
		auto message = MessageToSend(action);
		message.textWithTags = { chunk, {} };
		message.action.clearDraft = false;
		api.sendMessage(std::move(message));
	}
}

void SendMergeGroup(
		SendAction action,
		std::vector<MergeAlbumMedia> items,
		const QString &caption,
		Fn<void(QString)> done) {
	Expects(!items.empty());

	const auto history = action.history;
	const auto peer = history->peer;
	const auto session = &history->session();
	const auto api = &session->api();
	const auto multi = (items.size() > 1);
	LOG(("MergeAlbum: send dest=%1 method=%2 count=%3"
	).arg(peer->id.value
	).arg(multi ? "sendMultiMedia" : "sendMedia"
	).arg(items.size()));
	for (const auto &item : items) {
		LOG(("MergeAlbum:   %1").arg(DescribeMergeMedia(item)));
	}
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
		auto itemCaption = (i == 0 && !caption.isEmpty())
			? TextWithEntities{ caption }
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

	const auto failRequest = [=](const MTP::Error &error, bool refreshed) {
		LOG(("MergeAlbum: fail dest=%1 method=%2 count=%3 error=%4 refreshed=%5"
		).arg(peer->id.value
		).arg(multi ? "sendMultiMedia" : "sendMedia"
		).arg(items.size()
		).arg(error.type()
		).arg(refreshed ? 1 : 0));
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
		const auto finishOk = [=] {
			LOG(("MergeAlbum: ok dest=%1 method=%2 count=%3 refreshed=%4"
			).arg(peer->id.value
			).arg(multi ? "sendMultiMedia" : "sendMedia"
			).arg(items.size()
			).arg(refreshed ? 1 : 0));
			if (done) {
				done(QString());
			}
		};
		const auto retryOrFail = [=](
				const MTP::Error &error,
				const MTP::Response &response) {
			if (refreshed
				|| (error.code() != 400)
				|| !error.type().startsWith(u"FILE_REFERENCE_"_q)
				|| refreshItems.empty()) {
				failRequest(error, refreshed);
				return;
			}
			LOG(("MergeAlbum: file_reference retry dest=%1 error=%2 items=%3"
			).arg(peer->id.value
			).arg(error.type()
			).arg(refreshItems.size()));
			const auto changed = std::make_shared<bool>(false);
			const auto left = std::make_shared<int>(int(refreshItems.size()));
			for (const auto &refresh : refreshItems) {
				api->refreshFileReference(refresh.origin, [=](const auto &) {
					const auto now = refresh.photo
						? refresh.photo->fileReference()
						: refresh.document->fileReference();
					*changed = *changed || (now != refresh.usedFileReference);
					if (!--*left) {
						LOG(("MergeAlbum: file_reference result dest=%1 changed=%2"
						).arg(peer->id.value
						).arg(*changed ? 1 : 0));
						if (*changed) {
							repeatRequest(repeatRequest, true);
						} else {
							failRequest(error, true);
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
					finishOk();
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
				finishOk();
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

QString DedupeMergeText(const QString &text) {
	return DedupeRepeatedParagraphs(DedupeHashtagsAndMentions(text));
}

QString SummarizeMergeCaptions(const std::vector<QString> &captions) {
	auto parts = QStringList();
	for (const auto &caption : captions) {
		const auto text = caption.trimmed();
		if (!text.isEmpty()) {
			parts.push_back(text);
		}
	}
	return DedupeMergeText(parts.join(u"\n\n"_q));
}

void SendMergedAlbums(
		SendAction action,
		const std::vector<not_null<HistoryItem*>> &items,
		Fn<void(MergeAlbumResult)> done) {
	action.clearDraft = false;
	const auto session = &action.history->session();
	const auto captionLimit = session->serverConfig().captionLengthMax;
	auto prepared = PrepareMergeSlots(items);
	auto media = std::vector<MergeAlbumMedia>();
	auto kinds = std::vector<MergeAlbumKind>();
	auto skipped = 0;
	media.reserve(prepared.slots.size());
	kinds.reserve(prepared.slots.size());
	for (const auto &slot : prepared.slots) {
		if (slot.type == MergeSlot::Type::Media) {
			media.push_back(slot.media);
			kinds.push_back(slot.kind);
		} else if (slot.type == MergeSlot::Type::Skip
			|| slot.mediaIndex < 0) {
			++skipped;
		}
	}
	auto result = MergeAlbumResult{ .skipped = skipped };
	if (media.empty()) {
		LOG(("MergeAlbum: no media dest=%1 selected=%2 skipped=%3"
		).arg(action.history->peer->id.value
		).arg(items.size()
		).arg(result.skipped));
		if (done) {
			done(std::move(result));
		}
		return;
	}
	Expects(kinds.size() == media.size());
	const auto groups = PackAlbumGroups(kinds, Ui::MaxAlbumItems());
	if (groups.empty()) {
		if (done) {
			done(std::move(result));
		}
		return;
	}
	auto sourceCaptions = AssignSourceCaptions(
		prepared.slots,
		groups,
		prepared.sourceCount);

	struct State {
		SendAction action;
		std::vector<MergeSlot> slots;
		std::vector<MergeAlbumMedia> media;
		std::vector<MergeAlbumGroup> groups;
		std::vector<SourceCaptionTarget> sourceCaptions;
		int sourceCount = 0;
		MergeAlbumResult result;
		int index = 0;
		Fn<void(MergeAlbumResult)> done;
	};
	const auto state = std::make_shared<State>(State{
		.action = action,
		.slots = std::move(prepared.slots),
		.media = std::move(media),
		.groups = groups,
		.sourceCaptions = std::move(sourceCaptions),
		.sourceCount = prepared.sourceCount,
		.result = std::move(result),
		.index = 0,
		.done = std::move(done),
	});
	LOG(("MergeAlbum: start dest=%1 selected=%2 media=%3 groups=%4 skipped=%5"
	).arg(action.history->peer->id.value
	).arg(items.size()
	).arg(state->media.size()
	).arg(state->groups.size()
	).arg(state->result.skipped));
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
		const auto groupIndex = state->index++;
		const auto group = state->groups[groupIndex];
		auto batch = std::vector<MergeAlbumMedia>();
		batch.reserve(group.till - group.from);
		for (auto i = group.from; i != group.till; ++i) {
			batch.push_back(state->media[i]);
		}
		auto captions = std::vector<QString>();
		auto sentIds = MessageIdsList();
		auto emittedSources = std::vector<char>(state->sourceCount, 0);
		captions.reserve(state->slots.size());
		sentIds.reserve(state->slots.size());
		for (const auto &slot : state->slots) {
			if (slot.mediaIndex < group.from
				|| slot.mediaIndex >= group.till) {
				continue;
			} else if (slot.type == MergeSlot::Type::Media) {
				sentIds.push_back(slot.sourceId);
				const auto key = slot.sourceKey;
				if (key >= 0
					&& key < state->sourceCount
					&& !emittedSources[key]
					&& state->sourceCaptions[key].packedGroup == groupIndex) {
					emittedSources[key] = 1;
					for (const auto &part : state->sourceCaptions[key].parts) {
						captions.push_back(part);
					}
				}
			} else if (slot.type == MergeSlot::Type::Text) {
				if (!slot.text.isEmpty()) {
					captions.push_back(slot.text);
				}
				sentIds.push_back(slot.sourceId);
			}
		}
		const auto mergedText = SummarizeMergeCaptions(captions);
		const auto caption = (mergedText.size() <= captionLimit)
			? mergedText
			: QString();
		const auto overflowText = caption.isEmpty() ? mergedText : QString();
		const auto count = int(batch.size());
		SendMergeGroup(
			state->action,
			std::move(batch),
			caption,
			[=](QString error) {
			if (!error.isEmpty()) {
				LOG(("MergeAlbum: group fail dest=%1 index=%2/%3 error=%4"
				).arg(state->action.history->peer->id.value
				).arg(state->index
				).arg(state->groups.size()
				).arg(error));
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
			if (!overflowText.isEmpty()) {
				SendTextChunks(state->action, overflowText);
			}
			self(self);
		});
	};
	sendNext(sendNext);
}

MergeAlbumCleanup CleanupMergedSources(
		not_null<Main::Session*> session,
		const MessageIdsList &ids) {
	auto result = MergeAlbumCleanup();
	auto deleteIds = MessageIdsList();
	for (const auto &id : ids) {
		const auto item = session->data().message(id);
		if (!item) {
			continue;
		} else if (item->canDelete()) {
			deleteIds.push_back(id);
		} else {
			++result.kept;
		}
	}
	if (!deleteIds.empty()) {
		session->data().histories().deleteMessages(deleteIds, true);
		session->data().sendHistoryChangeNotifications();
	}
	result.deleted = int(deleteIds.size());
	return result;
}

} // namespace Api
