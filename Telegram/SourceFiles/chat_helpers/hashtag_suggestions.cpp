#include "chat_helpers/hashtag_suggestions.h"

#include <QtGui/QFontMetricsF>
#include <QtGui/QPainter>
#include <QtGui/QTextLayout>

#include <algorithm>
#include <cmath>

namespace ChatHelpers {

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
	_infix.clear();
	if (!limit) {
		return _matches;
	}
	const auto size = std::min(limit, int(_entries.size()));
	_matches.reserve(size);
	if (!_query.isEmpty()) {
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
		if (offset == 0) {
			_matches.push_back(match(entry, offset, int(_query.size())));
			if (_matches.size() == limit) {
				break;
			}
		} else if (_infix.size() < limit) {
			_infix.push_back(match(entry, offset, int(_query.size())));
		}
	}
	const auto remaining = std::min(
		limit - int(_matches.size()),
		int(_infix.size()));
	for (auto i = 0; i < remaining; ++i) {
		_matches.push_back(_infix[i]);
	}
	_infix.clear();
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
