#pragma once

#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <vector>

class QColor;
class QFont;
class QPainter;
class QPoint;

namespace ChatHelpers {

struct HashtagMatch {
	QString tag;
	int offset = 0;
	int length = 0;

	friend bool operator==(const HashtagMatch&, const HashtagMatch&) = default;
};

using HashtagMatches = QVector<HashtagMatch>;

class HashtagSuggestions final {
public:
	using Normalize = QString (*)(const QString &);

	explicit HashtagSuggestions(Normalize normalize = nullptr);
	void invalidate();
	[[nodiscard]] const HashtagMatches &filter(
		const QList<QPair<QString, ushort>> &tags,
		const QString &query,
		int limit);

private:
	struct Span {
		int offset = 0;
		int end = 0;
	};
	struct Entry {
		QString tag;
		QString key;
		std::vector<Span> spans;
	};

	[[nodiscard]] QString normalize(const QString &text) const;
	[[nodiscard]] Entry prepare(const QString &tag) const;
	[[nodiscard]] HashtagMatch match(
		const Entry &entry,
		int offset,
		int length) const;

	const Normalize _normalize;
	std::vector<Entry> _entries;
	HashtagMatches _matches;
	HashtagMatches _infix;
	QString _query;
	int _limit = 0;
	bool _dirty = true;
	bool _filtered = false;

};

int PaintHashtagMatch(
	QPainter &p,
	const QFont &font,
	const HashtagMatch &match,
	QPoint baseline,
	int width,
	const QColor &normal,
	const QColor &active);

} // namespace ChatHelpers
