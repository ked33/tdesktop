/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/experimental_options.h"

#include "base/basic_types.h"
#include "base/options.h"
#include "ui/widgets/kinetic_scroller.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

#include <optional>

namespace Core {
namespace {

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
[[nodiscard]] std::optional<bool> LegacyKineticScrollerValue(
		const QJsonObject &values) {
	if (values.contains(QString::fromLatin1(Ui::kOptionKineticScroller))) {
		return std::nullopt;
	}
	const auto legacy = values.value(u"qscroller"_q);
	return legacy.isBool()
		? std::make_optional(legacy.toBool())
		: std::nullopt;
}
#endif

} // namespace

void InitExperimentalOptions(const QString &path) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	auto file = QFile(path);
	if (file.open(QIODevice::ReadOnly)) {
		const auto parsed = QJsonDocument::fromJson(file.readAll());
		if (const auto legacy = LegacyKineticScrollerValue(parsed.object())) {
			base::options::lookup<bool>(Ui::kOptionKineticScroller).set(*legacy);
		}
	}
#endif
	base::options::init(path);
}

QString MigrateExperimentalOptions(const QString &json) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	const auto parsed = QJsonDocument::fromJson(json.toUtf8());
	auto values = parsed.object();
	if (const auto legacy = LegacyKineticScrollerValue(values)) {
		values.insert(QString::fromLatin1(Ui::kOptionKineticScroller), *legacy);
		values.remove(u"qscroller"_q);
		return QString::fromUtf8(
			QJsonDocument(values).toJson(QJsonDocument::Compact));
	}
#endif
	return json;
}

} // namespace Core
