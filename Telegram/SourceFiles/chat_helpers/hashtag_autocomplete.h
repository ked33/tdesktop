/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "ui/rp_widget.h"

#include <rpl/event_stream.h>

namespace Ui {
class FlatLabel;
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

	HashtagAutocomplete(
		QWidget *parent,
		not_null<Ui::InputField*> field,
		not_null<Main::Session*> session,
		Source source = Source::Sent);
	~HashtagAutocomplete();

	void setTitle(rpl::producer<QString> title);
	void setTargetRect(QRect rect);
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
		QString tag;
		int count = 0;

		friend inline bool operator==(const Row&, const Row&) = default;
	};

	void updateQuery();
	void updateFiltered(const QString &filter);
	void updateListGeometry();
	void hideFast();
	void setSelected(int index);
	void moveSelection(int delta);
	bool chooseSelected();
	void removeSelected();
	[[nodiscard]] bool acceptsKeyboard() const;
	[[nodiscard]] QString computeQuery() const;

	const not_null<Ui::InputField*> _field;
	const not_null<Main::Session*> _session;
	const Source _source;
	object_ptr<Ui::FlatLabel> _title = { nullptr };
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
	rpl::event_stream<QString> _chosen;

};

} // namespace ChatHelpers
