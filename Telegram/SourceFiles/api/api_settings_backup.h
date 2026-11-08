#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QStringList>

namespace Api::SettingsBackup {

void Collect(Fn<void(QJsonObject, QStringList)> done);
[[nodiscard]] bool Validate(const QJsonObject &data);
void Restore(QJsonObject data, Fn<void(QStringList)> done);

} // namespace Api::SettingsBackup
