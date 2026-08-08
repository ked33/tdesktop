/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "rpl/producer.h"

namespace Main {
class Session;
} // namespace Main

namespace Lang::TranslateBackend {

inline constexpr auto kProviderKey = "translation_provider";
inline constexpr auto kProviderGoogle = "google";
inline constexpr auto kProviderTelegram = "telegram";
inline constexpr auto kKeepProtectedFormatKey
	= "translation_keep_protected_format";

[[nodiscard]] bool PrefersTelegram();
void SetPrefersTelegram(bool preferTelegram);

[[nodiscard]] bool KeepProtectedFormat();
void SetKeepProtectedFormat(bool enabled);

[[nodiscard]] bool UsingTelegram(not_null<Main::Session*> session);
[[nodiscard]] bool UnlocksChatTranslateWithoutPremium();

[[nodiscard]] rpl::producer<> changes();

} // namespace Lang::TranslateBackend
