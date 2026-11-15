/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "dialogs/ui/search_filters_button.h"

#include "base/event_filter.h"
#include "lang/lang_keys.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/inner_dropdown.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_dialogs.h"
#include "styles/style_window.h"

#include <algorithm>
#include <array>

#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>

namespace Dialogs::Ui {
namespace {

constexpr auto kFields = std::array{
	&EnhancedSettings::SearchOptions::mainAndArchive,
	&EnhancedSettings::SearchOptions::includePorn,
	&EnhancedSettings::SearchOptions::chineseKeywords,
	&EnhancedSettings::SearchOptions::dialogFilter,
	&EnhancedSettings::SearchOptions::exactMatch,
};

} // namespace

SearchFiltersButton::SearchFiltersButton(
		QWidget *parent,
		QWidget *panelParent)
: ::Ui::IconButton(parent, st::dialogsSearchDialogFilter)
, _panelParent(panelParent) {
	setFocusPolicy(Qt::StrongFocus);
	setClickedCallback([=] {
		if (_panel && !_panel->isHidden()) {
			_panel->hideAnimated();
		} else {
			showPanel(true);
		}
	});
	tr::lng_search_filters() | rpl::on_next([=](const QString &text) {
		setAccessibleName(text);
	}, lifetime());
	rpl::single(EnhancedSettings::GetSearchOptions())
		| rpl::then(EnhancedSettings::SearchOptionsChanges())
		| rpl::on_next([=](EnhancedSettings::SearchOptions options) {
		const auto active = std::any_of(
			begin(kFields),
			end(kFields),
			[&](auto field) { return options.*field; });
		const auto icon = active ? &st::dialogsSearchDialogFilterActive : nullptr;
		setIconOverride(icon, icon);
	}, lifetime());
	base::install_event_filter(this, [=](not_null<QEvent*> event) {
		if (event->type() == QEvent::Enter) {
			showPanel(false);
		} else if (event->type() == QEvent::Leave && _panel) {
			_panel->otherLeave();
		} else if (event->type() == QEvent::Hide && _panel) {
			_panel->hideFast();
		} else if (event->type() == QEvent::KeyPress) {
			const auto key = static_cast<QKeyEvent*>(event.get())->key();
			if (key == Qt::Key_Space || key == Qt::Key_Return
				|| key == Qt::Key_Enter || key == Qt::Key_Down) {
				showPanel(true);
				return base::EventFilterResult::Cancel;
			}
		}
		return base::EventFilterResult::Continue;
	});
	base::install_event_filter(_panelParent, [=](not_null<QEvent*> event) {
		if (_panel && !_panel->isHidden()
			&& (event->type() == QEvent::Resize || event->type() == QEvent::Hide)) {
			_panel->hideAnimated();
		}
		return base::EventFilterResult::Continue;
	}, lifetime());
}

void SearchFiltersButton::watchPanelEvents() {
	base::install_event_filter(qApp, [=](not_null<QEvent*> event) {
		if (!_panel || _panel->isHidden()) {
			return base::EventFilterResult::Continue;
		}
		if (event->type() == QEvent::KeyPress
			&& static_cast<QKeyEvent*>(event.get())->key() == Qt::Key_Escape) {
			_panel->hideAnimated();
			return base::EventFilterResult::Cancel;
		} else if (event->type() == QEvent::MouseButtonPress) {
			const auto position = static_cast<QMouseEvent*>(event.get())
				->globalPosition().toPoint();
			if (!_panel->rect().contains(_panel->mapFromGlobal(position))
				&& !rect().contains(mapFromGlobal(position))) {
				_panel->hideAnimated();
			}
		}
		return base::EventFilterResult::Continue;
	}, _panelLifetime);
}

SearchFiltersButton::~SearchFiltersButton() {
	_panelLifetime.destroy();
	if (_panel) {
		_panel->setHiddenCallback(nullptr);
		_panel.destroy();
	}
}

void SearchFiltersButton::showPanel(bool focus) {
	if (!_panel) {
		_panel.create(_panelParent);
		const auto content = _panel->setOwnedWidget(
			object_ptr<::Ui::VerticalLayout>(_panel));
		const auto labels = std::array<rpl::producer<QString>, 5>{
			tr::lng_settings_search_main_and_archive(),
			tr::lng_settings_search_include_porn(),
			tr::lng_search_chinese_keywords(),
			tr::lng_settings_search_dialog_filter(),
			tr::lng_settings_search_exact_match(),
		};
		for (auto i = 0; i != int(kFields.size()); ++i) {
			const auto field = kFields[i];
			const auto check = content->add(
				object_ptr<::Ui::Checkbox>(content.data(), rpl::duplicate(labels[i])),
				style::margins(12, 8, 12, 8));
			check->setAllowTextLines(0);
			check->setFocusPolicy(Qt::StrongFocus);
			check->checkedChanges() | rpl::on_next([=](bool checked) {
				_draft.*field = checked;
			}, check->lifetime());
			_checks.push_back(check);
		}
		_panel->setHiddenCallback([=] { apply(); });
	}
	if (_panel->isHidden()) {
		_panelLifetime.destroy();
		watchPanelEvents();
		_draft = _original = EnhancedSettings::GetSearchOptions();
		for (auto i = 0; i != int(kFields.size()); ++i) {
			_checks[i]->setChecked(
				_draft.*kFields[i],
				::Ui::Checkbox::NotifyAboutChange::DontNotify);
		}
	}
	updatePanelGeometry();
	_panel->raise();
	_panel->otherEnter();
	if (focus && !_checks.empty()) {
		_panel->finishAnimating();
		_checks.front()->setFocus(Qt::OtherFocusReason);
	}
}

void SearchFiltersButton::updatePanelGeometry() {
	const auto position = _panelParent->mapFromGlobal(
		mapToGlobal(QPoint(0, height())));
	_panel->setMaxHeight(std::max(_panelParent->height() - position.y(), 1));
	_panel->resizeToWidth(
		std::min(st::columnMinimalWidthLeft, _panelParent->width()));
	_panel->move(
		std::clamp(
			position.x(),
			0,
			std::max(_panelParent->width() - _panel->width(), 0)),
		position.y());
}

void SearchFiltersButton::apply() {
	_panelLifetime.destroy();
	auto options = EnhancedSettings::GetSearchOptions();
	for (const auto field : kFields) {
		if (_draft.*field != _original.*field) {
			options.*field = _draft.*field;
		}
	}
	EnhancedSettings::SetSearchOptions(options);
}

} // namespace Dialogs::Ui
