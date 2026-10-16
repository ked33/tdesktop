/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "ui/rp_widget.h"

#include <rpl/lifetime.h>

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
	HashtagAutocomplete(
		QWidget *parent,
		not_null<Ui::InputField*> field,
		not_null<Main::Session*> session);
	~HashtagAutocomplete();

	// The rect in parent coordinates the list opens under.
	void setTargetRect(QRect rect);
	bool handleKeyPress(not_null<QKeyEvent*> e);

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
	};

	void updateQuery();
	void updateFiltered(const QString &filter);
	void updateListGeometry();
	void hideFast();
	void setSelected(int index);
	void moveSelection(int delta);
	bool chooseSelected();
	[[nodiscard]] QString computeQuery() const;

	const not_null<Ui::InputField*> _field;
	const not_null<Main::Session*> _session;
	object_ptr<Ui::ScrollArea> _scroll;
	QPointer<Inner> _inner;
	std::vector<Row> _rows;
	QRect _target;
	QString _filter;
	int _selected = -1;
	rpl::lifetime _lifetime;

};

} // namespace ChatHelpers
