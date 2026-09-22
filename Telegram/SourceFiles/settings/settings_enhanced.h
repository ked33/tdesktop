/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#pragma once

#include "settings/settings_common_session.h"

class BoxContent;

namespace Window {
class Controller;
class SessionController;
} // namespace Window

namespace Settings {
	class Enhanced : public Section<Enhanced> {
	public:
		Enhanced(
				QWidget *parent,
				not_null<Window::SessionController *> controller);
		[[nodiscard]] rpl::producer<QString> title() override;
		void showFinished() override;

	private:
		void setupContent(not_null<Window::SessionController *> controller);
		void SetupEnhancedNetwork(not_null<Ui::VerticalLayout *> container);
		void SetupEnhancedMessages(
			not_null<Window::SessionController*> controller,
			not_null<Ui::VerticalLayout *> container);
		void SetupEnhancedButton(not_null<Ui::VerticalLayout *> container);
		void SetupEnhancedVoiceChat(not_null<Ui::VerticalLayout *> container);
		void SetupEnhancedOthers(not_null<Window::SessionController*> controller, not_null<Ui::VerticalLayout *> container);
		void reqBlocked(int offset);
		void writeBlocklistFile();

		template <typename Widget>
		not_null<Widget*> trackSearch(
				not_null<Widget*> widget,
				const QString &id) {
			_searchTargets.push_back(qMakePair(
				id,
				static_cast<QWidget*>(widget.get())));
			return widget;
		}

			rpl::event_stream<QString> _AlwaysDeleteChanged;
			rpl::event_stream<QString> _BitrateChanged;
			rpl::event_stream<QString> _ChatSwitchShortcutChanged;
			rpl::event_stream<QString> _CodeBlockBgColorChanged;
			rpl::event_stream<QString> _CustomChatShortcutsChanged;
			rpl::event_stream<QString> _FloodPremiumWaitChanged;
			rpl::event_stream<QString> _GlobalSearchShortcutChanged;
			rpl::event_stream<QString> _JumpToDialogShortcutChanged;
			rpl::event_stream<QString> _MpvPathChanged;
			rpl::event_stream<QString> _NoForwardsBadgeColorChanged;
			rpl::event_stream<QString> _PreviewBrightnessChanged;
			rpl::event_stream<QString> _QuickCopyTargetsChanged;
			rpl::event_stream<QString> _SelectedActionShortcutsChanged;
			rpl::event_stream<QString> _SearchMessageHighlightBgColorChanged;

			mtpRequestId _requestId = 0;
		QList<int64> blockList;
		int32 blockCount = 0;
		QList<QPair<QString, QWidget*>> _searchTargets;

	};

} // namespace Settings
