/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/round_rect.h"
#include "ui/userpic_view.h"

namespace Data {
class Thread;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Shortcuts {
struct ChatSwitchRequest;
} // namespace Shortcuts

namespace Ui {
class AbstractButton;
class RpWidget;
class ScrollArea;
class FlatLabel;
} // namespace Ui

namespace Window {

class ChatSwitchProcess final {
public:
	// Create widget in geometry->parentWidget() and geometry->geometry().
	ChatSwitchProcess(
		not_null<Ui::RpWidget*> geometry,
		not_null<Main::Session*> session,
		Data::Thread *opened);
	~ChatSwitchProcess();

	[[nodiscard]] rpl::producer<not_null<Data::Thread*>> chosen() const;
	[[nodiscard]] rpl::producer<> closeRequests() const;

	using Request = Shortcuts::ChatSwitchRequest;
	void process(const Request &request);

	[[nodiscard]] rpl::lifetime &lifetime();

private:
	struct Entry;

	void setupWidget(not_null<Ui::RpWidget*> geometry);
	void setupContent(Data::Thread *opened);
	void setupView();

	void layout(QSize size);
	void refreshContent();
	void scheduleRefresh();
	void updateVisibleEntries();

	void setSelected(int index, bool scroll = true);

	const not_null<Main::Session*> _session;
	const std::unique_ptr<Ui::RpWidget> _widget;
	const not_null<Ui::RpWidget*> _view;
	const not_null<Ui::ScrollArea*> _scroll;
	const not_null<Ui::RpWidget*> _content;
	const not_null<Ui::FlatLabel*> _empty;

	QRect _shadowed;
	QRect _outer;
	QRect _inner;
	Ui::RoundRect _bg;

	base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView> _userpics;
	std::vector<Entry> _entries;

	int _selected = -1;
	int _shownPerRow = 1;
	bool _refreshScheduled = false;
	bool _layingOut = false;
	bool _persistent = false;

	rpl::event_stream<not_null<Data::Thread*>> _chosen;
	rpl::event_stream<> _closeRequests;

	rpl::lifetime _lifetime;
	rpl::lifetime _contentLifetime;

};

} // namespace Window
