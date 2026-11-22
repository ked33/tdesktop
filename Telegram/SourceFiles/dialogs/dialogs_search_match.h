/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/text/text.h"
#include "ui/text/text_entity.h"
#include "ui/emoji_config.h"

#include <QtCore/QStringView>

#include <algorithm>
#include <utility>
#include <vector>

namespace Dialogs {

class SearchQuery final {
public:
	struct Term {
		QString text;
		bool checkBoundaries = true;
		bool hashtag = false;
	};

	SearchQuery() = default;

	explicit SearchQuery(std::vector<Term> terms)
	: _terms(std::move(terms))
	, _hasHashtags(std::any_of(
		_terms.begin(),
		_terms.end(),
		[](const Term &term) { return term.hashtag; }))
	, _needsTagProtection(_hasHashtags && std::any_of(
		_terms.begin(),
		_terms.end(),
		[](const Term &term) { return !term.hashtag; })) {
	}

	[[nodiscard]] bool empty() const {
		return _terms.empty();
	}

	[[nodiscard]] const std::vector<Term> &terms() const {
		return _terms;
	}

	[[nodiscard]] bool hasHashtags() const {
		return _hasHashtags;
	}

	[[nodiscard]] bool needsTagProtection() const {
		return _needsTagProtection;
	}

private:
	std::vector<Term> _terms;
	bool _hasHashtags = false;
	bool _needsTagProtection = false;

};

namespace SearchMatchDetails {

[[nodiscard]] inline char32_t CharacterAt(QStringView text, qsizetype position) {
	const auto ch = text[position];
	return (ch.isHighSurrogate()
		&& position + 1 < text.size()
		&& text[position + 1].isLowSurrogate())
		? QChar::surrogateToUcs4(ch, text[position + 1])
		: ch.unicode();
}

[[nodiscard]] inline qsizetype PreviousCharacter(
		QStringView text,
		qsizetype position) {
	return (position > 1
		&& text[position - 1].isLowSurrogate()
		&& text[position - 2].isHighSurrogate())
		? position - 2
		: position - 1;
}

[[nodiscard]] inline bool IsTagCharacter(char32_t ch) {
	if (ch < 128) {
		return (ch >= 'a' && ch <= 'z')
			|| (ch >= 'A' && ch <= 'Z')
			|| (ch >= '0' && ch <= '9')
			|| ch == '_';
	}
	const auto category = QChar::category(ch);
	return QChar::isLetterOrNumber(ch)
		|| category == QChar::Mark_NonSpacing
		|| category == QChar::Mark_SpacingCombining
		|| category == QChar::Mark_Enclosing;
}

[[nodiscard]] inline bool IsWordCharacter(char32_t ch) {
	return ch != '_'
		&& (ch < 128 || QChar::script(ch) != QChar::Script_Han)
		&& IsTagCharacter(ch);
}

class CanonicalText final {
public:
	struct TagRange {
		qsizetype from = 0;
		qsizetype till = 0;
		bool protectedMatch = false;
	};
	struct EmojiPosition {
		bool inside = false;
		bool before = false;
		bool after = false;
	};

	explicit CanonicalText(QStringView text, bool collectTags = false)
	: _source(text)
	, _collectTags(collectTags) {
		auto plainFrom = qsizetype(0);
		auto needsFold = false;
		auto fullwidth = false;
		for (auto from = qsizetype(0); from < _source.size();) {
			const auto ch = _source[from];
			const auto code = ch.unicode();
			auto length = 0;
			const auto couldBeEmoji = code > 0x7F
				|| (code >= '0' && code <= '9')
				|| code == '#'
				|| code == '*';
			if (couldBeEmoji && ::Ui::Emoji::Find(_source.mid(from), &length)) {
				appendPlain(plainFrom, from - plainFrom, needsFold, fullwidth);
				appendEmoji(from, length);
				from += length;
				plainFrom = from;
				needsFold = false;
				fullwidth = false;
			} else {
				needsFold = needsFold || code > 0x7F;
				fullwidth = fullwidth || (code >= 0xFF01 && code <= 0xFF5E);
				from += (ch.isHighSurrogate()
					&& from + 1 < _source.size()
					&& _source[from + 1].isLowSurrogate()) ? 2 : 1;
			}
		}
		appendPlain(plainFrom, _source.size() - plainFrom, needsFold, fullwidth);
		finishTag(_size);
	}

	[[nodiscard]] QStringView text() const {
		return _changed ? QStringView(_storage) : _source;
	}

	[[nodiscard]] qsizetype toOriginal(qsizetype position) const {
		const auto i = std::upper_bound(
			_offsets.begin(),
			_offsets.end(),
			position,
			[](qsizetype at, const OffsetEdit &edit) {
				return at < edit.normalized;
			});
		return position + ((i == _offsets.begin())
			? 0
			: (i - 1)->original - (i - 1)->normalized);
	}

	[[nodiscard]] qsizetype toNormalized(qsizetype position) const {
		const auto i = std::lower_bound(
			_offsets.begin(),
			_offsets.end(),
			position,
			[](const OffsetEdit &edit, qsizetype at) {
				return edit.original < at;
			});
		const auto delta = (i == _offsets.begin())
			? 0
			: (i - 1)->original - (i - 1)->normalized;
		return std::min(
			position - delta,
			(i == _offsets.end()) ? _size : i->normalized);
	}

	[[nodiscard]] bool validRange(qsizetype from, qsizetype till) const {
		return isCharacterBoundary(from)
			&& isCharacterBoundary(till)
			&& !emojiPosition(from).inside
			&& !emojiPosition(till).inside;
	}

	[[nodiscard]] EmojiPosition emojiPosition(qsizetype position) const {
		const auto i = std::lower_bound(
			_emoji.begin(),
			_emoji.end(),
			position,
			[](const EmojiRange &range, qsizetype at) {
				return range.till < at;
			});
		if (i == _emoji.end()) {
			return {};
		}
		return {
			.inside = i->from < position && position < i->till,
			.before = i->till == position,
			.after = i->from == position
				|| (i->till == position
					&& i + 1 != _emoji.end()
					&& (i + 1)->from == position),
		};
	}

	[[nodiscard]] bool emojiOnly(
			qsizetype from,
			qsizetype till,
			bool includeKeycaps = false) const {
		auto i = std::lower_bound(
			_emoji.begin(),
			_emoji.end(),
			from,
			[](const EmojiRange &range, qsizetype at) {
				return range.till <= at;
			});
		auto position = from;
		while (i != _emoji.end()
			&& i->from == position
			&& (includeKeycaps || !i->keycap)) {
			position = i->till;
			if (position >= till) {
				return position == till;
			}
			++i;
		}
		return false;
	}

	[[nodiscard]] std::vector<TagRange> &tags() {
		return _tags;
	}

private:
	struct EmojiRange {
		qsizetype from = 0;
		qsizetype till = 0;
		bool keycap = false;
	};
	struct OffsetEdit {
		qsizetype normalized = 0;
		qsizetype original = 0;
	};

	[[nodiscard]] bool isCharacterBoundary(qsizetype position) const {
		const auto value = text();
		return !position
			|| position == value.size()
			|| !value[position].isLowSurrogate()
			|| !value[position - 1].isHighSurrogate();
	}

	void appendPiece(
			qsizetype from,
			qsizetype length,
			QStringView normalized) {
		const auto original = _source.mid(from, length);
		if (!_changed && original != normalized) {
			_storage.reserve(_source.size());
			_storage.append(_source.left(from));
			_changed = true;
		}
		if (_changed) {
			_storage.append(normalized);
		}
		_size += normalized.size();
	}

	void appendPlain(
			qsizetype from,
			qsizetype length,
			bool needsFold,
			bool fullwidth) {
		if (!length) {
			return;
		}
		const auto original = _source.mid(from, length);
		const auto normalizedFrom = _size;
		if (!needsFold) {
			trackTags(original, normalizedFrom);
			appendPiece(from, length, original);
			return;
		}
		auto normalized = TextUtilities::RemoveAccents(
			QString::fromRawData(original.data(), original.size()));
		if (fullwidth) {
			for (auto i = qsizetype(0); i < normalized.size(); ++i) {
				const auto code = normalized.at(i).unicode();
				if (code >= 0xFF01 && code <= 0xFF5E) {
					normalized[i] = QChar(code - 0xFEE0);
				}
			}
		}
		const auto value = QStringView(normalized);
		trackTags(value, normalizedFrom);
		appendPiece(from, length, value);
		if (value.size() != length) {
			recordOffsets(from, original, normalizedFrom, value);
		}
	}

	void appendEmoji(qsizetype from, qsizetype length) {
		finishTag(_size);
		const auto code = _source[from].unicode();
		const auto keycap = code >= '0'
			&& code <= '9'
			&& _source[from + length - 1].unicode() == 0x20E3;
		const auto normalized = _source.mid(from, keycap ? 1 : length);
		_emoji.push_back({ _size, _size + normalized.size(), keycap });
		appendPiece(from, length, normalized);
		if (keycap) {
			recordEdit(_size, from + length);
		}
	}

	void recordOffsets(
			qsizetype originalFrom,
			QStringView original,
			qsizetype normalizedFrom,
			QStringView normalized) {
		auto position = qsizetype(0);
		for (auto i = qsizetype(0); i < original.size();) {
			const auto ch = original[i];
			if (ch.unicode() >= 128 && ::Ui::Text::IsDiacritic(ch)) {
				++i;
				recordEdit(normalizedFrom + position, originalFrom + i);
			} else if (ch.isHighSurrogate()
				&& i + 1 < original.size()
				&& original[i + 1].isLowSurrogate()) {
				const auto unchanged = position + 1 < normalized.size()
					&& normalized[position] == ch
					&& normalized[position + 1] == original[i + 1];
				i += 2;
				position += unchanged ? 2 : 1;
				if (!unchanged) {
					recordEdit(normalizedFrom + position, originalFrom + i);
				}
			} else {
				++i;
				++position;
			}
		}
	}

	void recordEdit(qsizetype normalized, qsizetype original) {
		if (!_offsets.empty() && _offsets.back().normalized == normalized) {
			_offsets.back().original = original;
		} else {
			_offsets.push_back({ normalized, original });
		}
	}

	void trackTags(QStringView value, qsizetype from) {
		if (!_collectTags) {
			return;
		}
		for (auto i = qsizetype(0); i < value.size();) {
			const auto ch = CharacterAt(value, i);
			const auto position = from + i;
			if (ch == '#') {
				finishTag(position);
				_tagFrom = position;
			} else if (_tagFrom >= 0 && !IsTagCharacter(ch)) {
				finishTag(position);
			}
			i += (ch > 0xFFFF) ? 2 : 1;
		}
	}

	void finishTag(qsizetype till) {
		if (_tagFrom >= 0 && till > _tagFrom + 1) {
			_tags.push_back({ _tagFrom, till });
		}
		_tagFrom = -1;
	}

	QStringView _source;
	QString _storage;
	std::vector<EmojiRange> _emoji;
	std::vector<OffsetEdit> _offsets;
	std::vector<TagRange> _tags;
	qsizetype _size = 0;
	qsizetype _tagFrom = -1;
	bool _changed = false;
	bool _collectTags = false;

};

class TextContext final {
public:
	TextContext(QStringView text, const SearchQuery &query)
	: _data(text, query.needsTagProtection()) {
		if (query.needsTagProtection()) {
			protectTags(query);
		}
	}

	[[nodiscard]] QStringView text() const {
		return _data.text();
	}

	[[nodiscard]] qsizetype toNormalized(qsizetype position) const {
		return _data.toNormalized(position);
	}

	[[nodiscard]] qsizetype toOriginal(qsizetype position) const {
		return _data.toOriginal(position);
	}

	[[nodiscard]] bool accepts(
			qsizetype from,
			qsizetype till,
			const SearchQuery::Term &term) {
		if (!_data.validRange(from, till)) {
			return false;
		}
		if (term.hashtag) {
			return true;
		} else if (usesProtectedTagPart(from, till)) {
			return false;
		} else if (!term.checkBoundaries || _data.emojiOnly(from, till, true)) {
			return true;
		}
		const auto value = text();
		const auto left = _data.emojiPosition(from);
		const auto right = _data.emojiPosition(till);
		return (!from
				|| left.before
				|| !IsWordCharacter(CharacterAt(
					value,
					PreviousCharacter(value, from))))
			&& (till == value.size()
				|| right.after
				|| !IsWordCharacter(CharacterAt(value, till)));
	}

private:
	void protectTags(const SearchQuery &query) {
		auto &tags = _data.tags();
		const auto value = text();
		for (const auto &term : query.terms()) {
			if (!term.hashtag) {
				continue;
			}
			const auto word = QStringView(term.text);
			auto from = value.indexOf(word, 0, Qt::CaseInsensitive);
			while (from >= 0) {
				const auto till = from + word.size();
				if (_data.validRange(from, till)) {
					auto i = std::lower_bound(
						tags.begin(),
						tags.end(),
						from,
						[](const CanonicalText::TagRange &tag, qsizetype at) {
							return tag.till <= at;
						});
					while (i != tags.end() && i->from < till) {
						i->protectedMatch = true;
						++i;
					}
				}
				from = value.indexOf(word, from + 1, Qt::CaseInsensitive);
			}
		}
	}

	[[nodiscard]] bool usesProtectedTagPart(
			qsizetype from,
			qsizetype till) {
		const auto &tags = _data.tags();
		auto i = std::upper_bound(
			tags.begin(),
			tags.end(),
			from,
			[](qsizetype at, const CanonicalText::TagRange &tag) {
				return at < tag.from;
			});
		if (i == tags.begin()) {
			return false;
		}
		--i;
		return i->protectedMatch
			&& from >= i->from + 1
			&& till <= i->till
			&& (from != i->from + 1 || till != i->till);
	}

	CanonicalText _data;

};

template <typename Callback>
void ForEachTermMatch(
		TextContext &context,
		const SearchQuery::Term &term,
		qsizetype start,
		Callback callback) {
	const auto text = context.text();
	const auto query = QStringView(term.text);
	auto from = text.indexOf(query, start, Qt::CaseInsensitive);
	while (from >= 0) {
		const auto till = from + query.size();
		if (context.accepts(from, till, term)) {
			const auto originalFrom = context.toOriginal(from);
			const auto originalTill = context.toOriginal(till);
			if (!callback(int(originalFrom), int(originalTill - originalFrom))) {
				return;
			}
		}
		from = text.indexOf(query, from + 1, Qt::CaseInsensitive);
	}
}

} // namespace SearchMatchDetails

[[nodiscard]] inline SearchQuery ParseSearchQuery(QStringView query) {
	const auto normalized = SearchMatchDetails::CanonicalText(query);
	const auto value = normalized.text();
	auto terms = std::vector<SearchQuery::Term>();
	for (auto from = qsizetype(0); from < value.size();) {
		if (value[from].isSpace() || value[from] == '-') {
			++from;
			continue;
		}
		auto till = from + 1;
		while (till < value.size()
			&& !value[till].isSpace()
			&& value[till] != '-') {
			++till;
		}
		const auto text = value.mid(from, till - from);
		const auto duplicate = std::any_of(
			terms.begin(),
			terms.end(),
			[&](const SearchQuery::Term &term) {
				return QStringView(term.text).compare(text, Qt::CaseInsensitive) == 0;
			});
		if (!duplicate) {
			const auto hashtag = text.front() == '#';
			auto checkBoundaries = !hashtag && !normalized.emojiOnly(from, till);
			if (checkBoundaries) {
				for (auto i = qsizetype(0); i < text.size();) {
					const auto ch = SearchMatchDetails::CharacterAt(text, i);
					if (QChar::script(ch) == QChar::Script_Han) {
						checkBoundaries = false;
						break;
					}
					i += (ch > 0xFFFF) ? 2 : 1;
				}
			}
			terms.push_back({ text.toString(), checkBoundaries, hashtag });
		}
		from = till;
	}
	return SearchQuery(std::move(terms));
}

[[nodiscard]] inline bool SearchTextMatches(
		QStringView text,
		const SearchQuery &query) {
	if (query.empty()) {
		return true;
	}
	auto context = SearchMatchDetails::TextContext(text, query);
	for (const auto &term : query.terms()) {
		auto found = false;
		SearchMatchDetails::ForEachTermMatch(
			context,
			term,
			0,
			[&](int, int) {
				found = true;
				return false;
			});
		if (!found) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] inline bool SearchTextMatches(QStringView text, QStringView query) {
	return SearchTextMatches(text, ParseSearchQuery(query));
}

template <typename Callback>
void ForEachSearchMatch(
		QStringView text,
		const SearchQuery &query,
		Callback callback,
		int start = 0) {
	if (query.empty()) {
		return;
	}
	auto context = SearchMatchDetails::TextContext(text, query);
	const auto originalOffset = std::clamp(qsizetype(start), qsizetype(0), text.size());
	const auto offset = context.toNormalized(originalOffset);
	for (const auto &term : query.terms()) {
		SearchMatchDetails::ForEachTermMatch(
			context,
			term,
			offset,
			[&](int from, int length) {
				callback(from, length);
				return true;
			});
	}
}

template <typename Callback>
void ForEachSearchMatch(QStringView text, QStringView query, Callback callback) {
	ForEachSearchMatch(text, ParseSearchQuery(query), std::move(callback));
}

[[nodiscard]] inline int HighlightSearchText(
		TextWithEntities &text,
		const SearchQuery &query,
		int start = 0) {
	struct Range {
		int from = 0;
		int till = 0;
	};
	auto matches = std::vector<Range>();
	ForEachSearchMatch(
		QStringView(text.text),
		query,
		[&](int from, int length) {
			matches.push_back({ from, from + length });
		},
		start);
	if (matches.empty()) {
		return -1;
	}
	std::sort(
		matches.begin(),
		matches.end(),
		[](const Range &a, const Range &b) {
			return a.from < b.from || (a.from == b.from && a.till < b.till);
		});
	auto count = std::size_t(0);
	for (const auto match : matches) {
		if (count && match.from < matches[count - 1].till) {
			matches[count - 1].till = std::max(matches[count - 1].till, match.till);
		} else {
			matches[count++] = match;
		}
	}
	matches.resize(count);
	auto entities = EntitiesInText();
	entities.reserve(text.entities.size() + matches.size());
	for (const auto &entity : text.entities) {
		if (entity.type() != EntityType::Colorized) {
			entities.push_back(entity);
			continue;
		}
		auto from = entity.offset();
		const auto till = from + entity.length();
		const auto append = [&](int end) {
			if (end > from) {
				entities.push_back(EntityInText(
					EntityType::Colorized,
					from,
					end - from,
					entity.data()));
			}
		};
		for (const auto &match : matches) {
			if (match.till <= from) {
				continue;
			} else if (match.from >= till) {
				break;
			}
			append(match.from);
			from = std::min(match.till, till);
		}
		append(till);
	}
	for (const auto &match : matches) {
		entities.push_back(EntityInText(
			EntityType::Colorized,
			match.from,
			match.till - match.from,
			QString(QChar(1))));
	}
	std::stable_sort(
		entities.begin(),
		entities.end(),
		[](const auto &a, const auto &b) { return a.offset() < b.offset(); });
	text.entities = std::move(entities);
	return matches.front().from;
}

[[nodiscard]] inline int HighlightSearchText(
		TextWithEntities &text,
		QStringView query,
		int start = 0) {
	return HighlightSearchText(text, ParseSearchQuery(query), start);
}

} // namespace Dialogs
