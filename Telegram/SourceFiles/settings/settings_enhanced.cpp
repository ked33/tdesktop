/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#include <base/timer_rpl.h>
#include <ui/toast/toast.h>
#include <mainwindow.h>
#include <QJsonArray>
#include <QJsonDocument>
#include "settings/settings_enhanced.h"

#include "settings/sections/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include <ui/vertical_list.h>
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/text/text_utilities.h" // Ui::Text::ToUpper
#include "boxes/connection_box.h"
#include "boxes/enhanced_options_box.h"
#include "boxes/about_box.h"
#include "ui/boxes/confirm_box.h"
#include "platform/platform_specific.h"
#include "window/window_session_controller.h"
#include "lang/lang_keys.h"
#include "lang/lang_instance.h"
#include "core/update_checker.h"
#include "core/enhanced_settings.h"
#include "core/message_folding.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "storage/localstorage.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_boost.h"
#include "media/streaming/media_streaming_diagnostics.h"
#include "layout/layout_item_base.h"
#include "facades.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "apiwrap.h"
#include "api/api_blocked_peers.h"

namespace Settings {
namespace {

void ExportEnhancedSettings() {
	const auto bytes = EnhancedSettings::ExportDocument();
	if (bytes.isEmpty()) {
		Ui::Toast::Show(tr::lng_settings_enhanced_export_failed(tr::now));
		return;
	}
	FileDialog::GetWritePath(
		Core::App().getFileDialogParent(),
		tr::lng_settings_enhanced_export(tr::now),
		u"JSON (*.json)"_q,
		u"enhanced-settings.json"_q,
		[=](QString &&path) {
			if (path.isEmpty()) {
				return;
			}
			auto file = QFile(path);
			if (!file.open(QIODevice::WriteOnly)
				|| file.write(bytes) != qint64(bytes.size())) {
				Ui::Toast::Show(
					tr::lng_settings_enhanced_export_failed(tr::now));
				return;
			}
			Ui::Toast::Show(tr::lng_settings_enhanced_export_done(tr::now));
		});
}

void ImportEnhancedSettings() {
	FileDialog::GetOpenPath(
		Core::App().getFileDialogParent(),
		tr::lng_settings_enhanced_import(tr::now),
		u"JSON (*.json)"_q,
		[=](const FileDialog::OpenResult &result) {
			auto bytes = result.remoteContent;
			if (bytes.isEmpty()) {
				if (result.paths.isEmpty()) {
					return;
				}
				auto file = QFile(result.paths.front());
				if (!file.open(QIODevice::ReadOnly)) {
					Ui::Toast::Show(
						tr::lng_settings_enhanced_import_failed(tr::now));
					return;
				}
				bytes = file.readAll();
			}
			if (!EnhancedSettings::CanImportDocument(bytes)) {
				Ui::Toast::Show(
					tr::lng_settings_enhanced_import_failed(tr::now));
				return;
			}
			const auto imported = bytes;
			Ui::show(Ui::MakeConfirmBox({
				.text = tr::lng_settings_enhanced_import_confirm(tr::now),
				.confirmed = [=](Fn<void()> &&close) {
					if (!EnhancedSettings::ImportDocument(imported)) {
						close();
						Ui::Toast::Show(
							tr::lng_settings_enhanced_import_failed(tr::now));
						return;
					}
					close();
					Core::Restart();
				},
				.confirmText = tr::lng_settings_restart_now(tr::now),
				.cancelText = tr::lng_cancel(tr::now),
			}));
		});
}

} // namespace

	void Enhanced::SetupEnhancedBackup(
			not_null<Ui::VerticalLayout *> container) {
		AddSkip(container);
		trackSearch(
			AddButtonWithIcon(
				container,
				tr::lng_settings_enhanced_export(),
				st::settingsButtonNoIcon),
			u"enhanced/export"_q)->addClickHandler([] {
			ExportEnhancedSettings();
		});
		trackSearch(
			AddButtonWithIcon(
				container,
				tr::lng_settings_enhanced_import(),
				st::settingsButtonNoIcon),
			u"enhanced/import"_q)->addClickHandler([] {
			ImportEnhancedSettings();
		});
		AddDividerText(container, tr::lng_settings_enhanced_backup_about());
	}

	void Enhanced::SetupEnhancedNetwork(not_null<Ui::VerticalLayout *> container) {
		const auto wrap = container->add(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
						container,
						object_ptr<Ui::VerticalLayout>(container)));
		const auto inner = wrap->entity();

		AddDividerText(inner, tr::lng_settings_restart_hint());
		AddSkip(inner);
		trackSearch(
			AddSubsectionTitle(inner, tr::lng_settings_network()),
			u"enhanced/section_network"_q);

		auto uploadBoostBtn = trackSearch(
			AddButtonWithLabel(
				inner,
				tr::lng_settings_net_upload_speed_boost(),
				rpl::single(NetBoostBox::BoostLabel(GetEnhancedInt("net_speed_boost"))),
				st::settingsButtonNoIcon
			),
			u"enhanced/net_speed_boost"_q);
		uploadBoostBtn->setColorOverride(QColor(255, 0, 0));
		uploadBoostBtn->addClickHandler([=] {
			Ui::show(Box<NetBoostBox>());
		});

		auto downloadBoostBtn = trackSearch(
			AddButtonWithLabel(
				inner,
				tr::lng_settings_net_download_speed_boost(),
				rpl::single(DownloadBoostBox::BoostLabel(
					GetEnhancedInt("net_download_speed_boost"))),
				st::settingsButtonNoIcon
			),
			u"enhanced/net_download_speed_boost"_q);
		downloadBoostBtn->setColorOverride(QColor(255, 0, 0));
		downloadBoostBtn->addClickHandler([=] {
			Ui::show(Box<DownloadBoostBox>());
		});

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_video_player_prefer_original(),
				st::settingsButtonNoIcon
			),
			u"enhanced/video_player_prefer_original"_q)->toggleOn(
			rpl::single(GetEnhancedBool(u"video_player_prefer_original"_q))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return toggled
				!= GetEnhancedBool(u"video_player_prefer_original"_q);
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue(u"video_player_prefer_original"_q, toggled);
			EnhancedSettings::Write();
		}, container->lifetime());
		AddDividerText(
			inner,
			tr::lng_settings_video_player_prefer_original_about());

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_online_playback_parameters_title(),
				st::settingsButtonNoIcon
			),
			u"enhanced/online_playback_parameters"_q)->addClickHandler([=] {
			Ui::show(Box<DownloadBoostProfilesBox>());
		});

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_online_playback_debug_logs(),
				st::settingsButtonNoIcon
			),
			u"enhanced/online_playback_debug_logs"_q)->toggleOn(
			rpl::single(GetEnhancedBool("online_playback_debug_logs"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled
				!= GetEnhancedBool("online_playback_debug_logs"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("online_playback_debug_logs", toggled);
			Media::Streaming::RefreshPlaybackDiagnosticsSettings();
			if (toggled) {
				Media::Streaming::LogOnlinePlaybackProfile();
			}
			EnhancedSettings::Write();
		}, container->lifetime());

		const auto currentFloodPremiumWaitLabel = [] {
			return FloodPremiumWaitBox::DelayLabel(
				GetEnhancedString("flood_premium_wait_override_ms"));
		};
		auto floodPremiumWaitValue = rpl::single(
			currentFloodPremiumWaitLabel()
		) | rpl::then(
			_FloodPremiumWaitChanged.events()
		) | rpl::map([=] {
			return currentFloodPremiumWaitLabel();
		});
		auto floodPremiumWaitButton = trackSearch(
			AddButtonWithLabel(
				inner,
				tr::lng_settings_flood_premium_wait_title(),
				std::move(floodPremiumWaitValue),
				st::settingsButtonNoIcon
			),
			u"enhanced/flood_premium_wait_override_ms"_q);
		floodPremiumWaitButton->setColorOverride(QColor(255, 0, 0));
		floodPremiumWaitButton->events(
		) | rpl::on_next([=](not_null<QEvent*> e) {
			if (e->type() == QEvent::UpdateLater) {
				_FloodPremiumWaitChanged.fire({});
			}
		}, container->lifetime());
		floodPremiumWaitButton->addClickHandler([=] {
			Ui::show(Box<FloodPremiumWaitBox>());
		});

		AddSkip(container);
	}

	void Enhanced::writeBlocklistFile() {
		QFile file(cWorkingDir() + qsl("tdata/blocklist.json"));
		if (file.open(QIODevice::WriteOnly)) {
			auto toArray = [&] {
				QJsonArray array;
				for (auto id : blockList) {
					array.append(id);
				}
				return array;
			};
			auto doc = QJsonDocument(toArray());
			file.write(doc.toJson(QJsonDocument::Compact));
			file.close();
			Ui::Toast::Show("Restart in 3 seconds!");
			QTimer::singleShot(3 * 1000, []{ Core::Restart(); });
		} else {
			Ui::Toast::Show("Failed to save blocklist.");
		}
	}

	void Enhanced::reqBlocked(int offset) {
		if (_requestId) {
			return;
		}
		_requestId = App::wnd()->sessionController()->session().api().request(MTPcontacts_GetBlocked(
				MTP_flags(0),
				MTP_int(offset),
				MTP_int(100)
		)).done([=](const MTPcontacts_Blocked &result) {
			_requestId = 0;
			result.match([&](const MTPDcontacts_blockedSlice& data) { // Incomplete list of blocked users response.
				blockCount = data.vcount().v;
				for (const auto& user : data.vusers().v) {
					blockList.append(int64(UserId(user.c_user().vid().v).bare));
				}
				if (blockCount > blockList.length()) {
					reqBlocked(offset+100);
				} else {
					writeBlocklistFile();
				}
			}, [&](const MTPDcontacts_blocked& data) { // 	Full list of blocked users response.
				for (const auto& user : data.vusers().v) {
					blockList.append(int64(UserId(user.c_user().vid().v).bare));
				}
				writeBlocklistFile();
			});
		}).fail([=] {
			_requestId = 0;
		}).send();
	}

	void Enhanced::SetupEnhancedMessages(
			not_null<Window::SessionController*> controller,
			not_null<Ui::VerticalLayout *> container) {
		AddDivider(container);
		AddSkip(container);
		trackSearch(
			AddSubsectionTitle(container, tr::lng_settings_messages()),
			u"enhanced/section_messages"_q);

		const auto foldingEnabled = [] {
			return rpl::single(MessageFolding::Enabled())
				| rpl::then(MessageFolding::Changes() | rpl::map([] {
					return MessageFolding::Enabled();
				}));
		};
		trackSearch(AddButtonWithIcon(
			container,
			tr::lng_message_folding_enabled(),
			st::settingsButtonNoIcon),
			u"enhanced/message_folding_enabled"_q
		)->toggleOn(foldingEnabled())->toggledChanges(
		) | rpl::on_next([](bool enabled) {
			MessageFolding::SetEnabled(enabled);
		}, container->lifetime());
		const auto foldingRules = trackSearch(AddButtonWithIcon(
			container,
			tr::lng_message_folding_rules(),
			st::settingsButtonNoIcon),
			u"enhanced/message_folding_rules"_q);
		foldingRules->addClickHandler([] {
			Ui::show(Box<MessageFoldingBox>());
		});
		foldingEnabled() | rpl::on_next([=](bool enabled) {
			foldingRules->setDisabled(!enabled);
		}, container->lifetime());
		AddDividerText(container, tr::lng_message_folding_about());

		const auto wrap = container->add(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
						container,
						object_ptr<Ui::VerticalLayout>(container)));
		const auto inner = wrap->entity();

		auto MsgIdBtn = trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_show_message_id(),
				st::settingsButtonNoIcon
			),
			u"enhanced/show_messages_id"_q);
		MsgIdBtn->setColorOverride(QColor(255, 0, 0));
		MsgIdBtn->toggleOn(
				rpl::single(GetEnhancedBool("show_messages_id"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("show_messages_id"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("show_messages_id", toggled);
			EnhancedSettings::Write();
			Core::Restart();
		}, container->lifetime());

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_label_channel_user(),
				st::settingsButtonNoIcon
			),
			u"enhanced/label_channel_user"_q)->toggleOn(
				rpl::single(GetEnhancedBool("label_channel_user"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("label_channel_user"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("label_channel_user", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		auto messageMediaSizeButton = trackSearch(
			AddButtonWithLabel(
				inner,
				tr::lng_settings_message_media_size(),
				rpl::single(MessageMediaSizeBox::SizeLabel()),
				st::settingsButtonNoIcon
			),
			u"enhanced/message_media_size"_q);
		messageMediaSizeButton->setColorOverride(QColor(255, 0, 0));
		messageMediaSizeButton->addClickHandler([=] {
			Ui::show(Box<MessageMediaSizeBox>());
		});

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_show_repeater_option(),
				st::settingsButtonNoIcon
			),
			u"enhanced/show_repeater_option"_q)->toggleOn(
				rpl::single(GetEnhancedBool("show_repeater_option"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("show_repeater_option"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("show_repeater_option", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		if (GetEnhancedBool("show_repeater_option")) {
			trackSearch(
				AddButtonWithIcon(
					inner,
					tr::lng_settings_repeater_reply_to_orig_msg(),
					st::settingsButtonNoIcon
				),
				u"enhanced/repeater_reply_to_orig_msg"_q)->toggleOn(
					rpl::single(GetEnhancedBool("repeater_reply_to_orig_msg"))
			)->toggledChanges(
			) | rpl::filter([=](bool toggled) {
				return (toggled != GetEnhancedBool("repeater_reply_to_orig_msg"));
			}) | rpl::on_next([=](bool toggled) {
				SetEnhancedValue("repeater_reply_to_orig_msg", toggled);
				EnhancedSettings::Write();
			}, container->lifetime());
		}

		auto value = rpl::single(
				AlwaysDeleteBox::DeleteLabel(GetEnhancedInt("always_delete_for"))
		) | rpl::then(
				_AlwaysDeleteChanged.events()
		) | rpl::map([] {
			return AlwaysDeleteBox::DeleteLabel(GetEnhancedInt("always_delete_for"));
		});

		auto btn = trackSearch(
			AddButtonWithLabel(
				container,
				tr::lng_settings_always_delete_for(),
				std::move(value),
				st::settingsButtonNoIcon
			),
			u"enhanced/always_delete_for"_q);
		btn->events(
		) | rpl::on_next([=](not_null<QEvent*> e) {
			const auto event = e->type();
			if (event == QEvent::UpdateLater) _AlwaysDeleteChanged.fire({});
		}, container->lifetime());
		btn->addClickHandler([=] {
			Ui::show(Box<AlwaysDeleteBox>());
		});

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_disable_cloud_draft_sync(),
				st::settingsButtonNoIcon
			),
			u"enhanced/disable_cloud_draft_sync"_q)->toggleOn(
				rpl::single(GetEnhancedBool("disable_cloud_draft_sync"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("disable_cloud_draft_sync"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("disable_cloud_draft_sync", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		AddSkip(container);

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_hide_classic_forward(),
				st::settingsButtonNoIcon
			),
			u"enhanced/hide_classic_fwd"_q)->toggleOn(
				rpl::single(GetEnhancedBool("hide_classic_fwd"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("hide_classic_fwd"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("hide_classic_fwd", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_keep_selected_messages_across_chats(),
				st::settingsButtonNoIcon
			),
			u"enhanced/keep_selected_messages_across_chats"_q)->toggleOn(
				rpl::single(GetEnhancedBool("keep_selected_messages_across_chats"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("keep_selected_messages_across_chats"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("keep_selected_messages_across_chats", toggled);
			if (!toggled) {
				controller->session().data().clearGlobalSelectedMessages();
			}
			EnhancedSettings::Write();
		}, container->lifetime());

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_lift_message_selection_limit(),
				st::settingsButtonNoIcon
			),
			u"enhanced/lift_message_selection_limit"_q)->toggleOn(
				rpl::single(GetEnhancedBool("lift_message_selection_limit"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("lift_message_selection_limit"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("lift_message_selection_limit", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		const auto currentQuickCopyTargetsLabel = [] {
			return QuickCopyTargetsBox::TargetsLabel(
				GetEnhancedString("quick_copy_targets"));
		};
		auto quickCopyTargetsValue = rpl::single(
			currentQuickCopyTargetsLabel()
		) | rpl::then(
			_QuickCopyTargetsChanged.events()
		) | rpl::map([=] {
			return currentQuickCopyTargetsLabel();
		});
		auto quickCopyTargetsButton = trackSearch(
			AddButtonWithLabel(
				inner,
				tr::lng_settings_quick_copy_targets_title(),
				std::move(quickCopyTargetsValue),
				st::settingsButtonNoIcon
			),
			u"enhanced/quick_copy_targets"_q);
		quickCopyTargetsButton->events(
		) | rpl::on_next([=](not_null<QEvent*> e) {
			if (e->type() == QEvent::UpdateLater) {
				_QuickCopyTargetsChanged.fire({});
			}
		}, container->lifetime());
		quickCopyTargetsButton->addClickHandler([=] {
			Ui::show(Box<QuickCopyTargetsBox>());
		});

		const auto addSelectedShortcutButton = [&](
				const QString &key,
				auto titleFn) {
			const auto currentLabel = [=] {
				return ChatSwitchShortcutBox::ShortcutLabel(
					GetEnhancedString(key));
			};
			auto value = rpl::single(
				currentLabel()
			) | rpl::then(
				_SelectedActionShortcutsChanged.events()
			) | rpl::map([=] {
				return currentLabel();
			});
			auto button = trackSearch(
				AddButtonWithLabel(
					inner,
					titleFn(),
					std::move(value),
					st::settingsButtonNoIcon
				),
				u"enhanced/"_q + key);
			button->events(
			) | rpl::on_next([=](not_null<QEvent*> e) {
				if (e->type() == QEvent::UpdateLater) {
					_SelectedActionShortcutsChanged.fire({});
				}
			}, container->lifetime());
			button->addClickHandler([=] {
				Ui::show(Box<ChatSwitchShortcutBox>(
					key,
					titleFn(),
					tr::lng_settings_shortcut_selected_placeholder(),
					[] {
						return tr::lng_settings_chat_switch_shortcut_invalid(
							tr::now);
					}));
			});
		};
		addSelectedShortcutButton(
			u"shortcut_selected_forward"_q,
			[] { return tr::lng_settings_shortcut_selected_forward(); });
		addSelectedShortcutButton(
			u"shortcut_selected_forward_no_quote"_q,
			[] { return tr::lng_settings_shortcut_selected_forward_no_quote(); });
		addSelectedShortcutButton(
			u"shortcut_selected_saved"_q,
			[] { return tr::lng_settings_shortcut_selected_saved(); });
		addSelectedShortcutButton(
			u"shortcut_selected_quick_copy"_q,
			[] { return tr::lng_settings_shortcut_selected_quick_copy(); });
		addSelectedShortcutButton(
			u"shortcut_selected_merge_forward"_q,
			[] { return tr::lng_settings_shortcut_selected_merge_forward(); });
		addSelectedShortcutButton(
			u"shortcut_selected_merge_album"_q,
			[] { return tr::lng_settings_shortcut_selected_merge_album(); });

		const auto currentCustomChatShortcutsLabel = [] {
			return CustomChatShortcutsBox::ShortcutsLabel(
				GetEnhancedString("custom_chat_shortcuts"));
		};
		auto customChatShortcutsValue = rpl::single(
			currentCustomChatShortcutsLabel()
		) | rpl::then(
			_CustomChatShortcutsChanged.events()
		) | rpl::map([=] {
			return currentCustomChatShortcutsLabel();
		});
		auto customChatShortcutsButton = trackSearch(
			AddButtonWithLabel(
				inner,
				tr::lng_settings_custom_chat_shortcuts_title(),
				std::move(customChatShortcutsValue),
				st::settingsButtonNoIcon
			),
			u"enhanced/custom_chat_shortcuts"_q);
		customChatShortcutsButton->events(
		) | rpl::on_next([=](not_null<QEvent*> e) {
			if (e->type() == QEvent::UpdateLater) {
				_CustomChatShortcutsChanged.fire({});
			}
		}, container->lifetime());
		customChatShortcutsButton->addClickHandler([=] {
			Ui::show(Box<CustomChatShortcutsBox>());
		});

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_double_click_copy_link(),
				st::settingsButtonNoIcon
			),
			u"enhanced/double_click_copy_link"_q)->toggleOn(
			rpl::single(GetEnhancedBool("double_click_copy_link"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("double_click_copy_link"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("double_click_copy_link", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		const auto currentNoForwardsBadgeColorLabel = [] {
			const auto color = GetEnhancedString("no_forwards_badge_color");
			return color.isEmpty() ? QString("#ecbb71") : color;
		};
		auto noForwardsBadgeColorValue = rpl::single(
			currentNoForwardsBadgeColorLabel()
		) | rpl::then(
			_NoForwardsBadgeColorChanged.events()
		) | rpl::map([=] {
			return currentNoForwardsBadgeColorLabel();
		});
		auto noForwardsBadgeColorButton = trackSearch(
			AddButtonWithLabel(
				inner,
				tr::lng_settings_no_forwards_badge_color(),
				std::move(noForwardsBadgeColorValue),
				st::settingsButtonNoIcon
			),
			u"enhanced/no_forwards_badge_color"_q);
		noForwardsBadgeColorButton->events(
		) | rpl::on_next([=](not_null<QEvent*> e) {
			if (e->type() == QEvent::UpdateLater) {
				_NoForwardsBadgeColorChanged.fire({});
			}
		}, container->lifetime());
		noForwardsBadgeColorButton->addClickHandler([=] {
			Ui::show(Box<NoForwardsBadgeColorBox>());
		});

		const auto currentCodeBlockBgColorLabel = [] {
			return CodeBlockBgColorBox::ColorLabel(
				GetEnhancedString("code_block_bg_color"));
		};
		auto codeBlockBgColorValue = rpl::single(
			currentCodeBlockBgColorLabel()
		) | rpl::then(
			_CodeBlockBgColorChanged.events()
		) | rpl::map([=] {
			return currentCodeBlockBgColorLabel();
		});
		auto codeBlockBgColorButton = trackSearch(
			AddButtonWithLabel(
				inner,
				tr::lng_settings_code_block_bg_color(),
				std::move(codeBlockBgColorValue),
				st::settingsButtonNoIcon
			),
			u"enhanced/code_block_bg_color"_q);
		codeBlockBgColorButton->events(
		) | rpl::on_next([=](not_null<QEvent*> e) {
			if (e->type() == QEvent::UpdateLater) {
				_CodeBlockBgColorChanged.fire({});
			}
		}, container->lifetime());
		codeBlockBgColorButton->addClickHandler([=] {
			Ui::show(Box<CodeBlockBgColorBox>());
		});

		const auto currentSearchMessageHighlightBgColorLabel = [] {
			return SearchMessageHighlightBgColorBox::ColorLabel(
				GetEnhancedString("search_message_highlight_bg_color"));
		};
		auto searchMessageHighlightBgColorValue = rpl::single(
			currentSearchMessageHighlightBgColorLabel()
		) | rpl::then(
			_SearchMessageHighlightBgColorChanged.events()
		) | rpl::map([=] {
			return currentSearchMessageHighlightBgColorLabel();
		});
		auto searchMessageHighlightBgColorButton = trackSearch(
			AddButtonWithLabel(
				inner,
				tr::lng_settings_search_message_highlight_bg_color(),
				std::move(searchMessageHighlightBgColorValue),
				st::settingsButtonNoIcon
			),
			u"enhanced/search_message_highlight_bg_color"_q);
		searchMessageHighlightBgColorButton->events(
		) | rpl::on_next([=](not_null<QEvent*> e) {
			if (e->type() == QEvent::UpdateLater) {
				_SearchMessageHighlightBgColorChanged.fire({});
			}
		}, container->lifetime());
		searchMessageHighlightBgColorButton->addClickHandler([=] {
			Ui::show(Box<SearchMessageHighlightBgColorBox>());
		});

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_disable_link_warning(),
				st::settingsButtonNoIcon
			),
			u"enhanced/disable_link_warning"_q)->toggleOn(
				rpl::single(GetEnhancedBool("disable_link_warning"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("disable_link_warning"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("disable_link_warning", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_disable_premium_animation(),
				st::settingsButtonNoIcon
			),
			u"enhanced/disable_premium_animation"_q)->toggleOn(
				rpl::single(GetEnhancedBool("disable_premium_animation"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("disable_premium_animation"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("disable_premium_animation", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_disable_global_search(),
				st::settingsButtonNoIcon
			),
			u"enhanced/disable_global_search"_q)->toggleOn(
				rpl::single(GetEnhancedBool("disable_global_search"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("disable_global_search"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("disable_global_search", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_search_main_and_archive(),
				st::settingsButtonNoIcon
			),
			u"enhanced/search_main_and_archive"_q)->toggleOn(
				rpl::single(GetEnhancedBool("search_main_and_archive"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("search_main_and_archive"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("search_main_and_archive", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_search_include_porn(),
				st::settingsButtonNoIcon
			),
			u"enhanced/search_include_porn"_q)->toggleOn(
				rpl::single(EnhancedSettings::SearchIncludePorn())
		)->toggledChanges(
		) | rpl::on_next([](bool toggled) {
			EnhancedSettings::SetSearchIncludePorn(toggled);
		}, container->lifetime());

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_search_dialog_filter(),
				st::settingsButtonNoIcon
			),
			u"enhanced/search_dialog_filter"_q)->toggleOn(
				rpl::single(EnhancedSettings::SearchDialogFilterEnabled())
		)->toggledChanges(
		) | rpl::on_next([](bool toggled) {
			EnhancedSettings::SetSearchDialogFilterEnabled(toggled);
		}, container->lifetime());

		auto searchDialogFilterIdsValue = rpl::single(
			SearchDialogFilterBox::IdsLabel(
				EnhancedSettings::SearchDialogFilterIds())
		) | rpl::then(
			EnhancedSettings::SearchDialogFilterChanges()
			| rpl::map([] {
				return SearchDialogFilterBox::IdsLabel(
					EnhancedSettings::SearchDialogFilterIds());
			})
		);
		trackSearch(
			AddButtonWithLabel(
				inner,
				rpl::single(QString()),
				std::move(searchDialogFilterIdsValue),
				st::settingsButtonNoIcon
			),
			u"enhanced/search_dialog_filter_ids"_q)->addClickHandler([] {
			Ui::show(Box<SearchDialogFilterBox>());
		});

		trackSearch(
			AddButtonWithLabel(
				inner,
				tr::lng_settings_search_porn_concurrency(),
				rpl::single(EnhancedSettings::SearchPornConcurrency())
					| rpl::then(EnhancedSettings::SearchPornConcurrencyChanges())
					| rpl::map([](int value) { return QString::number(value); }),
				st::settingsButtonNoIcon
			),
			u"enhanced/search_porn_concurrency"_q)->addClickHandler([] {
			Ui::show(Box<SearchPornConcurrencyBox>());
		});

		trackSearch(
			AddButtonWithLabel(
				inner,
				tr::lng_settings_search_porn_interval(),
				rpl::single(EnhancedSettings::SearchPornRequestInterval())
					| rpl::then(
						EnhancedSettings::SearchPornRequestIntervalChanges())
					| rpl::map([](int value) {
						return QString::number(value) + u" ms"_q;
					}),
				st::settingsButtonNoIcon
			),
			u"enhanced/search_porn_interval"_q)->addClickHandler([] {
			Ui::show(Box<SearchPornIntervalBox>());
		});
		AddDividerText(inner, tr::lng_settings_search_porn_interval_about());

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_show_group_sender_avatar(),
				st::settingsButtonNoIcon
			),
			u"enhanced/show_group_sender_avatar"_q)->toggleOn(
				rpl::single(GetEnhancedBool("show_group_sender_avatar"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("show_group_sender_avatar"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("show_group_sender_avatar", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		QString langPackBaseId = Lang::GetInstance().baseId();
		if (langPackBaseId == "zh-hant-raw" || langPackBaseId == "zh-hans-raw") {
			trackSearch(
				AddButtonWithIcon(
					inner,
					tr::lng_settings_translate_to_tc(),
					st::settingsButtonNoIcon
				),
				u"enhanced/translate_to_tc"_q)->toggleOn(
					rpl::single(GetEnhancedBool("translate_to_tc"))
			)->toggledChanges(
			) | rpl::filter([=](bool toggled) {
				return (toggled != GetEnhancedBool("translate_to_tc"));
			}) | rpl::on_next([=](bool toggled) {
				SetEnhancedValue("translate_to_tc", toggled);
				EnhancedSettings::Write();
			}, container->lifetime());
		}

		auto secondsBtn = trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_show_seconds(),
				st::settingsButtonNoIcon
			),
			u"enhanced/show_seconds"_q);
		secondsBtn->setColorOverride(QColor(255, 0, 0));
		secondsBtn->toggleOn(
			rpl::single(GetEnhancedBool("show_seconds"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("show_seconds"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("show_seconds", toggled);
			EnhancedSettings::Write();
			QTimer::singleShot(1 * 1000, []{ Core::Restart(); });
		}, container->lifetime());

		const auto addMessageContextToggle = [&](
				const QString &text,
				const char *key) {
			trackSearch(
				AddButtonWithIcon(
					inner,
					rpl::single(text),
					st::settingsButtonNoIcon
				),
				u"enhanced/"_q + QLatin1String(key))->toggleOn(
				rpl::single(GetEnhancedBool(key))
			)->toggledChanges(
			) | rpl::filter([=](bool toggled) {
				return (toggled != GetEnhancedBool(key));
			}) | rpl::on_next([=](bool toggled) {
				SetEnhancedValue(key, toggled);
				Media::Streaming::RefreshPlaybackDiagnosticsSettings();
				EnhancedSettings::Write();
			}, container->lifetime());
		};

			addMessageContextToggle(
				tr::lng_settings_message_read_reactions_info(tr::now),
				"show_message_context_read_info");
			addMessageContextToggle(
				tr::lng_context_details(tr::now),
				"show_message_context_details");
			addMessageContextToggle(
				tr::lng_context_reply_msg(tr::now),
				"show_message_context_reply");
			addMessageContextToggle(
				tr::lng_todo_add_title(tr::now),
				"show_message_context_add_task");
			addMessageContextToggle(
				tr::lng_context_copy_message_link(tr::now),
				"show_message_context_copy_link");
	#ifdef Q_OS_WIN
			addMessageContextToggle(
				tr::lng_context_stream_in_mpv_special(tr::now),
				"show_message_context_stream_in_mpv_special");
			addMessageContextToggle(
				tr::lng_context_stream_in_mpv(tr::now),
				"show_message_context_stream_in_mpv");
			AddDividerText(inner, tr::lng_settings_mpv_special_desc());
			addMessageContextToggle(
				tr::lng_settings_mpv_debug_logs(tr::now),
				"mpv_streaming_debug_logs");

			const auto currentMpvPathLabel = [=] {
				const auto path = GetEnhancedString("mpv_path").trimmed();
				return path.isEmpty()
					? tr::lng_settings_mpv_path_from_path(tr::now)
					: path;
			};
			auto mpvPathValue = rpl::single(
				currentMpvPathLabel()
			) | rpl::then(
				_MpvPathChanged.events()
			) | rpl::map([=] {
				return currentMpvPathLabel();
			});
			auto mpvPathButton = trackSearch(
				AddButtonWithLabel(
					inner,
					tr::lng_settings_mpv_path(),
					std::move(mpvPathValue),
					st::settingsButtonNoIcon
				),
				u"enhanced/mpv_path"_q);
			mpvPathButton->events(
			) | rpl::on_next([=](not_null<QEvent*> e) {
				if (e->type() == QEvent::UpdateLater) {
					_MpvPathChanged.fire({});
				}
			}, container->lifetime());
			mpvPathButton->addClickHandler([=] {
				Ui::show(Box<MpvPathBox>());
			});

			AddDividerText(inner, tr::lng_settings_mpv_path_desc());
	#endif
			addMessageContextToggle(
				tr::lng_context_show_messages_from(tr::now),
				"show_message_context_show_messages_from");
			addMessageContextToggle(
				tr::lng_context_forward(tr::now),
				"show_message_context_forward");
			addMessageContextToggle(
				tr::lng_context_repeater(tr::now),
				"show_message_context_repeater");
			addMessageContextToggle(
				tr::lng_context_send_now_msg(tr::now),
				"show_message_context_send_now");
			addMessageContextToggle(
				tr::lng_context_to_msg(tr::now),
				"show_message_context_go_to_message");
			addMessageContextToggle(
				tr::lng_replies_view_thread(tr::now),
				"show_message_context_view_replies");
			addMessageContextToggle(
				tr::lng_context_edit_msg(tr::now),
				"show_message_context_edit");
			addMessageContextToggle(
				tr::lng_context_add_factcheck(tr::now),
				"show_message_context_factcheck");
			addMessageContextToggle(
				tr::lng_context_pin_msg(tr::now),
				"show_message_context_pin");
			addMessageContextToggle(
				tr::lng_context_delete_msg(tr::now),
				"show_message_context_delete");
			addMessageContextToggle(
				tr::lng_context_save_file(tr::now),
				"show_message_context_save_as");
			addMessageContextToggle(
				tr::lng_context_report_msg(tr::now),
				"show_message_context_report");
			addMessageContextToggle(
				tr::lng_context_select_msg(tr::now),
				"show_message_context_select");
			addMessageContextToggle(
				tr::lng_context_reschedule(tr::now),
				"show_message_context_reschedule");

		auto jsonBtn = trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_show_view_as_json(),
				st::settingsButtonNoIcon
			),
			u"enhanced/show_json"_q);
		jsonBtn->toggleOn(
			rpl::single(GetEnhancedBool("show_json"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("show_json"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("show_json", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		auto enhancedJsonBtn = trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_show_view_as_enhanced_json(),
				st::settingsButtonNoIcon
			),
			u"enhanced/show_enhanced_json"_q);
		enhancedJsonBtn->toggleOn(
			rpl::single(GetEnhancedBool("show_enhanced_json"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("show_enhanced_json"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("show_enhanced_json", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		auto messageStatsBtn = trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_show_message_stats(),
				st::settingsButtonNoIcon
			),
			u"enhanced/show_message_stats"_q);
		messageStatsBtn->toggleOn(
			rpl::single(GetEnhancedBool("show_message_stats"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("show_message_stats"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("show_message_stats", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		auto hideBtn = trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_hide_messages(),
				st::settingsButtonNoIcon
			),
			u"enhanced/blocked_user_spoiler_mode"_q);
		hideBtn->setColorOverride(QColor(255, 0, 0));
		hideBtn->toggleOn(
				rpl::single(GetEnhancedBool("blocked_user_spoiler_mode"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("blocked_user_spoiler_mode"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("blocked_user_spoiler_mode", toggled);
			EnhancedSettings::Write();
			if (toggled) {
				Ui::Toast::Show("Please wait a moment, fetching blocklist...");

				App::wnd()->sessionController()->session().api().blockedPeers().slice() | rpl::take(
					1
				) | rpl::on_next([&](const Api::BlockedPeers::Slice &result) {
					if (blockList.length() == result.total) {
						return;
					}
					blockList = QList<int64>();
					reqBlocked(0);
				}, container->lifetime());
			}
		}, container->lifetime());

		AddDividerText(inner, tr::lng_settings_hide_messages_desc());
	}

	void Enhanced::SetupEnhancedButton(not_null<Ui::VerticalLayout *> container) {
		AddDivider(container);
		AddSkip(container);
		trackSearch(
			AddSubsectionTitle(container, tr::lng_settings_button()),
			u"enhanced/section_button"_q);

		const auto wrap = container->add(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
						container,
						object_ptr<Ui::VerticalLayout>(container)));
		const auto inner = wrap->entity();

		trackSearch(
			AddButtonWithLabel(
				inner,
				tr::lng_settings_hashtag_autocomplete_limit(),
				rpl::single(EnhancedSettings::HashtagAutocompleteLimit())
					| rpl::then(
						EnhancedSettings::HashtagAutocompleteLimitChanges())
					| rpl::map([](int value) { return QString::number(value); }),
				st::settingsButtonNoIcon
			),
			u"enhanced/hashtag_autocomplete_limit"_q)->addClickHandler([] {
			Ui::show(Box<HashtagAutocompleteLimitBox>());
		});
		AddDividerText(
			inner,
			tr::lng_settings_hashtag_autocomplete_limit_about());

		auto EmojiBtn = trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_show_emoji_button_as_text(),
				st::settingsButtonNoIcon
			),
			u"enhanced/show_emoji_button_as_text"_q);
		EmojiBtn->setColorOverride(QColor(255, 0, 0));
		EmojiBtn->toggleOn(
				rpl::single(GetEnhancedBool("show_emoji_button_as_text"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("show_emoji_button_as_text"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("show_emoji_button_as_text", toggled);
			EnhancedSettings::Write();
			Core::Restart();
		}, container->lifetime());

		AddDividerText(inner, tr::lng_show_emoji_button_as_text_desc());

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_show_scheduled_button(),
				st::settingsButtonNoIcon
			),
			u"enhanced/show_scheduled_button"_q)->toggleOn(
				rpl::single(GetEnhancedBool("show_scheduled_button"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("show_scheduled_button"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("show_scheduled_button", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		AddSkip(container);
	}

	void Enhanced::SetupEnhancedVoiceChat(not_null<Ui::VerticalLayout *> container) {
		AddDivider(container);
		AddSkip(container);
		trackSearch(
			AddSubsectionTitle(container, tr::lng_settings_voice_chat()),
			u"enhanced/section_voice_chat"_q);

		const auto wrap = container->add(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
						container,
						object_ptr<Ui::VerticalLayout>(container)));
		const auto inner = wrap->entity();

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_radio_controller(),
				st::settingsButtonNoIcon
			),
			u"enhanced/radio_controller"_q)->addClickHandler([=] {
			Ui::show(Box<RadioController>());
		});

		AddDividerText(inner, tr::lng_radio_controller_desc());

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_auto_unmute(),
				st::settingsButtonNoIcon
			),
			u"enhanced/auto_unmute"_q)->toggleOn(
				rpl::single(GetEnhancedBool("auto_unmute"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("auto_unmute"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("auto_unmute", toggled);
			EnhancedSettings::Write();
		}, container->lifetime());

		AddDividerText(inner, tr::lng_auto_unmute_desc());

		auto value = rpl::single(
				BitrateController::BitrateLabel(GetEnhancedInt("bitrate"))
		) | rpl::then(
				_BitrateChanged.events()
		) | rpl::map([=] {
			return BitrateController::BitrateLabel(GetEnhancedInt("bitrate"));
		});

		auto btn = trackSearch(
			AddButtonWithLabel(
				container,
				tr::lng_bitrate_controller(),
				std::move(value),
				st::settingsButtonNoIcon
			),
			u"enhanced/bitrate"_q);
		btn->events(
		) | rpl::on_next([=](not_null<QEvent*> e) {
			const auto event = e->type();
			if (event == QEvent::UpdateLater) _BitrateChanged.fire({});
		}, container->lifetime());
		btn->addClickHandler([=] {
			Ui::show(Box<BitrateController>());
		});

		trackSearch(
			AddButtonWithIcon(
				inner,
				tr::lng_settings_enable_hd_video(),
				st::settingsButtonNoIcon
			),
			u"enhanced/hd_video"_q)->toggleOn(
				rpl::single(GetEnhancedBool("hd_video"))
		)->toggledChanges(
		) | rpl::filter([=](bool toggled) {
			return (toggled != GetEnhancedBool("hd_video"));
		}) | rpl::on_next([=](bool toggled) {
			SetEnhancedValue("hd_video", toggled);
			Ui::Toast::Show(tr::lng_hd_video_hint(tr::now));
			EnhancedSettings::Write();
		}, container->lifetime());

		AddSkip(container);
	}

	void Enhanced::SetupEnhancedOthers(not_null<Window::SessionController*> controller, not_null<Ui::VerticalLayout *> container) {
		AddDivider(container);
		AddSkip(container);
		trackSearch(
			AddSubsectionTitle(container, tr::lng_settings_other()),
			u"enhanced/section_other"_q);

		const auto currentChatSwitchShortcutLabel = [] {
			return ChatSwitchShortcutBox::ShortcutLabel(
				GetEnhancedString("chat_switch_persistent_shortcut"));
		};
		auto chatSwitchShortcutValue = rpl::single(
			currentChatSwitchShortcutLabel()
		) | rpl::then(
			_ChatSwitchShortcutChanged.events()
		) | rpl::map([=] {
			return currentChatSwitchShortcutLabel();
		});
		auto chatSwitchShortcutButton = trackSearch(
			AddButtonWithLabel(
				container,
				tr::lng_settings_chat_switch_shortcut_title(),
				std::move(chatSwitchShortcutValue),
				st::settingsButtonNoIcon
			),
			u"enhanced/chat_switch_persistent_shortcut"_q);
		chatSwitchShortcutButton->events(
		) | rpl::on_next([=](not_null<QEvent*> e) {
			if (e->type() == QEvent::UpdateLater) {
				_ChatSwitchShortcutChanged.fire({});
			}
		}, container->lifetime());
		chatSwitchShortcutButton->addClickHandler([=] {
			Ui::show(Box<ChatSwitchShortcutBox>());
		});

		const auto currentJumpToDialogShortcutLabel = [] {
			return ChatSwitchShortcutBox::ShortcutLabel(
				GetEnhancedString("jump_to_dialog_shortcut"));
		};
		auto jumpToDialogShortcutValue = rpl::single(
			currentJumpToDialogShortcutLabel()
		) | rpl::then(
			_JumpToDialogShortcutChanged.events()
		) | rpl::map([=] {
			return currentJumpToDialogShortcutLabel();
		});
		auto jumpToDialogShortcutButton = trackSearch(
			AddButtonWithLabel(
				container,
				tr::lng_settings_jump_to_dialog_shortcut_title(),
				std::move(jumpToDialogShortcutValue),
				st::settingsButtonNoIcon
			),
			u"enhanced/jump_to_dialog_shortcut"_q);
		jumpToDialogShortcutButton->events(
		) | rpl::on_next([=](not_null<QEvent*> e) {
			if (e->type() == QEvent::UpdateLater) {
				_JumpToDialogShortcutChanged.fire({});
			}
		}, container->lifetime());
		jumpToDialogShortcutButton->addClickHandler([=] {
			Ui::show(Box<ChatSwitchShortcutBox>(
				u"jump_to_dialog_shortcut"_q,
				tr::lng_settings_jump_to_dialog_shortcut_title(),
				tr::lng_settings_jump_to_dialog_shortcut_placeholder(),
				[] {
					return tr::lng_settings_jump_to_dialog_shortcut_invalid(
						tr::now);
				}));
		});

		const auto currentGlobalSearchShortcutLabel = [] {
			return ChatSwitchShortcutBox::ShortcutLabel(
				GetEnhancedString("global_search_shortcut"));
		};
		auto globalSearchShortcutValue = rpl::single(
			currentGlobalSearchShortcutLabel()
		) | rpl::then(
			_GlobalSearchShortcutChanged.events()
		) | rpl::map([=] {
			return currentGlobalSearchShortcutLabel();
		});
		auto globalSearchShortcutButton = trackSearch(
			AddButtonWithLabel(
				container,
				tr::lng_settings_global_search_shortcut_title(),
				std::move(globalSearchShortcutValue),
				st::settingsButtonNoIcon
			),
			u"enhanced/global_search_shortcut"_q);
		globalSearchShortcutButton->events(
		) | rpl::on_next([=](not_null<QEvent*> e) {
			if (e->type() == QEvent::UpdateLater) {
				_GlobalSearchShortcutChanged.fire({});
			}
		}, container->lifetime());
		globalSearchShortcutButton->addClickHandler([=] {
			Ui::show(Box<ChatSwitchShortcutBox>(
				u"global_search_shortcut"_q,
				tr::lng_settings_global_search_shortcut_title(),
				tr::lng_settings_global_search_shortcut_placeholder(),
				[] {
					return tr::lng_settings_global_search_shortcut_invalid(
						tr::now);
				}));
		});

		trackSearch(
			AddButtonWithIcon(
				container,
				tr::lng_settings_multiple_chat_windows(),
				st::settingsButtonNoIcon
			),
			u"enhanced/multiple_chat_windows"_q)->toggleOn(
				rpl::single(EnhancedSettings::MultipleChatWindows())
		)->toggledValue(
		) | rpl::filter([](bool enabled) {
			return (enabled != EnhancedSettings::MultipleChatWindows());
		}) | rpl::on_next([=](bool enabled) {
			SetEnhancedValue("multiple_chat_windows", enabled);
			EnhancedSettings::Write();
		}, container->lifetime());
		AddDividerText(
			container,
			tr::lng_settings_multiple_chat_windows_about());

		auto hideBtn = trackSearch(
			AddButtonWithIcon(
				container,
				tr::lng_settings_hide_all_chats(),
				st::settingsButtonNoIcon
			),
			u"enhanced/hide_all_chats"_q);
		hideBtn->setColorOverride(QColor(255, 0, 0));
		hideBtn->toggleOn(
				rpl::single(GetEnhancedBool("hide_all_chats"))
		)->toggledValue(
		) | rpl::filter([](bool enabled) {
			return (enabled != GetEnhancedBool("hide_all_chats"));
		}) | rpl::on_next([=](bool enabled) {
			SetEnhancedValue("hide_all_chats", enabled);
			EnhancedSettings::Write();
			Core::Restart();
		}, container->lifetime());

		trackSearch(
			AddButtonWithIcon(
				container,
				tr::lng_settings_replace_edit_button(),
				st::settingsButtonNoIcon
			),
			u"enhanced/replace_edit_button"_q)->toggleOn(
				rpl::single(GetEnhancedBool("replace_edit_button"))
		)->toggledValue(
		) | rpl::filter([](bool enabled) {
			return (enabled != GetEnhancedBool("replace_edit_button"));
		}) | rpl::on_next([=](bool enabled) {
			SetEnhancedValue("replace_edit_button", enabled);
			EnhancedSettings::Write();
			controller->reloadFiltersMenu();
		}, container->lifetime());

		trackSearch(
			AddButtonWithIcon(
				container,
				tr::lng_settings_skip_message(),
				st::settingsButtonNoIcon
			),
			u"enhanced/skip_to_next"_q)->toggleOn(
				rpl::single(GetEnhancedBool("skip_to_next"))
		)->toggledValue(
		) | rpl::filter([](bool enabled) {
			return (enabled != GetEnhancedBool("skip_to_next"));
		}) | rpl::on_next([=](bool enabled) {
			SetEnhancedValue("skip_to_next", enabled);
			EnhancedSettings::Write();
		}, container->lifetime());

		AddDividerText(container, tr::lng_settings_skip_message_desc());

		trackSearch(
			AddButtonWithIcon(
				container,
				tr::lng_settings_hide_counter(),
				st::settingsButtonNoIcon
			),
			u"enhanced/hide_counter"_q)->toggleOn(
				rpl::single(GetEnhancedBool("hide_counter"))
		)->toggledValue(
		) | rpl::filter([](bool enabled) {
			return (enabled != GetEnhancedBool("hide_counter"));
		}) | rpl::on_next([=](bool enabled) {
			SetEnhancedValue("hide_counter", enabled);
			EnhancedSettings::Write();
		}, container->lifetime());

		trackSearch(
			AddButtonWithIcon(
				container,
				tr::lng_settings_hide_stories(),
				st::settingsButtonNoIcon
			),
			u"enhanced/hide_stories"_q)->toggleOn(
				rpl::single(GetEnhancedBool("hide_stories"))
		)->toggledValue(
		) | rpl::filter([](bool enabled) {
			return (enabled != GetEnhancedBool("hide_stories"));
		}) | rpl::on_next([=](bool enabled) {
			SetEnhancedValue("hide_stories", enabled);
			EnhancedSettings::Write();
		}, container->lifetime());

		const auto brightnessToggleHolder
			= std::make_shared<Ui::SlideWrap<Ui::VerticalLayout>*>(nullptr);

		trackSearch(
			AddButtonWithIcon(
				container,
				tr::lng_settings_preview_brightness(),
				st::settingsButtonNoIcon
			),
			u"enhanced/preview_brightness_enabled"_q)->toggleOn(
				rpl::single(GetEnhancedBool("preview_brightness_enabled"))
		)->toggledValue(
		) | rpl::filter([](bool enabled) {
			return (enabled != GetEnhancedBool("preview_brightness_enabled"));
		}) | rpl::on_next([=](bool enabled) {
			SetEnhancedValue("preview_brightness_enabled", enabled);
			EnhancedSettings::Write();
			if (*brightnessToggleHolder) {
				(*brightnessToggleHolder)->toggle(enabled, anim::type::normal);
			}
		}, container->lifetime());

		const auto brightnessWrap = container->add(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
						container,
						object_ptr<Ui::VerticalLayout>(container)));
		*brightnessToggleHolder = brightnessWrap;
		const auto brightnessInner = brightnessWrap->entity();

		brightnessWrap->toggle(
			GetEnhancedBool("preview_brightness_enabled"),
			anim::type::instant);

		auto brightnessValue = rpl::single(
				PreviewBrightnessBox::BrightnessLabel(
					GetEnhancedInt("preview_brightness"))
		) | rpl::then(
				_PreviewBrightnessChanged.events()
		) | rpl::map([] {
			return PreviewBrightnessBox::BrightnessLabel(
				GetEnhancedInt("preview_brightness"));
		});

		auto brightnessBtn = trackSearch(
			AddButtonWithLabel(
				brightnessInner,
				tr::lng_settings_preview_brightness_value(),
				std::move(brightnessValue),
				st::settingsButtonNoIcon
			),
			u"enhanced/preview_brightness"_q);
		brightnessBtn->events(
		) | rpl::on_next([=](not_null<QEvent*> e) {
			if (e->type() == QEvent::UpdateLater) {
				_PreviewBrightnessChanged.fire({});
			}
		}, container->lifetime());
		brightnessBtn->addClickHandler([=] {
			Ui::show(Box<PreviewBrightnessBox>());
		});

		AddDividerText(
			brightnessInner,
			tr::lng_settings_preview_brightness_desc());

		trackSearch(
			AddButtonWithIcon(
				container,
				tr::lng_settings_media_wheel_control(),
				st::settingsButtonNoIcon
			),
			u"enhanced/media_viewer_wheel_control_enabled"_q)->toggleOn(
				rpl::single(
					GetEnhancedBool("media_viewer_wheel_control_enabled"))
		)->toggledValue(
		) | rpl::filter([](bool enabled) {
			return (enabled
				!= GetEnhancedBool("media_viewer_wheel_control_enabled"));
		}) | rpl::on_next([=](bool enabled) {
			SetEnhancedValue("media_viewer_wheel_control_enabled", enabled);
			EnhancedSettings::Write();
		}, container->lifetime());

		AddDividerText(
			container,
			tr::lng_settings_media_wheel_control_desc());

		AddSkip(container);
		AddDivider(container);
		AddSkip(container);
		trackSearch(
			AddSubsectionTitle(container, tr::lng_settings_performance()),
			u"enhanced/section_performance"_q);

		const auto trayIdleLabel = [](int minutes) {
			return minutes
				? QString::number(minutes) + u" 分钟"_q
				: tr::lng_settings_tray_idle_memory_off(tr::now);
		};
		trackSearch(
			AddButtonWithLabel(
				container,
				tr::lng_settings_tray_idle_memory(),
				rpl::single(trayIdleLabel(
					EnhancedSettings::TrayIdleMemoryMinutes()))
					| rpl::then(
						EnhancedSettings::TrayIdleMemoryMinutesChanges()
						| rpl::map(trayIdleLabel)),
				st::settingsButtonNoIcon
			),
			u"enhanced/tray_idle_memory"_q)->addClickHandler([] {
			Ui::show(Box<TrayIdleMemoryBox>());
		});
		trackSearch(
			AddButtonWithIcon(
				container,
				tr::lng_settings_tray_idle_memory_clear(),
				st::settingsButtonNoIcon
			),
			u"enhanced/tray_idle_memory_clear"_q)->addClickHandler([] {
			if (Core::App().cleanupIdleMemory()) {
				Ui::Toast::Show(
					tr::lng_settings_tray_idle_memory_cleared(tr::now));
			} else {
				Ui::Toast::Show(
					tr::lng_settings_tray_idle_memory_busy(tr::now));
			}
		});
		AddDividerText(container, tr::lng_settings_tray_idle_memory_about());

		AddSkip(container);
	}

	namespace {

	[[nodiscard]] Builder::SearchEntryCheckIcon EnhancedCheckIcon(bool checked) {
		return checked
			? Builder::SearchEntryCheckIcon::Checked
			: Builder::SearchEntryCheckIcon::Unchecked;
	}

	void AddEnhancedSearchButton(
			Builder::SectionBuilder &builder,
			const QString &id,
			const QString &title,
			QStringList keywords = {}) {
		builder.add(nullptr, [=] {
			return Builder::SearchEntry{
				.id = id,
				.title = title,
				.keywords = keywords,
			};
		});
	}

	void AddEnhancedSearchToggle(
			Builder::SectionBuilder &builder,
			const QString &id,
			const QString &title,
			bool checked,
			QStringList keywords = {}) {
		builder.add(nullptr, [=] {
			return Builder::SearchEntry{
				.id = id,
				.title = title,
				.keywords = keywords,
				.checkIcon = EnhancedCheckIcon(checked),
			};
		});
	}

	void FillEnhancedSearch(Builder::SectionBuilder &builder) {
		const auto addButton = [&](
				const QString &id,
				const QString &title,
				QStringList keywords = {}) {
			AddEnhancedSearchButton(
				builder,
				id,
				title,
				std::move(keywords));
		};
		const auto addToggle = [&](
				const QString &id,
				const QString &title,
				bool checked,
				QStringList keywords = {}) {
			AddEnhancedSearchToggle(
				builder,
				id,
				title,
				checked,
				std::move(keywords));
		};
		const auto addBool = [&](
				const char *key,
				const QString &title,
				QStringList keywords = {}) {
			addToggle(
				u"enhanced/"_q + QLatin1String(key),
				title,
				GetEnhancedBool(key),
				std::move(keywords));
		};

		addButton(
			u"enhanced/export"_q,
			tr::lng_settings_enhanced_export(tr::now),
			{ tr::lng_settings_enhanced_backup_about(tr::now) });
		addButton(
			u"enhanced/import"_q,
			tr::lng_settings_enhanced_import(tr::now),
			{ tr::lng_settings_enhanced_backup_about(tr::now) });
		addButton(
			u"enhanced/section_network"_q,
			tr::lng_settings_network(tr::now));
		addButton(
			u"enhanced/net_speed_boost"_q,
			tr::lng_settings_net_upload_speed_boost(tr::now));
		addButton(
			u"enhanced/net_download_speed_boost"_q,
			tr::lng_settings_net_download_speed_boost(tr::now));
		addBool(
			"video_player_prefer_original",
			tr::lng_settings_video_player_prefer_original(tr::now),
			{ tr::lng_settings_video_player_prefer_original_about(tr::now) });
		addButton(
			u"enhanced/online_playback_parameters"_q,
			tr::lng_settings_online_playback_parameters_title(tr::now));
		addBool(
			"online_playback_debug_logs",
			tr::lng_settings_online_playback_debug_logs(tr::now));
		addButton(
			u"enhanced/flood_premium_wait_override_ms"_q,
			tr::lng_settings_flood_premium_wait_title(tr::now));

		addButton(
			u"enhanced/section_messages"_q,
			tr::lng_settings_messages(tr::now));
		addBool(
			"message_folding_enabled",
			tr::lng_message_folding_enabled(tr::now));
		addButton(
			u"enhanced/message_folding_rules"_q,
			tr::lng_message_folding_rules(tr::now),
			{ tr::lng_message_folding_keywords(tr::now),
				tr::lng_message_folding_user_ids(tr::now) });
		addBool(
			"show_messages_id",
			tr::lng_settings_show_message_id(tr::now));
		addBool(
			"label_channel_user",
			tr::lng_settings_label_channel_user(tr::now));
		addButton(
			u"enhanced/message_media_size"_q,
			tr::lng_settings_message_media_size(tr::now));
		addBool(
			"show_repeater_option",
			tr::lng_settings_show_repeater_option(tr::now));
		if (GetEnhancedBool("show_repeater_option")) {
			addBool(
				"repeater_reply_to_orig_msg",
				tr::lng_settings_repeater_reply_to_orig_msg(tr::now));
		}
		addButton(
			u"enhanced/always_delete_for"_q,
			tr::lng_settings_always_delete_for(tr::now));
		addBool(
			"disable_cloud_draft_sync",
			tr::lng_settings_disable_cloud_draft_sync(tr::now));
		addBool(
			"hide_classic_fwd",
			tr::lng_settings_hide_classic_forward(tr::now));
		addBool(
			"keep_selected_messages_across_chats",
			tr::lng_settings_keep_selected_messages_across_chats(tr::now));
		addBool(
			"lift_message_selection_limit",
			tr::lng_settings_lift_message_selection_limit(tr::now));
		addButton(
			u"enhanced/quick_copy_targets"_q,
			tr::lng_settings_quick_copy_targets_title(tr::now));
		addButton(
			u"enhanced/shortcut_selected_forward"_q,
			tr::lng_settings_shortcut_selected_forward(tr::now));
		addButton(
			u"enhanced/shortcut_selected_forward_no_quote"_q,
			tr::lng_settings_shortcut_selected_forward_no_quote(tr::now));
		addButton(
			u"enhanced/shortcut_selected_saved"_q,
			tr::lng_settings_shortcut_selected_saved(tr::now));
		addButton(
			u"enhanced/shortcut_selected_quick_copy"_q,
			tr::lng_settings_shortcut_selected_quick_copy(tr::now));
		addButton(
			u"enhanced/shortcut_selected_merge_forward"_q,
			tr::lng_settings_shortcut_selected_merge_forward(tr::now));
		addButton(
			u"enhanced/shortcut_selected_merge_album"_q,
			tr::lng_settings_shortcut_selected_merge_album(tr::now));
		addButton(
			u"enhanced/custom_chat_shortcuts"_q,
			tr::lng_settings_custom_chat_shortcuts_title(tr::now));
		addBool(
			"double_click_copy_link",
			tr::lng_settings_double_click_copy_link(tr::now));
		addButton(
			u"enhanced/no_forwards_badge_color"_q,
			tr::lng_settings_no_forwards_badge_color(tr::now));
		addButton(
			u"enhanced/code_block_bg_color"_q,
			tr::lng_settings_code_block_bg_color(tr::now));
		addButton(
			u"enhanced/search_message_highlight_bg_color"_q,
			tr::lng_settings_search_message_highlight_bg_color(tr::now));
		addBool(
			"disable_link_warning",
			tr::lng_settings_disable_link_warning(tr::now));
		addBool(
			"disable_premium_animation",
			tr::lng_settings_disable_premium_animation(tr::now));
		addBool(
			"disable_global_search",
			tr::lng_settings_disable_global_search(tr::now));
		addBool(
			"search_main_and_archive",
			tr::lng_settings_search_main_and_archive(tr::now));
		addToggle(
			u"enhanced/search_include_porn"_q,
			tr::lng_settings_search_include_porn(tr::now),
			EnhancedSettings::SearchIncludePorn());
		addToggle(
			u"enhanced/search_dialog_filter"_q,
			tr::lng_settings_search_dialog_filter(tr::now),
			EnhancedSettings::SearchDialogFilterEnabled());
		addButton(
			u"enhanced/search_dialog_filter_ids"_q,
			tr::lng_settings_search_dialog_filter(tr::now),
			{ EnhancedSettings::SearchDialogFilterIds() });
		addButton(
			u"enhanced/search_porn_concurrency"_q,
			tr::lng_settings_search_porn_concurrency(tr::now));
		addButton(
			u"enhanced/search_porn_interval"_q,
			tr::lng_settings_search_porn_interval(tr::now),
			{ tr::lng_settings_search_porn_interval_about(tr::now) });
		addBool(
			"show_group_sender_avatar",
			tr::lng_settings_show_group_sender_avatar(tr::now));
		const auto baseId = Lang::GetInstance().baseId();
		if (baseId == "zh-hant-raw" || baseId == "zh-hans-raw") {
			addBool(
				"translate_to_tc",
				tr::lng_settings_translate_to_tc(tr::now));
		}
		addBool(
			"show_seconds",
			tr::lng_settings_show_seconds(tr::now));
		addBool(
			"show_message_context_read_info",
			tr::lng_settings_message_read_reactions_info(tr::now));
		addBool(
			"show_message_context_details",
			tr::lng_context_details(tr::now));
		addBool(
			"show_message_context_reply",
			tr::lng_context_reply_msg(tr::now));
		addBool(
			"show_message_context_add_task",
			tr::lng_todo_add_title(tr::now));
		addBool(
			"show_message_context_copy_link",
			tr::lng_context_copy_message_link(tr::now));
#ifdef Q_OS_WIN
		addBool(
			"show_message_context_stream_in_mpv_special",
			tr::lng_context_stream_in_mpv_special(tr::now),
			{ tr::lng_settings_mpv_special_desc(tr::now) });
		addBool(
			"show_message_context_stream_in_mpv",
			tr::lng_context_stream_in_mpv(tr::now));
		addBool(
			"mpv_streaming_debug_logs",
			tr::lng_settings_mpv_debug_logs(tr::now));
		addButton(
			u"enhanced/mpv_path"_q,
			tr::lng_settings_mpv_path(tr::now),
			{ tr::lng_settings_mpv_path_desc(tr::now) });
#endif // Q_OS_WIN
		addBool(
			"show_message_context_show_messages_from",
			tr::lng_context_show_messages_from(tr::now));
		addBool(
			"show_message_context_forward",
			tr::lng_context_forward(tr::now));
		addBool(
			"show_message_context_repeater",
			tr::lng_context_repeater(tr::now));
		addBool(
			"show_message_context_send_now",
			tr::lng_context_send_now_msg(tr::now));
		addBool(
			"show_message_context_go_to_message",
			tr::lng_context_to_msg(tr::now));
		addBool(
			"show_message_context_view_replies",
			tr::lng_replies_view_thread(tr::now));
		addBool(
			"show_message_context_edit",
			tr::lng_context_edit_msg(tr::now));
		addBool(
			"show_message_context_factcheck",
			tr::lng_context_add_factcheck(tr::now));
		addBool(
			"show_message_context_pin",
			tr::lng_context_pin_msg(tr::now));
		addBool(
			"show_message_context_delete",
			tr::lng_context_delete_msg(tr::now));
		addBool(
			"show_message_context_save_as",
			tr::lng_context_save_file(tr::now));
		addBool(
			"show_message_context_report",
			tr::lng_context_report_msg(tr::now));
		addBool(
			"show_message_context_select",
			tr::lng_context_select_msg(tr::now));
		addBool(
			"show_message_context_reschedule",
			tr::lng_context_reschedule(tr::now));
		addBool(
			"show_json",
			tr::lng_settings_show_view_as_json(tr::now));
		addBool(
			"show_enhanced_json",
			tr::lng_settings_show_view_as_enhanced_json(tr::now));
		addBool(
			"show_message_stats",
			tr::lng_settings_show_message_stats(tr::now));
		addBool(
			"blocked_user_spoiler_mode",
			tr::lng_settings_hide_messages(tr::now),
			{ tr::lng_settings_hide_messages_desc(tr::now) });

		addButton(
			u"enhanced/section_button"_q,
			tr::lng_settings_button(tr::now));
		addButton(
			u"enhanced/hashtag_autocomplete_limit"_q,
			tr::lng_settings_hashtag_autocomplete_limit(tr::now),
			{ tr::lng_settings_hashtag_autocomplete_limit_about(tr::now) });
		addBool(
			"show_emoji_button_as_text",
			tr::lng_settings_show_emoji_button_as_text(tr::now),
			{ tr::lng_show_emoji_button_as_text_desc(tr::now) });
		addBool(
			"show_scheduled_button",
			tr::lng_settings_show_scheduled_button(tr::now));

		addButton(
			u"enhanced/section_voice_chat"_q,
			tr::lng_settings_voice_chat(tr::now));
		addButton(
			u"enhanced/radio_controller"_q,
			tr::lng_settings_radio_controller(tr::now),
			{ tr::lng_radio_controller_desc(tr::now) });
		addBool(
			"auto_unmute",
			tr::lng_settings_auto_unmute(tr::now),
			{ tr::lng_auto_unmute_desc(tr::now) });
		addButton(
			u"enhanced/bitrate"_q,
			tr::lng_bitrate_controller(tr::now));
		addBool(
			"hd_video",
			tr::lng_settings_enable_hd_video(tr::now),
			{ tr::lng_hd_video_hint(tr::now) });

		addButton(
			u"enhanced/section_other"_q,
			tr::lng_settings_other(tr::now));
		addButton(
			u"enhanced/chat_switch_persistent_shortcut"_q,
			tr::lng_settings_chat_switch_shortcut_title(tr::now));
		addButton(
			u"enhanced/jump_to_dialog_shortcut"_q,
			tr::lng_settings_jump_to_dialog_shortcut_title(tr::now));
		addButton(
			u"enhanced/global_search_shortcut"_q,
			tr::lng_settings_global_search_shortcut_title(tr::now));
		addToggle(
			u"enhanced/multiple_chat_windows"_q,
			tr::lng_settings_multiple_chat_windows(tr::now),
			EnhancedSettings::MultipleChatWindows(),
			{ tr::lng_settings_multiple_chat_windows_about(tr::now) });
		addBool(
			"hide_all_chats",
			tr::lng_settings_hide_all_chats(tr::now));
		addBool(
			"replace_edit_button",
			tr::lng_settings_replace_edit_button(tr::now));
		addBool(
			"skip_to_next",
			tr::lng_settings_skip_message(tr::now),
			{ tr::lng_settings_skip_message_desc(tr::now) });
		addBool(
			"hide_counter",
			tr::lng_settings_hide_counter(tr::now));
		addBool(
			"hide_stories",
			tr::lng_settings_hide_stories(tr::now));
		addBool(
			"preview_brightness_enabled",
			tr::lng_settings_preview_brightness(tr::now));
		addButton(
			u"enhanced/preview_brightness"_q,
			tr::lng_settings_preview_brightness_value(tr::now),
			{ tr::lng_settings_preview_brightness_desc(tr::now) });
		addBool(
			"media_viewer_wheel_control_enabled",
			tr::lng_settings_media_wheel_control(tr::now),
			{ tr::lng_settings_media_wheel_control_desc(tr::now) });

		addButton(
			u"enhanced/section_performance"_q,
			tr::lng_settings_performance(tr::now));
		addButton(
			u"enhanced/tray_idle_memory"_q,
			tr::lng_settings_tray_idle_memory(tr::now),
			{ tr::lng_settings_tray_idle_memory_about(tr::now) });
		addButton(
			u"enhanced/tray_idle_memory_clear"_q,
			tr::lng_settings_tray_idle_memory_clear(tr::now));
	}

	} // namespace

	const auto kEnhancedSearch = Builder::BuildHelper({
		.id = Enhanced::Id(),
		.parentId = MainId(),
		.title = &tr::lng_settings_enhanced,
		.icon = &st::menuIconManage,
	}, [](Builder::SectionBuilder &builder) {
		FillEnhancedSearch(builder);
	});

	rpl::producer<QString> Enhanced::title() {
		return tr::lng_settings_enhanced();
	}

	void Enhanced::showFinished() {
		Section<Enhanced>::showFinished();
		for (const auto &target : _searchTargets) {
			controller()->checkHighlightControl(target.first, target.second);
		}
	}

	Enhanced::Enhanced(
			QWidget *parent,
			not_null<Window::SessionController *> controller)
			: Section(parent, controller) {
		setupContent(controller);
	}

	void Enhanced::setupContent(not_null<Window::SessionController *> controller) {
		Expects(kEnhancedSearch.build != nullptr);

		const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

		SetupEnhancedBackup(content);
		SetupEnhancedNetwork(content);
		SetupEnhancedMessages(controller, content);
		SetupEnhancedButton(content);
		SetupEnhancedVoiceChat(content);
		SetupEnhancedOthers(controller, content);

		Ui::ResizeFitChild(this, content);
	}
} // namespace Settings

