/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include <facades.h>
#include <ui/toast/toast.h>
#include "boxes/enhanced_options_box.h"
#include "core/message_folding.h"

#include "lang/lang_keys.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/style/style_core.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"
#include "styles/style_passcode_box.h"
#include "styles/style_widgets.h"
#include "ui/boxes/confirm_box.h"
#include "core/application.h"
#include "core/enhanced_settings.h"
#include "core/shortcuts.h"
#include "settings/settings_enhanced.h"

#include <QKeySequence>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr auto kDownloadRateFieldIndex = 22;

[[nodiscard]] QString PixelRangeLabel(int minimum, int maximum) {
	return u"%1-%2 px"_q.arg(minimum).arg(maximum);
}

} // namespace

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
, _scroll(base::make_unique_q<Ui::ScrollArea>(this, st::boxScroll))
, _editingProfile(std::clamp(
	GetEnhancedInt("net_download_speed_boost"),
	0,
	int(_profiles.size()) - 1)) {
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
		st::onlinePlaybackProfilesAbout));
	Ui::AddSkip(_content, st::onlinePlaybackProfilesSectionSkip);

	const auto addField = [&](
			not_null<Ui::VerticalLayout*> container,
			int index,
			const QString &title,
			const QString &about) {
		container->add(object_ptr<Ui::FlatLabel>(
			container,
			title,
			st::onlinePlaybackProfilesTitle));
		Ui::AddSkip(container, st::onlinePlaybackProfilesTitleSkip);
		container->add(object_ptr<Ui::FlatLabel>(
			container,
			about,
			st::onlinePlaybackProfilesAbout));
		Ui::AddSkip(container, st::onlinePlaybackProfilesFieldSkip);
		const auto row = container->add(object_ptr<Ui::RpWidget>(container));
		const auto field = Ui::CreateChild<Ui::InputField>(
			row,
			st::onlinePlaybackProfilesField);
		_fields[index] = field;
		const auto label = Ui::CreateChild<Ui::FlatLabel>(
			row,
			QString(),
			st::defaultInputFieldLimit);
		label->setAttribute(Qt::WA_TransparentForMouseEvents);
		_defaultLabels[index] = label;
		rpl::combine(
			row->widthValue(),
			field->heightValue(),
			label->naturalWidthValue()
		) | rpl::on_next([=](int width, int fieldHeight, int) {
			if (field->width() != st::onlinePlaybackProfilesFieldWidth) {
				field->resizeToWidth(st::onlinePlaybackProfilesFieldWidth);
			}
			field->moveToLeft(0, 0);
			const auto labelLeft = field->width()
				+ st::onlinePlaybackProfilesDefaultSkip;
			label->resizeToNaturalWidth(std::max(width - labelLeft, 0));
			label->moveToLeft(
				labelLeft,
				std::max((fieldHeight - label->height()) / 2, 0));
			row->resize(
				width,
				std::max(fieldHeight, label->y() + label->height()));
		}, row->lifetime());
		Ui::AddSkip(container, st::onlinePlaybackProfilesItemSkip);
	};
	const auto addCheck = [&](
			not_null<Ui::VerticalLayout*> container,
			Ui::Checkbox *&store,
			const QString &title,
			const QString &about) {
		store = container->add(object_ptr<Ui::Checkbox>(
			container,
			title,
			false,
			st::defaultBoxCheckbox));
		store->setAllowTextLines();
		Ui::AddSkip(container, st::onlinePlaybackProfilesTitleSkip);
		container->add(object_ptr<Ui::FlatLabel>(
			container,
			about,
			st::onlinePlaybackProfilesAbout));
		Ui::AddSkip(container, st::onlinePlaybackProfilesItemSkip);
	};
	Ui::AddSubsectionTitle(
		_content,
		tr::lng_online_playback_group_playback_reader());
	addField(
		_content,
		0,
		tr::lng_online_playback_profile_requests_limit(tr::now),
		tr::lng_online_playback_profile_requests_limit_about(tr::now));
	addField(
		_content,
		1,
		tr::lng_online_playback_profile_preload_parts(tr::now),
		tr::lng_online_playback_profile_preload_parts_about(tr::now));
	addField(
		_content,
		2,
		tr::lng_online_playback_profile_tail_prefetch_parts(tr::now),
		tr::lng_online_playback_profile_tail_prefetch_parts_about(tr::now));
	addField(
		_content,
		3,
		tr::lng_online_playback_profile_seek_jump_parts(tr::now),
		tr::lng_online_playback_profile_seek_jump_parts_about(tr::now));
	addField(
		_content,
		4,
		tr::lng_online_playback_profile_seek_guard_parts(tr::now),
		tr::lng_online_playback_profile_seek_guard_parts_about(tr::now));
	addField(
		_content,
		5,
		tr::lng_online_playback_profile_load_ahead_ms(tr::now),
		tr::lng_online_playback_profile_load_ahead_ms_about(tr::now));
	addField(
		_content,
		6,
		tr::lng_online_playback_profile_waiting_buffer_ms(tr::now),
		tr::lng_online_playback_profile_waiting_buffer_ms_about(tr::now));
	addCheck(
		_content,
		_seekCancel,
		tr::lng_online_playback_profile_seek_cancel_enabled(tr::now),
		tr::lng_online_playback_profile_seek_cancel_enabled_about(tr::now));
	addCheck(
		_content,
		_tailPrefetch,
		tr::lng_online_playback_profile_tail_prefetch_enabled(tr::now),
		tr::lng_online_playback_profile_tail_prefetch_enabled_about(tr::now));

	Ui::AddSkip(_content, st::onlinePlaybackProfilesSectionSkip);
	Ui::AddSubsectionTitle(
		_content,
		tr::lng_online_playback_group_download_mpv());
	addField(
		_content,
		7,
		tr::lng_online_playback_profile_start_waited_parts(tr::now),
		tr::lng_online_playback_profile_start_waited_parts_about(tr::now));
	addField(
		_content,
		8,
		tr::lng_online_playback_profile_max_waited_parts(tr::now),
		tr::lng_online_playback_profile_max_waited_parts_about(tr::now));
	addField(
		_content,
		9,
		tr::lng_online_playback_profile_start_sessions(tr::now),
		tr::lng_online_playback_profile_start_sessions_about(tr::now));
	addField(
		_content,
		10,
		tr::lng_online_playback_profile_max_sessions(tr::now),
		tr::lng_online_playback_profile_max_sessions_about(tr::now));
	addField(
		_content,
		11,
		tr::lng_online_playback_profile_mpv_tail_prefetch(tr::now),
		tr::lng_online_playback_profile_mpv_tail_prefetch_about(tr::now));
	addField(
		_content,
		12,
		tr::lng_online_playback_profile_mpv_cache_max(tr::now),
		tr::lng_online_playback_profile_mpv_cache_max_about(tr::now));
	addField(
		_content,
		13,
		tr::lng_online_playback_profile_mpv_cache_back(tr::now),
		tr::lng_online_playback_profile_mpv_cache_back_about(tr::now));

	_smartSection = _content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			_content,
			object_ptr<Ui::VerticalLayout>(_content)));
	const auto smartContent = _smartSection->entity();
	Ui::AddSkip(smartContent, st::onlinePlaybackProfilesSectionSkip);
	Ui::AddSubsectionTitle(
		smartContent,
		tr::lng_online_playback_group_smart());
	smartContent->add(object_ptr<Ui::FlatLabel>(
		smartContent,
		tr::lng_settings_online_playback_smart_desc(tr::now),
		st::onlinePlaybackProfilesAbout));
	Ui::AddSkip(smartContent, st::onlinePlaybackProfilesItemSkip);
	addField(
		smartContent,
		14,
		tr::lng_online_playback_profile_nonpremium_preload(tr::now),
		tr::lng_online_playback_profile_nonpremium_preload_about(tr::now));
	addField(
		smartContent,
		15,
		tr::lng_online_playback_profile_smart_min_preload(tr::now),
		tr::lng_online_playback_profile_smart_min_preload_about(tr::now));
	addField(
		smartContent,
		16,
		tr::lng_online_playback_profile_smart_min_requests(tr::now),
		tr::lng_online_playback_profile_smart_min_requests_about(tr::now));
	addField(
		smartContent,
		17,
		tr::lng_online_playback_profile_smart_max_preload(tr::now),
		tr::lng_online_playback_profile_smart_max_preload_about(tr::now));
	addField(
		smartContent,
		18,
		tr::lng_online_playback_profile_smart_dc_initial(tr::now),
		tr::lng_online_playback_profile_smart_dc_initial_about(tr::now));
	addField(
		smartContent,
		19,
		tr::lng_online_playback_profile_smart_dc_min(tr::now),
		tr::lng_online_playback_profile_smart_dc_min_about(tr::now));
	addField(
		smartContent,
		20,
		tr::lng_online_playback_profile_smart_dc_max(tr::now),
		tr::lng_online_playback_profile_smart_dc_max_about(tr::now));
	addField(
		smartContent,
		21,
		tr::lng_online_playback_profile_smart_capacity_floor(tr::now),
		tr::lng_online_playback_profile_smart_capacity_floor_about(tr::now));
	addField(
		smartContent,
		kDownloadRateFieldIndex,
		tr::lng_online_playback_profile_smart_download_rate(tr::now),
		tr::lng_online_playback_profile_smart_download_rate_about(tr::now));
	addField(
		smartContent,
		23,
		tr::lng_online_playback_profile_smart_download_burst(tr::now),
		tr::lng_online_playback_profile_smart_download_burst_about(tr::now));
	addCheck(
		smartContent,
		_adaptivePacing,
		tr::lng_online_playback_profile_adaptive_pacing(tr::now),
		tr::lng_online_playback_profile_adaptive_pacing_about(tr::now));

	loadProfile(_editingProfile);
	showChildren();
	const auto outer = getDelegate()->outerContainer();
	const auto availableWidth = (outer
		&& outer->width() > 2 * st::onlinePlaybackProfilesOuterSkip)
		? std::max(
			outer->width() - 2 * st::onlinePlaybackProfilesOuterSkip,
			st::boxWidth)
		: st::onlinePlaybackProfilesPreferredWidth;
	setDimensions(
		std::min(st::onlinePlaybackProfilesPreferredWidth, availableWidth),
		st::onlinePlaybackProfilesMaximumHeight);
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
		const auto margins = _content->getMargins();
		_content->resizeToWidth(
			_scroll->width() - margins.left() - margins.right());
	}
}

auto DownloadBoostProfilesBox::NumericFieldValues(
		const Media::Streaming::BoostProfile &value)
-> std::array<int, kNumericFieldCount> {
	return {
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
		value.smartDownloadMaxKiBps,
		value.smartDownloadBurstParts,
	};
}

void DownloadBoostProfilesBox::loadProfile(int profile) {
	const auto &value = _profiles[profile];
	const auto values = NumericFieldValues(value);
	const auto defaults = NumericFieldValues(
		Media::Streaming::DefaultBoostProfiles()[profile]);
	for (auto i = 0; i != kNumericFieldCount; ++i) {
		const auto text = [&](int number) {
			return (i == kDownloadRateFieldIndex)
				? QString::number(number * 1024. / 1'000'000., 'g', 12)
				: QString::number(number);
		};
		_fields[i]->setText(text(values[i]));
		_defaultLabels[i]->setText(tr::lng_online_playback_profile_default(
			tr::now,
			lt_value,
			text(defaults[i])));
	}
	_seekCancel->setChecked(
		value.seekCancelEnabled,
		Ui::Checkbox::NotifyAboutChange::DontNotify);
	_tailPrefetch->setChecked(
		value.tailPrefetchParts > 0,
		Ui::Checkbox::NotifyAboutChange::DontNotify);
	_adaptivePacing->setChecked(
		value.smartAdaptivePacing,
		Ui::Checkbox::NotifyAboutChange::DontNotify);
	_smartSection->toggle(profile == 6, anim::type::instant);
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
		std::pair{0, 65536},
		std::pair{0, 50000},
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
		&value.smartDownloadMaxKiBps,
		&value.smartDownloadBurstParts,
	};
	const auto showError = [&](
			int primary,
			int related,
			const QString &message) {
		Ui::Toast::Show(message);
		_fields[primary]->showError();
		if (related >= 0) {
			_fields[related]->showErrorNoFocus();
		}
		_scroll->scrollToWidget(_fields[primary]);
		return false;
	};
	for (const auto field : _fields) {
		field->hideError();
	}
	for (auto i = 0; i != kNumericFieldCount; ++i) {
		auto ok = false;
		const auto text = _fields[i]->getLastText().trimmed();
		if (i == kDownloadRateFieldIndex) {
			const auto rate = text.toDouble(&ok);
			const auto maximum = ranges[i].second * 1024. / 1'000'000.;
			if (!ok
				|| !std::isfinite(rate)
				|| rate < 0.
				|| (rate > 0. && rate < 1024. / 1'000'000.)
				|| rate > maximum) {
				return showError(
					i,
					-1,
					tr::lng_online_playback_profile_invalid_rate(tr::now));
			}
			*current[i] = int(std::llround(rate * 1'000'000.) / 1024);
			continue;
		}
		const auto number = text.toInt(&ok);
		if (!ok || number < ranges[i].first || number > ranges[i].second) {
			return showError(
				i,
				-1,
				tr::lng_online_playback_profile_invalid_integer(tr::now));
		}
		*current[i] = number;
	}
	value.seekCancelEnabled = _seekCancel->checked();
	value.smartAdaptivePacing = _adaptivePacing->checked();
	if (value.maxWaitedParts < value.startWaitedParts) {
		return showError(
			8,
			7,
			tr::lng_online_playback_error_waited_parts(tr::now));
	}
	if (value.maxSessions < value.startSessions) {
		return showError(
			10,
			9,
			tr::lng_online_playback_error_sessions(tr::now));
	}
	if (value.mpvCacheBackMb > value.mpvCacheMaxMb) {
		return showError(
			13,
			12,
			tr::lng_online_playback_error_mpv_cache(tr::now));
	}
	if (value.smartMaximumPreload < value.smartMinimumPreload) {
		return showError(
			17,
			15,
			tr::lng_online_playback_error_smart_preload(tr::now));
	}
	if (value.smartMaximumRequestLimit
			< value.smartMinimumRequestLimit) {
		return showError(
			20,
			19,
			tr::lng_online_playback_error_smart_dc_range(tr::now));
	}
	if (value.smartInitialRequestLimit
			< value.smartMinimumRequestLimit) {
		return showError(
			18,
			19,
			tr::lng_online_playback_error_smart_dc_initial_min(tr::now));
	}
	if (value.smartInitialRequestLimit
			> value.smartMaximumRequestLimit) {
		return showError(
			18,
			20,
			tr::lng_online_playback_error_smart_dc_initial_max(tr::now));
	}
	if (value.smartCapacityMinimumRequestLimit
			< value.smartMinimumRequestLimit) {
		return showError(
			21,
			19,
			tr::lng_online_playback_error_smart_capacity_min(tr::now));
	}
	if (value.smartCapacityMinimumRequestLimit
			> value.smartMaximumRequestLimit) {
		return showError(
			21,
			20,
			tr::lng_online_playback_error_smart_capacity_max(tr::now));
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

MessageMediaSizeBox::MessageMediaSizeBox(QWidget *parent)
: _emojiSize(
	this,
	st::defaultInputField,
	tr::lng_settings_message_media_size_placeholder())
, _stickerSize(
	this,
	st::defaultInputField,
	tr::lng_settings_message_media_size_placeholder()) {
}

QString MessageMediaSizeBox::SizeLabel() {
	return u"%1 px / %2 px"_q
		.arg(EnhancedSettings::MessageEmojiSize())
		.arg(EnhancedSettings::MessageStickerSize());
}

void MessageMediaSizeBox::prepare() {
	setTitle(tr::lng_settings_message_media_size());

	addButton(tr::lng_settings_message_media_size_reset(), [=] { reset(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });
	addButton(tr::lng_settings_save(), [=] { save(); });

	_emojiSize->setText(QString::number(EnhancedSettings::MessageEmojiSize()));
	_stickerSize->setText(QString::number(
		EnhancedSettings::MessageStickerSize()));
	_emojiSize->setMaxLength(3);
	_stickerSize->setMaxLength(3);

	auto y = st::boxOptionListPadding.top();
	const auto emojiLabel = Ui::CreateChild<Ui::FlatLabel>(
		this,
		tr::lng_settings_message_emoji_size(
			tr::now,
			lt_size,
			PixelRangeLabel(
				EnhancedSettings::kMessageEmojiSizeMinimum,
				EnhancedSettings::kMessageEmojiSizeMaximum)),
		st::boxLabel);
	emojiLabel->moveToLeft(st::boxPadding.left(), y);
	y += emojiLabel->height() + st::boxMediumSkip;
	_emojiSize->moveToLeft(st::boxPadding.left(), y);
	y += _emojiSize->height() + st::boxMediumSkip;

	const auto stickerLabel = Ui::CreateChild<Ui::FlatLabel>(
		this,
		tr::lng_settings_message_sticker_size(
			tr::now,
			lt_size,
			PixelRangeLabel(
				EnhancedSettings::kMessageStickerSizeMinimum,
				EnhancedSettings::kMessageStickerSizeMaximum)),
		st::boxLabel);
	stickerLabel->moveToLeft(st::boxPadding.left(), y);
	y += stickerLabel->height() + st::boxMediumSkip;
	_stickerSize->moveToLeft(st::boxPadding.left(), y);
	y += _stickerSize->height() + st::boxOptionListPadding.bottom();

	setDimensions(st::boxWidth, y);
}

void MessageMediaSizeBox::setInnerFocus() {
	_emojiSize->setFocusFast();
}

void MessageMediaSizeBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);

	const auto innerWidth = width()
		- st::boxPadding.left()
		- st::boxPadding.right();
	_emojiSize->resize(innerWidth, _emojiSize->height());
	_stickerSize->resize(innerWidth, _stickerSize->height());
	_emojiSize->moveToLeft(st::boxPadding.left(), _emojiSize->y());
	_stickerSize->moveToLeft(st::boxPadding.left(), _stickerSize->y());
}

void MessageMediaSizeBox::reset() {
	_emojiSize->hideError();
	_stickerSize->hideError();
	_emojiSize->setText(QString::number(
		EnhancedSettings::kMessageEmojiSizeDefault));
	_stickerSize->setText(QString::number(
		EnhancedSettings::kMessageStickerSizeDefault));
}

void MessageMediaSizeBox::save() {
	_emojiSize->hideError();
	_stickerSize->hideError();

	auto emojiOk = false;
	const auto emojiSize = _emojiSize->getLastText().trimmed().toInt(
		&emojiOk);
	if (!emojiOk
		|| emojiSize < EnhancedSettings::kMessageEmojiSizeMinimum
		|| emojiSize > EnhancedSettings::kMessageEmojiSizeMaximum) {
		Ui::Toast::Show(tr::lng_settings_message_emoji_size_invalid(
			tr::now,
			lt_size,
			PixelRangeLabel(
				EnhancedSettings::kMessageEmojiSizeMinimum,
				EnhancedSettings::kMessageEmojiSizeMaximum)));
		_emojiSize->showError();
		return;
	}

	auto stickerOk = false;
	const auto stickerSize = _stickerSize->getLastText().trimmed().toInt(
		&stickerOk);
	if (!stickerOk
		|| stickerSize < EnhancedSettings::kMessageStickerSizeMinimum
		|| stickerSize > EnhancedSettings::kMessageStickerSizeMaximum) {
		Ui::Toast::Show(tr::lng_settings_message_sticker_size_invalid(
			tr::now,
			lt_size,
			PixelRangeLabel(
				EnhancedSettings::kMessageStickerSizeMinimum,
				EnhancedSettings::kMessageStickerSizeMaximum)));
		_stickerSize->showError();
		return;
	}

	if (emojiSize == EnhancedSettings::MessageEmojiSize()
		&& stickerSize == EnhancedSettings::MessageStickerSize()) {
		closeBox();
		return;
	}

	const auto apply = [=](Fn<void()> &&) {
		SetEnhancedValue("message_emoji_size", emojiSize);
		SetEnhancedValue("message_sticker_size", stickerSize);
		EnhancedSettings::Write();
		Core::Restart();
	};
	getDelegate()->show(Ui::MakeConfirmBox({
		.text = tr::lng_settings_message_media_size_restart(tr::now),
		.confirmed = apply,
		.confirmText = tr::lng_settings_restart_now(tr::now),
		.cancelText = tr::lng_cancel(tr::now),
	}));
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

SearchPornConcurrencyBox::SearchPornConcurrencyBox(QWidget *parent)
: _limit(
	this,
	st::defaultInputField,
	tr::lng_settings_search_porn_concurrency_placeholder()) {
}

void SearchPornConcurrencyBox::prepare() {
	setTitle(tr::lng_settings_search_porn_concurrency());
	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });
	_limit->setText(QString::number(EnhancedSettings::SearchPornConcurrency()));
	_limit->setMaxLength(2);
	_limit->submits() | rpl::on_next([=] { save(); }, lifetime());
	setDimensions(
		st::boxWidth,
		_limit->height() + st::boxPadding.top() + st::boxPadding.bottom());
}

void SearchPornConcurrencyBox::setInnerFocus() {
	_limit->setFocusFast();
}

void SearchPornConcurrencyBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	_limit->resizeToWidth(
		width() - st::boxPadding.left() - st::boxPadding.right());
	_limit->moveToLeft(st::boxPadding.left(), st::boxPadding.top());
}

void SearchPornConcurrencyBox::save() {
	auto valid = false;
	const auto value = _limit->getLastText().trimmed().toInt(&valid);
	if (!valid
		|| value < EnhancedSettings::kSearchPornConcurrencyMinimum
		|| value > EnhancedSettings::kSearchPornConcurrencyMaximum) {
		_limit->showError();
		Ui::Toast::Show(tr::lng_settings_search_porn_concurrency_invalid(tr::now));
		return;
	}
	EnhancedSettings::SetSearchPornConcurrency(value);
	closeBox();
}

SearchPornIntervalBox::SearchPornIntervalBox(QWidget *parent)
: _interval(
	this,
	st::defaultInputField,
	tr::lng_settings_search_porn_interval_placeholder()) {
}

void SearchPornIntervalBox::prepare() {
	setTitle(tr::lng_settings_search_porn_interval());
	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });
	_interval->setText(QString::number(
		EnhancedSettings::SearchPornRequestInterval()));
	_interval->setMaxLength(4);
	_interval->submits() | rpl::on_next([=] { save(); }, lifetime());
	setDimensions(
		st::boxWidth,
		_interval->height() + st::boxPadding.top() + st::boxPadding.bottom());
}

void SearchPornIntervalBox::setInnerFocus() {
	_interval->setFocusFast();
}

void SearchPornIntervalBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	_interval->resizeToWidth(
		width() - st::boxPadding.left() - st::boxPadding.right());
	_interval->moveToLeft(st::boxPadding.left(), st::boxPadding.top());
}

void SearchPornIntervalBox::save() {
	auto valid = false;
	const auto value = _interval->getLastText().trimmed().toInt(&valid);
	if (!valid
		|| value < EnhancedSettings::kSearchPornRequestIntervalMinimum
		|| value > EnhancedSettings::kSearchPornRequestIntervalMaximum) {
		_interval->showError();
		Ui::Toast::Show(tr::lng_settings_search_porn_interval_invalid(tr::now));
		return;
	}
	EnhancedSettings::SetSearchPornRequestInterval(value);
	closeBox();
}

HashtagAutocompleteLimitBox::HashtagAutocompleteLimitBox(QWidget *parent)
: _limit(
	this,
	st::defaultInputField,
	tr::lng_settings_hashtag_autocomplete_limit_placeholder()) {
}

void HashtagAutocompleteLimitBox::prepare() {
	setTitle(tr::lng_settings_hashtag_autocomplete_limit());
	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });
	_limit->setText(QString::number(
		EnhancedSettings::HashtagAutocompleteLimit()));
	_limit->setMaxLength(4);
	_limit->submits() | rpl::on_next([=] { save(); }, lifetime());
	setDimensions(
		st::boxWidth,
		_limit->height() + st::boxPadding.top() + st::boxPadding.bottom());
}

void HashtagAutocompleteLimitBox::setInnerFocus() {
	_limit->setFocusFast();
}

void HashtagAutocompleteLimitBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	_limit->resizeToWidth(
		width() - st::boxPadding.left() - st::boxPadding.right());
	_limit->moveToLeft(st::boxPadding.left(), st::boxPadding.top());
}

void HashtagAutocompleteLimitBox::save() {
	auto valid = false;
	const auto value = _limit->getLastText().trimmed().toInt(&valid);
	if (!valid
		|| value < EnhancedSettings::kHashtagAutocompleteLimitMinimum
		|| value > EnhancedSettings::kHashtagAutocompleteLimitMaximum) {
		_limit->showError();
		Ui::Toast::Show(
			tr::lng_settings_hashtag_autocomplete_limit_invalid(tr::now));
		return;
	}
	EnhancedSettings::SetHashtagAutocompleteLimit(value);
	closeBox();
}

TrayIdleMemoryBox::TrayIdleMemoryBox(QWidget *parent)
: _minutes(
	this,
	st::defaultInputField,
	tr::lng_settings_tray_idle_memory_placeholder()) {
}

void TrayIdleMemoryBox::prepare() {
	setTitle(tr::lng_settings_tray_idle_memory());
	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });
	_minutes->setText(QString::number(
		EnhancedSettings::TrayIdleMemoryMinutes()));
	_minutes->setMaxLength(4);
	_minutes->submits() | rpl::on_next([=] { save(); }, lifetime());
	setDimensions(
		st::boxWidth,
		_minutes->height() + st::boxPadding.top() + st::boxPadding.bottom());
}

void TrayIdleMemoryBox::setInnerFocus() {
	_minutes->setFocusFast();
}

void TrayIdleMemoryBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	_minutes->resizeToWidth(
		width() - st::boxPadding.left() - st::boxPadding.right());
	_minutes->moveToLeft(st::boxPadding.left(), st::boxPadding.top());
}

void TrayIdleMemoryBox::save() {
	auto valid = false;
	const auto value = _minutes->getLastText().trimmed().toInt(&valid);
	if (!valid
		|| value < EnhancedSettings::kTrayIdleMemoryMinutesMinimum
		|| value > EnhancedSettings::kTrayIdleMemoryMinutesMaximum) {
		_minutes->showError();
		Ui::Toast::Show(tr::lng_settings_tray_idle_memory_invalid(tr::now));
		return;
	}
	EnhancedSettings::SetTrayIdleMemoryMinutes(value);
	closeBox();
}

MessageFoldingBox::MessageFoldingBox(QWidget *parent)
: _keywords(this, st::defaultInputField, tr::lng_message_folding_keywords())
, _ids(this, st::defaultInputField, tr::lng_message_folding_user_ids()) {
}

void MessageFoldingBox::prepare() {
	setTitle(tr::lng_message_folding_rules());
	_keywords->setMaxLength(MessageFolding::kMaxRuleLength);
	_ids->setMaxLength(MessageFolding::kMaxRuleLength);
	_keywords->setText(MessageFolding::Keywords());
	_ids->setText(MessageFolding::UserIds());
	const auto applyEnabled = [=] {
		_keywords->setDisabled(!MessageFolding::Enabled());
		_ids->setDisabled(!MessageFolding::Enabled());
	};
	applyEnabled();
	MessageFolding::Changes() | rpl::on_next(applyEnabled, lifetime());
	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });
	setDimensions(st::boxWidth,
		_keywords->height() + _ids->height() + st::boxPadding.top());
}

void MessageFoldingBox::setInnerFocus() {
	_keywords->setFocusFast();
}

void MessageFoldingBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	const auto width = this->width()
		- st::boxPadding.left() - st::boxPadding.right();
	_keywords->resize(width, _keywords->height());
	_ids->resize(width, _ids->height());
	_keywords->moveToLeft(st::boxPadding.left(), 0);
	_ids->moveToLeft(st::boxPadding.left(),
		_keywords->height() + st::boxPadding.top());
}

void MessageFoldingBox::save() {
	if (!MessageFolding::Enabled()) {
		closeBox();
		return;
	}
	if (!MessageFolding::Save(_keywords->getLastText(), _ids->getLastText())) {
		_ids->showError();
		_ids->setFocusFast();
		Ui::Toast::Show(tr::lng_message_folding_invalid_ids(tr::now));
		return;
	}
	closeBox();
}

SearchDialogFilterBox::SearchDialogFilterBox(QWidget *parent)
	: _ids(
		this,
		st::defaultInputField,
		tr::lng_settings_search_dialog_filter_placeholder()) {
}

QString SearchDialogFilterBox::IdsLabel(const QString &value) {
	const auto trimmed = value.trimmed();
	return trimmed.isEmpty()
		? tr::lng_settings_search_dialog_filter_disabled(tr::now)
		: trimmed;
}

void SearchDialogFilterBox::prepare() {
	setTitle(tr::lng_settings_search_dialog_filter());

	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	_ids->setText(EnhancedSettings::SearchDialogFilterIds());
	_ids->setMaxLength(4096);

	setDimensions(st::boxWidth, _ids->height());
}

void SearchDialogFilterBox::setInnerFocus() {
	_ids->setFocusFast();
}

void SearchDialogFilterBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);

	const auto width = st::boxWidth
		- st::boxPadding.left()
		- st::boxPadding.right();
	_ids->resize(width, _ids->height());
	_ids->moveToLeft(st::boxPadding.left(), 0);
}

void SearchDialogFilterBox::save() {
	EnhancedSettings::SetSearchDialogFilterIds(_ids->getLastText().trimmed());
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
