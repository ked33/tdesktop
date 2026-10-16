/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/hashtag_autocomplete.h"

#include "chat_helpers/compose/compose_features.h"
#include "chat_helpers/message_field.h"
#include "core/enhanced_settings.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/painter.h"
#include "settings.h"

#include <crl/crl_on_main.h>

#include <QtGui/QCursor>
#include <QtWidgets/QTextEdit>

#include "styles/style_basic.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_dialogs.h"

namespace ChatHelpers {
namespace {

constexpr auto kSearchHistoryVisibleRows = 5;

} // namespace

class HashtagAutocomplete::Inner final : public Ui::RpWidget {
public:
	Inner(QWidget *parent, not_null<HashtagAutocomplete*> owner);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	[[nodiscard]] int rowIndexAt(QPoint position) const;
	[[nodiscard]] bool deleteAt(QPoint position) const;

	const not_null<HashtagAutocomplete*> _owner;
	QString _pressedTag;
	int _pressed = -1;
	bool _deletePressed = false;

};

HashtagAutocomplete::Inner::Inner(
	QWidget *parent,
	not_null<HashtagAutocomplete*> owner)
: RpWidget(parent)
, _owner(owner) {
	setMouseTracking(true);
}

int HashtagAutocomplete::Inner::rowIndexAt(QPoint position) const {
	if (!rect().contains(position)) {
		return -1;
	}
	const auto index = position.y() / st::mentionHeight;
	return (index >= 0 && index < int(_owner->_rows.size()))
		? index
		: -1;
}

bool HashtagAutocomplete::Inner::deleteAt(QPoint position) const {
	return _owner->_source == Source::SearchHistory
		&& rowIndexAt(position) >= 0
		&& (rtl() ? position.x() : width() - position.x())
			< st::mentionHeight;
}

void HashtagAutocomplete::Inner::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);

	const auto clip = e->rect();
	const auto from = clip.top() / st::mentionHeight;
	const auto till = clip.bottom() / st::mentionHeight + 1;
	const auto last = int(_owner->_rows.size());
	const auto &font = st::mentionFont;
	const auto &filter = _owner->_filter;
	const auto filterSize = int(filter.size());
	const auto filterIsEmpty = filter.isEmpty();
	const auto left = st::mentionPadding.left();

	for (auto i = from; i < till && i < last; ++i) {
		const auto &row = _owner->_rows[i];
		const auto top = i * st::mentionHeight;
		const auto selected = (i == _owner->_selected);
		if (selected) {
			p.fillRect(0, top, width(), st::mentionHeight, st::mentionBgOver);
		}

		auto first = filterIsEmpty
			? QString()
			: ('#' + row.tag.mid(0, filterSize));
		auto second = filterIsEmpty
			? ('#' + row.tag)
			: row.tag.mid(filterSize);
		const auto history = (_owner->_source == Source::SearchHistory);
		const auto count = history ? u" (%1)"_q.arg(row.count) : QString();
		const auto available = std::max(
			width() - left - font->width(count)
				- (history ? st::mentionHeight : st::mentionPadding.right()),
			0);
		if (available < font->width(first) + font->width(second)) {
			if (available < font->width(first) + font->elidew) {
				first = font->elided(first + second, available);
				second = QString();
			} else {
				second = font->elided(second, available - font->width(first));
			}
		}
		const auto firstWidth = font->width(first);
		const auto secondWidth = font->width(second);

		p.setFont(font);
		const auto textTop = top + st::mentionTop + font->ascent;
		if (!first.isEmpty()) {
			p.setPen(selected
				? st::mentionFgOverActive
				: st::mentionFgActive);
			p.drawText(left, textTop, first);
		}
		if (!second.isEmpty()) {
			p.setPen(selected ? st::mentionFgOver : st::mentionFg);
			p.drawText(left + firstWidth, textTop, second);
		}
		p.setPen(selected ? st::mentionFgOver : st::mentionFg);
		p.drawText(left + firstWidth + secondWidth, textTop, count);
		if (history && selected) {
			const auto &icon = st::smallCloseIconOver;
			const auto skip = (st::mentionHeight - icon.width()) / 2;
			icon.paint(
				p,
				QPoint(width() - st::mentionHeight + skip, top + skip),
				width());
		}
	}
}

void HashtagAutocomplete::Inner::mouseMoveEvent(QMouseEvent *e) {
	const auto position = e->globalPosition().toPoint();
	if (_owner->_lastMousePosition == position) {
		return;
	}
	_owner->_lastMousePosition = position;
	_owner->_mouseSelection = true;
	_owner->setSelected(rowIndexAt(e->pos()));
}

void HashtagAutocomplete::Inner::leaveEventHook(QEvent *e) {
	if (_owner->_mouseSelection) {
		_owner->setSelected(-1);
	}
}

void HashtagAutocomplete::Inner::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = rowIndexAt(e->pos());
	_deletePressed = deleteAt(e->pos());
	if (_pressed >= 0) {
		_pressedTag = _owner->_rows[_pressed].tag;
		_owner->_mouseSelection = true;
		_owner->setSelected(_pressed);
	}
}

void HashtagAutocomplete::Inner::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto index = rowIndexAt(e->pos());
	if (index >= 0 && index == _pressed
		&& _pressedTag == _owner->_rows[index].tag
		&& _deletePressed == deleteAt(e->pos())) {
		_owner->setSelected(index);
		if (_deletePressed) {
			_owner->removeSelected();
		} else {
			_owner->chooseSelected();
		}
	}
	_pressed = -1;
}

HashtagAutocomplete::HashtagAutocomplete(
	QWidget *parent,
	not_null<Ui::InputField*> field,
	not_null<Main::Session*> session,
	Source source)
: RpWidget(parent)
, _field(field)
, _session(session)
, _source(source)
, _scroll(this) {
	hide();

	_scroll->setGeometry(rect());
	_inner = _scroll->setOwnedWidget(
		object_ptr<Inner>(this, this));

	_field->rawTextEdit()->installEventFilter(this);

	QObject::connect(
		_field->rawTextEdit().get(),
		&QTextEdit::cursorPositionChanged,
		this,
		[=] { updateQuery(); },
		Qt::QueuedConnection); // So getLastText() works already.

	_field->changes(
	) | rpl::on_next([=] {
		updateQuery();
	}, lifetime());

	_field->focusedChanges(
	) | rpl::on_next([=] {
		crl::on_main(this, [=] { updateQuery(); });
	}, lifetime());

	_field->tabbed(
	) | rpl::on_next([=](not_null<Ui::InputField::TabbedRequest*> request) {
		if (!request->handled && acceptsKeyboard()) {
			chooseSelected();
			request->handled = true;
		}
	}, lifetime());

	_session->local().recentHashtagsChanges(
	) | rpl::on_next([=] {
		crl::on_main(this, [=] { updateQuery(); });
	}, lifetime());
}

HashtagAutocomplete::~HashtagAutocomplete() = default;

void HashtagAutocomplete::setTitle(rpl::producer<QString> title) {
	_title = object_ptr<Ui::FlatLabel>(
		this,
		std::move(title),
		st::searchedBarLabel);
	_title->setAttribute(Qt::WA_TransparentForMouseEvents);
	_title->show();
	updateQuery();
}

void HashtagAutocomplete::setTargetRect(QRect rect) {
	_target = rect;
	updateQuery();
}

void HashtagAutocomplete::setAllowed(bool allowed) {
	if (_allowed != allowed) {
		_allowed = allowed;
		updateQuery();
	}
}

void HashtagAutocomplete::setKeyboardEnabled(bool enabled) {
	_keyboardEnabled = enabled;
}

bool HashtagAutocomplete::acceptsKeyboard() const {
	return !isHidden()
		&& _keyboardEnabled
		&& _field->rawTextEdit()->hasFocus();
}

bool HashtagAutocomplete::handleKeyPress(not_null<QKeyEvent*> e) {
	if (!acceptsKeyboard()) {
		return false;
	}
	const auto modifiers = Qt::AltModifier
		| Qt::ControlModifier
		| Qt::ShiftModifier
		| Qt::MetaModifier;
	if (e->modifiers() & modifiers) {
		return false;
	}
	const auto key = e->key();
	if (key == Qt::Key_Up || key == Qt::Key_Down) {
		moveSelection((key == Qt::Key_Up) ? -1 : 1);
		return true;
	} else if (key == Qt::Key_Enter || key == Qt::Key_Return) {
		return !_mouseSelection && chooseSelected();
	} else if (key == Qt::Key_Escape && _source == Source::Sent) {
		_dismissedQuery = computeQuery();
		hideFast();
		return true;
	}
	return false;
}

bool HashtagAutocomplete::eventFilter(QObject *obj, QEvent *e) {
	if (e->type() == QEvent::KeyPress) {
		if (handleKeyPress(static_cast<QKeyEvent*>(e))) {
			return true;
		}
	} else if (e->type() == QEvent::Hide) {
		hideFast();
	} else if (e->type() == QEvent::Show) {
		crl::on_main(this, [=] { updateQuery(); });
	}
	return QWidget::eventFilter(obj, e);
}

void HashtagAutocomplete::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);

	p.fillRect(rect(), st::mentionBg);
	const auto line = st::lineWidth;
	if (_title) {
		p.fillRect(0, 0, width(), st::searchedBarHeight, st::searchedBarBg);
	}
	p.fillRect(0, 0, width(), line, st::shadowFg);
	p.fillRect(0, height() - line, width(), line, st::shadowFg);
}

void HashtagAutocomplete::resizeEvent(QResizeEvent *e) {
	RpWidget::resizeEvent(e);

	const auto titleHeight = _title ? st::searchedBarHeight : 0;
	if (_title) {
		_title->resizeToWidth(std::max(
			width() - 2 * st::searchedBarPosition.x(),
			0));
		_title->moveToLeft(
			st::searchedBarPosition.x(),
			st::searchedBarPosition.y());
	}
	_scroll->setGeometry(
		0,
		titleHeight,
		width(),
		std::max(height() - titleHeight, 0));
}

void HashtagAutocomplete::updateQuery() {
	if (!_allowed || !_field->isVisible()
		|| (_source == Source::Sent && !_field->rawTextEdit()->hasFocus())
		|| _target.width() <= 0 || _target.height() <= 0) {
		hideFast();
		return;
	}
	const auto query = computeQuery();
	if (query != _dismissedQuery) {
		_dismissedQuery.clear();
	}
	if (query.isEmpty() || query == _dismissedQuery) {
		hideFast();
		return;
	}
	updateFiltered(query.mid(1));
	if (_rows.empty()) {
		hideFast();
		return;
	}
	updateListGeometry();
	show();
	raise();
}

QString HashtagAutocomplete::computeQuery() const {
	return ParseMentionHashtagBotCommandQuery(_field, {
		.autocompleteHashtags = true,
		.autocompleteMentions = false,
		.autocompleteCommands = false,
	}).query;
}

void HashtagAutocomplete::updateFiltered(const QString &filter) {
	const auto history = (_source == Source::SearchHistory);
	const auto normalized = history
		? filter.toLower()
		: TextUtilities::RemoveAccents(filter.toLower());
	const auto resetScroll = isHidden() || (normalized != _filter);
	_filter = normalized;

	_session->local().readRecentHashtagsAndBots();
	const auto &recent = history
		? cRecentSearchHashtags()
		: cRecentWriteHashtags();
	const auto limit = history
		? 64
		: EnhancedSettings::HashtagAutocompleteLimit();
	const auto listAllSuggestions = _filter.isEmpty();
	auto rows = std::vector<Row>();
	for (const auto &item : recent) {
		const auto &tag = item.first;
		const auto match = history ? tag : TextUtilities::RemoveAccents(tag);
		if (!listAllSuggestions
			&& (tag.size() == _filter.size()
				|| !match.startsWith(_filter, Qt::CaseInsensitive))) {
			continue;
		}
		rows.push_back({ tag, int(item.second) });
		if (int(rows.size()) >= limit) {
			break;
		}
	}
	if (!resetScroll && rows == _rows) {
		return;
	}
	const auto selected = (_selected >= 0 && _selected < int(_rows.size()))
		? _rows[_selected].tag
		: QString();
	_rows = std::move(rows);
	if (resetScroll) {
		_mouseSelection = false;
		_lastMousePosition = QCursor::pos();
		setSelected(history || _rows.empty() ? -1 : 0);
		_scroll->scrollToY(0);
	} else {
		const auto i = ranges::find(_rows, selected, &Row::tag);
		setSelected(i == end(_rows) ? -1 : int(i - begin(_rows)));
	}
	if (_inner) {
		_inner->update();
	}
}

void HashtagAutocomplete::updateListGeometry() {
	const auto titleHeight = _title ? st::searchedBarHeight : 0;
	const auto maxHeight = (_source == Source::SearchHistory)
		? titleHeight + kSearchHistoryVisibleRows * st::mentionHeight
		: int(4.5 * st::mentionHeight);
	const auto fullHeight = titleHeight
		+ int(_rows.size()) * st::mentionHeight;
	const auto width = _target.width();
	if (_inner) {
		_inner->resize(width, int(_rows.size()) * st::mentionHeight);
	}
	setGeometry(
		_target.x(),
		_target.y(),
		width,
		std::min({ _target.height(), fullHeight, maxHeight }));
}

void HashtagAutocomplete::hideFast() {
	if (!isHidden()) {
		hide();
	}
}

void HashtagAutocomplete::setSelected(int index) {
	if (_selected != index) {
		_selected = index;
		if (_inner) {
			_inner->update();
		}
	}
}

void HashtagAutocomplete::moveSelection(int delta) {
	if (_rows.empty()) {
		return;
	}
	const auto count = int(_rows.size());
	_mouseSelection = false;
	_lastMousePosition = QCursor::pos();
	if (_selected < 0 || _selected >= count) {
		setSelected((delta > 0) ? 0 : (count - 1));
	} else {
		const auto next = _selected + delta;
		setSelected((next < 0 || next >= count) ? -1 : next);
	}
	if (_selected >= 0) {
		_scroll->scrollToY(
			_selected * st::mentionHeight,
			(_selected + 1) * st::mentionHeight);
	}
}

bool HashtagAutocomplete::chooseSelected() {
	if (_selected < 0 || _selected >= int(_rows.size())
		|| computeQuery().isEmpty()) {
		return false;
	}
	const auto tag = _rows[_selected].tag;
	_field->insertTag('#' + tag);
	hideFast();
	_chosen.fire_copy(tag);
	return true;
}

void HashtagAutocomplete::removeSelected() {
	if (_source != Source::SearchHistory
		|| _selected < 0 || _selected >= int(_rows.size())) {
		return;
	}
	const auto tag = _rows[_selected].tag;
	auto recent = cRecentSearchHashtags();
	for (auto i = recent.begin(); i != recent.end();) {
		if (i->first == tag) {
			i = recent.erase(i);
		} else {
			++i;
		}
	}
	cSetRecentSearchHashtags(recent);
	_session->local().writeRecentHashtagsAndBots();
	setSelected(-1);
	updateQuery();
}

rpl::producer<QString> HashtagAutocomplete::chosen() const {
	return _chosen.events();
}

} // namespace ChatHelpers
