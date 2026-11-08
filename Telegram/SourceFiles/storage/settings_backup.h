#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QStringList>

namespace Main {
class Session;
} // namespace Main

namespace Storage::SettingsBackup {

inline constexpr auto kMaximumFileSize = 128 * 1024 * 1024;

struct Selection {
	bool application = true;
	bool enhanced = true;
	bool accounts = true;
	bool shortcuts = true;
	bool experimental = true;
	bool appearance = true;
	bool system = true;
	bool proxy = false;
	bool cloud = false;
};

struct Preview {
	QJsonObject document;
	QStringList changes;
	QStringList skipped;
	QString error;
};

[[nodiscard]] QJsonObject Collect(Selection selection);
[[nodiscard]] Preview Inspect(QJsonObject document);
[[nodiscard]] bool Apply(const Preview &preview, QString &error);
[[nodiscard]] bool Recover(QString &error);
[[nodiscard]] bool HasRecovery();
[[nodiscard]] QByteArray Encrypt(
	const QByteArray &plain,
	const QString &password);
[[nodiscard]] QByteArray Decrypt(
	const QByteArray &data,
	const QString &password);
[[nodiscard]] QJsonObject ReadLegacy(const QByteArray &data);

} // namespace Storage::SettingsBackup
