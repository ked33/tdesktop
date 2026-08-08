/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/text/text_entity.h"

#include <optional>

namespace Lang::TranslateCache {

[[nodiscard]] QString MakeKey(
	const QString &originalText,
	const QString &toLang,
	bool keepProtectedFormat);

[[nodiscard]] std::optional<TextWithEntities> Get(const QString &key);
void Put(const QString &key, TextWithEntities value);
void Clear();

} // namespace Lang::TranslateCache
