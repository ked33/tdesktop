/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "ui/rp_widget.h"
#include "ui/widgets/scroll_area.h"

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Dialogs {

class QueryRow;

class RecentQueries final : public Ui::RpWidget {
public:
	enum class Jump : uchar {
		NotApplied,
		Applied,
		AppliedAndOut,
	};

	RecentQueries(
		QWidget *parent,
		not_null<Window::SessionController*> controller);
	~RecentQueries();

	[[nodiscard]] int count() const;
	[[nodiscard]] rpl::producer<int> countValue() const;
	[[nodiscard]] rpl::producer<int> selectedIndexValue() const;
	[[nodiscard]] rpl::producer<QString> chosen() const;
	[[nodiscard]] auto scrollToRequests() const
		-> rpl::producer<Ui::ScrollToRequest>;

	Jump selectJump(Qt::Key direction, int pageSize);
	bool choose();

protected:
	int resizeGetHeight(int newWidth) override;

private:
	void setupHeader();
	void rebuild();
	void setSelected(int index, bool scroll);
	void applySelected();
	void showMenu(not_null<QWidget*> row, const QString &query);

	const not_null<Window::SessionController*> _controller;
	Ui::RpWidget *_header = nullptr;
	std::vector<QString> _queries;
	std::vector<not_null<QueryRow*>> _rows;
	rpl::variable<int> _count;
	rpl::variable<int> _selected = -1;
	rpl::event_stream<QString> _chosen;
	rpl::event_stream<Ui::ScrollToRequest> _scrollToRequests;
	base::unique_qptr<Ui::PopupMenu> _menu;
	bool _rebuildScheduled = false;

};

} // namespace Dialogs
