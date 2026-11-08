#include "storage/settings_backup.h"

#include "api/api_settings_backup.h"
#include "base/options.h"
#include "base/parse_helper.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/enhanced_settings.h"
#include "core/experimental_options.h"
#include "core/shortcuts.h"
#include "data/data_session.h"
#include "lang/lang_instance.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "platform/platform_specific.h"
#include "settings.h"
#include "storage/localstorage.h"
#include "storage/settings_backup_archive.h"
#include "storage/storage_account.h"
#include "ui/power_saving.h"
#include "window/themes/window_theme.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QSysInfo>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace Storage::SettingsBackup {
namespace {

constexpr auto kIterations = 600000;
const auto kMagic = QByteArray("TDSETTINGS1\n");

[[nodiscard]] std::vector<not_null<Main::Session*>> Sessions() {
	auto result = std::vector<not_null<Main::Session*>>();
	for (const auto &entry : Core::App().domain().accounts()) {
		if (entry.account->sessionExists()) {
			result.push_back(&entry.account->session());
		}
	}
	return result;
}

[[nodiscard]] QJsonObject EnhancedObject() {
	auto result = QJsonDocument::fromJson(
	    base::parse::stripComments(EnhancedSettings::ExportDocument()))
	                  .object();
	auto file = QFile(cWorkingDir() + u"tdata/blocklist.json"_q);
	auto blocked = QJsonArray();
	if (file.exists()) {
		if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
			result.insert(u"_localBlocklist"_q, QJsonValue::Null);
			return result;
		}
		const auto document = QJsonDocument::fromJson(file.read(1024 * 1024 + 1));
		if (file.error() != QFile::NoError || !document.isArray()) {
			result.insert(u"_localBlocklist"_q, QJsonValue::Null);
			return result;
		}
		blocked = document.array();
	}
	result.insert(u"_localBlocklist"_q, blocked);
	return result;
}

bool ApplyEnhanced(const QJsonObject &incoming) {
	if (!EnhancedSettings::ImportDocument(QJsonDocument(incoming).toJson())) {
		return false;
	}
	if (!incoming.contains(u"_localBlocklist"_q)) {
		return true;
	}
	const auto entries = incoming.value(u"_localBlocklist"_q).toArray();
	const auto bytes = QJsonDocument(entries).toJson();
	auto file = QSaveFile(cWorkingDir() + u"tdata/blocklist.json"_q);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()
	    || !file.commit()) {
		return false;
	}
	auto ids = QList<int64>();
	for (const auto &entry : entries) {
		ids.push_back(int64(entry.toDouble()));
	}
	cSetBlockList(ids);
	return true;
}

[[nodiscard]] QJsonObject ProxyObject() {
	const auto &settings = Core::App().settings().proxy();
	const auto encode = [](const MTP::ProxyData &proxy) {
		return QJsonObject{
			{ u"type"_q, int(proxy.type) },
			{ u"host"_q, proxy.host },
			{ u"port"_q, int(proxy.port) },
			{ u"user"_q, proxy.user },
			{ u"password"_q, proxy.password },
		};
	};
	auto list = QJsonArray();
	for (const auto &proxy : settings.list()) {
		list.push_back(encode(proxy));
	}
	return {
		{ u"list"_q, list },
		{ u"selected"_q, encode(settings.selected()) },
		{ u"mode"_q, int(settings.settings()) },
		{ u"ipv6"_q, settings.tryIPv6() },
		{ u"calls"_q, settings.useProxyForCalls() },
		{ u"rotation"_q, settings.proxyRotationEnabled() },
		{ u"timeout"_q, settings.proxyRotationTimeout() },
		{ u"preferred"_q,
		    Codec<std::vector<int>>::Encode(
		        settings.proxyRotationPreferredIndices()) },
	};
}

[[nodiscard]] bool ProxyRead(const QJsonObject &data, bool apply) {
	auto mode = 0;
	auto timeout = 0;
	auto preferred = std::vector<int>();
	if (!Codec<int>::Decode(data.value(u"mode"_q), mode) || mode < 0 || mode > 2
	    || !Codec<int>::Decode(data.value(u"timeout"_q), timeout)
	    || !ranges::contains(
	        Core::SettingsProxy::kProxyRotationTimeouts, timeout)
	    || !Codec<std::vector<int>>::Decode(
	        data.value(u"preferred"_q), preferred)
	    || !data.value(u"list"_q).isArray()
	    || data.value(u"list"_q).toArray().size() > 1000) {
		return false;
	}
	for (const auto key : { u"ipv6"_q, u"calls"_q, u"rotation"_q }) {
		if (!data.value(key).isBool()) {
			return false;
		}
	}
	const auto decode = [](const QJsonValue &value, MTP::ProxyData &proxy) {
		if (!value.isObject()) {
			return false;
		}
		const auto object = value.toObject();
		auto type = 0;
		if (!Codec<int>::Decode(object.value(u"type"_q), type) || type < 0
		    || type > 4
		    || !Codec<uint32>::Decode(object.value(u"port"_q), proxy.port)
		    || proxy.port > 65535
		    || !Codec<QString>::Decode(object.value(u"host"_q), proxy.host)
		    || !Codec<QString>::Decode(object.value(u"user"_q), proxy.user)
		    || !Codec<QString>::Decode(
		        object.value(u"password"_q), proxy.password)) {
			return false;
		}
		proxy.type = MTP::ProxyData::Type(type);
		return proxy.type == MTP::ProxyData::Type::None || proxy.valid();
	};
	auto selected = MTP::ProxyData();
	if (!decode(data.value(u"selected"_q), selected)) {
		return false;
	}
	auto list = std::vector<MTP::ProxyData>();
	for (const auto &value : data.value(u"list"_q).toArray()) {
		auto proxy = MTP::ProxyData();
		if (!decode(value, proxy) || !proxy.valid()) {
			return false;
		}
		list.push_back(std::move(proxy));
	}
	for (const auto index : preferred) {
		if (index < 0 || index >= list.size()) {
			return false;
		}
	}
	if (mode == int(MTP::ProxyData::Settings::Enabled) && !selected.valid()) {
		return false;
	}
	if (apply) {
		auto &settings = Core::App().settings().proxy();
		settings.setList(std::move(list));
		settings.setSelected(std::move(selected));
		settings.setSettings(MTP::ProxyData::Settings(mode));
		settings.setTryIPv6(data.value(u"ipv6"_q).toBool());
		settings.setUseProxyForCalls(data.value(u"calls"_q).toBool());
		settings.setProxyRotationEnabled(data.value(u"rotation"_q).toBool());
		settings.setProxyRotationTimeout(timeout);
		settings.setProxyRotationPreferredIndices(std::move(preferred));
	}
	return true;
}

[[nodiscard]] QJsonObject SystemObject() {
	return {
		{ u"scale"_q, cConfigScale() },
		{ u"startMinimized"_q, cStartMinimized() },
		{ u"autoUpdate"_q, cAutoUpdate() },
		{ u"powerSaving"_q, int(PowerSaving::Current().value()) },
		{ u"language"_q, Lang::GetInstance().exportBackup() },
		{ u"autoStart"_q, cAutoStart() },
		{ u"sendToMenu"_q, cSendToMenu() },
	};
}

[[nodiscard]] bool SystemRead(const QJsonObject &data, bool apply) {
	auto scale = cConfigScale();
	auto minimized = cStartMinimized();
	auto update = cAutoUpdate();
	auto power = int(PowerSaving::Current().value());
	const auto language = data.value(u"language"_q);
	if (!language.isUndefined()
	    && (!language.isObject()
	        || !Lang::GetInstance().importBackup(language.toObject(), false))) {
		return false;
	}
	auto autoStart = cAutoStart();
	auto sendToMenu = cSendToMenu();
	auto archive = Archive(data, true);
	archive.field("scale", scale, [](int value) {
		return value == 0 || (value >= 50 && value <= 300);
	});
	archive.field("autoStart", autoStart);
	archive.field("sendToMenu", sendToMenu);
	archive.field("startMinimized", minimized);
	archive.field("autoUpdate", update);
	archive.bounded("powerSaving", power, 0, int(PowerSaving::kAll));
	if (!archive.ok()) {
		return false;
	}
	if (apply) {
		cSetConfigScale(scale);
		cSetAutoStart(autoStart);
		cSetSendToMenu(sendToMenu);
		psSendToMenu(sendToMenu);
		if (!language.isUndefined()
		    && (!Lang::GetInstance().importBackup(language.toObject(), true)
		        || !Local::writeLangPackChecked())) {
			return false;
		}
		cSetStartMinimized(minimized);
		cSetAutoUpdate(update);
		PowerSaving::Set(PowerSaving::Flags(PowerSaving::Flag(power)));
	}
	return true;
}

[[nodiscard]] bool SaveJson(const QString &path, const QJsonObject &object) {
	const auto bytes = QJsonDocument(object).toJson();
	auto file = QSaveFile(path);
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()
	       && file.commit();
}

[[nodiscard]] bool ApplyLocal(const QJsonObject &sections) {
	if (sections.contains(u"application"_q)
	    && !Core::App().settings().importBackup(
	        sections.value(u"application"_q).toObject(), true)) {
		return false;
	}
	if (sections.contains(u"system"_q)
	    && !SystemRead(sections.value(u"system"_q).toObject(), true)) {
		return false;
	}
	if (sections.contains(u"proxy"_q)
	    && !ProxyRead(sections.value(u"proxy"_q).toObject(), true)) {
		return false;
	}
	if (sections.contains(u"enhanced"_q)
	    && !ApplyEnhanced(sections.value(u"enhanced"_q).toObject())) {
		return false;
	}
	for (const auto session : Sessions()) {
		const auto key = QString::number(session->uniqueId());
		const auto accounts = sections.value(u"accounts"_q).toObject();
		if (!accounts.contains(key)) {
			continue;
		}
		const auto data = accounts.value(key).toObject();
		if (!session->settings().importBackup(data, true)
		    || (data.contains(u"cache"_q)
		        && !session->local().importCacheBackup(
		            data.value(u"cache"_q).toObject(), true))
		    || !session->local().writeSessionSettingsChecked()) {
			return false;
		}
	}
	if (sections.contains(u"shortcuts"_q)
	    && !Shortcuts::WriteBackup(sections.value(u"shortcuts"_q).toObject())) {
		return false;
	}
	if (sections.contains(u"experimental"_q)
	    && (!base::options::deserialize(QString::fromUtf8(
	            QJsonDocument(sections.value(u"experimental"_q).toObject())
	                .toJson()))
	        || !SaveJson(cWorkingDir() + u"tdata/experimental_options.json"_q,
	            sections.value(u"experimental"_q).toObject()))) {
		return false;
	}
	if (sections.contains(u"appearance"_q)
	    && !Local::ImportThemeBackup(
	        sections.value(u"appearance"_q).toObject())) {
		return false;
	}
	return Local::writeSettingsChecked();
}

} // namespace

QJsonObject Collect(Selection selection) {
	auto sections = QJsonObject();
	if (selection.application) {
		sections.insert(
		    u"application"_q, Core::App().settings().exportBackup());
	}
	if (selection.enhanced) {
		sections.insert(u"enhanced"_q, EnhancedObject());
	}
	if (selection.accounts) {
		auto accounts = QJsonObject();
		for (const auto session : Sessions()) {
			auto data = session->settings().exportBackup();
			data.insert(u"cache"_q, session->local().exportCacheBackup());
			accounts.insert(QString::number(session->uniqueId()), data);
		}
		sections.insert(u"accounts"_q, accounts);
	}
	if (selection.shortcuts) {
		sections.insert(u"shortcuts"_q, Shortcuts::ExportBackup());
	}
	if (selection.experimental) {
		sections.insert(u"experimental"_q, Core::ExportExperimentalBackup());
	}
	if (selection.appearance) {
		sections.insert(u"appearance"_q, Local::ExportThemeBackup());
	}
	if (selection.system) {
		sections.insert(u"system"_q, SystemObject());
	}
	if (selection.proxy) {
		sections.insert(u"proxy"_q, ProxyObject());
	}
	return {
		{ u"format"_q, u"telegram-settings"_q },
		{ u"version"_q, 1 },
		{ u"platform"_q, QSysInfo::productType() },
		{ u"created"_q, QDateTime::currentDateTimeUtc().toString(Qt::ISODate) },
		{ u"sections"_q, sections },
	};
}

Preview Inspect(QJsonObject document) {
	auto result = Preview{ .document = std::move(document) };
	if (result.document.value(u"format"_q) != u"telegram-settings"_q
	    || result.document.value(u"version"_q) != 1
	    || !result.document.value(u"sections"_q).isObject()) {
		result.error = u"Unsupported backup format or version."_q;
		return result;
	}
	const auto sections = result.document.value(u"sections"_q).toObject();
	auto normalized = QJsonObject();
	const auto current
	    = Collect({ .proxy = true }).value(u"sections"_q).toObject();
	for (auto i = sections.begin(); i != sections.end(); ++i) {
		if (i.key() == u"cloud"_q) {
			if (!i->isObject()) {
				result.error = i.key();
				return result;
			}
			if (!Api::SettingsBackup::Validate(i->toObject())) {
				result.skipped.push_back(u"cloud/incompatible"_q);
				continue;
			}
			result.changes.push_back(u"cloud/accounts"_q);
			normalized.insert(i.key(), *i);
			continue;
		}
		if (!current.contains(i.key())) {
			result.skipped.push_back(i.key());
			continue;
		}
		if (!i->isObject()) {
			result.error = i.key();
			return result;
		}
		const auto incoming = i->toObject();
		auto valid = true;
		if (i.key() == u"application"_q) {
			valid = Core::App().settings().importBackup(incoming, false);
		} else if (i.key() == u"system"_q) {
			valid = SystemRead(incoming, false);
		} else if (i.key() == u"proxy"_q) {
			valid = ProxyRead(incoming, false);
		} else if (i.key() == u"enhanced"_q) {
			const auto known = EnhancedObject();
			const auto blocked = incoming.value(u"_localBlocklist"_q);
			if (!blocked.isUndefined()) {
				if (!blocked.isArray() || blocked.toArray().size() > 10000) {
					result.error = u"Invalid local block list."_q;
					return result;
				}
				for (const auto &id : blocked.toArray()) {
					if (!id.isDouble() || id.toDouble() <= 0
					    || id.toDouble() > 9007199254740991.
					    || std::trunc(id.toDouble()) != id.toDouble()) {
						result.error = u"Invalid local block list."_q;
						return result;
					}
				}
			}
			valid = EnhancedSettings::CanImportDocument(
			    QJsonDocument(incoming).toJson());
			for (auto field = incoming.begin(); field != incoming.end();
			    ++field) {
				if (!known.contains(field.key())) {
					continue;
				}
				if (field->type() != known.value(field.key()).type()) {
					valid = false;
				}
				if (field->isDouble()
				    && (std::trunc(field->toDouble()) != field->toDouble()
				        || field->toDouble() < INT_MIN
				        || field->toDouble() > INT_MAX)) {
					valid = false;
				}
				if (field->isString() && field->toString().size() > 65536) {
					valid = false;
				}
			}
		} else if (i.key() == u"accounts"_q) {
			for (auto account = incoming.begin(); account != incoming.end();
			    ++account) {
				auto scratch = Main::SessionSettings();
				if (!account->isObject()
				    || !scratch.importBackup(account->toObject(), false)) {
					valid = false;
				}
				const auto cache = account->toObject().value(u"cache"_q);
				if (!cache.isUndefined()) {
					const auto sessions = Sessions();
					if (!cache.isObject() || sessions.empty()
					    || !sessions.front()->local().importCacheBackup(
					        cache.toObject(), false)) {
						valid = false;
					}
				}
				if (!current.value(u"accounts"_q)
				        .toObject()
				        .contains(account.key())) {
					result.skipped.push_back(u"account/"_q + account.key());
				}
			}
		} else if (i.key() == u"shortcuts"_q) {
			valid = Shortcuts::ValidateBackup(incoming);
		} else if (i.key() == u"appearance"_q) {
			valid = Local::ValidateThemeBackup(incoming);
		} else if (i.key() == u"experimental"_q) {
			const auto known = Core::ExportExperimentalBackup();
			for (auto option = incoming.begin(); option != incoming.end();
			    ++option) {
				if (known.contains(option.key())
				    && option->type() != known.value(option.key()).type()) {
					valid = false;
				}
			}
		}
		if (!valid) {
			result.error = i.key();
			return result;
		}
		const auto previous = current.value(i.key()).toObject();
		auto accepted = QJsonObject();
		for (auto field = incoming.begin(); field != incoming.end(); ++field) {
			if (!previous.contains(field.key())) {
				result.skipped.push_back(i.key() + '/' + field.key());
			} else {
				accepted.insert(field.key(), *field);
				if (previous.value(field.key()) != *field) {
					result.changes.push_back(i.key() + '/' + field.key());
				}
			}
		}
		if (i.key() == u"experimental"_q) {
			auto merged
			    = QJsonDocument::fromJson(base::options::serialize().toUtf8())
			          .object();
			for (auto field = previous.begin(); field != previous.end();
			    ++field) {
				merged.insert(field.key(), *field);
			}
			for (auto field = accepted.begin(); field != accepted.end();
			    ++field) {
				merged.insert(field.key(), *field);
			}
			accepted = merged;
		}
		if (!accepted.isEmpty()) {
			normalized.insert(i.key(), accepted);
		}
	}
	result.document.insert(u"sections"_q, normalized);
	if (normalized.isEmpty()) {
		result.error = u"Empty backup."_q;
	}
	return result;
}

bool Apply(const Preview &preview, QString &error) {
	const auto checked = Inspect(preview.document);
	if (!checked.error.isEmpty()) {
		error = checked.error;
		return false;
	}
	if (HasRecovery()) {
		error = u"An earlier import requires recovery."_q;
		return false;
	}
	if (checked.document.value(u"sections"_q).toObject().keys()
	    == QStringList{ u"cloud"_q }) {
		return true;
	}
	auto snapshot = Collect({ .proxy = true });
	auto saved = snapshot.value(u"sections"_q).toObject();
	const auto chosen = checked.document.value(u"sections"_q).toObject();
	for (const auto &key : saved.keys()) {
		if (!chosen.contains(key)) {
			saved.remove(key);
		}
	}
	snapshot.insert(u"sections"_q, saved);
	if (!Inspect(snapshot).error.isEmpty()) {
		error
		    = u"Current settings cannot be safely restored from a recovery snapshot."_q;
		return false;
	}
	if (!Local::WriteSettingsBackupJournal(QJsonDocument(snapshot).toJson())) {
		error = u"Could not save the rollback snapshot."_q;
		return false;
	}
	Local::sync();
	if (!ApplyLocal(checked.document.value(u"sections"_q).toObject())) {
		const auto recovered = Recover(error);
		error
		    = recovered
		          ? u"Import failed. Previous settings restored."_q
		          : u"Import and recovery failed. The encrypted recovery snapshot was kept."_q;
		return false;
	}
	if (!Local::ClearSettingsBackupJournal()) {
		error
		    = u"Could not remove the recovery snapshot. Restore before trying again."_q;
		return false;
	}
	return true;
}

bool HasRecovery() {
	return Local::HasSettingsBackupJournal();
}

bool Recover(QString &error) {
	const auto bytes = Local::ReadSettingsBackupJournal();
	const auto document = QJsonDocument::fromJson(bytes).object();
	const auto accounts = document.value(u"sections"_q).toObject()
		.value(u"accounts"_q).toObject();
	for (const auto &id : accounts.keys()) {
		if (!ranges::any_of(Sessions(), [&](const auto session) {
			return QString::number(session->uniqueId()) == id;
		})) {
			error = u"Sign in to the original accounts before recovering their local settings."_q;
			return false;
		}
	}
	const auto checked = Inspect(document);
	if (bytes.isEmpty() || !checked.error.isEmpty()
	    || !ApplyLocal(checked.document.value(u"sections"_q).toObject())) {
		error = u"Could not restore the previous settings."_q;
		return false;
	}
	if (!Local::ClearSettingsBackupJournal()) {
		error
		    = u"Could not remove the recovery snapshot. Restore before trying again."_q;
		return false;
	}
	return true;
}

QByteArray Crypt(
    const QByteArray &data, const QString &password, bool encrypt) {
	const auto header = kMagic.size() + 44;
	if (data.size() > kMaximumFileSize || password.isEmpty()
	    || (!encrypt && (!data.startsWith(kMagic) || data.size() < header))) {
		return {};
	}
	auto salt = encrypt ? QByteArray(16, Qt::Uninitialized)
	                    : data.mid(kMagic.size(), 16);
	auto nonce = encrypt ? QByteArray(12, Qt::Uninitialized)
	                     : data.mid(kMagic.size() + 16, 12);
	auto tag = encrypt ? QByteArray(16, Qt::Uninitialized)
	                   : data.mid(kMagic.size() + 28, 16);
	const auto input = encrypt ? data : data.mid(header);
	auto key = QByteArray(32, Qt::Uninitialized);
	auto pass = password.toUtf8();
	const auto wipe = gsl::finally([&] {
		OPENSSL_cleanse(key.data(), key.size());
		OPENSSL_cleanse(pass.data(), pass.size());
	});
	if (encrypt
	    && (RAND_bytes(
	            reinterpret_cast<unsigned char*>(salt.data()), salt.size())
	            != 1
	        || RAND_bytes(
	               reinterpret_cast<unsigned char*>(nonce.data()), nonce.size())
	               != 1)) {
		return {};
	}
	if (PKCS5_PBKDF2_HMAC(pass.data(),
	        pass.size(),
	        reinterpret_cast<const unsigned char*>(salt.data()),
	        salt.size(),
	        kIterations,
	        EVP_sha256(),
	        key.size(),
	        reinterpret_cast<unsigned char*>(key.data()))
	    != 1) {
		return {};
	}
	const auto context
	    = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>(
	        EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
	if (!context) {
		return {};
	}
	auto output = QByteArray(input.size() + 16, Qt::Uninitialized);
	auto size = 0;
	auto tail = 0;
	auto ok = EVP_CipherInit_ex(context.get(),
	              EVP_aes_256_gcm(),
	              nullptr,
	              reinterpret_cast<const unsigned char*>(key.data()),
	              reinterpret_cast<const unsigned char*>(nonce.data()),
	              encrypt ? 1 : 0)
	          == 1;
	ok = ok
	     && EVP_CipherUpdate(context.get(),
	            reinterpret_cast<unsigned char*>(output.data()),
	            &size,
	            reinterpret_cast<const unsigned char*>(input.data()),
	            input.size())
	            == 1;
	if (!encrypt) {
		ok = ok
		     && EVP_CIPHER_CTX_ctrl(
		            context.get(), EVP_CTRL_GCM_SET_TAG, tag.size(), tag.data())
		            == 1;
	}
	ok = ok
	     && EVP_CipherFinal_ex(context.get(),
	            reinterpret_cast<unsigned char*>(output.data()) + size,
	            &tail)
	            == 1;
	if (encrypt) {
		ok = ok
		     && EVP_CIPHER_CTX_ctrl(
		            context.get(), EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data())
		            == 1;
	}
	if (!ok) {
		OPENSSL_cleanse(output.data(), output.size());
		return {};
	}
	output.resize(size + tail);
	return encrypt ? kMagic + salt + nonce + tag + output : output;
}

QByteArray Encrypt(const QByteArray &plain, const QString &password) {
	return Crypt(plain, password, true);
}

QByteArray Decrypt(const QByteArray &data, const QString &password) {
	return Crypt(data, password, false);
}

QJsonObject ReadLegacy(const QByteArray &data) {
	if (data.size() > kMaximumFileSize
	    || !EnhancedSettings::CanImportDocument(data)) {
		return {};
	}
	return {
		{ u"format"_q, u"telegram-settings"_q },
		{ u"version"_q, 1 },
		{ u"sections"_q,
		    QJsonObject{ { u"enhanced"_q,
		        QJsonDocument::fromJson(base::parse::stripComments(data))
		            .object() } } },
	};
}

} // namespace Storage::SettingsBackup
