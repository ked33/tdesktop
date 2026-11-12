/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_chat_switch_process.h"

#include "core/application.h"
#include "core/shortcuts.h"
#include "base/event_filter.h"
#include "base/weak_ptr.h"
#include "data/components/recent_peers.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_saved_sublist.h"
#include "data/data_thread.h"
#include "info/profile/info_profile_cover.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/shadow.h"
#include "ui/controls/userpic_button.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "window/window_lock_widgets.h"
#include "window/window_separate_id.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_userpic_button.h"
#include "styles/style_window.h"

#include <QtWidgets/QApplication>

namespace Window {
namespace {

class Button final : public Ui::AbstractButton {
public:
	Button(
		QWidget *parent,
		not_null<Data::Thread*> thread,
		base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView> &userpics);

	[[nodiscard]] rpl::producer<> selectRequests() const;

	void setSelected(
		bool selected,
		anim::type animated = anim::type::normal);

private:
	void setup(
		not_null<Data::Thread*> thread,
		base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView> &userpics);

	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;

	rpl::event_stream<> _selectRequests;
	Ui::Animations::Simple _overAnimation;
	bool _selected = false;

};

void CloseInWindows(not_null<Data::Thread*> thread) {
	using WindowPointer = base::weak_ptr<Window::SessionController>;
	auto closing = std::vector<WindowPointer>();
	auto clearing = std::vector<WindowPointer>();
	for (const auto &window : thread->session().windows()) {
		if (window->windowId().chat() == thread) {
			closing.push_back(base::make_weak(window));
		} else if (window->activeChatCurrent().thread() == thread) {
			clearing.push_back(base::make_weak(window));
		}
	}
	for (const auto &window : closing) {
		if (const auto strong = window.get()) {
			Core::App().closeWindow(&strong->window());
		}
	}
	for (const auto &window : clearing) {
		if (const auto strong = window.get()) {
			strong->clearSectionStack();
		}
	}
}

Button::Button(
	QWidget *parent,
	not_null<Data::Thread*> thread,
	base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView> &userpics)
: AbstractButton(parent) {
	setup(thread, userpics);
}

void Button::setup(
		not_null<Data::Thread*> thread,
		base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView> &userpics) {
	resize(st::chatSwitchSize);

	auto userpicSt = &st::chatSwitchUserpic;
	const auto userpicSize = userpicSt->size;
	if (const auto topic = thread->asTopic()) {
		using namespace Info::Profile;
		const auto userpic = Ui::CreateChild<TopicIconButton>(
			this,
			topic,
			[] { return true; }); // paused
		userpic->show();
		userpic->move(
			((width() - userpic->width()) / 2),
			st::chatSwitchUserpicTop);
		userpic->setAttribute(Qt::WA_TransparentForMouseEvents);

		userpicSt = &st::chatSwitchUserpicSmall;
	} else if (const auto sublist = thread->asSublist()) {
		const auto sublistPeer = sublist->sublistPeer();
		const auto userpic = Ui::CreateChild<Ui::UserpicButton>(
			this,
			sublistPeer,
			st::chatSwitchUserpicSublist);
		userpic->showMyNotesOnSelf(true);
		userpic->show();
		userpic->move(
			((width() - userpicSize.width()) / 2),
			st::chatSwitchUserpicTop);
		userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
		userpics.emplace(sublistPeer, sublistPeer->createUserpicView());

		userpicSt = &st::chatSwitchUserpicSmall;
	}
	const auto peer = thread->peer();
	const auto userpic = Ui::CreateChild<Ui::UserpicButton>(
		this,
		peer,
		*userpicSt);
	userpic->showSavedMessagesOnSelf(true);
	userpic->show();
	userpic->move(
		(((width() - userpicSize.width()) / 2)
			+ (userpicSize.width() - userpicSt->size.width())),
		(st::chatSwitchUserpicTop
			+ (userpicSize.height() - userpicSt->size.height())));
	userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
	userpics.emplace(peer, peer->createUserpicView());

	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		this,
		((thread->asHistory() && peer->isSelf())
			? tr::lng_saved_messages(tr::now)
			: thread->chatListName()),
		st::chatSwitchNameLabel);
	label->setBreakEverywhere(true);
	label->show();
	label->resizeToNaturalWidth(
		width() - 2 * st::chatSwitchNameSkip);
	label->move(
		(width() - label->width()) / 2,
		(height() + userpic->y() + userpic->height() - label->height()) / 2);

	show();
}

rpl::producer<> Button::selectRequests() const {
	return _selectRequests.events();
}

void Button::setSelected(bool selected, anim::type animated) {
	if (_selected == selected) {
		if (animated == anim::type::instant) {
			_overAnimation.stop();
		}
		return;
	}
	_selected = selected;
	if (animated == anim::type::instant) {
		_overAnimation.stop();
		update();
	} else {
		_overAnimation.start(
			[=] { update(); },
			selected ? 0. : 1.,
			selected ? 1. : 0.,
			st::slideWrapDuration);
	}
}

void Button::paintEvent(QPaintEvent *e) {
	const auto selected = _selected ? 1. : 0.;
	const auto selection = _overAnimation.value(selected);
	if (selection <= 0.) {
		return;
	}

	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = st::boxRadius;
	const auto line = st::chatSwitchSelectLine;
	auto pen = st::defaultRoundCheckbox.bgActive->p;
	pen.setWidthF(line * selection);
	p.setPen(pen);
	const auto r = QRectF(rect()).marginsRemoved(
		{ line / 2., line / 2., line / 2., line / 2. });
	p.drawRoundedRect(r, radius, radius);
}

void Button::mouseMoveEvent(QMouseEvent *e) {
	if (!_selected) {
		_selectRequests.fire({});
	}
}

} // namespace

struct ChatSwitchProcess::Entry {
	base::weak_ptr<Data::Thread> thread;
	std::unique_ptr<Button> button;
};

ChatSwitchProcess::ChatSwitchProcess(
	not_null<Ui::RpWidget*> geometry,
	not_null<Main::Session*> session,
	Data::Thread *opened)
: _session(session)
, _widget(std::make_unique<Ui::RpWidget>(
	geometry->parentWidget() ? geometry->parentWidget() : geometry))
, _view(Ui::CreateChild<Ui::RpWidget>(_widget.get()))
, _scroll(Ui::CreateChild<Ui::ScrollArea>(_view.get()))
, _content(_scroll->setOwnedWidget(object_ptr<Ui::RpWidget>(_scroll.get())).data())
, _empty(Ui::CreateChild<Ui::FlatLabel>(
	_content.get(),
	tr::lng_recent_chats_empty(tr::now),
	st::chatSwitchEmptyLabel))
, _bg(st::boxRadius, st::boxBg) {
	if (opened) {
		_session->recentPeers().chatOpenPush(opened);
	}
	setupWidget(geometry);
	setupContent(opened);
	setupView();
}

ChatSwitchProcess::~ChatSwitchProcess() {
	_session->recentPeers().chatOpenKeepUserpics(std::move(_userpics));
}

rpl::producer<not_null<Data::Thread*>> ChatSwitchProcess::chosen() const {
	return _chosen.events();
}

rpl::producer<> ChatSwitchProcess::closeRequests() const {
	return _closeRequests.events();
}

void ChatSwitchProcess::process(const Request &request) {
	if (Core::App().passcodeLocked() || _session->termsLocked().has_value()) {
		return;
	}
	if (request.started) {
		_persistent = (request.action == Qt::Key());
	}
	const auto count = int(_entries.size());
	if (request.action == Qt::Key_Escape) {
		_closeRequests.fire({});
	} else if (request.action == Qt::Key_Enter) {
		const auto thread = (_selected >= 0 && _selected < count)
			? _entries[_selected].thread.get()
			: nullptr;
		if (thread) {
			_chosen.fire_copy(not_null(thread));
		} else {
			_closeRequests.fire({});
		}
	} else if (!count) {
		return;
	} else if (request.action == Qt::Key_Tab
		|| request.action == Qt::Key_Right) {
		setSelected((_selected + 1) % count);
	} else if (request.action == Qt::Key_Backtab
		|| request.action == Qt::Key_Left) {
		setSelected((_selected <= 0) ? count - 1 : _selected - 1);
	} else if (request.action == Qt::Key_Up) {
		setSelected((std::max(_selected, 0) + count - _shownPerRow) % count);
	} else if (request.action == Qt::Key_Down) {
		setSelected((std::max(_selected, 0) + _shownPerRow) % count);
	} else if (request.action == Qt::Key_Q && _selected >= 0) {
		const auto weak = _entries[_selected].thread;
		if (const auto thread = weak.get()) {
			_session->recentPeers().chatOpenRemove(thread);
			if (const auto alive = weak.get()) {
				CloseInWindows(alive);
			}
		}
	}
}

void ChatSwitchProcess::setSelected(int index, bool scroll) {
	if (index < 0 || index >= int(_entries.size())) {
		return;
	}
	if (_selected != index) {
		if (_selected >= 0 && _entries[_selected].button) {
			_entries[_selected].button->setSelected(false);
		}
		_selected = index;
		if (_entries[_selected].button) {
			_entries[_selected].button->setSelected(true);
		}
	}
	if (scroll) {
		const auto top = (index / _shownPerRow) * st::chatSwitchSize.height();
		_scroll->scrollToY(top, top + st::chatSwitchSize.height());
		updateVisibleEntries();
	}
}

void ChatSwitchProcess::setupWidget(not_null<Ui::RpWidget*> geometry) {
	geometry->geometryValue(
	) | rpl::on_next([=](QRect value) {
		const auto parent = geometry->parentWidget();
		_widget->setGeometry((parent == _widget->parentWidget())
			? value
			: QRect(QPoint(), value.size()));
	}, _widget->lifetime());

	_widget->events() | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::MouseButtonPress) {
			crl::on_main(_widget.get(), [=] {
				_closeRequests.fire({});
			});
		}
	}, _widget->lifetime());

	_widget->show();
}

void ChatSwitchProcess::setupContent(Data::Thread *opened) {
	refreshContent();
	if (opened) {
		const auto i = ranges::find(_entries, opened, [](const Entry &entry) {
			return entry.thread.get();
		});
		if (i != end(_entries)) {
			_selected = int(i - begin(_entries));
		}
	} else if (!_entries.empty()) {
		_selected = 0;
	}
	_session->recentPeers().chatOpenHistoryChanges(
	) | rpl::on_next([=] { scheduleRefresh(); }, _lifetime);
}

void ChatSwitchProcess::scheduleRefresh() {
	if (std::exchange(_refreshScheduled, true)) {
		return;
	}
	crl::on_main(_widget.get(), [=] {
		_refreshScheduled = false;
		refreshContent();
	});
}

void ChatSwitchProcess::refreshContent() {
	const auto previous = _selected;
	const auto selected = (_selected >= 0 && _selected < int(_entries.size()))
		? _entries[_selected].thread
		: base::weak_ptr<Data::Thread>();
	_contentLifetime.destroy();
	_layingOut = true;
	_entries.clear();
	_userpics.clear();
	_selected = -1;
	for (const auto thread : _session->recentPeers().collectChatOpenHistory()) {
		_entries.push_back({ base::make_weak(thread), nullptr });
		if (thread.get() == selected.get()) {
			_selected = int(_entries.size()) - 1;
		}
		auto destroyed = thread->asTopic()
			? thread->asTopic()->destroyed()
			: thread->asSublist()
			? thread->asSublist()->destroyed()
			: nullptr;
		if (destroyed) {
			const auto index = int(_entries.size()) - 1;
			std::move(destroyed) | rpl::on_next([=] {
				_entries[index].button = nullptr;
				scheduleRefresh();
			}, _contentLifetime);
		}
	}
	if (_selected < 0 && previous >= 0 && !_entries.empty()) {
		_selected = std::min(previous, int(_entries.size()) - 1);
	}
	_layingOut = false;
	layout(_widget->size());
}

void ChatSwitchProcess::setupView() {
	_scroll->show();
	_content->show();
	_scroll->scrolls() | rpl::on_next([=] {
		updateVisibleEntries();
	}, _lifetime);
	base::install_event_filter(qApp, [=](not_null<QEvent*> event) {
		if (!_persistent
			|| Core::App().passcodeLocked()
			|| _session->termsLocked().has_value()
			|| !_widget->window()->isActiveWindow()
			|| (event->type() != QEvent::ShortcutOverride
				&& event->type() != QEvent::KeyPress)) {
			return base::EventFilterResult::Continue;
		}
		const auto key = static_cast<QKeyEvent*>(event.get());
		if (key->modifiers()
			& (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
			return base::EventFilterResult::Continue;
		}
		auto action = Qt::Key(key->key());
		switch (action) {
		case Qt::Key_Return:
			action = Qt::Key_Enter;
			break;
		case Qt::Key_Tab:
			if (key->modifiers() & Qt::ShiftModifier) {
				action = Qt::Key_Backtab;
			}
			break;
		case Qt::Key_Backtab:
		case Qt::Key_Left:
		case Qt::Key_Right:
		case Qt::Key_Up:
		case Qt::Key_Down:
		case Qt::Key_Enter:
		case Qt::Key_Escape:
		case Qt::Key_Q:
			break;
		default:
			return base::EventFilterResult::Continue;
		}
		event->accept();
		if (event->type() == QEvent::KeyPress) {
			crl::on_main(_widget.get(), [=] { process({ .action = action }); });
		}
		return base::EventFilterResult::Cancel;
	}, _lifetime);
	_widget->sizeValue() | rpl::on_next([=](QSize size) {
		layout(size);
	}, _view->lifetime());

	_view->paintRequest() | rpl::on_next([=](QRect clip) {
		if (_outer.isEmpty()) {
			return;
		}
		auto p = QPainter(_view);
		p.translate(-_shadowed.topLeft());
		Ui::Shadow::paint(p, _outer, _view->width(), st::boxRoundShadow);
		_bg.paint(p, _outer);
	}, _view->lifetime());

	_view->events() | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::MouseButtonPress) {
			e->accept();
		}
	}, _view->lifetime());
}

void ChatSwitchProcess::layout(QSize size) {
	const auto full = QRect(QPoint(), size);
	const auto available = full.marginsRemoved(
		st::chatSwitchMargins + st::chatSwitchPadding + st::boxRoundShadow.extend);
	const auto bar = st::defaultScrollArea.width;
	const auto cell = st::chatSwitchSize;
	if (available.width() < cell.width() + bar || available.height() < 1) {
		_view->hide();
		return;
	}
	_layingOut = true;
	const auto count = int(_entries.size());
	const auto maxColumns = (available.width() - bar) / cell.width();
	const auto maxRows = std::max(available.height() / cell.height(), 1);
	auto columns = 1;
	while (columns < maxColumns && columns * columns < 2 * count) {
		++columns;
	}
	_shownPerRow = std::min(std::max(count, 1), columns);
	const auto rows = std::max((count + _shownPerRow - 1) / _shownPerRow, 1);
	const auto width = count
		? _shownPerRow * cell.width() + bar
		: std::min(available.width(), st::chatSwitchEmptyWidth);
	const auto height = std::min(available.height(), std::min(rows, maxRows) * cell.height());
	_inner = QRect((full.width() - width) / 2, (full.height() - height) / 2, width, height);
	_outer = _inner.marginsAdded(st::chatSwitchPadding);
	_shadowed = _outer.marginsAdded(st::boxRoundShadow.extend);
	_view->setGeometry(_shadowed);
	_scroll->setGeometry(QRect(_inner.topLeft() - _shadowed.topLeft(), _inner.size()));
	_content->resize(width, rows * cell.height());
	_empty->setVisible(!count);
	if (!count) {
		_empty->resizeToWidth(width - bar);
		_empty->moveToLeft(0, std::max((height - _empty->height()) / 2, 0));
	}
	_layingOut = false;
	if (_selected >= 0) {
		setSelected(_selected);
	} else {
		updateVisibleEntries();
	}
	_view->show();
	_view->update();
}

void ChatSwitchProcess::updateVisibleEntries() {
	if (_layingOut) {
		return;
	}
	const auto cell = st::chatSwitchSize;
	const auto first = std::max(_scroll->scrollTop() / cell.height() - 1, 0)
		* _shownPerRow;
	const auto last = std::min(int(_entries.size()),
		((_scroll->scrollTop() + _scroll->height()) / cell.height() + 2)
			* _shownPerRow);
	auto peers = base::flat_set<not_null<PeerData*>>();
	for (auto i = 0; i < int(_entries.size()); ++i) {
		auto &entry = _entries[i];
		const auto thread = entry.thread.get();
		if (i < first || i >= last || !thread) {
			entry.button = nullptr;
			continue;
		}
		peers.emplace(thread->peer());
		if (const auto sublist = thread->asSublist()) {
			peers.emplace(sublist->sublistPeer());
		}
		if (!entry.button) {
			entry.button = std::make_unique<Button>(_content.get(), thread, _userpics);
			const auto raw = entry.button.get();
			raw->selectRequests() | rpl::on_next([=] {
				setSelected(i, false);
			}, raw->lifetime());
			const auto weak = entry.thread;
			raw->setClickedCallback([=] {
				if (const auto alive = weak.get()) {
					_chosen.fire_copy(not_null(alive));
				}
			});
		}
		entry.button->setSelected(_selected == i, anim::type::instant);
		entry.button->moveToLeft(
			(i % _shownPerRow) * cell.width(),
			(i / _shownPerRow) * cell.height(),
			_content->width() - st::defaultScrollArea.width);
	}
	for (auto i = begin(_userpics); i != end(_userpics);) {
		if (peers.contains(i->first)) {
			++i;
		} else {
			i = _userpics.erase(i);
		}
	}
}

rpl::lifetime &ChatSwitchProcess::lifetime() {
	return _lifetime;
}

} // namespace Window
