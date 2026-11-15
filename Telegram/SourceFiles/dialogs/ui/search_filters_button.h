/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/enhanced_settings.h"
#include "ui/widgets/buttons.h"
#include "base/object_ptr.h"

#include <vector>

namespace Ui {
class Checkbox;
class InnerDropdown;
} // namespace Ui

namespace Dialogs::Ui {

using namespace ::Ui;

class SearchFiltersButton final : public ::Ui::IconButton {
public:
	SearchFiltersButton(QWidget *parent, QWidget *panelParent);
	~SearchFiltersButton();

private:
	void showPanel(bool focus);
	void watchPanelEvents();
	void updatePanelGeometry();
	void apply();

	QWidget *const _panelParent;
	object_ptr<::Ui::InnerDropdown> _panel = { nullptr };
	EnhancedSettings::SearchOptions _draft;
	EnhancedSettings::SearchOptions _original;
	std::vector<not_null<::Ui::Checkbox*>> _checks;
	rpl::lifetime _panelLifetime;

};

} // namespace Dialogs::Ui
