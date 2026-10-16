/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/hashtag_autocomplete.h"

#include "main/main_session.h"
#include "settings.h"
#include "storage/localstorage.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/scroll_area.h"
#include "styles/style_basic.h"
#include "styles/style_chat_helpers.h"

namespace ChatHelpers {
namespace {

constexpr auto kMaxVisibleRows = 5;

} // namespace

class HashtagAutocomplete::Inner final : public Ui::RpWidget {
public:
	Inner(QWidget *parent, not_null<HashtagAutocomplete*> owner);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	[[nodiscard]] int rowIndexAt(QPoint position) const;

	const not_null<HashtagAutocomplete*> _owner;
	int _pressed = -1;

};

Inner::Inner(QWidget *parent, not_null<HashtagAutocomplete*> owner)
: RpWidget(parent)
, _owner(owner) {
	setMouseTracking(true);
}

int Inner::rowIndexAt(QPoint position) const {
	const auto index = position.y() / st::mentionHeight;
	return (index >= 0 && index < int(_owner->_rows.size()))
		? index
		: -1;
}

void Inner::paintEvent(QPaintEvent *e) {
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
		const auto count = u" (%1)"_q.arg(row.count);
		const auto available = std::max(
			width() - left - font->width(count) - st::mentionPadding.right(),
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
	}
}

void Inner::mouseMoveEvent(QMouseEvent *e) {
	_owner->setSelected(rowIndexAt(e->pos()));
}

void Inner::mousePressEvent(QMouseEvent *e) {
	_pressed = rowIndexAt(e->pos());
	if (_pressed >= 0) {
		_owner->setSelected(_pressed);
	}
}

void Inner::mouseReleaseEvent(QMouseEvent *e) {
	const auto index = rowIndexAt(e->pos());
	if (index >= 0 && index == _pressed) {
		_owner->chooseSelected();
	}
	_pressed = -1;
}

HashtagAutocomplete::HashtagAutocomplete(
	QWidget *parent,
	not_null<Ui::InputField*> field,
	not_null<Main::Session*> session)
: RpWidget(parent)
, _field(field)
, _session(session)
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
	}, _lifetime);
}

HashtagAutocomplete::~HashtagAutocomplete() = default;

void HashtagAutocomplete::setTargetRect(QRect rect) {
	_target = rect;
	if (!isHidden()) {
		updateListGeometry();
	}
}

bool HashtagAutocomplete::handleKeyPress(not_null<QKeyEvent*> e) {
	if (isHidden()) {
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
		return chooseSelected();
	}
	return false;
}

bool HashtagAutocomplete::eventFilter(QObject *obj, QEvent *e) {
	if (e->type() == QEvent::KeyPress) {
		if (handleKeyPress(static_cast<QKeyEvent*>(e))) {
			return true;
		}
	}
	return QWidget::eventFilter(obj, e);
}

void HashtagAutocomplete::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);

	p.fillRect(rect(), st::mentionBg);
	const auto line = st::lineWidth;
	p.fillRect(0, 0, width(), line, st::shadowFg);
	p.fillRect(0, height() - line, width(), line, st::shadowFg);
}

void HashtagAutocomplete::resizeEvent(QResizeEvent *e) {
	RpWidget::resizeEvent(e);

	const auto line = st::lineWidth;
	_scroll->setGeometry(0, line, width(), std::max(height() - 2 * line, 0));
}

void HashtagAutocomplete::updateQuery() {
	if (!_field->isVisible() || _target.width() <= 0) {
		hideFast();
		return;
	}
	const auto query = computeQuery();
	if (query.isEmpty()) {
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
	const auto text = _field->getLastText();
	const auto cursor = _field->textCursor().position();
	auto start = -1;
	for (auto i = cursor; i > 0;) {
		if (text.size() < i) {
			break;
		}
		const auto ch = text[i - 1];
		if (ch == '#') {
			start = i - 1;
			break;
		} else if (!ch.isLetterOrNumber() && ch != '_') {
			break;
		}
		--i;
	}
	return (start >= 0) ? text.mid(start, cursor - start) : QString();
}

void HashtagAutocomplete::updateFiltered(const QString &filter) {
	const auto normalized = TextUtilities::RemoveAccents(filter.toLower());
	const auto resetScroll = (normalized != _filter);
	_filter = normalized;

	if (cRecentWriteHashtags().isEmpty()) {
		_session->local().readRecentHashtagsAndBots();
	}
	const auto listAllSuggestions = _filter.isEmpty();
	auto rows = std::vector<Row>();
	for (const auto &item : cRecentWriteHashtags()) {
		const auto &tag = item.first;
		if (!listAllSuggestions
			&& (tag.size() == _filter.size()
				|| !TextUtilities::RemoveAccents(tag).startsWith(
					_filter,
					Qt::CaseInsensitive))) {
			continue;
		}
		rows.push_back({ tag, int(item.second) });
	}
	_rows = std::move(rows);
	setSelected(_rows.empty() ? -1 : 0);
	if (resetScroll) {
		_scroll->scrollToY(0);
	}
	if (_inner) {
		_inner->update();
	}
}

void HashtagAutocomplete::updateListGeometry() {
	const auto visible = std::min(int(_rows.size()), kMaxVisibleRows);
	const auto width = _target.width();
	if (_inner) {
		_inner->resize(width, int(_rows.size()) * st::mentionHeight);
	}
	setGeometry(_target.x(), _target.y(), width, visible * st::mentionHeight);
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
	if (_selected < 0 || _selected >= int(_rows.size())) {
		return false;
	}
	const auto tag = _rows[_selected].tag;

	const auto text = _field->getLastText();
	auto cursor = _field->textCursor().position();
	auto start = -1;
	for (auto i = cursor; i > 0;) {
		if (text.size() < i) {
			break;
		}
		const auto ch = text[i - 1];
		if (ch == '#') {
			start = i - 1;
			break;
		} else if (!ch.isLetterOrNumber() && ch != '_') {
			break;
		}
		--i;
	}
	auto replaced = QString();
	auto position = 0;
	if (start < 0) {
		replaced = text.mid(0, cursor) + '#' + tag + ' ' + text.mid(cursor);
		position = cursor + 1 + int(tag.size()) + 1;
	} else {
		while (cursor < text.size() && cursor - start - 1 < tag.size()) {
			if (text[cursor].toCaseFolded()
				!= tag[cursor - start - 1].toCaseFolded()) {
				break;
			}
			++cursor;
		}
		if (cursor - start - 1 == tag.size()
			&& cursor < text.size()
			&& text[cursor] == ' ') {
			++cursor;
		}
		replaced = text.mid(0, start + 1) + tag + ' ' + text.mid(cursor);
		position = start + 1 + int(tag.size()) + 1;
	}
	_field->setText(replaced);
	_field->setCursorPosition(position);
	hideFast();
	return true;
}

} // namespace ChatHelpers
