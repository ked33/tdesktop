/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/share_message_phrase_factory.h"

#include "chat_helpers/compose/compose_show.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/text/text_utilities.h"
#include "window/window_session_controller.h"

namespace ChatHelpers {

rpl::producer<TextWithEntities> ForwardedMessagePhrase(
		const ForwardedMessagePhraseArgs &args) {
	if (args.toCount <= 1) {
		Assert(args.to1);

		if (args.to1->isSelf()) {
			if (args.toSelfWithPremiumIsEmpty && args.to1->isPremium()) {
				return {};
			}
			return (args.singleMessage
				? tr::lng_share_message_to_saved
				: tr::lng_share_messages_to_saved)(
					lt_chat,
					rpl::single(Ui::Text::Link(
						tr::lng_saved_messages(tr::now))),
					tr::rich);
		} else {
			return (args.singleMessage
				? tr::lng_share_message_to_chat
				: tr::lng_share_messages_to_chat)(
					lt_chat,
					rpl::single(TextWithEntities{ args.to1->name() }),
					tr::rich);
		}
	} else if ((args.toCount == 2) && (args.to1 && args.to2)) {
		return (args.singleMessage
			? tr::lng_share_message_to_two_chats
			: tr::lng_share_messages_to_two_chats)(
				lt_user,
				rpl::single(TextWithEntities{ args.to1->name() }),
				lt_chat,
				rpl::single(TextWithEntities{ args.to2->name() }),
				tr::rich);
	} else {
		return (args.singleMessage
			? tr::lng_share_message_to_many_chats
			: tr::lng_share_messages_to_many_chats)(
				lt_count,
				rpl::single(args.toCount) | tr::to_count(),
				tr::rich);
	}
}

QString ForwardedMessagePhraseIcon(
		const ForwardedMessagePhraseArgs &args) {
	const auto toSelf = (args.toCount <= 1)
		&& args.to1
		&& args.to1->isSelf();
	return toSelf
		? u"toast/saved_messages"_q
		: u"toast/forward"_q;
}

Ui::Toast::ClickHandlerFilter ForwardedToSavedMessagesFilter(
		not_null<Main::Session*> session) {
	return [=](const ClickHandlerPtr &, Qt::MouseButton) {
		if (const auto window = ResolveWindowDefault()(session)) {
			window->showPeerHistory(window->session().user());
		}
		return false;
	};
}

namespace {

[[nodiscard]] QString DisplayChatName(not_null<PeerData*> peer) {
	return peer->isSelf()
		? tr::lng_saved_messages(tr::now)
		: peer->name();
}

} // namespace

QString BracketChatName(const QString &name) {
	return u"【"_q + name + u"】"_q;
}

QString BracketChatName(not_null<PeerData*> peer) {
	return BracketChatName(DisplayChatName(peer));
}

QString BracketChatName(not_null<HistoryItem*> item) {
	if (const auto topic = item->topic()) {
		return BracketChatName(topic->title());
	}
	return BracketChatName(item->history()->peer);
}

QString BracketChatName(not_null<Data::Thread*> thread) {
	if (const auto topic = thread->asTopic()) {
		return BracketChatName(topic->title());
	}
	return BracketChatName(thread->peer());
}

QString DestinationLines(
		const std::vector<not_null<Data::Thread*>> &threads) {
	auto lines = QStringList();
	lines.reserve(int(threads.size()));
	for (const auto &thread : threads) {
		lines.push_back(BracketChatName(thread));
	}
	return lines.join(u'\n');
}

QString JoinToastParts(
		const QString &header,
		const QString &destinations,
		const QString &footer) {
	auto text = header;
	if (!destinations.isEmpty()) {
		text += u'\n';
		text += destinations;
	}
	if (!footer.isEmpty()) {
		text += u"\n\n"_q;
		text += footer;
	}
	return text;
}

} // namespace ChatHelpers
