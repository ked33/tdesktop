/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/hashtag_autocomplete.h"

#include "chat_helpers/compose/compose_features.h"
#include "chat_helpers/field_autocomplete.h"
#include "chat_helpers/message_field.h"
#include "core/enhanced_settings.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/scroll_area.h"
#include "ui/painter.h"
#include "settings.h"

#include <crl/crl_on_main.h>

#include <QtGui/QCursor>
#include <QtWidgets/QTextEdit>

#include "styles/style_basic.h"
#include "styles/style_chat_helpers.h"

namespace ChatHelpers {
namespace {

constexpr auto kSearchHistoryVisibleRows = 5;

} // namespace

class HashtagAutocomplete::Inner final : public Ui::RpWidget {
public:
	Inner(QWidget *parent, not_null<HashtagAutocomplete*> owner);
	void clearPressed();

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

void HashtagAutocomplete::Inner::clearPressed() {
	_pressed = -1;
	_pressedTag.clear();
	_deletePressed = false;
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
	const auto left = st::mentionPadding.left();

	for (auto i = from; i < till && i < last; ++i) {
		const auto &row = _owner->_rows[i];
		const auto top = i * st::mentionHeight;
		const auto selected = (i == _owner->_selected)
			&& (_owner->_keyboardEnabled || _owner->_mouseSelection);
		if (selected) {
			p.fillRect(0, top, width(), st::mentionHeight, st::mentionBgOver);
		}

		const auto history = (_owner->_source == Source::SearchHistory);
		const auto count = history ? u" (%1)"_q.arg(row.count) : QString();
		const auto available = std::max(
			width() - left - font->width(count)
				- (history ? st::mentionHeight : st::mentionPadding.right()),
			0);
		p.setFont(font);
		const auto textTop = top + st::mentionTop + font->ascent;
		const auto textWidth = PaintHashtagMatch(
			p,
			font->f,
			row.match,
			QPoint(left, textTop),
			available,
			(selected ? st::mentionFgOver : st::mentionFg)->c,
			(selected ? st::mentionFgOverActive : st::mentionFgActive)->c);
		p.setPen(selected ? st::mentionFgOver : st::mentionFg);
		p.drawText(left + textWidth, textTop, count);
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
	if (!_owner->_mouseSelection) {
		_owner->_mouseSelection = true;
		update();
	}
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
		_pressedTag = _owner->_rows[_pressed].match.tag;
		_owner->_mouseSelection = true;
		_owner->setSelected(_pressed);
		update();
	}
}

void HashtagAutocomplete::Inner::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto index = rowIndexAt(e->pos());
	if (index >= 0 && index == _pressed
		&& _pressedTag == _owner->_rows[index].match.tag
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
, _suggestions(source == Source::Sent ? TextUtilities::RemoveAccents : nullptr)
, _scroll(this) {
	hide();

	_scroll->setGeometry(rect());
	_inner = _scroll->setOwnedWidget(
		object_ptr<Inner>(this, this));
	_scroll->scrolls(
	) | rpl::on_next([=] {
		clearInvisibleSelection();
	}, lifetime());

	_field->rawTextEdit()->installEventFilter(this);

	QObject::connect(
		_field->rawTextEdit().get(),
		&QTextEdit::cursorPositionChanged,
		this,
		[=] { updateQuery(); },
		Qt::QueuedConnection);
	QObject::connect(
		_field->rawTextEdit().get(),
		&QTextEdit::selectionChanged,
		this,
		[=] { updateQuery(); },
		Qt::QueuedConnection);

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
		_rowsDirty = true;
		_suggestions.invalidate();
		crl::on_main(this, [=] { updateQuery(); });
	}, lifetime());
}

HashtagAutocomplete::~HashtagAutocomplete() = default;

void HashtagAutocomplete::setTargetRect(QRect rect, Direction direction) {
	_target = rect;
	_direction = direction;
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
	_mouseSelection = false;
	_lastMousePosition = QCursor::pos();
	_inner->update();
}

bool HashtagAutocomplete::keyboardEnabled() const {
	return _keyboardEnabled;
}

void HashtagAutocomplete::activateKeyboardSelection() {
	setKeyboardEnabled(true);
	moveSelection(0);
}

void HashtagAutocomplete::dismiss() {
	_dismissedQuery = computeQuery();
	hideFast();
}

bool HashtagAutocomplete::acceptsKeyboard() const {
	return !isHidden()
		&& _keyboardEnabled
		&& _field->rawTextEdit()->hasFocus()
		&& !computeQuery().isEmpty();
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
		dismiss();
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

	p.fillRect(rect(), st::filterInputActiveBg);
	if (_source == Source::SearchHistory) {
		const auto line = st::lineWidth;
		p.fillRect(0, 0, width(), line, st::shadowFg);
		p.fillRect(0, height() - line, width(), line, st::shadowFg);
	}
}

void HashtagAutocomplete::resizeEvent(QResizeEvent *e) {
	RpWidget::resizeEvent(e);

	const auto line = (_source == Source::SearchHistory) ? st::lineWidth : 0;
	_scroll->setGeometry(
		0,
		line,
		width(),
		std::max(height() - 2 * line, 0));
}

void HashtagAutocomplete::updateQuery() {
	const auto borderHeight = (_source == Source::SearchHistory)
		? 2 * st::lineWidth
		: 0;
	if (!_allowed || !_field->isVisible()
		|| (_source == Source::Sent && !_field->rawTextEdit()->hasFocus())
		|| _target.width() <= 0 || _target.height() <= borderHeight) {
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
	const auto resetSelection = updateFiltered(query.mid(1));
	if (_rows.empty()) {
		hideFast();
		return;
	}
	updateListGeometry();
	show();
	raise();
	if (resetSelection) {
		_mouseSelection = false;
		_lastMousePosition = QCursor::pos();
		_scroll->scrollToY(0);
		setSelected(_source == Source::SearchHistory ? -1 : 0);
	}
	clearInvisibleSelection();
}

QString HashtagAutocomplete::computeQuery() const {
	return ParseMentionHashtagBotCommandQuery(_field, {
		.autocompleteHashtags = true,
		.autocompleteMentions = false,
		.autocompleteCommands = false,
	}).query;
}

bool HashtagAutocomplete::updateFiltered(const QString &filter) {
	const auto history = (_source == Source::SearchHistory);
	const auto normalized = history
		? filter.toLower()
		: TextUtilities::RemoveAccents(filter.toLower());
	const auto resetScroll = isHidden() || (normalized != _filter);
	if (!_rowsDirty && normalized == _filter) {
		return resetScroll;
	}
	_session->local().readRecentHashtagsAndBots();
	_rowsDirty = false;
	_filter = normalized;

	const auto &recent = history
		? cRecentSearchHashtags()
		: cRecentWriteHashtags();
	const auto limit = history
		? 64
		: EnhancedSettings::HashtagAutocompleteLimit();
	const auto &matches = _suggestions.filter(recent, filter, limit);
	auto rows = std::vector<Row>();
	rows.reserve(matches.size());
	for (const auto &match : matches) {
		rows.push_back({
			match,
			history ? _session->local().recentSearchHashtagCount(match.tag) : 0,
		});
	}
	if (!resetScroll && rows == _rows) {
		return false;
	}
	const auto selected = (_selected >= 0 && _selected < int(_rows.size()))
		? _rows[_selected].match.tag
		: QString();
	_rows = std::move(rows);
	if (resetScroll) {
		setSelected(-1);
	} else {
		const auto i = ranges::find(_rows, selected, [](const Row &row) {
			return row.match.tag;
		});
		setSelected(i == end(_rows) ? -1 : int(i - begin(_rows)));
	}
	if (_inner) {
		_inner->update();
	}
	return resetScroll;
}

void HashtagAutocomplete::updateListGeometry() {
	const auto listHeight = (_source == Source::SearchHistory)
		? kSearchHistoryVisibleRows * st::mentionHeight + 2 * st::lineWidth
		: std::min(
			int(_rows.size()) * st::mentionHeight,
			FieldAutocomplete::MaxHeight());
	const auto width = _target.width();
	const auto height = std::min(_target.height(), listHeight);
	const auto top = _target.y() + ((_direction == Direction::Up)
		? _target.height() - height
		: 0);
	if (_inner) {
		_inner->resize(width, int(_rows.size()) * st::mentionHeight);
	}
	setGeometry(
		_target.x(),
		top,
		width,
		height);
}

void HashtagAutocomplete::hideFast() {
	if (!isHidden()) {
		hide();
	}
	setSelected(-1);
	_mouseSelection = false;
	_scroll->scrollToY(0);
	_inner->clearPressed();
}

void HashtagAutocomplete::setSelected(int index) {
	if (_selected != index) {
		_selected = index;
		if (_inner) {
			_inner->update();
		}
	}
}

bool HashtagAutocomplete::selectedVisible() const {
	const auto top = _selected * st::mentionHeight;
	return _selected >= 0
		&& _selected < int(_rows.size())
		&& top < _scroll->scrollTop() + _scroll->height()
		&& top + st::mentionHeight > _scroll->scrollTop();
}

void HashtagAutocomplete::clearInvisibleSelection() {
	if (!selectedVisible()) {
		setSelected(-1);
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
		setSelected((delta >= 0) ? 0 : (count - 1));
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
	if (!selectedVisible() || computeQuery().isEmpty()) {
		return false;
	}
	const auto tag = _rows[_selected].match.tag;
	auto cursor = _field->textCursor();
	_dismissedQuery = '#' + tag;
	if (!CompleteHashtag(cursor, '#' + tag)) {
		_dismissedQuery.clear();
		return false;
	}
	_field->setTextCursor(cursor);
	dismiss();
	_chosen.fire_copy(tag);
	return true;
}

void HashtagAutocomplete::removeSelected() {
	if (_source != Source::SearchHistory
		|| _selected < 0 || _selected >= int(_rows.size())) {
		return;
	}
	const auto tag = _rows[_selected].match.tag;
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
