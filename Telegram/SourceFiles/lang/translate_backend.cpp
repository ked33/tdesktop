/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lang/translate_backend.h"

#include "core/enhanced_settings.h"
#include "main/main_session.h"
#include "settings.h"

namespace Lang::TranslateBackend {
namespace {

rpl::event_stream<> &ChangedStream() {
	static auto stream = rpl::event_stream<>();
	return stream;
}

[[nodiscard]] QString ProviderSetting() {
	const auto value = GetEnhancedString(kProviderKey);
	if (value.isEmpty()) {
		return kProviderGoogle;
	}
	return value;
}

} // namespace

bool PrefersTelegram() {
	return ProviderSetting() == kProviderTelegram;
}

void SetPrefersTelegram(bool preferTelegram) {
	const auto next = preferTelegram ? kProviderTelegram : kProviderGoogle;
	if (ProviderSetting() == next) {
		return;
	}
	SetEnhancedValue(kProviderKey, next);
	EnhancedSettings::Write();
	ChangedStream().fire({});
}

bool UsingTelegram(not_null<Main::Session*> session) {
	return PrefersTelegram() && session->premium();
}

bool UnlocksChatTranslateWithoutPremium() {
	return !PrefersTelegram();
}

rpl::producer<> changes() {
	return ChangedStream().events();
}

} // namespace Lang::TranslateBackend
