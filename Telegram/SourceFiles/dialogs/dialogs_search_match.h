/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

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
	};

	SearchQuery() = default;

	explicit SearchQuery(std::vector<Term> terms)
	: _terms(std::move(terms)) {
	}

	[[nodiscard]] bool empty() const {
		return _terms.empty();
	}

	[[nodiscard]] const std::vector<Term> &terms() const {
		return _terms;
	}

private:
	std::vector<Term> _terms;

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

[[nodiscard]] inline bool IsWordCharacter(char32_t ch) {
	const auto category = QChar::category(ch);
	return QChar::isLetterOrNumber(ch)
		|| ch == '_'
		|| category == QChar::Mark_NonSpacing
		|| category == QChar::Mark_SpacingCombining
		|| category == QChar::Mark_Enclosing;
}

class TextContext final {
public:
	explicit TextContext(QStringView text) : _text(text) {
	}

	[[nodiscard]] QStringView text() const {
		return _text;
	}

	[[nodiscard]] bool accepts(
			qsizetype from,
			qsizetype till,
			bool checkBoundaries) {
		if (!isCharacterBoundary(from) || !isCharacterBoundary(till)) {
			return false;
		}
		ensureEmojiRanges();
		const auto left = emojiPosition(from);
		const auto right = emojiPosition(till);
		if (left.inside || right.inside) {
			return false;
		} else if (!checkBoundaries) {
			return true;
		}
		return (!from
				|| left.before
				|| !IsWordCharacter(CharacterAt(
					_text,
					PreviousCharacter(_text, from))))
			&& (till == _text.size()
				|| right.after
				|| !IsWordCharacter(CharacterAt(_text, till)));
	}

private:
	struct EmojiRange {
		qsizetype from = 0;
		qsizetype till = 0;
	};
	struct EmojiPosition {
		bool inside = false;
		bool before = false;
		bool after = false;
	};

	[[nodiscard]] bool isCharacterBoundary(qsizetype position) const {
		return !position
			|| position == _text.size()
			|| !_text[position].isLowSurrogate()
			|| !_text[position - 1].isHighSurrogate();
	}

	void ensureEmojiRanges() {
		if (_emojiScanned) {
			return;
		}
		_emojiScanned = true;
		for (auto from = qsizetype(0); from < _text.size();) {
			const auto ch = _text[from];
			auto length = 0;
			const auto code = ch.unicode();
			const auto couldBeEmoji = code > 0x7F
				|| (code >= '0' && code <= '9')
				|| code == '#'
				|| code == '*';
			if (couldBeEmoji && ::Ui::Emoji::Find(_text.mid(from), &length)) {
				_emoji.push_back({ from, from + length });
				from += length;
			} else {
				from += (ch.isHighSurrogate()
					&& from + 1 < _text.size()
					&& _text[from + 1].isLowSurrogate()) ? 2 : 1;
			}
		}
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

	QStringView _text;
	std::vector<EmojiRange> _emoji;
	bool _emojiScanned = false;

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
		if (context.accepts(from, till, term.checkBoundaries)
			&& !callback(int(from), int(query.size()))) {
			return;
		}
		from = text.indexOf(query, from + 1, Qt::CaseInsensitive);
	}
}

} // namespace SearchMatchDetails

[[nodiscard]] inline SearchQuery ParseSearchQuery(QStringView query) {
	auto terms = std::vector<SearchQuery::Term>();
	for (auto from = qsizetype(0); from < query.size();) {
		if (query[from].isSpace()) {
			++from;
			continue;
		}
		auto till = from + 1;
		while (till < query.size() && !query[till].isSpace()) {
			++till;
		}
		const auto text = query.mid(from, till - from);
		const auto duplicate = std::any_of(
			terms.begin(),
			terms.end(),
			[&](const SearchQuery::Term &term) {
				return QStringView(term.text).compare(text, Qt::CaseInsensitive) == 0;
			});
		if (!duplicate) {
			auto checkBoundaries = true;
			if (text.front() != '#') {
				for (auto i = qsizetype(0); i < text.size();) {
					const auto ch = SearchMatchDetails::CharacterAt(text, i);
					if (QChar::script(ch) == QChar::Script_Han) {
						checkBoundaries = false;
						break;
					}
					i += (ch > 0xFFFF) ? 2 : 1;
				}
			}
			terms.push_back({ text.toString(), checkBoundaries });
		}
		from = till;
	}
	return SearchQuery(std::move(terms));
}

[[nodiscard]] inline bool SearchTextMatches(
		QStringView text,
		const SearchQuery &query) {
	auto context = SearchMatchDetails::TextContext(text);
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
	auto context = SearchMatchDetails::TextContext(text);
	const auto offset = std::clamp(qsizetype(start), qsizetype(0), text.size());
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
