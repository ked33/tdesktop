/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "chat_helpers/hashtag_suggestions.h"
#include "ui/rp_widget.h"

#include <rpl/event_stream.h>

namespace Ui {
class InputField;
class ScrollArea;
} // namespace Ui

namespace Main {
class Session;
} // namespace Main

namespace ChatHelpers {

class HashtagAutocomplete final : public Ui::RpWidget {
public:
	enum class Source {
		SearchHistory,
		Sent,
	};
	enum class Direction {
		Down,
		Up,
	};

	HashtagAutocomplete(
		QWidget *parent,
		not_null<Ui::InputField*> field,
		not_null<Main::Session*> session,
		Source source = Source::Sent);
	~HashtagAutocomplete();

	void setTargetRect(QRect rect, Direction direction = Direction::Down);
	void setAllowed(bool allowed);
	void setKeyboardEnabled(bool enabled);
	bool handleKeyPress(not_null<QKeyEvent*> e);
	[[nodiscard]] rpl::producer<QString> chosen() const;

protected:
	bool eventFilter(QObject *obj, QEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	class Inner;
	friend class Inner;

	struct Row {
		HashtagMatch match;
		quint64 count = 0;

		friend inline bool operator==(const Row&, const Row&) = default;
	};

	void updateQuery();
	bool updateFiltered(const QString &filter);
	void updateListGeometry();
	void hideFast();
	void setSelected(int index);
	void clearInvisibleSelection();
	[[nodiscard]] bool selectedVisible() const;
	void moveSelection(int delta);
	bool chooseSelected();
	void removeSelected();
	[[nodiscard]] bool acceptsKeyboard() const;
	[[nodiscard]] QString computeQuery() const;

	const not_null<Ui::InputField*> _field;
	const not_null<Main::Session*> _session;
	const Source _source;
	HashtagSuggestions _suggestions;
	Direction _direction = Direction::Down;
	object_ptr<Ui::ScrollArea> _scroll;
	QPointer<Inner> _inner;
	std::vector<Row> _rows;
	QRect _target;
	QString _filter;
	QString _dismissedQuery;
	QPoint _lastMousePosition;
	int _selected = -1;
	bool _allowed = true;
	bool _keyboardEnabled = true;
	bool _mouseSelection = false;
	bool _rowsDirty = true;
	rpl::event_stream<QString> _chosen;

};

} // namespace ChatHelpers
