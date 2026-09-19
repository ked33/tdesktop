/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/toast/toast.h"

#include <vector>

class HistoryItem;
class PeerData;

namespace Data {
class Thread;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace ChatHelpers {

inline constexpr auto kSelectedActionToastDuration = crl::time(4000);

struct ForwardedMessagePhraseArgs final {
	size_t toCount = 0;
	bool singleMessage = false;
	PeerData *to1 = nullptr;
	PeerData *to2 = nullptr;
	bool toSelfWithPremiumIsEmpty = true;
};

[[nodiscard]] rpl::producer<TextWithEntities> ForwardedMessagePhrase(
	const ForwardedMessagePhraseArgs &args);

[[nodiscard]] QString ForwardedMessagePhraseIcon(
	const ForwardedMessagePhraseArgs &args);

[[nodiscard]] Ui::Toast::ClickHandlerFilter ForwardedToSavedMessagesFilter(
	not_null<Main::Session*> session);

[[nodiscard]] QString BracketChatName(const QString &name);
[[nodiscard]] QString BracketChatName(not_null<PeerData*> peer);
[[nodiscard]] QString BracketChatName(not_null<HistoryItem*> item);
[[nodiscard]] QString BracketChatName(not_null<Data::Thread*> thread);
[[nodiscard]] QString DestinationLines(
	const std::vector<not_null<Data::Thread*>> &threads);
[[nodiscard]] QString JoinToastParts(
	const QString &header,
	const QString &destinations,
	const QString &footer);

} // namespace ChatHelpers
