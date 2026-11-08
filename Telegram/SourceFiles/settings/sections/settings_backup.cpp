#include "settings/sections/settings_backup.h"

#include "api/api_settings_backup.h"
#include "base/parse_helper.h"
#include "boxes/abstract_box.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "platform/platform_specific.h"
#include "settings.h"
#include "settings/settings_common.h"
#include "storage/localstorage.h"
#include "storage/settings_backup.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/qt_object_factory.h"
#include "ui/toast/toast.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

using namespace Storage::SettingsBackup;

std::weak_ptr<int> ActiveOperation;

std::shared_ptr<int> BeginOperation() {
	if (!ActiveOperation.expired()) {
		Ui::Toast::Show(tr::lng_backup_loading(tr::now));
		return nullptr;
	}
	auto result = std::make_shared<int>(0);
	ActiveOperation = result;
	return result;
}

void RestartWithSystemSettings(bool restoreSystem) {
	if (!restoreSystem) {
		Core::Restart();
		return;
	}
	const auto requested = cAutoStart();
	Platform::AutostartToggle(requested, [=](bool enabled) {
		cSetAutoStart(enabled);
		Local::writeSettings();
		if (requested != enabled) {
			Ui::show(Ui::MakeConfirmBox({
			    .text = tr::lng_backup_autostart_failed(tr::now),
			    .confirmed =
			        [](Fn<void()> close) {
				        close();
				        Core::Restart();
			        },
			    .confirmText = tr::lng_settings_restart_now(tr::now),
			}));
		} else {
			Core::Restart();
		}
	});
}

struct Category {
	const char* key = nullptr;
	const tr::phrase<>* label = nullptr;
	bool (*get)(const Selection &) = nullptr;
	void (*set)(Selection &, bool) = nullptr;
};

const auto kCategories = std::array{
	Category{ "application",
	    &tr::lng_backup_application,
	    [](const Selection &s) { return s.application; },
	    [](Selection &s, bool v) { s.application = v; } },
	Category{ "enhanced",
	    &tr::lng_settings_enhanced,
	    [](const Selection &s) { return s.enhanced; },
	    [](Selection &s, bool v) { s.enhanced = v; } },
	Category{ "accounts",
	    &tr::lng_backup_accounts,
	    [](const Selection &s) { return s.accounts; },
	    [](Selection &s, bool v) { s.accounts = v; } },
	Category{ "shortcuts",
	    &tr::lng_enhanced_group_shortcuts,
	    [](const Selection &s) { return s.shortcuts; },
	    [](Selection &s, bool v) { s.shortcuts = v; } },
	Category{ "experimental",
	    &tr::lng_backup_experimental,
	    [](const Selection &s) { return s.experimental; },
	    [](Selection &s, bool v) { s.experimental = v; } },
	Category{ "appearance",
	    &tr::lng_backup_appearance,
	    [](const Selection &s) { return s.appearance; },
	    [](Selection &s, bool v) { s.appearance = v; } },
	Category{ "system",
	    &tr::lng_backup_system,
	    [](const Selection &s) { return s.system; },
	    [](Selection &s, bool v) { s.system = v; } },
	Category{ "proxy",
	    &tr::lng_backup_proxy,
	    [](const Selection &s) { return s.proxy; },
	    [](Selection &s, bool v) { s.proxy = v; } },
	Category{ "cloud",
	    &tr::lng_backup_cloud,
	    [](const Selection &s) { return s.cloud; },
	    [](Selection &s, bool v) { s.cloud = v; } },
};

void ShowError(const QString &details = {}) {
	Ui::show(Ui::MakeInformBox(
		tr::lng_backup_failed(tr::now)
			+ (details.isEmpty() ? QString() : u"\n\n"_q + details)),
		Ui::LayerOption::KeepOther);
}

void RequestPassword(bool exporting, Fn<void(QString)> done) {
	Ui::show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_backup_password_title());
		Ui::AddDividerText(
			box->verticalLayout(),
			tr::lng_backup_password_about());
		const auto addPassword = [&](rpl::producer<QString> placeholder) {
			const auto row = box->addRow(object_ptr<Ui::RpWidget>(box));
			row->resize(row->width(), st::defaultInputField.heightMin);
			const auto field = Ui::CreateChild<Ui::PasswordInput>(
				row,
				st::defaultInputField,
				std::move(placeholder));
			row->widthValue() | rpl::on_next([=](int width) {
				field->resize(width, field->height());
			}, row->lifetime());
			return field;
		};
		const auto password = addPassword(tr::lng_backup_password());
		const auto confirmation = exporting
			? addPassword(tr::lng_backup_password_repeat())
			: nullptr;
		box->addButton(tr::lng_continue(), [=] {
			const auto text = password->getLastText();
			if (text.isEmpty() || (exporting && text.size() < 8)) {
				password->showError();
				return;
			}
			if (confirmation && confirmation->getLastText() != text) {
				confirmation->showError();
				return;
			}
			box->closeBox();
			done(text);
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		box->setFocusCallback([=] { password->setFocus(); });
	}));
}

void WriteBackupFile(QByteArray bytes, std::shared_ptr<int> operation) {
	if (bytes.isEmpty() || bytes.size() > kMaximumFileSize) {
		ShowError();
		return;
	}
	FileDialog::GetWritePath(Core::App().getFileDialogParent(),
	    tr::lng_backup_export(tr::now),
	    u"Telegram settings (*.tdsettings)"_q,
	    u"telegram-settings.tdsettings"_q,
	    [=](QString &&path) {
		    if (!operation || path.isEmpty()) {
			    return;
		    }
		    auto file = QSaveFile(path);
		    if (!file.open(QIODevice::WriteOnly)
		        || file.write(bytes) != bytes.size() || !file.commit()) {
			    ShowError();
			    return;
		    }
		    Ui::Toast::Show(tr::lng_backup_exported(tr::now));
	    });
}

void SaveBackup(
    QJsonObject document, QString password, std::shared_ptr<int> operation) {
	Ui::Toast::Show(tr::lng_backup_loading(tr::now));
	crl::async([=] {
		const auto plain
		    = QJsonDocument(document).toJson(QJsonDocument::Indented);
		const auto bytes
		    = password.isEmpty() ? plain : Encrypt(plain, password);
		crl::on_main([=] { WriteBackupFile(bytes, operation); });
	});
}

void ShowPreview(QJsonObject document, std::shared_ptr<int> operation) {
	const auto preview = Inspect(document);
	if (!preview.error.isEmpty()) {
		ShowError(preview.error);
		return;
	}
	Ui::show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_backup_import());
		Ui::AddDividerText(
			box->verticalLayout(),
			tr::lng_backup_import_about());
		const auto selected = box->lifetime().make_state<Selection>();
		const auto sections = preview.document.value(u"sections"_q).toObject();
		for (const auto category : kCategories) {
			category.set(*selected,
			    sections.contains(category.key)
			        && QString::fromLatin1(category.key) != u"cloud"_q);
			if (!sections.contains(category.key)) {
				continue;
			}
			const auto label = (*category.label)(tr::now);
			const auto count
			    = ranges::count_if(preview.changes, [&](const auto &key) {
				      return key.startsWith(
				          QString::fromLatin1(category.key) + '/');
			      });
			const auto checkbox = box->addRow(object_ptr<Ui::Checkbox>(box,
			    label + u" (%1)"_q.arg(count),
			    category.get(*selected),
			    st::defaultBoxCheckbox));
			checkbox->checkedChanges()
			    | rpl::on_next(
			        [=](bool value) { category.set(*selected, value); },
			        checkbox->lifetime());
		}
		if (!preview.skipped.isEmpty()) {
			Ui::AddDividerText(box->verticalLayout(),
			    rpl::single(tr::lng_backup_unknown_count(tr::now)
			                + u" (%1)"_q.arg(preview.skipped.size())));
		}
		box->addButton(tr::lng_backup_changes(), [=] {
			auto lines = QStringList();
			for (const auto category : kCategories) {
				const auto count
				    = ranges::count_if(preview.changes, [&](const auto &key) {
					      return key.startsWith(
					          QString::fromLatin1(category.key) + '/');
				      });
				if (count) {
					lines.push_back(
					    (*category.label)(tr::now) + u": %1"_q.arg(count));
				}
			}
			Ui::show(Ui::MakeInformBox(lines.join(u"\n"_q)),
				Ui::LayerOption::KeepOther);
		});
		Ui::AddDividerText(
			box->verticalLayout(),
			tr::lng_backup_cloud_about());
		box->addButton(tr::lng_backup_apply(), [=] {
			auto chosen = preview.document;
			auto filtered = sections;
			for (const auto category : kCategories) {
				if (!(category.get(*selected))) {
					filtered.remove(category.key);
				}
			}
			if (!operation || filtered.isEmpty()) {
				return;
			}
			chosen.insert(u"sections"_q, filtered);
			box->closeBox();
			const auto apply = [=] {
				if (!operation) {
					return;
				}
				auto error = QString();
				if (!Apply(Inspect(chosen), error)) {
					if (HasRecovery()) {
						OfferSettingsRecovery();
					} else {
						ShowError(error);
					}
					return;
				}
				RestartWithSystemSettings(filtered.contains(u"system"_q));
			};
			if (filtered.contains(u"cloud"_q)) {
				Ui::Toast::Show(tr::lng_backup_loading(tr::now));
				Api::SettingsBackup::Restore(
				    filtered.value(u"cloud"_q).toObject(),
				    [=](QStringList errors) {
					    if (errors.isEmpty()) {
						    apply();
						    return;
					    }
					    Ui::show(Ui::MakeConfirmBox({
					        .text = tr::lng_backup_cloud_partial(tr::now)
					                + u"\n"_q + errors.join(u"\n"_q),
					        .confirmed =
					            [=](Fn<void()> close) {
						            close();
						            apply();
					            },
					        .confirmText = tr::lng_backup_apply(tr::now),
					    }));
				    });
			} else {
				apply();
			}
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void OpenBackup(QByteArray bytes, std::shared_ptr<int> operation) {
	const auto parsed
	    = QJsonDocument::fromJson(base::parse::stripComments(bytes));
	if (parsed.isObject() && parsed.object().contains(u"format"_q)) {
		ShowPreview(parsed.object(), operation);
	} else {
		const auto legacy = ReadLegacy(bytes);
		if (legacy.isEmpty()) {
			ShowError();
		} else {
			ShowPreview(legacy, operation);
		}
	}
}

} // namespace

void OfferSettingsRecovery() {
	if (!HasRecovery()) {
		return;
	}
	Ui::show(Box([](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_backup_recover());
		box->setCloseByEscape(false);
		box->setCloseByOutsideClick(false);
		Ui::AddDividerText(
			box->verticalLayout(),
			tr::lng_backup_recovery());
		box->addButton(tr::lng_backup_recover(), [=] {
			auto error = QString();
			if (!Recover(error)) {
				ShowError(error);
				return;
			}
			Core::Restart();
		});
		box->addButton(tr::lng_backup_quit(), [] { Core::Quit(); });
	}));
}

void ExportConfiguration(not_null<Window::SessionController*> controller) {
	const auto operation = BeginOperation();
	if (!operation) {
		return;
	}
	const auto weak = base::make_weak(controller);
	if (HasRecovery()) {
		OfferSettingsRecovery();
		return;
	}
	Ui::show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_backup_export());
		Ui::AddDividerText(
			box->verticalLayout(),
			tr::lng_backup_export_about());
		Ui::AddDividerText(
			box->verticalLayout(),
			tr::lng_backup_device_about());
		const auto selected = box->lifetime().make_state<Selection>();
		for (const auto category : kCategories) {
			const auto checkbox = box->addRow(object_ptr<Ui::Checkbox>(box,
			    (*category.label)(tr::now),
			    category.get(*selected),
			    st::defaultBoxCheckbox));
			checkbox->checkedChanges()
			    | rpl::on_next(
			        [=](bool value) { category.set(*selected, value); },
			        checkbox->lifetime());
		}
		const auto encrypted = box->addRow(object_ptr<Ui::Checkbox>(box,
		    tr::lng_backup_encrypt(tr::now),
		    true,
		    st::defaultBoxCheckbox));
		box->addButton(tr::lng_backup_export(), [=] {
			if (!weak) {
				return;
			}
			const auto document = Collect(*selected);
			if (!document.value(u"sections"_q).toObject().isEmpty()
				&& !Inspect(document).error.isEmpty()) {
				ShowError();
				return;
			}
			if (document.value(u"sections"_q).toObject().isEmpty()
			    && !selected->cloud) {
				return;
			}
			const auto encrypt
			    = encrypted->checked() || selected->proxy || selected->cloud;
			const auto cloud = selected->cloud;
			box->closeBox();
			const auto save = [=](QJsonObject collected) {
				if (encrypt) {
					RequestPassword(true, [=](QString password) {
						SaveBackup(collected, password, operation);
					});
				} else {
					SaveBackup(collected, {}, operation);
				}
			};
			if (cloud) {
				Ui::Toast::Show(tr::lng_backup_loading(tr::now));
				Api::SettingsBackup::Collect([=](QJsonObject data,
				                                 QStringList errors) {
					auto collected = document;
					auto sections = collected.value(u"sections"_q).toObject();
					sections.insert(u"cloud"_q, data);
					collected.insert(u"sections"_q, sections);
					collected.insert(
					    u"cloudErrors"_q, QJsonArray::fromStringList(errors));
					if (!errors.isEmpty()) {
						Ui::show(Ui::MakeConfirmBox({
						    .text = tr::lng_backup_cloud_partial(tr::now)
						            + u"\n"_q + errors.join(u"\n"_q),
						    .confirmed =
						        [=](Fn<void()> &&close) {
							        close();
							        save(collected);
						        },
						    .confirmText = tr::lng_backup_export(tr::now),
						}));
					} else {
						save(collected);
					}
				});
			} else {
				save(document);
			}
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ImportConfiguration(not_null<Window::SessionController*> controller) {
	const auto operation = BeginOperation();
	if (!operation) {
		return;
	}
	const auto weak = base::make_weak(controller);
	if (HasRecovery()) {
		OfferSettingsRecovery();
		return;
	}
	FileDialog::GetOpenPath(Core::App().getFileDialogParent(),
	    tr::lng_backup_import(tr::now),
	    u"Telegram settings (*.tdsettings *.json)"_q,
	    [=](FileDialog::OpenResult &&result) {
		    if (!weak) {
			    return;
		    }
		    auto bytes = std::move(result.remoteContent);
		    if (bytes.isEmpty()) {
			    if (result.paths.isEmpty()) {
				    return;
			    }
			    auto file = QFile(result.paths.front());
			    if (!file.open(QIODevice::ReadOnly)
			        || file.size() > kMaximumFileSize) {
				    ShowError();
				    return;
			    }
			    bytes = file.read(kMaximumFileSize + 1);
		    }
		    if (bytes.size() > kMaximumFileSize) {
			    ShowError();
			    return;
		    }
		    if (bytes.startsWith("TDSETTINGS1\n")) {
			    RequestPassword(false, [=](QString password) {
				    Ui::Toast::Show(tr::lng_backup_loading(tr::now));
				    crl::async([=] {
					    const auto plain = Decrypt(bytes, password);
					    crl::on_main([=] {
						    if (!weak) {
							    return;
						    }
						    if (plain.isEmpty()) {
							    ShowError(
							        tr::lng_backup_password_invalid(tr::now));
						    } else {
							    OpenBackup(plain, operation);
						    }
					    });
				    });
			    });
		    } else {
			    OpenBackup(bytes, operation);
		    }
	    });
}

} // namespace Settings
