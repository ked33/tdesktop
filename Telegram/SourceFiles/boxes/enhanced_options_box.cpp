/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include <facades.h>
#include <ui/toast/toast.h>
#include "boxes/enhanced_options_box.h"

#include "lang/lang_keys.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/style/style_core.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"
#include "ui/boxes/confirm_box.h"
#include "core/application.h"
#include "core/enhanced_settings.h"
#include "core/shortcuts.h"
#include "settings/settings_enhanced.h"

#include <QKeySequence>

#include <algorithm>
#include <array>

NetBoostBox::NetBoostBox(QWidget *parent) {
}

void NetBoostBox::prepare() {
	setTitle(tr::lng_settings_net_upload_speed_boost());

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	auto y = st::boxOptionListPadding.top();
	_description.create(
			this,
			tr::lng_net_speed_boost_desc(tr::now),
			st::boxLabel);
	_description->moveToLeft(st::boxPadding.left(), y);

	y += _description->height() + st::boxMediumSkip;

	_boostGroup = std::make_shared<Ui::RadiobuttonGroup>(GetEnhancedInt("net_speed_boost"));
	

	for (int i = 0; i <= 3; i++) {
		const auto button = Ui::CreateChild<Ui::Radiobutton>(
				this,
				_boostGroup,
				i,
				BoostLabel(i),
				st::autolockButton);
		button->moveToLeft(st::boxPadding.left(), y);
		y += button->heightNoMargins() + st::boxOptionListSkip;
	}
	showChildren();
	setDimensions(st::boxWidth, y);
}

QString NetBoostBox::BoostLabel(int boost) {
	switch (boost) {
		case 0:
			return tr::lng_net_speed_boost_default(tr::now);
		case 1:
			return tr::lng_net_speed_boost_slight(tr::now);
		case 2:
			return tr::lng_net_speed_boost_medium(tr::now);
		case 3:
			return tr::lng_net_speed_boost_big(tr::now);
		default:
			Unexpected("Boost in NetBoostBox::BoostLabel.");
	}
}

DownloadBoostBox::DownloadBoostBox(QWidget *parent) {
}

void DownloadBoostBox::prepare() {
	setTitle(tr::lng_settings_net_download_speed_boost());

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	auto y = st::boxOptionListPadding.top();
	_description.create(
			this,
			tr::lng_net_download_speed_boost_desc(tr::now),
			st::boxLabel);
	_description->moveToLeft(st::boxPadding.left(), y);

	y += _description->height() + st::boxMediumSkip;

	_boostGroup = std::make_shared<Ui::RadiobuttonGroup>(
		GetEnhancedInt("net_download_speed_boost"));

	for (int i = 0; i <= 6; i++) {
		const auto button = Ui::CreateChild<Ui::Radiobutton>(
				this,
				_boostGroup,
				i,
				BoostLabel(i),
				st::autolockButton);
		button->moveToLeft(st::boxPadding.left(), y);
		y += button->heightNoMargins() + st::boxOptionListSkip;
	}
	showChildren();
	setDimensions(st::boxWidth, y);
}

QString DownloadBoostBox::BoostLabel(int boost) {
	switch (boost) {
		case 0:
			return tr::lng_net_speed_boost_default(tr::now);
		case 1:
			return tr::lng_net_speed_boost_slight(tr::now);
		case 2:
			return tr::lng_net_speed_boost_medium(tr::now);
		case 3:
			return tr::lng_net_speed_boost_big(tr::now);
		case 4:
			return tr::lng_net_speed_boost_aggressive(tr::now);
		case 5:
			return tr::lng_net_speed_boost_extreme(tr::now);
		case 6:
			return tr::lng_net_speed_boost_smart(tr::now);
		default:
			Unexpected("Boost in DownloadBoostBox::BoostLabel.");
	}
}

void DownloadBoostBox::save() {
	const auto changeBoost = [=](Fn<void()> &&close) {
		SetDownloadBoost(_boostGroup->current());
		EnhancedSettings::Write();
		Core::Restart();
	};

	getDelegate()->show(
		Ui::MakeConfirmBox({
				.text = tr::lng_net_boost_restart_desc(tr::now),
				.confirmed = changeBoost,
				.confirmText = tr::lng_settings_restart_now(tr::now),
				.cancelText = tr::lng_cancel(tr::now),
		}));
}

DownloadBoostProfilesBox::DownloadBoostProfilesBox(QWidget *parent)
	: _profiles(Media::Streaming::LoadBoostProfiles())
, _scroll(base::make_unique_q<Ui::ScrollArea>(this, st::boxScroll)) {
}

void DownloadBoostProfilesBox::prepare() {
	setTitle(tr::lng_settings_online_playback_parameters_title());

	addButton(tr::lng_settings_online_playback_parameters_reset(), [=] {
		reset();
	});
	addButton(tr::lng_cancel(), [=] { closeBox(); });
	addButton(tr::lng_settings_save(), [=] {
		if (!save()) {
			return;
		}
		getDelegate()->show(Ui::MakeConfirmBox({
			.text = tr::lng_settings_online_playback_parameters_saved_restart(
				tr::now),
			.confirmed = [=](Fn<void()> &&) {
				EnhancedSettings::Write();
				Core::Restart();
			},
			.confirmText = tr::lng_settings_restart_now(tr::now),
			.cancelText = tr::lng_cancel(tr::now),
		}));
	});

	_profileGroup = std::make_shared<Ui::RadiobuttonGroup>(_editingProfile);
	const auto top = st::boxOptionListPadding.top();
	for (auto i = 0; i != 7; ++i) {
		const auto button = Ui::CreateChild<Ui::Radiobutton>(
			this,
			_profileGroup,
			i,
			DownloadBoostBox::BoostLabel(i),
			st::autolockButton);
		button->moveToLeft(
			st::boxPadding.left(),
			top + i * (button->heightNoMargins() + st::boxOptionListSkip));
		_radioHeight = button->heightNoMargins();
	}
	_profileGroup->setChangedCallback([=](int value) {
		if (_revertingProfile) {
			return;
		}
		if (!saveCurrentProfile()) {
			_revertingProfile = true;
			_profileGroup->setValue(_editingProfile);
			_revertingProfile = false;
			return;
		}
		_editingProfile = value;
		loadProfile(value);
	});

	_content = new Ui::VerticalLayout(this);
	_scroll->setOwnedWidget(object_ptr<Ui::VerticalLayout>::fromRaw(_content));
	_content->add(object_ptr<Ui::FlatLabel>(
		_content,
		tr::lng_settings_online_playback_parameters_desc(tr::now),
		st::boxLabel));

	const auto addField = [&](int index, const QString &description) {
		_content->add(object_ptr<Ui::FlatLabel>(
			_content,
			description,
			st::boxLabel));
		_fields[index] = _content->add(object_ptr<Ui::InputField>(
			_content,
			st::defaultInputField,
			tr::lng_settings_online_playback_integer_placeholder()));
	};
	addField(0, tr::lng_online_playback_profile_requests_limit(tr::now));
	addField(1, tr::lng_online_playback_profile_preload_parts(tr::now));
	addField(2, tr::lng_online_playback_profile_tail_prefetch_parts(tr::now));
	addField(3, tr::lng_online_playback_profile_seek_jump_parts(tr::now));
	addField(4, tr::lng_online_playback_profile_seek_guard_parts(tr::now));
	addField(5, tr::lng_online_playback_profile_load_ahead_ms(tr::now));
	addField(6, tr::lng_online_playback_profile_waiting_buffer_ms(tr::now));
	addField(7, tr::lng_online_playback_profile_start_waited_parts(tr::now));
	addField(8, tr::lng_online_playback_profile_max_waited_parts(tr::now));
	addField(9, tr::lng_online_playback_profile_start_sessions(tr::now));
	addField(10, tr::lng_online_playback_profile_max_sessions(tr::now));
	addField(11, tr::lng_online_playback_profile_mpv_tail_prefetch(tr::now));
	addField(12, tr::lng_online_playback_profile_mpv_cache_max(tr::now));
	addField(13, tr::lng_online_playback_profile_mpv_cache_back(tr::now));
	addField(14, tr::lng_online_playback_profile_nonpremium_preload(tr::now));
	addField(15, tr::lng_online_playback_profile_smart_min_preload(tr::now));
	addField(16, tr::lng_online_playback_profile_smart_min_requests(tr::now));
	addField(17, tr::lng_online_playback_profile_smart_max_preload(tr::now));
	addField(18, tr::lng_online_playback_profile_smart_dc_initial(tr::now));
	addField(19, tr::lng_online_playback_profile_smart_dc_min(tr::now));
	addField(20, tr::lng_online_playback_profile_smart_dc_max(tr::now));
	addField(21, tr::lng_online_playback_profile_smart_capacity_floor(tr::now));

	_seekCancel = _content->add(object_ptr<Ui::Checkbox>(
		_content,
		tr::lng_online_playback_profile_seek_cancel_enabled(tr::now),
		false,
		st::defaultBoxCheckbox));
	_tailPrefetch = _content->add(object_ptr<Ui::Checkbox>(
		_content,
		tr::lng_online_playback_profile_tail_prefetch_enabled(tr::now),
		false,
		st::defaultBoxCheckbox));

	loadProfile(_editingProfile);
	showChildren();
	setDimensions(
		st::onlinePlaybackProfilesWidth,
		st::onlinePlaybackProfilesHeight);
}

void DownloadBoostProfilesBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	const auto top = st::boxOptionListPadding.top();
	const auto radioHeight = 7 * (_radioHeight + st::boxOptionListSkip);
	_scroll->setGeometry(
		st::boxPadding.left(),
		top + radioHeight,
		width() - st::boxPadding.left() - st::boxPadding.right(),
		height() - top - radioHeight - st::boxPadding.bottom());
	if (_content) {
		_content->resizeToWidth(_scroll->width());
	}
}

void DownloadBoostProfilesBox::loadProfile(int profile) {
	const auto &value = _profiles[profile];
	const auto values = std::array<int, kNumericFieldCount>{
		value.requestsLimit,
		value.preloadPartsAhead,
		value.tailPrefetchParts,
		value.seekCancelJumpParts,
		value.seekCancelGuardParts,
		value.loadInAdvanceMs,
		value.waitingBufferMs,
		value.startWaitedParts,
		value.maxWaitedParts,
		value.startSessions,
		value.maxSessions,
		value.mpvTailPrefetchParts,
		value.mpvCacheMaxMb,
		value.mpvCacheBackMb,
		value.nonPremiumPreloadLimit,
		value.smartMinimumPreload,
		value.smartMinimumRequests,
		value.smartMaximumPreload,
		value.smartInitialRequestLimit,
		value.smartMinimumRequestLimit,
		value.smartMaximumRequestLimit,
		value.smartCapacityMinimumRequestLimit,
	};
	for (auto i = 0; i != kNumericFieldCount; ++i) {
		_fields[i]->setText(QString::number(values[i]));
	}
	_seekCancel->setChecked(
		value.seekCancelEnabled,
		Ui::Checkbox::NotifyAboutChange::DontNotify);
	_tailPrefetch->setChecked(
		value.tailPrefetchParts > 0,
		Ui::Checkbox::NotifyAboutChange::DontNotify);
}

bool DownloadBoostProfilesBox::saveCurrentProfile() {
	auto &value = _profiles[_editingProfile];
	const auto ranges = std::array<std::pair<int, int>, kNumericFieldCount>{
		std::pair{1, 32},
		std::pair{1, 64},
		std::pair{0, 16},
		std::pair{1, 256},
		std::pair{0, 64},
		std::pair{0, 300000},
		std::pair{0, 30000},
		std::pair{1, 128},
		std::pair{1, 256},
		std::pair{1, 32},
		std::pair{1, 64},
		std::pair{0, 16},
		std::pair{0, 4096},
		std::pair{0, 1024},
		std::pair{1, 128},
		std::pair{1, 64},
		std::pair{1, 32},
		std::pair{1, 64},
		std::pair{1, 32},
		std::pair{1, 32},
		std::pair{1, 32},
		std::pair{1, 32},
	};
	const auto current = std::array<int*, kNumericFieldCount>{
		&value.requestsLimit,
		&value.preloadPartsAhead,
		&value.tailPrefetchParts,
		&value.seekCancelJumpParts,
		&value.seekCancelGuardParts,
		&value.loadInAdvanceMs,
		&value.waitingBufferMs,
		&value.startWaitedParts,
		&value.maxWaitedParts,
		&value.startSessions,
		&value.maxSessions,
		&value.mpvTailPrefetchParts,
		&value.mpvCacheMaxMb,
		&value.mpvCacheBackMb,
		&value.nonPremiumPreloadLimit,
		&value.smartMinimumPreload,
		&value.smartMinimumRequests,
		&value.smartMaximumPreload,
		&value.smartInitialRequestLimit,
		&value.smartMinimumRequestLimit,
		&value.smartMaximumRequestLimit,
		&value.smartCapacityMinimumRequestLimit,
	};
	for (auto i = 0; i != kNumericFieldCount; ++i) {
		auto ok = false;
		const auto number = _fields[i]->getLastText().trimmed().toInt(&ok);
		if (!ok || number < ranges[i].first || number > ranges[i].second) {
			Ui::Toast::Show(
				tr::lng_online_playback_profile_invalid_integer(tr::now));
			_fields[i]->showError();
			return false;
		}
		*current[i] = number;
	}
	value.seekCancelEnabled = _seekCancel->checked();
	if (value.maxWaitedParts < value.startWaitedParts
		|| value.maxSessions < value.startSessions
		|| value.mpvCacheBackMb > value.mpvCacheMaxMb
		|| value.smartMaximumPreload < value.smartMinimumPreload
		|| value.smartMaximumRequestLimit
			< value.smartMinimumRequestLimit
		|| value.smartInitialRequestLimit
			< value.smartMinimumRequestLimit
		|| value.smartInitialRequestLimit
			> value.smartMaximumRequestLimit
		|| value.smartCapacityMinimumRequestLimit
			< value.smartMinimumRequestLimit
		|| value.smartCapacityMinimumRequestLimit
			> value.smartMaximumRequestLimit) {
		Ui::Toast::Show(
			tr::lng_online_playback_profile_invalid_relations(tr::now));
		return false;
	}
	if (_tailPrefetch->checked() && value.tailPrefetchParts == 0) {
		value.tailPrefetchParts = 1;
	}
	if (!_tailPrefetch->checked()) {
		value.tailPrefetchParts = 0;
	}
	return true;
}

bool DownloadBoostProfilesBox::save() {
	if (!saveCurrentProfile()) {
		return false;
	}
	SetEnhancedValue(
		"net_download_speed_boost_profiles",
		Media::Streaming::SerializeBoostProfiles(_profiles));
	EnhancedSettings::Write();
	return true;
}

void DownloadBoostProfilesBox::reset() {
	_profiles[_editingProfile]
		= Media::Streaming::DefaultBoostProfiles()[_editingProfile];
	loadProfile(_editingProfile);
}

void NetBoostBox::save() {
	const auto changeBoost = [=](Fn<void()> &&close) {
		SetNetworkBoost(_boostGroup->current());
		EnhancedSettings::Write();
		Core::Restart();
	};

	getDelegate()->show(
		Ui::MakeConfirmBox({
				.text = tr::lng_net_boost_restart_desc(tr::now),
				.confirmed = changeBoost,
				.confirmText = tr::lng_settings_restart_now(tr::now),
				.cancelText = tr::lng_cancel(tr::now),
		}));
}

AlwaysDeleteBox::AlwaysDeleteBox(QWidget *parent) {
}

void AlwaysDeleteBox::prepare() {
	setTitle(tr::lng_settings_always_delete_for());

	addButton(tr::lng_box_ok(), [=] { closeBox(); });

	auto y = st::boxOptionListPadding.top();
	_optionGroup = std::make_shared<Ui::RadiobuttonGroup>(GetEnhancedInt("always_delete_for"));

	for (int i = 0; i <= 3; i++) {
		const auto button = Ui::CreateChild<Ui::Radiobutton>(
				this,
				_optionGroup,
				i,
				DeleteLabel(i),
				st::autolockButton);
		button->moveToLeft(st::boxPadding.left(), y);
		y += button->heightNoMargins() + st::boxOptionListSkip;
	}
	_optionGroup->setChangedCallback([=](int value) { save(); });
	setDimensions(st::boxWidth, y);
}

QString AlwaysDeleteBox::DeleteLabel(int boost) {
	switch (boost) {
		case 0:
			return tr::lng_settings_delete_disabled(tr::now);
		case 1:
			return tr::lng_settings_delete_for_group(tr::now);
		case 2:
			return tr::lng_settings_delete_for_person(tr::now);
		case 3:
			return tr::lng_settings_delete_for_both(tr::now);
		default:
			Unexpected("Delete in AlwaysDeleteBox::DeleteLabel.");
	}
}

void AlwaysDeleteBox::save() {
	SetEnhancedValue("always_delete_for", _optionGroup->current());
	EnhancedSettings::Write();
	closeBox();
}

RadioController::RadioController(QWidget *parent)
		: _url(this, st::defaultInputField, tr::lng_formatting_link_url()) {
}

void RadioController::prepare() {
	setTitle(tr::lng_settings_radio_controller());

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	_url->setText(GetEnhancedString("radio_controller"));

	setDimensions(st::boxWidth, _url->height());
}

void RadioController::setInnerFocus() {
	_url->setFocusFast();
}

void RadioController::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);

	int32 w = st::boxWidth - st::boxPadding.left() - st::boxPadding.right();
	_url->resize(w, _url->height());
	_url->moveToLeft(st::boxPadding.left(), 0);
}

void RadioController::save() {
	auto host = _url->getLastText().trimmed();
	if (host == "") {
		host = "http://localhost:2468";
	}
	SetEnhancedValue("radio_controller", host);
	EnhancedSettings::Write();
	closeBox();
}

MpvPathBox::MpvPathBox(QWidget *parent)
	: _path(this, st::defaultInputField, tr::lng_settings_mpv_path_placeholder()) {
}

void MpvPathBox::prepare() {
	setTitle(tr::lng_settings_mpv_path());

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	_path->setText(GetEnhancedString("mpv_path"));

	setDimensions(st::boxWidth, _path->height());
}

void MpvPathBox::setInnerFocus() {
	_path->setFocusFast();
}

void MpvPathBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);

	const auto width = st::boxWidth
		- st::boxPadding.left()
		- st::boxPadding.right();
	_path->resize(width, _path->height());
	_path->moveToLeft(st::boxPadding.left(), 0);
}

void MpvPathBox::save() {
	SetEnhancedValue("mpv_path", _path->getLastText().trimmed());
	EnhancedSettings::Write();
	closeBox();
}

FloodPremiumWaitBox::FloodPremiumWaitBox(QWidget *parent)
	: _delay(
		this,
		st::defaultInputField,
		tr::lng_settings_flood_premium_wait_placeholder()) {
}

QString FloodPremiumWaitBox::DelayLabel(const QString &value) {
	const auto trimmed = value.trimmed();
	return trimmed.isEmpty()
		? tr::lng_settings_flood_premium_wait_default(tr::now)
		: QString("%1 ms").arg(trimmed);
}

void FloodPremiumWaitBox::prepare() {
	setTitle(tr::lng_settings_flood_premium_wait_title());

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	_delay->setText(GetEnhancedString("flood_premium_wait_override_ms"));
	_delay->setMaxLength(12);

	setDimensions(st::boxWidth, _delay->height());
}

void FloodPremiumWaitBox::setInnerFocus() {
	_delay->setFocusFast();
}

void FloodPremiumWaitBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);

	const auto width = st::boxWidth
		- st::boxPadding.left()
		- st::boxPadding.right();
	_delay->resize(width, _delay->height());
	_delay->moveToLeft(st::boxPadding.left(), 0);
}

void FloodPremiumWaitBox::save() {
	const auto value = _delay->getLastText().trimmed();
	if (!value.isEmpty()) {
		auto ok = false;
		value.toInt(&ok);
		if (!ok) {
			Ui::Toast::Show(tr::lng_settings_flood_premium_wait_invalid(tr::now));
			return;
		}
	}
	SetEnhancedValue("flood_premium_wait_override_ms", value);
	EnhancedSettings::Write();
	closeBox();
}

QuickCopyTargetsBox::QuickCopyTargetsBox(QWidget *parent)
	: _targets(
		this,
		st::defaultInputField,
		tr::lng_settings_quick_copy_targets_placeholder()) {
}

QString QuickCopyTargetsBox::TargetsLabel(const QString &value) {
	const auto trimmed = value.trimmed();
	return trimmed.isEmpty()
		? tr::lng_settings_quick_copy_targets_disabled(tr::now)
		: trimmed;
}

void QuickCopyTargetsBox::prepare() {
	setTitle(tr::lng_settings_quick_copy_targets_title());

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	_targets->setText(GetEnhancedString("quick_copy_targets"));
	_targets->setMaxLength(4096);

	setDimensions(st::boxWidth, _targets->height());
}

void QuickCopyTargetsBox::setInnerFocus() {
	_targets->setFocusFast();
}

void QuickCopyTargetsBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);

	const auto width = st::boxWidth
		- st::boxPadding.left()
		- st::boxPadding.right();
	_targets->resize(width, _targets->height());
	_targets->moveToLeft(st::boxPadding.left(), 0);
}

void QuickCopyTargetsBox::save() {
	SetEnhancedValue("quick_copy_targets", _targets->getLastText().trimmed());
	EnhancedSettings::Write();
	closeBox();
}

CustomChatShortcutsBox::CustomChatShortcutsBox(QWidget *parent)
	: _shortcuts(
		this,
		st::defaultInputField,
		tr::lng_settings_custom_chat_shortcuts_placeholder()) {
}

QString CustomChatShortcutsBox::ShortcutsLabel(const QString &value) {
	const auto trimmed = value.trimmed();
	return trimmed.isEmpty()
		? tr::lng_settings_chat_switch_shortcut_disabled(tr::now)
		: trimmed;
}

void CustomChatShortcutsBox::prepare() {
	setTitle(tr::lng_settings_custom_chat_shortcuts_title());

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	_shortcuts->setText(GetEnhancedString("custom_chat_shortcuts"));
	_shortcuts->setMaxLength(4096);

	setDimensions(st::boxWidth, _shortcuts->height());
}

void CustomChatShortcutsBox::setInnerFocus() {
	_shortcuts->setFocusFast();
}

void CustomChatShortcutsBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);

	const auto width = st::boxWidth
		- st::boxPadding.left()
		- st::boxPadding.right();
	_shortcuts->resize(width, _shortcuts->height());
	_shortcuts->moveToLeft(st::boxPadding.left(), 0);
}

void CustomChatShortcutsBox::save() {
	SetEnhancedValue(
		"custom_chat_shortcuts",
		_shortcuts->getLastText().trimmed());
	Shortcuts::ReloadCustomChatShortcuts();
	EnhancedSettings::Write();
	closeBox();
}

NoForwardsBadgeColorBox::NoForwardsBadgeColorBox(QWidget *parent)
	: _color(
		this,
		st::defaultInputField,
		tr::lng_settings_no_forwards_badge_color_placeholder()) {
}

QString NoForwardsBadgeColorBox::ColorLabel(const QString &value) {
	const auto trimmed = value.trimmed();
	return trimmed.isEmpty() ? QString("#ecbb71") : trimmed;
}

void NoForwardsBadgeColorBox::prepare() {
	setTitle(tr::lng_settings_no_forwards_badge_color());

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	_color->setText(GetEnhancedString("no_forwards_badge_color"));
	_color->setMaxLength(7);

	setDimensions(st::boxWidth, _color->height());
}

void NoForwardsBadgeColorBox::setInnerFocus() {
	_color->setFocusFast();
}

void NoForwardsBadgeColorBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);

	const auto width = st::boxWidth
		- st::boxPadding.left()
		- st::boxPadding.right();
	_color->resize(width, _color->height());
	_color->moveToLeft(st::boxPadding.left(), 0);
}

void NoForwardsBadgeColorBox::save() {
	const auto colorText = _color->getLastText().trimmed();
	if (!colorText.isEmpty() && !QColor::isValidColor(colorText)) {
		Ui::Toast::Show(tr::lng_settings_no_forwards_badge_color_invalid(tr::now));
		return;
	}
	SetEnhancedValue("no_forwards_badge_color", colorText.isEmpty() ? QString("#ecbb71") : colorText);
	EnhancedSettings::Write();
	closeBox();
}

CodeBlockBgColorBox::CodeBlockBgColorBox(QWidget *parent)
	: _color(
		this,
		st::defaultInputField,
		tr::lng_settings_code_block_bg_color_placeholder()) {
}

QString CodeBlockBgColorBox::ColorLabel(const QString &value) {
	const auto trimmed = value.trimmed();
	return trimmed.isEmpty() ? QString("#495a7b") : trimmed;
}

void CodeBlockBgColorBox::prepare() {
	setTitle(tr::lng_settings_code_block_bg_color());

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	_color->setText(GetEnhancedString("code_block_bg_color"));
	_color->setMaxLength(7);

	setDimensions(st::boxWidth, _color->height());
}

void CodeBlockBgColorBox::setInnerFocus() {
	_color->setFocusFast();
}

void CodeBlockBgColorBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);

	const auto width = st::boxWidth
		- st::boxPadding.left()
		- st::boxPadding.right();
	_color->resize(width, _color->height());
	_color->moveToLeft(st::boxPadding.left(), 0);
}

void CodeBlockBgColorBox::save() {
	const auto colorText = _color->getLastText().trimmed();
	if (!colorText.isEmpty() && !QColor::isValidColor(colorText)) {
		Ui::Toast::Show(tr::lng_settings_code_block_bg_color_invalid(tr::now));
		return;
	}
	SetEnhancedValue("code_block_bg_color", ColorLabel(colorText));
	EnhancedSettings::Write();
	style::NotifyPaletteChanged();
	closeBox();
}

SearchMessageHighlightBgColorBox::SearchMessageHighlightBgColorBox(
		QWidget *parent)
: _color(
	this,
	st::defaultInputField,
	tr::lng_settings_search_message_highlight_bg_color_placeholder()) {
}

QString SearchMessageHighlightBgColorBox::ColorLabel(const QString &value) {
	const auto trimmed = value.trimmed();
	return trimmed.isEmpty() ? QString("#3482d555") : trimmed;
}

void SearchMessageHighlightBgColorBox::prepare() {
	setTitle(tr::lng_settings_search_message_highlight_bg_color());

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	_color->setText(GetEnhancedString("search_message_highlight_bg_color"));
	_color->setMaxLength(9);

	setDimensions(st::boxWidth, _color->height());
}

void SearchMessageHighlightBgColorBox::setInnerFocus() {
	_color->setFocusFast();
}

void SearchMessageHighlightBgColorBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);

	const auto width = st::boxWidth
		- st::boxPadding.left()
		- st::boxPadding.right();
	_color->resize(width, _color->height());
	_color->moveToLeft(st::boxPadding.left(), 0);
}

void SearchMessageHighlightBgColorBox::save() {
	const auto colorText = _color->getLastText().trimmed();
	const auto validLength = (colorText.size() == 7 || colorText.size() == 9);
	auto ok = false;
	if (!colorText.isEmpty() && colorText.startsWith('#') && validLength) {
		colorText.mid(1).toUInt(&ok, 16);
	}
	if (!colorText.isEmpty()
		&& (!colorText.startsWith('#')
		|| !validLength
		|| !ok)) {
		Ui::Toast::Show(
			tr::lng_settings_search_message_highlight_bg_color_invalid(
				tr::now));
		return;
	}
	SetEnhancedValue(
		"search_message_highlight_bg_color",
		ColorLabel(colorText));
	EnhancedSettings::Write();
	closeBox();
}

ChatSwitchShortcutBox::ChatSwitchShortcutBox(QWidget *parent)
: ChatSwitchShortcutBox(
	parent,
	u"chat_switch_persistent_shortcut"_q,
	tr::lng_settings_chat_switch_shortcut_title(),
	tr::lng_settings_chat_switch_shortcut_placeholder(),
	[] { return tr::lng_settings_chat_switch_shortcut_invalid(tr::now); }) {
}

ChatSwitchShortcutBox::ChatSwitchShortcutBox(
	QWidget*,
	QString key,
	rpl::producer<QString> title,
	rpl::producer<QString> placeholder,
	Fn<QString()> invalidToast)
: _shortcut(
		this,
		st::defaultInputField,
		std::move(placeholder))
, _key(std::move(key))
, _title(std::move(title))
, _invalidToast(std::move(invalidToast)) {
}

QString ChatSwitchShortcutBox::ShortcutLabel(const QString &value) {
	const auto trimmed = value.trimmed();
	if (trimmed.isEmpty()) {
		return tr::lng_settings_chat_switch_shortcut_disabled(tr::now);
	}
	const auto sequence = QKeySequence(trimmed, QKeySequence::PortableText);
	return sequence.isEmpty()
		? trimmed
		: sequence.toString(QKeySequence::NativeText);
}

void ChatSwitchShortcutBox::prepare() {
	setTitle(std::move(_title));

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	_shortcut->setText(GetEnhancedString(_key));
	_shortcut->setMaxLength(64);

	setDimensions(st::boxWidth, _shortcut->height());
}

void ChatSwitchShortcutBox::setInnerFocus() {
	_shortcut->setFocusFast();
}

void ChatSwitchShortcutBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);

	const auto width = st::boxWidth
		- st::boxPadding.left()
		- st::boxPadding.right();
	_shortcut->resize(width, _shortcut->height());
	_shortcut->moveToLeft(st::boxPadding.left(), 0);
}

void ChatSwitchShortcutBox::save() {
	const auto value = _shortcut->getLastText().trimmed();
	auto stored = QString();
	if (!value.isEmpty()) {
		const auto sequence = QKeySequence(value, QKeySequence::PortableText);
		if (sequence.isEmpty()) {
			Ui::Toast::Show(_invalidToast());
			return;
		}
		stored = sequence.toString(QKeySequence::PortableText).toLower();
	}
	SetEnhancedValue(_key, stored);
	EnhancedSettings::Write();
	closeBox();
}

BitrateController::BitrateController(QWidget *parent) {
}

void BitrateController::prepare() {
	setTitle(tr::lng_bitrate_controller());

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	auto y = st::boxOptionListPadding.top();
	_description.create(
			this,
			tr::lng_bitrate_controller_desc(tr::now),
			st::boxLabel);
	_description->moveToLeft(st::boxPadding.left(), y);

	y += _description->height() + st::boxMediumSkip;

	_bitrateGroup = std::make_shared<Ui::RadiobuttonGroup>(GetEnhancedInt("bitrate"));

	for (int i = 0; i <= 7; i++) {
		const auto button = Ui::CreateChild<Ui::Radiobutton>(
				this,
				_bitrateGroup,
				i,
				BitrateLabel(i),
				st::autolockButton);
		button->moveToLeft(st::boxPadding.left(), y);
		y += button->heightNoMargins() + st::boxOptionListSkip;
	}
	showChildren();
	setDimensions(st::boxWidth, y);
}

QString BitrateController::BitrateLabel(int boost) {
	switch (boost) {
		case 0:
			return tr::lng_bitrate_controller_default(tr::now);
		case 1:
			return tr::lng_bitrate_controller_64k(tr::now);
		case 2:
			return tr::lng_bitrate_controller_96k(tr::now);
		case 3:
			return tr::lng_bitrate_controller_128k(tr::now);
		case 4:
			return tr::lng_bitrate_controller_160k(tr::now);
		case 5:
			return tr::lng_bitrate_controller_192k(tr::now);
		case 6:
			return tr::lng_bitrate_controller_256k(tr::now);
		case 7:
			return tr::lng_bitrate_controller_320k(tr::now);
		default:
			Unexpected("Bitrate not found.");
	}
}

void BitrateController::save() {
	SetEnhancedValue("bitrate", _bitrateGroup->current());
	EnhancedSettings::Write();
	Ui::Toast::Show(tr::lng_bitrate_controller_hint(tr::now));
	closeBox();
}

PreviewBrightnessBox::PreviewBrightnessBox(QWidget *parent) {
}

void PreviewBrightnessBox::prepare() {
	setTitle(tr::lng_settings_preview_brightness_title());

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	auto y = st::boxOptionListPadding.top();
	_description.create(
		this,
		tr::lng_settings_preview_brightness_desc(tr::now),
		st::boxLabel);
	_description->moveToLeft(st::boxPadding.left(), y);

	y += _description->height() + st::boxMediumSkip;

	const auto current = GetEnhancedInt("preview_brightness");
	const auto initial = (current >= 10 && current <= 100)
		? ((current / 10) * 10)
		: 70;
	_brightnessGroup = std::make_shared<Ui::RadiobuttonGroup>(initial);

	for (auto percent = 100; percent >= 10; percent -= 10) {
		const auto button = Ui::CreateChild<Ui::Radiobutton>(
			this,
			_brightnessGroup,
			percent,
			BrightnessLabel(percent),
			st::autolockButton);
		button->moveToLeft(st::boxPadding.left(), y);
		y += button->heightNoMargins() + st::boxOptionListSkip;
	}
	showChildren();
	setDimensions(st::boxWidth, y);
}

QString PreviewBrightnessBox::BrightnessLabel(int percent) {
	return tr::lng_settings_preview_brightness_percent(
		tr::now,
		lt_percent,
		QString::number(percent));
}

void PreviewBrightnessBox::save() {
	SetEnhancedValue("preview_brightness", _brightnessGroup->current());
	EnhancedSettings::Write();
	closeBox();
}
