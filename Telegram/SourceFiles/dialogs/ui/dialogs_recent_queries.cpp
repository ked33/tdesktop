/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "dialogs/ui/dialogs_recent_queries.h"

#include "base/event_filter.h"
#include "base/unique_qptr.h"
#include "data/components/recent_search_queries.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/ripple_animation.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/padding_wrap.h"
#include "window/window_session_controller.h"
#include "styles/style_dialogs.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <crl/crl_on_main.h>

namespace Dialogs {

class QueryRow final : public Ui::RippleButton {
public:
	QueryRow(QWidget *parent, QString query);

	void setKeyboardSelected(bool selected);

protected:
	void paintEvent(QPaintEvent *e) override;
	QImage prepareRippleMask() const override;

private:
	QString _query;
	bool _keyboardSelected = false;

};

QueryRow::QueryRow(QWidget *parent, QString query)
: RippleButton(parent, st::recentQueryRipple)
, _query(std::move(query)) {
	setPointerCursor(true);
	setAccessibleName(_query);
}

void QueryRow::setKeyboardSelected(bool selected) {
	if (_keyboardSelected == selected) {
		return;
	}
	_keyboardSelected = selected;
	update();
}

void QueryRow::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto over = isOver() || isDown();
	if (_keyboardSelected) {
		p.fillRect(rect(), st::dialogsBgActive);
	} else if (over) {
		p.fillRect(rect(), st::dialogsBgOver);
	}
	paintRipple(p, 0, 0);

	const auto &icon = _keyboardSelected
		? st::recentQueryIconActive
		: over
		? st::recentQueryIconOver
		: st::recentQueryIcon;
	icon.paint(
		p,
		st::recentQueryIconLeft,
		(height() - icon.height()) / 2,
		width());

	const auto available = width()
		- st::recentQueryTextLeft
		- st::recentQueryTextSkip;
	if (available <= 0) {
		return;
	}
	p.setFont(st::semiboldFont);
	p.setPen(_keyboardSelected
		? st::dialogsNameFgActive
		: over
		? st::dialogsNameFgOver
		: st::dialogsNameFg);
	p.drawText(
		QRect(st::recentQueryTextLeft, 0, available, height()),
		Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
		st::semiboldFont->elided(_query, available));
}

QImage QueryRow::prepareRippleMask() const {
	return Ui::RippleAnimation::RectMask(size());
}

RecentQueries::RecentQueries(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _controller(controller) {
	setupHeader();
	rebuild();

	_controller->session().recentSearchQueries().updates(
	) | rpl::on_next([=] {
		crl::on_main(this, [=] { rebuild(); });
	}, lifetime());
}

RecentQueries::~RecentQueries() = default;

int RecentQueries::count() const {
	return _count.current();
}

rpl::producer<int> RecentQueries::countValue() const {
	return _count.value();
}

rpl::producer<int> RecentQueries::selectedIndexValue() const {
	return _selected.value();
}

rpl::producer<QString> RecentQueries::chosen() const {
	return _chosen.events();
}

auto RecentQueries::scrollToRequests() const
-> rpl::producer<Ui::ScrollToRequest> {
	return _scrollToRequests.events();
}

RecentQueries::Jump RecentQueries::selectJump(
		Qt::Key direction,
		int pageSize) {
	const auto count = int(_queries.size());
	const auto had = (_selected.current() >= 0);
	if (direction == Qt::Key()) {
		return had ? Jump::Applied : Jump::NotApplied;
	} else if (!count || (direction == Qt::Key_Up && !had)) {
		return Jump::NotApplied;
	} else if (direction != Qt::Key_Down && direction != Qt::Key_Up) {
		return Jump::NotApplied;
	}
	const auto delta = (direction == Qt::Key_Down) ? 1 : -1;
	const auto rowHeight = std::max(st::recentQueryHeight, 1);
	const auto step = (pageSize > rowHeight) ? (pageSize / rowHeight) : 1;
	if (!had) {
		setSelected(0, true);
		return Jump::Applied;
	}
	const auto current = _selected.current();
	const auto next = current + delta * step;
	if (next >= 0 && next < count) {
		setSelected(next, true);
		return Jump::Applied;
	} else if (pageSize > rowHeight) {
		const auto edge = (delta > 0) ? (count - 1) : 0;
		if (current != edge) {
			setSelected(edge, true);
			return Jump::Applied;
		}
	}
	setSelected(-1, false);
	return Jump::AppliedAndOut;
}

bool RecentQueries::choose() {
	const auto selected = _selected.current();
	if (selected < 0 || selected >= int(_queries.size())) {
		return false;
	}
	_chosen.fire_copy(_queries[selected]);
	return true;
}

int RecentQueries::resizeGetHeight(int newWidth) {
	const auto rows = int(_rows.size());
	if (_header) {
		_header->setVisible(rows > 0);
		_header->setGeometry(0, 0, newWidth, st::searchedBarHeight);
	}
	for (auto i = 0; i != rows; ++i) {
		_rows[i]->setGeometry(
			0,
			st::searchedBarHeight + i * st::recentQueryHeight,
			newWidth,
			st::recentQueryHeight);
	}
	return rows
		? (st::searchedBarHeight + rows * st::recentQueryHeight)
		: 0;
}

void RecentQueries::setupHeader() {
	_header = Ui::CreateChild<Ui::FixedHeightWidget>(
		this,
		st::searchedBarHeight);
	const auto raw = _header;
	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		raw,
		tr::lng_recent_title(),
		st::searchedBarLabel);
	const auto clear = Ui::CreateChild<Ui::LinkButton>(
		raw,
		tr::lng_recent_clear(tr::now),
		st::searchedBarLink);
	const auto controller = _controller;
	clear->setClickedCallback([=] {
		controller->show(Ui::MakeConfirmBox({
			.text = tr::lng_recent_clear_sure(tr::now),
			.confirmed = [=](Fn<void()> close) {
				controller->session().recentSearchQueries().clear();
				close();
			},
		}));
	});
	rpl::combine(
		raw->sizeValue(),
		clear->widthValue()
	) | rpl::on_next([=](QSize size, int width) {
		const auto x = st::searchedBarPosition.x();
		const auto y = st::searchedBarPosition.y();
		clear->moveToRight(0, 0, size.width());
		label->resizeToWidth(size.width() - x - width);
		label->moveToLeft(x, y, size.width());
	}, raw->lifetime());
	raw->paintRequest() | rpl::on_next([=](QRect clip) {
		Painter(raw).fillRect(clip, st::searchedBarBg);
	}, raw->lifetime());
}

void RecentQueries::rebuild() {
	_menu = nullptr;
	const auto rows = base::take(_rows);
	for (const auto row : rows) {
		delete row.get();
	}
	_queries = _controller->session().recentSearchQueries().list();
	if (_selected.current() != -1) {
		_selected = -1;
	}
	_rows.reserve(_queries.size());
	for (auto i = 0; i != int(_queries.size()); ++i) {
		const auto query = _queries[i];
		const auto row = Ui::CreateChild<QueryRow>(this, query);
		row->setClickedCallback([=] {
			setSelected(i, false);
			_chosen.fire_copy(query);
		});
		row->events() | rpl::on_next([=](not_null<QEvent*> e) {
			if (e->type() == QEvent::Enter) {
				setSelected(i, false);
			}
		}, row->lifetime());
		base::install_event_filter(row, [=](not_null<QEvent*> e) {
			if (e->type() != QEvent::ContextMenu) {
				return base::EventFilterResult::Continue;
			}
			showMenu(row, query);
			return base::EventFilterResult::Cancel;
		}, row->lifetime());
		row->show();
		_rows.push_back(row);
	}
	applySelected();
	_count = int(_rows.size());
	if (width() > 0) {
		resizeToWidth(width());
	}
}

void RecentQueries::setSelected(int index, bool scroll) {
	if (_selected.current() == index) {
		return;
	}
	_selected = index;
	applySelected();
	if (!scroll || index < 0) {
		return;
	}
	const auto top = st::searchedBarHeight + index * st::recentQueryHeight;
	_scrollToRequests.fire(Ui::ScrollToRequest(
		top,
		top + st::recentQueryHeight));
}

void RecentQueries::applySelected() {
	const auto selected = _selected.current();
	for (auto i = 0; i != int(_rows.size()); ++i) {
		_rows[i]->setKeyboardSelected(i == selected);
	}
}

void RecentQueries::showMenu(not_null<QWidget*> row, const QString &query) {
	const auto controller = _controller;
	_menu = base::make_unique_q<Ui::PopupMenu>(
		row,
		st::popupMenuWithIcons);
	_menu->addAction(tr::lng_recent_remove(tr::now), [=] {
		controller->session().recentSearchQueries().remove(query);
	}, &st::menuIconDelete);
	_menu->addAction(tr::lng_recent_clear_all(tr::now), [=] {
		controller->show(Ui::MakeConfirmBox({
			.text = tr::lng_recent_clear_sure(tr::now),
			.confirmed = [=](Fn<void()> close) {
				controller->session().recentSearchQueries().clear();
				close();
			},
		}));
	}, &st::menuIconCancel);
	_menu->popup(QCursor::pos());
}

} // namespace Dialogs
