/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>
#include <QtCore/QJsonObject>

namespace Core {

[[nodiscard]] QJsonObject ExportExperimentalBackup();

void InitExperimentalOptions(const QString &path);
[[nodiscard]] QString MigrateExperimentalOptions(const QString &json);

} // namespace Core
