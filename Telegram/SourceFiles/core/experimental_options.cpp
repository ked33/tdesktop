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

QJsonObject ExportExperimentalBackup() {
	auto result = QJsonObject();
	const auto names = std::vector<const char*>{
		"forum-hide-chats-list",
		"dialogs-unread-on-top",
		"ctrl-click-chat-new-window",
		"dialogs-mute-icon",
		"use-new-chat-view",
		"auto-scroll-inactive-chat",
		"moderate-common-groups",
		"classic-profile-scroll",
		"use-small-msg-bubble-radius",
		"uncolored_quote",
		"unlimited-message-width",
		"mac-cmd-reply-immediately",
		"hide-ai-button",
		"force-compose-search-one-column",
		"view-profile-in-chats-list-context-menu",
		"show-peer-id-below-about",
		"show-channel-joined-below-about",
		"profile-media-tabs",
		"profile-media-tabs-expanded",
		"tabbed-panel-show-on-click",
		"unlimited-recent-stickers",
		"disable-autoplay-next",
		"external-media-viewer",
		"ffmpeg-multithread",
		"hide-reply-button",
		"custom-notification",
		"gnotification",
		"mac-modern-notifications",
		"fractional-scaling-enabled",
		"high-dpi-downscale",
		"use-qt-rhi",
		"force-angle",
		"enable-vulkan-rhi",
		"freetype",
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && !defined DESKTOP_APP_DISABLE_PRIVATE_QT_API
		"kinetic-scroller",
#endif
		"touchbar-disabled",
		"new-windows-size-as-first",
		"prefer-ipv6",
		"skip-url-scheme-register",
		"deadlock-detector",
		"webview-debug-enabled",
		"webview-legacy-edge",
		"fast-buttons-mode",
	};
	for (const auto name : names) {
		const auto &option = base::options::lookup<bool>(name);
		if (option.relevant()) {
			result.insert(option.id(), option.value());
		}
	}
	const auto &favorite
		= base::options::lookup<QString>("folder-favorite-link");
	result.insert(favorite.id(), favorite.value());
	return result;
}

} // namespace Core
