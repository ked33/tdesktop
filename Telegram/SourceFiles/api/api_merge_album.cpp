/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_merge_album.h"
#include "api/api_selected_action.h"
#include "lang/lang_keys.h"

#include "api/api_editing.h"
#include "api/api_sending.h"
#include "api/api_text_entities.h"
#include "apiwrap.h"
#include "base/debug_log.h"
#include "base/flat_set.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "data/business/data_shortcut_messages.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_document.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "main/main_session.h"
#include "mtproto/mtproto_config.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/text/text_entity.h"
#include "ui/text/text_utilities.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QStringList>

#include <algorithm>

namespace Api {
namespace {

constexpr auto kMergeMessageTextLimit = 4096;
constexpr auto kMinSentenceChars = 12;

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

struct MappedText {
	QString text;
	std::vector<int> origin;
};

void AppendSourceRange(
		MappedText &out,
		const QString &text,
		int from,
		int till) {
	if (till <= from) {
		return;
	}
	out.text.append(text.mid(from, till - from));
	out.origin.reserve(out.origin.size() + (till - from));
	for (auto i = from; i != till; ++i) {
		out.origin.push_back(i);
	}
}

void AppendSynthetic(MappedText &out, QChar ch) {
	out.text.append(ch);
	out.origin.push_back(-1);
}

[[nodiscard]] bool IsMergeLinkEntity(EntityType type) {
	return (type == EntityType::Url) || (type == EntityType::CustomUrl);
}

void TrimEdges(TextWithEntities &result) {
	auto end = result.text.size();
	while (end > 0 && result.text.at(end - 1).isSpace()) {
		--end;
	}
	if (!end) {
		result = {};
		return;
	}
	if (end < result.text.size()) {
		for (auto &entity : result.entities) {
			entity.updateTextEnd(end);
		}
		result.text.resize(end);
	}
	auto start = 0;
	while (start < result.text.size() && result.text.at(start).isSpace()) {
		++start;
	}
	if (start > 0) {
		for (auto &entity : result.entities) {
			entity.shiftLeft(start);
		}
		result.text.remove(0, start);
	}
	auto write = 0;
	for (auto read = 0; read != result.entities.size(); ++read) {
		const auto entity = result.entities[read];
		if (entity.length() > 0 && entity.validForText(result.text.size())) {
			if (write != read) {
				result.entities[write] = entity;
			}
			++write;
		}
	}
	if (write != result.entities.size()) {
		result.entities.erase(
			result.entities.begin() + write,
			result.entities.end());
	}
}

[[nodiscard]] TextWithEntities PreserveLinkText(const TextWithEntities &source) {
	auto result = TextWithEntities();
	result.text = source.text;
	result.entities.reserve(source.entities.size());
	for (const auto &entity : source.entities) {
		if (!IsMergeLinkEntity(entity.type())
			|| !entity.validForText(source.text.size())) {
			continue;
		}
		result.entities.push_back(entity);
	}
	return result;
}

[[nodiscard]] TextWithEntities FinishLinkEntities(
		QString text,
		const EntitiesInText &entities,
		const std::vector<int> &origin,
		int sourceLength) {
	auto result = TextWithEntities();
	result.text = std::move(text);
	if (int(origin.size()) != result.text.size() || sourceLength < 0) {
		return result;
	}
	auto sourceToOut = std::vector<int>(sourceLength, -1);
	for (auto out = 0; out != int(origin.size()); ++out) {
		const auto src = origin[out];
		if (src >= 0 && src < sourceLength) {
			sourceToOut[src] = out;
		}
	}
	result.entities.reserve(entities.size());
	for (const auto &entity : entities) {
		if (!IsMergeLinkEntity(entity.type())
			|| !entity.validForText(sourceLength)) {
			continue;
		}
		const auto start = entity.offset();
		const auto end = start + entity.length();
		auto outFirst = -1;
		auto outLast = -1;
		auto kept = 0;
		auto keptStart = start;
		auto keptEnd = start;
		for (auto index = start; index != end; ++index) {
			const auto out = sourceToOut[index];
			if (out < 0) {
				continue;
			} else if (outFirst < 0) {
				outFirst = out;
				keptStart = index;
			}
			outLast = out;
			keptEnd = index + 1;
			++kept;
		}
		if (kept <= 0 || (outLast - outFirst + 1) != kept) {
			continue;
		}
		auto hole = false;
		for (auto index = keptStart; index != keptEnd; ++index) {
			if (sourceToOut[index] < 0) {
				hole = true;
				break;
			}
		}
		if (hole) {
			continue;
		}
		result.entities.push_back(EntityInText(
			entity.type(),
			outFirst,
			kept,
			entity.data()));
	}
	std::stable_sort(
		result.entities.begin(),
		result.entities.end(),
		[](const EntityInText &a, const EntityInText &b) {
			if (a.offset() < b.offset()) {
				return true;
			} else if (a.offset() > b.offset()) {
				return false;
			}
			return a.length() > b.length();
		});
	return result;
}

[[nodiscard]] MappedText DedupeHashtagsAndMentionsMapped(const QString &text) {
	auto seen = QSet<QString>();
	auto result = MappedText();
	result.text.reserve(text.size());
	result.origin.reserve(text.size());
	auto last = 0;
	for (auto it = MergeTagTokenRe().globalMatch(text); it.hasNext();) {
		const auto match = it.next();
		AppendSourceRange(result, text, last, match.capturedStart());
		const auto token = match.captured(0);
		const auto key = MergeTagKey(token);
		if (!seen.contains(key)) {
			seen.insert(key);
			AppendSourceRange(
				result,
				text,
				match.capturedStart(),
				match.capturedEnd());
		}
		last = match.capturedEnd();
	}
	AppendSourceRange(result, text, last, text.size());
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

[[nodiscard]] std::vector<int> DedupeLineRunIndices(const QStringList &lines) {
	auto kept = QStringList();
	auto indices = std::vector<int>();
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
				indices.push_back(index);
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
		indices.push_back(index);
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
	return indices;
}

struct TextSpan {
	int from = 0;
	int till = 0;
};

[[nodiscard]] const QRegularExpression &ParagraphGapRe() {
	static const auto re = QRegularExpression(u"\\n\\s*\\n"_q);
	return re;
}

[[nodiscard]] std::vector<TextSpan> ParagraphSpans(const QString &text) {
	auto result = std::vector<TextSpan>();
	auto last = 0;
	for (auto it = ParagraphGapRe().globalMatch(text); it.hasNext();) {
		const auto match = it.next();
		result.push_back({ last, int(match.capturedStart()) });
		last = int(match.capturedEnd());
	}
	result.push_back({ last, int(text.size()) });
	return result;
}

struct LineSpan {
	int from = 0;
	int till = 0;
	int newline = -1;
};

[[nodiscard]] std::vector<LineSpan> LinesOf(
		const QString &text,
		int paraFrom,
		int paraTill) {
	auto result = std::vector<LineSpan>();
	auto start = paraFrom;
	for (auto i = paraFrom; i <= paraTill; ++i) {
		if (i != paraTill && text[i] != QChar('\n')) {
			continue;
		}
		auto till = i;
		while (till > start && text[till - 1].isSpace()) {
			--till;
		}
		result.push_back({
			.from = start,
			.till = till,
			.newline = (i < paraTill) ? i : -1,
		});
		start = i + 1;
	}
	return result;
}

[[nodiscard]] MappedText DedupeRepeatedParagraphsMapped(const QString &text) {
	auto result = MappedText();
	const auto paragraphs = ParagraphSpans(text);
	auto seen = QSet<QString>();
	auto seenLineFps = QSet<QString>();
	for (const auto &paragraph : paragraphs) {
		const auto rawLines = LinesOf(text, paragraph.from, paragraph.till);
		auto lineStrings = QStringList();
		lineStrings.reserve(int(rawLines.size()));
		for (const auto &line : rawLines) {
			lineStrings.push_back(text.mid(line.from, line.till - line.from));
		}
		const auto keptIndices = DedupeLineRunIndices(lineStrings);
		struct KeptLine {
			int rawIndex = 0;
			int from = 0;
			int till = 0;
			int newline = -1;
			QString text;
		};
		auto cleaned = std::vector<KeptLine>();
		cleaned.reserve(keptIndices.size());
		for (const auto index : keptIndices) {
			const auto &line = rawLines[index];
			const auto &lineText = lineStrings[index];
			if (lineText.trimmed().isEmpty()) {
				continue;
			}
			cleaned.push_back({
				.rawIndex = index,
				.from = line.from,
				.till = line.till,
				.newline = line.newline,
				.text = lineText,
			});
		}
		if (cleaned.empty()) {
			continue;
		}
		auto cleanedText = QString();
		for (auto i = 0; i != int(cleaned.size()); ++i) {
			if (i) {
				cleanedText.append(u'\n');
			}
			cleanedText.append(cleaned[i].text);
		}
		const auto fingerprint = ParagraphFingerprint(cleanedText.split(u'\n'));
		auto lineFps = QStringList();
		auto lineCount = 0;
		for (const auto &line : cleanedText.split(u'\n')) {
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
		if (!result.text.isEmpty()) {
			AppendSynthetic(result, QChar('\n'));
		}
		for (auto i = 0; i != int(cleaned.size()); ++i) {
			if (i) {
				const auto &prev = cleaned[i - 1];
				const auto adjacent = (cleaned[i].rawIndex == (prev.rawIndex + 1));
				if (adjacent
					&& prev.newline >= 0
					&& prev.newline < text.size()) {
					AppendSourceRange(
						result,
						text,
						prev.newline,
						prev.newline + 1);
				} else {
					AppendSynthetic(result, QChar('\n'));
				}
			}
			AppendSourceRange(
				result,
				text,
				cleaned[i].from,
				cleaned[i].till);
		}
	}
	return result;
}

[[nodiscard]] int SafeCut(
		const TextWithEntities &text,
		int from,
		int proposed) {
	auto cut = proposed;
	const auto pull = [&](int start, int end) {
		if (start < cut && end > cut && start > from) {
			cut = start;
		}
	};
	for (const auto &entity : text.entities) {
		if (!IsMergeLinkEntity(entity.type())) {
			continue;
		}
		pull(entity.offset(), entity.offset() + entity.length());
	}
	for (auto it = MergeTagTokenRe().globalMatch(text.text); it.hasNext();) {
		const auto match = it.next();
		pull(int(match.capturedStart()), int(match.capturedEnd()));
	}
	return (cut > from) ? cut : proposed;
}

[[nodiscard]] std::vector<TextWithEntities> SplitTextChunks(
		TextWithEntities text,
		int limit) {
	TrimEdges(text);
	auto result = std::vector<TextWithEntities>();
	if (text.text.isEmpty()) {
		return result;
	} else if (text.text.size() <= limit) {
		result.push_back(std::move(text));
		return result;
	}
	struct Seg {
		int from = 0;
		int till = 0;
		bool synthetic = false;
	};
	const auto build = [&](const std::vector<Seg> &segs) {
		auto mapped = MappedText();
		for (auto i = 0; i != int(segs.size()); ++i) {
			if (i) {
				const auto &prev = segs[i - 1];
				const auto &cur = segs[i];
				if (!prev.synthetic
					&& !cur.synthetic
					&& prev.till >= 0
					&& (prev.till + 1) == cur.from
					&& prev.till < text.text.size()
					&& text.text.at(prev.till) == QChar('\n')) {
					AppendSourceRange(
						mapped,
						text.text,
						prev.till,
						prev.till + 1);
				} else {
					AppendSynthetic(mapped, QChar('\n'));
				}
			}
			const auto &seg = segs[i];
			if (seg.synthetic) {
				AppendSynthetic(mapped, QChar(' '));
			} else {
				AppendSourceRange(mapped, text.text, seg.from, seg.till);
			}
		}
		return FinishLinkEntities(
			std::move(mapped.text),
			text.entities,
			mapped.origin,
			text.text.size());
	};
	auto current = std::vector<Seg>();
	auto currentSize = 0;
	const auto emitCurrent = [&] {
		if (current.empty()) {
			return;
		}
		result.push_back(build(current));
		current.clear();
		currentSize = 0;
	};
	const auto appendPiece = [&](Seg seg, int segSize) {
		const auto extra = current.empty() ? 0 : 1;
		if ((currentSize + extra + segSize) <= limit) {
			current.push_back(seg);
			currentSize += extra + segSize;
			return;
		}
		emitCurrent();
		if (seg.synthetic || segSize <= limit) {
			current.push_back(seg);
			currentSize = segSize;
			return;
		}
		auto from = seg.from;
		const auto till = seg.till;
		while ((till - from) > limit) {
			auto cut = from + limit;
			const auto safe = SafeCut(text, from, cut);
			if (safe > from && safe < till) {
				cut = safe;
			}
			auto piece = std::vector<Seg>();
			piece.push_back({
				.from = from,
				.till = cut,
				.synthetic = false,
			});
			result.push_back(build(piece));
			from = cut;
		}
		current.push_back({
			.from = from,
			.till = till,
			.synthetic = false,
		});
		currentSize = till - from;
	};
	const auto size = text.text.size();
	auto lineFrom = 0;
	for (auto i = 0; i <= size; ++i) {
		if (i != size && text.text[i] != QChar('\n')) {
			continue;
		}
		if (lineFrom == i) {
			appendPiece({
				.from = 0,
				.till = 0,
				.synthetic = true,
			}, 1);
		} else {
			appendPiece({
				.from = lineFrom,
				.till = i,
				.synthetic = false,
			}, i - lineFrom);
		}
		lineFrom = i + 1;
	}
	emitCurrent();
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
	TextWithEntities text;
	FullMsgId sourceId;
	int mediaIndex = -1;
	int sourceKey = -1;
};

struct MergePrepared {
	std::vector<MergeSlot> slots;
	int sourceCount = 0;
};

struct SourceCaptionTarget {
	std::vector<TextWithEntities> parts;
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
			auto caption = PreserveLinkText(item->originalText());
			TrimEdges(caption);
			slot.media = {
				.origin = item->fullId(),
				.caption = std::move(caption),
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
			slot.text = PreserveLinkText(item->originalText());
			TrimEdges(slot.text);
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
		std::vector<TextWithEntities> parts;
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
		if (!slot.media.caption.empty()) {
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
		const TextWithEntities &text) {
	action.clearDraft = false;
	auto &api = action.history->session().api();
	for (const auto &chunk : SplitTextChunks(text, kMergeMessageTextLimit)) {
		auto message = MessageToSend(action);
		message.textWithTags = {
			chunk.text,
			TextUtilities::ConvertEntitiesToTextTags(chunk.entities),
		};
		message.action.clearDraft = false;
		api.sendMessage(std::move(message));
	}
}

void SendTextWithEntities(
		SendAction action,
		TextWithEntities text) {
	if (text.empty()) {
		return;
	}
	action.clearDraft = false;
	auto message = MessageToSend(action);
	message.textWithTags = {
		text.text,
		TextUtilities::ConvertEntitiesToTextTags(text.entities),
	};
	message.webPage.removed = true;
	message.action.clearDraft = false;
	action.history->session().api().sendMessage(std::move(message));
}

void SendMergeGroup(
		SendAction action,
		std::vector<MergeAlbumMedia> items,
		TextWithEntities caption,
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
		auto itemCaption = (i == 0)
			? caption
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

	const auto tracked = TrackSelectedAction(action.progress,
		tr::lng_selected_action_merge(tr::now), int(items.size()),
		SelectedActionPeer(peer));
	if (tracked) {
		tracked->start();
	}

	const auto failRequest = [=](const MTP::Error &error, bool refreshed) {
		if (tracked) {
			tracked->fail(error);
		}
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
			if (tracked) {
				tracked->done();
			}
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
			if (tracked) {
				tracked->retry(error);
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
				}, retryOrFail, tracked, false);
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
			}, retryOrFail, tracked, false);
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
	return PackAlbumGroups(kinds, {}, maxItems);
}

std::vector<MergeAlbumGroup> PackAlbumGroups(
		const std::vector<MergeAlbumKind> &kinds,
		const std::vector<PeerId> &sourcePeers,
		int maxItems) {
	Expects(maxItems > 0);
	Expects(sourcePeers.empty() || sourcePeers.size() == kinds.size());

	auto result = std::vector<MergeAlbumGroup>();
	auto from = -1;
	auto kind = MergeAlbumKind::Skip;
	const auto sameSource = [&](int i) {
		return sourcePeers.empty()
			|| (sourcePeers[i] == sourcePeers[from]);
	};
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
		} else if (current != kind
			|| !sameSource(i)
			|| (i - from) == maxItems) {
			flush(i);
			from = i;
			kind = current;
		}
	}
	flush(int(kinds.size()));
	return result;
}

TextWithEntities DedupeMergeText(const TextWithEntities &text) {
	const auto tags = DedupeHashtagsAndMentionsMapped(text.text);
	auto paragraphs = DedupeRepeatedParagraphsMapped(tags.text);
	auto origin = std::vector<int>();
	origin.reserve(paragraphs.origin.size());
	for (const auto mid : paragraphs.origin) {
		if (mid >= 0 && mid < int(tags.origin.size())) {
			origin.push_back(tags.origin[mid]);
		} else {
			origin.push_back(-1);
		}
	}
	return FinishLinkEntities(
		std::move(paragraphs.text),
		text.entities,
		origin,
		text.text.size());
}

TextWithEntities SummarizeMergeCaptions(
		const std::vector<TextWithEntities> &captions) {
	auto combined = TextWithEntities();
	for (const auto &caption : captions) {
		auto part = caption;
		TrimEdges(part);
		if (part.text.isEmpty()) {
			continue;
		}
		if (!combined.text.isEmpty()) {
			combined.append(u"\n"_q);
		}
		combined.append(u"- "_q);
		combined.append(std::move(part));
	}
	return DedupeMergeText(combined);
}

QString SourceMessageLink(not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	const auto &session = item->history()->session();
	const auto post = QString::number(item->id.bare);
	if (const auto channel = peer->asChannel()) {
		const auto base = channel->hasUsername()
			? channel->username()
			: (u"c/"_q + QString::number(peerToChannel(channel->id).bare));
		return session.createInternalLinkFull(base + '/' + post);
	} else if (!peer->username().isEmpty()) {
		return session.createInternalLinkFull(peer->username() + '/' + post);
	} else if (const auto user = peer->asUser()) {
		return u"tg://openmessage?user_id=%1&message_id=%2"_q
			.arg(peerToUser(user->id).bare)
			.arg(item->id.bare);
	} else if (const auto chat = peer->asChat()) {
		return u"tg://openmessage?chat_id=%1&message_id=%2"_q
			.arg(peerToChat(chat->id).bare)
			.arg(item->id.bare);
	}
	return QString();
}

TextWithEntities SourceLinkFooter(
		const std::vector<not_null<HistoryItem*>> &items) {
	auto links = std::vector<QString>();
	auto seen = base::flat_set<QString>();
	for (const auto &item : items) {
		const auto link = SourceMessageLink(item);
		if (link.isEmpty() || seen.contains(link)) {
			continue;
		}
		seen.emplace(link);
		links.push_back(link);
	}
	auto result = TextWithEntities();
	if (links.empty()) {
		return result;
	}
	result.append(QChar(' '));
	for (auto i = 0; i != int(links.size()); ++i) {
		if (i) {
			result.append(u", "_q);
		}
		result.append(Ui::Text::Link(
			u"原消息%1"_q.arg(i + 1),
			links[i]));
	}
	return result;
}

TextWithEntities AppendSourceLinkFooter(
		TextWithEntities content,
		const std::vector<not_null<HistoryItem*>> &items) {
	auto footer = SourceLinkFooter(items);
	if (footer.empty()) {
		return content;
	}
	if (!content.text.trimmed().isEmpty()
		&& !content.text.endsWith(u"\n\n"_q)) {
		content.append(content.text.endsWith(QChar('\n'))
			? u"\n"_q
			: u"\n\n"_q);
	}
	return content.append(std::move(footer));
}

void SendMergedAlbums(
		SendAction action,
		const std::vector<not_null<HistoryItem*>> &items,
		Fn<void(MergeAlbumResult)> done,
		bool appendSourceLinks) {
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
	if (action.progress) {
		action.progress->skip(skipped, u"Unsupported media"_q);
	}
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
		bool appendSourceLinks = false;
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
		.appendSourceLinks = appendSourceLinks,
	});
	auto uniqueSources = base::flat_set<PeerId>();
	for (const auto &item : state->media) {
		uniqueSources.emplace(item.sourceId.peer);
	}
	LOG(("MergeAlbum: start dest=%1 selected=%2 media=%3 groups=%4 sources=%5 skipped=%6"
	).arg(action.history->peer->id.value
	).arg(items.size()
	).arg(state->media.size()
	).arg(state->groups.size()
	).arg(int(uniqueSources.size())
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
		auto captions = std::vector<TextWithEntities>();
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
				if (!slot.text.empty()) {
					captions.push_back(slot.text);
				}
				sentIds.push_back(slot.sourceId);
			}
		}
		const auto merged = SummarizeMergeCaptions(captions);
		auto caption = merged;
		auto overflowText = TextWithEntities();
		auto overflowFooter = TextWithEntities();
		if (state->appendSourceLinks) {
			auto sourceItems = std::vector<not_null<HistoryItem*>>();
			sourceItems.reserve(sentIds.size());
			for (const auto &id : sentIds) {
				if (const auto item = session->data().message(id)) {
					sourceItems.push_back(item);
				}
			}
			const auto withFooter = AppendSourceLinkFooter(
				caption,
				sourceItems);
			if (withFooter.text.size() <= captionLimit) {
				caption = withFooter;
			} else if (merged.text.size() <= captionLimit) {
				overflowFooter = SourceLinkFooter(sourceItems);
			} else {
				caption = {};
				overflowText = merged;
				overflowFooter = SourceLinkFooter(sourceItems);
			}
		} else if (merged.text.size() > captionLimit) {
			caption = {};
			overflowText = merged;
		}
		const auto count = int(batch.size());
		SendMergeGroup(
			state->action,
			std::move(batch),
			std::move(caption),
			[=](QString error) {
			if (!error.isEmpty()) {
				LOG(("MergeAlbum: group fail dest=%1 index=%2/%3 error=%4"
				).arg(state->action.history->peer->id.value
				).arg(state->index
				).arg(state->groups.size()
				).arg(error));
				state->result.error = error;
				self(self);
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
			if (!overflowText.empty()) {
				SendTextChunks(state->action, overflowText);
			}
			if (!overflowFooter.empty()) {
				SendTextWithEntities(state->action, overflowFooter);
			}
			self(self);
		});
	};
	sendNext(sendNext);
}

void CollectNewMessageIds(
		const MTPUpdates &updates,
		PeerId dest,
		MessageIdsList &out) {
	const auto takeMessage = [&](const MTPMessage &message) {
		if (PeerFromMessage(message) != dest) {
			return;
		}
		const auto id = IdFromMessage(message);
		if (id) {
			out.push_back({ dest, id });
		}
	};
	const auto takeUpdate = [&](const MTPUpdate &update) {
		update.match([&](const MTPDupdateNewMessage &data) {
			takeMessage(data.vmessage());
		}, [&](const MTPDupdateNewChannelMessage &data) {
			takeMessage(data.vmessage());
		}, [](const auto &) {
		});
	};
	updates.match([&](const MTPDupdates &data) {
		for (const auto &update : data.vupdates().v) {
			takeUpdate(update);
		}
	}, [&](const MTPDupdatesCombined &data) {
		for (const auto &update : data.vupdates().v) {
			takeUpdate(update);
		}
	}, [](const auto &) {
	});
}

void AppendSourceLinksToCopiedMessages(
		not_null<Main::Session*> session,
		const std::vector<not_null<HistoryItem*>> &sources,
		const MessageIdsList &destIds,
		std::shared_ptr<SelectedAction> progress) {
	const auto n = std::min(int(sources.size()), int(destIds.size()));
	if (n <= 0) {
		if (progress && !sources.empty()) {
			const auto missing = progress->add(tr::lng_selected_action_links(tr::now), 1);
			missing->fail(u"Destination messages missing from server response"_q);
		}
		return;
	}
	for (auto i = 0; i < n;) {
		const auto dest = session->data().message(destIds[i]);
		if (!dest) {
			++i;
			continue;
		}
		auto groupSources = std::vector<not_null<HistoryItem*>>();
		groupSources.push_back(sources[i]);
		auto till = i + 1;
		const auto groupId = dest->groupId();
		if (groupId) {
			while (till < n) {
				const auto next = session->data().message(destIds[till]);
				if (!next || next->groupId() != groupId) {
					break;
				}
				groupSources.push_back(sources[till]);
				++till;
			}
		}
		const auto footer = SourceLinkFooter(groupSources);
		if (footer.empty()) {
			i = till;
			continue;
		}
		const auto combined = AppendSourceLinkFooter(
			dest->originalText(),
			groupSources);
		const auto media = dest->media();
		const auto captionLimit = session->serverConfig().captionLengthMax;
		const auto now = base::unixtime::now();
		const auto canEdit = dest->allowsEdit(now);
		const auto captionable = media && media->allowsEditCaption();
		const auto textEditable = !media || media->webpage();
		const auto history = dest->history();
		auto action = SendAction(history);
		action.progress = progress;
		const auto tracked = TrackSelectedAction(progress,
			tr::lng_selected_action_links(tr::now), 1, SelectedActionPeer(history->peer));
		if (tracked) {
			tracked->start();
		}
		auto options = SendOptions();
		options.invertCaption = dest->invertMedia();
		if (canEdit
			&& captionable
			&& combined.text.size() <= captionLimit) {
			const auto requestId = EditCaption(dest, combined, options, [=] {
				if (tracked) {
					tracked->done();
				}
			}, [=](const QString &error) {
				if (tracked) {
					tracked->fail(error);
				}
				SendTextWithEntities(action, footer);
			});
			if (tracked) {
				tracked->observe(session, requestId);
			}
		} else if (canEdit
			&& textEditable
			&& combined.text.size() <= kMergeMessageTextLimit) {
			const auto requestId = EditTextMessage(
				dest,
				combined,
				Data::WebPageDraft{ .removed = true },
				options,
				[=](mtpRequestId) {
					if (tracked) {
						tracked->done();
					}
				},
				[=](const QString &error, mtpRequestId) {
					if (tracked) {
						tracked->fail(error);
					}
					SendTextWithEntities(action, footer);
				},
				false);
			if (tracked) {
				tracked->observe(session, requestId);
			}
		} else {
			SendTextWithEntities(action, footer);
			if (tracked) {
				tracked->done();
			}
		}
		i = till;
	}
}

namespace {

void ForwardOneAsCopy(
		SendAction action,
		not_null<HistoryItem*> item,
		Fn<void(FullMsgId, QString)> done) {
	const auto history = action.history;
	const auto peer = history->peer;
	const auto session = &history->session();
	const auto fromPeer = item->history()->peer;
	using Flag = MTPmessages_ForwardMessages::Flag;
	auto sendFlags = MTPmessages_ForwardMessages::Flags(0);
	sendFlags |= Flag::f_drop_author;
	if (ShouldSendSilent(peer, action.options)) {
		sendFlags |= Flag::f_silent;
	}
	if (action.options.scheduled) {
		sendFlags |= Flag::f_schedule_date;
		if (action.options.scheduleRepeatPeriod) {
			sendFlags |= Flag::f_schedule_repeat_period;
		}
	}
	if (action.options.shortcutId) {
		sendFlags |= Flag::f_quick_reply_shortcut;
	}
	if (action.options.effectId) {
		sendFlags |= Flag::f_effect;
	}
	const auto kGeneralId = Data::ForumTopic::kGeneralId;
	const auto topicRootId = action.replyTo.topicRootId;
	const auto topMsgId = (topicRootId == kGeneralId)
		? MsgId(0)
		: topicRootId;
	if (topMsgId) {
		sendFlags |= Flag::f_top_msg_id;
	}
	const auto starsPaid = std::min(
		peer->starsPerMessageChecked(),
		action.options.starsApproved);
	if (starsPaid) {
		action.options.starsApproved -= starsPaid;
		sendFlags |= Flag::f_allow_paid_stars;
	}
	const auto randomId = base::RandomValue<uint64>();
	const auto dest = peer->id;
	LOG(("MergeAlbum: copy dest=%1 src=%2/%3"
	).arg(dest.value
	).arg(item->history()->peer->id.value
	).arg(item->id.bare));
	history->owner().histories().sendPreparedMessage(
		history,
		FullReplyTo{ .topicRootId = topicRootId },
		uint64(0),
		[=](not_null<History*> history, FullReplyTo)
		-> Data::Histories::PreparedMessage {
			return MTPmessages_ForwardMessages(
				MTP_flags(sendFlags),
				fromPeer->input(),
				MTP_vector<MTPint>(1, MTP_int(item->id)),
				MTP_vector<MTPlong>(1, MTP_long(randomId)),
				history->peer->input(),
				MTP_int(topMsgId),
				MTPInputReplyTo(),
				MTP_int(action.options.scheduled),
				MTP_int(action.options.scheduleRepeatPeriod),
				MTP_inputPeerEmpty(),
				Data::ShortcutIdToMTP(session, action.options.shortcutId),
				MTP_long(action.options.effectId),
				MTPint(),
				MTP_long(starsPaid),
				SuggestToMTP(action.options.suggest));
		},
		[=](const MTPUpdates &updates, const MTP::Response &) {
			auto ids = MessageIdsList();
			CollectNewMessageIds(updates, dest, ids);
			LOG(("MergeAlbum: copy ok dest=%1 got=%2"
			).arg(dest.value
			).arg(int(ids.size())));
			done(ids.empty() ? FullMsgId() : ids.front(), QString());
		},
		[=](const MTP::Error &error, const MTP::Response &) {
			LOG(("MergeAlbum: copy fail dest=%1 error=%2"
			).arg(dest.value
			).arg(error.type()));
			done(FullMsgId(), error.type());
		}, TrackSelectedAction(action.progress,
			tr::lng_selected_action_copy(tr::now), 1,
			SelectedActionPeer(item->history()->peer) + u" → "_q
				+ SelectedActionPeer(history->peer)));
}

} // namespace

void CopyThenMergeAlbums(
		SendAction action,
		const std::vector<not_null<HistoryItem*>> &items,
		Fn<void(MergeAlbumResult)> done) {
	action.clearDraft = false;
	action.generateLocal = false;
	if (items.empty()) {
		if (done) {
			done(MergeAlbumResult());
		}
		return;
	}
	const auto sameChat = (action.history == items.front()->history())
		&& (!items.front()->topic()
			|| (action.replyTo.topicRootId == items.front()->topicRootId()));
	if (sameChat) {
		SendMergedAlbums(
			std::move(action),
			items,
			std::move(done),
			true);
		return;
	}
	if (!action.options.scheduled && !action.options.shortcutId) {
		action.history->owner().histories().readInbox(action.history);
	}
	LOG(("MergeAlbum: copy-then-merge dest=%1 selected=%2"
	).arg(action.history->peer->id.value
	).arg(int(items.size())));

	struct State {
		SendAction action;
		MessageIdsList items;
		MessageIdsList copied;
		QString error;
		int index = 0;
		Fn<void(MergeAlbumResult)> done;
	};
	const auto state = std::make_shared<State>(State{
		.action = action,
		.items = action.history->owner().itemsToIds(items),
		.index = 0,
		.done = std::move(done),
	});
	const auto sendNext = [=](const auto &self) -> void {
		if (state->index >= int(state->items.size())) {
			const auto destItems = state->action.history->owner().idsToItems(
				state->copied);
			if (destItems.empty()) {
				auto result = MergeAlbumResult();
				result.error = state->error.isEmpty()
					? u"COPY_FAILED"_q
					: state->error;
				LOG(("MergeAlbum: copy empty dest=%1 error=%2"
				).arg(state->action.history->peer->id.value
				).arg(result.error));
				if (state->done) {
					state->done(std::move(result));
				}
				return;
			}
			LOG(("MergeAlbum: copy done dest=%1 copied=%2 merge"
			).arg(state->action.history->peer->id.value
			).arg(int(destItems.size())));
			SendMergedAlbums(
				state->action,
				destItems,
				std::move(state->done),
				true);
			return;
		}
		const auto item = state->action.history->owner().message(
			state->items[state->index++]);
		if (!item) {
			if (state->action.progress) {
				state->action.progress->skip(1, u"Source no longer available"_q);
			}
			self(self);
			return;
		}
		ForwardOneAsCopy(state->action, item, [=](FullMsgId id, QString error) {
			if (!error.isEmpty()) {
				state->error = error;
			} else if (id) {
				state->copied.push_back(id);
			}
			self(self);
		});
	};
	sendNext(sendNext);
}

MergeAlbumCleanup CleanupMergedSources(
		not_null<Main::Session*> session,
		const MessageIdsList &ids,
		std::shared_ptr<SelectedAction> progress) {
	auto result = MergeAlbumCleanup();
	auto deleteIds = MessageIdsList();
	auto seen = base::flat_set<FullMsgId>();
	auto duplicate = 0;
	for (const auto &id : ids) {
		if (!seen.emplace(id).second) {
			++duplicate;
			continue;
		}
		const auto item = session->data().message(id);
		if (!item) {
			continue;
		} else if (item->canDelete()) {
			deleteIds.push_back(id);
		} else {
			++result.kept;
		}
	}
	LOG(("MergeAlbum: cleanup requested=%1 unique=%2 duplicate=%3 delete=%4 kept=%5"
	).arg(int(ids.size())
	).arg(int(seen.size())
	).arg(duplicate
	).arg(int(deleteIds.size())
	).arg(result.kept));
	if (!deleteIds.empty()) {
		session->data().histories().deleteMessages(deleteIds, true, progress);
		session->data().sendHistoryChangeNotifications();
	}
	if (progress) {
		progress->skip(result.kept, u"No permission to delete source"_q);
	}
	result.deleted = int(deleteIds.size());
	return result;
}

} // namespace Api
