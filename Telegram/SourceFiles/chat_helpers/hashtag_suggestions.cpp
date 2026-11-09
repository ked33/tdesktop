#include "chat_helpers/hashtag_suggestions.h"

#include <QtGui/QFontMetricsF>
#include <QtGui/QPainter>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextLayout>

#include <algorithm>
#include <cmath>

namespace ChatHelpers {
namespace {

constexpr auto kMaxHashtagLength = 64;

bool IsHashtagCharacter(char32_t ch) {
	const auto category = QChar::category(ch);
	return QChar::isLetterOrNumber(ch)
		|| ch == '_'
		|| category == QChar::Mark_NonSpacing
		|| category == QChar::Mark_SpacingCombining
		|| category == QChar::Mark_Enclosing;
}

int PreviousCharacter(const QString &text, int position) {
	return (position > 1
		&& text[position - 1].isLowSurrogate()
		&& text[position - 2].isHighSurrogate())
		? position - 2
		: position - 1;
}

char32_t CharacterAt(const QString &text, int position) {
	const auto ch = text[position];
	return (ch.isHighSurrogate()
		&& position + 1 < text.size()
		&& text[position + 1].isLowSurrogate())
		? QChar::surrogateToUcs4(ch, text[position + 1])
		: ch.unicode();
}

} // namespace

std::optional<HashtagCompletion> FindHashtagCompletion(
		const QTextCursor &cursor) {
	if (cursor.isNull() || cursor.hasSelection()) {
		return {};
	}
	const auto block = cursor.block();
	const auto text = block.text();
	const auto position = cursor.positionInBlock();
	if (!position
		|| (position < text.size() && text[position].isLowSurrogate())) {
		return {};
	}
	auto from = position;
	while (from > 0) {
		const auto previous = PreviousCharacter(text, from);
		if (!IsHashtagCharacter(CharacterAt(text, previous))) {
			break;
		}
		from = previous;
		if (position - from > kMaxHashtagLength) {
			return {};
		}
	}
	if (!from || text[from - 1] != '#') {
		return {};
	}
	--from;
	if (from && IsHashtagCharacter(
			CharacterAt(text, PreviousCharacter(text, from)))) {
		return {};
	}
	auto till = position;
	while (till < text.size() && IsHashtagCharacter(CharacterAt(text, till))) {
		till += text[till].isHighSurrogate() ? 2 : 1;
		if (till - from > kMaxHashtagLength + 1) {
			return {};
		}
	}
	return HashtagCompletion{
		text.mid(from, till - from),
		block.position() + from,
		block.position() + till,
	};
}

bool CompleteHashtag(QTextCursor &cursor, const QString &tag) {
	const auto range = FindHashtagCompletion(cursor);
	if (!range || tag.size() < 2 || tag.front() != '#') {
		return false;
	}
	cursor.beginEditBlock();
	cursor.setPosition(range->from);
	cursor.setPosition(range->till, QTextCursor::KeepAnchor);
	cursor.insertText(tag);
	cursor.endEditBlock();
	return true;
}

HashtagSuggestions::HashtagSuggestions(Normalize normalize)
: _normalize(normalize) {
}

void HashtagSuggestions::invalidate() {
	_dirty = true;
}

QString HashtagSuggestions::normalize(const QString &text) const {
	return _normalize ? _normalize(text) : text;
}

HashtagSuggestions::Entry HashtagSuggestions::prepare(
		const QString &tag) const {
	auto result = Entry{ tag, normalize(tag), {} };
	if (result.key.size() == tag.size()) {
		return result;
	}
	result.spans.reserve(result.key.size());
	for (auto i = 0; i < tag.size();) {
		const auto length = (tag.at(i).isHighSurrogate()
			&& i + 1 < tag.size()
			&& tag.at(i + 1).isLowSurrogate()) ? 2 : 1;
		const auto end = i + length;
		const auto piece = normalize(tag.mid(i, length));
		if (piece.isEmpty() && !result.spans.empty()) {
			result.spans.back().end = end;
		}
		for (auto j = 0; j < piece.size(); ++j) {
			result.spans.push_back({ i, end });
		}
		i = end;
	}
	return result;
}

HashtagMatch HashtagSuggestions::match(
		const Entry &entry,
		int offset,
		int length) const {
	if (!length || entry.spans.empty()) {
		return { entry.tag, offset, length };
	}
	const auto start = entry.spans[offset].offset;
	return {
		entry.tag,
		start,
		entry.spans[offset + length - 1].end - start,
	};
}

const HashtagMatches &HashtagSuggestions::filter(
		const QList<QPair<QString, ushort>> &tags,
		const QString &query,
		int limit) {
	const auto normalized = normalize(query.toLower());
	limit = std::max(limit, 0);
	if (!_dirty && _filtered && normalized == _query && limit == _limit) {
		return _matches;
	}
	if (_dirty) {
		_entries.clear();
		_entries.reserve(tags.size());
		for (const auto &item : tags) {
			_entries.push_back(prepare(item.first));
		}
		_dirty = false;
	}
	_filtered = true;
	_query = normalized;
	_limit = limit;
	_matches.clear();
	_prefix.clear();
	_infix.clear();
	if (!limit) {
		return _matches;
	}
	const auto size = std::min(limit, int(_entries.size()));
	_matches.reserve(size);
	if (!_query.isEmpty()) {
		_prefix.reserve(size);
		_infix.reserve(size);
	}
	for (const auto &entry : _entries) {
		const auto offset = _query.isEmpty()
			? 0
			: int(entry.key.indexOf(_query, 0, Qt::CaseInsensitive));
		if (offset < 0 || (!_query.isEmpty()
			&& entry.key.size() == _query.size())) {
			continue;
		}
		if (_query.isEmpty()) {
			_matches.push_back(match(entry, offset, int(_query.size())));
			if (_matches.size() == limit) {
				break;
			}
		} else if (offset == 0 && _prefix.size() < limit) {
			_prefix.push_back(match(entry, offset, int(_query.size())));
		} else if (offset > 0 && _infix.size() < limit) {
			_infix.push_back(match(entry, offset, int(_query.size())));
		}
	}
	for (const auto list : { &_prefix, &_infix }) {
		const auto remaining = std::min(
			limit - int(_matches.size()),
			int(list->size()));
		for (auto i = 0; i < remaining; ++i) {
			_matches.push_back((*list)[i]);
		}
		list->clear();
	}
	return _matches;
}

int PaintHashtagMatch(
		QPainter &p,
		const QFont &font,
		const HashtagMatch &match,
		QPoint baseline,
		int width,
		const QColor &normal,
		const QColor &active) {
	if (width <= 0) {
		return 0;
	}
	const auto text = QChar('#') + match.tag;
	const auto metrics = QFontMetricsF(font, p.device());
	const auto elided = metrics.elidedText(text, Qt::ElideRight, width);
	auto preserved = 0;
	while (preserved < text.size()
		&& preserved < elided.size()
		&& text.at(preserved) == elided.at(preserved)) {
		++preserved;
	}
	const auto start = match.offset ? match.offset + 1 : 0;
	const auto end = std::min(match.offset + match.length + 1, preserved);
	auto layout = QTextLayout(elided, font, p.device());
	auto options = QTextOption();
	options.setWrapMode(QTextOption::NoWrap);
	layout.setTextOption(options);
	if (match.length && end > start) {
		auto range = QTextLayout::FormatRange();
		range.start = start;
		range.length = end - start;
		range.format.setForeground(active);
		layout.setFormats({ range });
	}
	layout.beginLayout();
	auto line = layout.createLine();
	if (line.isValid()) {
		line.setLineWidth(width);
	}
	layout.endLayout();
	if (!line.isValid()) {
		return 0;
	}
	p.setPen(normal);
	layout.draw(&p, QPointF(baseline.x(), baseline.y() - line.ascent()));
	return int(std::ceil(line.horizontalAdvance()));
}

} // namespace ChatHelpers
