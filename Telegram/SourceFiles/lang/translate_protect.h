/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/text/text_entity.h"

namespace Lang::TranslateProtect {

struct Span {
	int index = 0;
	EntityType type = EntityType::Invalid;
	QString data;
	QString original;
	QString placeholder;
};

struct Protected {
	QString text;
	std::vector<Span> spans;
	bool used = false;
};

[[nodiscard]] bool IsProtectedType(EntityType type);
[[nodiscard]] Protected Protect(const TextWithEntities &original);
[[nodiscard]] TextWithEntities Restore(
	const QString &translated,
	const std::vector<Span> &spans);

} // namespace Lang::TranslateProtect
