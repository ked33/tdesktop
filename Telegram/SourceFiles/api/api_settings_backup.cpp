#include "api/api_settings_backup.h"

#include "apiwrap.h"
#include "base/timer.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "mtproto/type_utils.h"

#include <QtCore/QJsonArray>
#include <cmath>
#include <crl/crl_on_main.h>

namespace Api::SettingsBackup {
namespace {

constexpr auto kStepCount = 22;

bool Busy = false;

struct Job {
	QJsonObject result;
	QStringList errors;
	std::vector<base::weak_ptr<Main::Session>> sessions;
	std::unique_ptr<MTP::Sender> sender;
	base::Timer timer;
	Fn<void(QJsonObject, QStringList)> done;
	int account = 0;
	int step = 0;
	bool restoring = false;
	bool finished = false;
};

[[nodiscard]] auto PrivacyKeys() {
	return std::array{
		MTPInputPrivacyKey(MTP_inputPrivacyKeyPhoneNumber()),
		MTPInputPrivacyKey(MTP_inputPrivacyKeyAddedByPhone()),
		MTPInputPrivacyKey(MTP_inputPrivacyKeyStatusTimestamp()),
		MTPInputPrivacyKey(MTP_inputPrivacyKeyPhoneCall()),
		MTPInputPrivacyKey(MTP_inputPrivacyKeyChatInvite()),
		MTPInputPrivacyKey(MTP_inputPrivacyKeyPhoneP2P()),
		MTPInputPrivacyKey(MTP_inputPrivacyKeyForwards()),
		MTPInputPrivacyKey(MTP_inputPrivacyKeyProfilePhoto()),
		MTPInputPrivacyKey(MTP_inputPrivacyKeyVoiceMessages()),
		MTPInputPrivacyKey(MTP_inputPrivacyKeyAbout()),
		MTPInputPrivacyKey(MTP_inputPrivacyKeyBirthday()),
		MTPInputPrivacyKey(MTP_inputPrivacyKeyStarGiftsAutoSave()),
		MTPInputPrivacyKey(MTP_inputPrivacyKeyNoPaidMessages()),
		MTPInputPrivacyKey(MTP_inputPrivacyKeySavedMusic()),
	};
}

[[nodiscard]] QString StepName(int step) {
	if (step < 14) {
		return u"privacy/%1"_q.arg(PrivacyKeys()[step].type(), 0, 16);
	}
	if (step < 17) {
		return u"notifications/%1"_q.arg(step - 14);
	}
	return step == 17   ? u"globalPrivacy"_q
	       : step == 18 ? u"folders"_q
	       : step == 19 ? u"reactionNotifications"_q
	       : step == 20 ? u"defaultMessageLifetime"_q
	                    : u"notificationExceptions"_q;
}

template <typename T> [[nodiscard]] QJsonArray Encode(const T &value) {
	auto words = mtpBuffer();
	value.write(words);
	auto result = QJsonArray();
	for (const auto word : words) {
		result.push_back(double(uint32(word)));
	}
	return result;
}

template <typename T>
[[nodiscard]] std::optional<T> Decode(const QJsonValue &value) {
	if (!value.isArray() || value.toArray().size() > 100000) {
		return {};
	}
	auto words = mtpBuffer();
	for (const auto entry : value.toArray()) {
		const auto number = entry.toDouble(-1);
		if (!entry.isDouble() || number < 0 || number > UINT32_MAX
		    || std::trunc(number) != number) {
			return {};
		}
		words.push_back(mtpPrime(uint32(number)));
	}
	if (words.empty()) {
		return {};
	}
	auto from = words.constData();
	const auto end = from + words.size();
	auto parsed = T();
	if (!parsed.read(from, end) || from != end) {
		return {};
	}
	return parsed;
}

[[nodiscard]] MTPInputNotifyPeer NotifyPeer(int index) {
	if (index == 0) {
		return MTP_inputNotifyUsers();
	}
	if (index == 1) {
		return MTP_inputNotifyChats();
	}
	return MTP_inputNotifyBroadcasts();
}

[[nodiscard]] MTPVector<MTPInputPrivacyRule> InputRules(
    not_null<Main::Session*> session, const MTPVector<MTPPrivacyRule> &rules) {
	auto result = QVector<MTPInputPrivacyRule>();
	const auto users = [&](const MTPVector<MTPlong> &ids) {
		auto list = QVector<MTPInputUser>();
		for (const auto &id : ids.v) {
			list.push_back(session->data().user(UserId(id.v))->inputUser());
		}
		return MTP_vector<MTPInputUser>(list);
	};
	for (const auto &rule : rules.v) {
		rule.match(
		    [&](const MTPDprivacyValueAllowAll &) {
			    result.push_back(MTP_inputPrivacyValueAllowAll());
		    },
		    [&](const MTPDprivacyValueAllowContacts &) {
			    result.push_back(MTP_inputPrivacyValueAllowContacts());
		    },
		    [&](const MTPDprivacyValueAllowCloseFriends &) {
			    result.push_back(MTP_inputPrivacyValueAllowCloseFriends());
		    },
		    [&](const MTPDprivacyValueAllowPremium &) {
			    result.push_back(MTP_inputPrivacyValueAllowPremium());
		    },
		    [&](const MTPDprivacyValueAllowBots &) {
			    result.push_back(MTP_inputPrivacyValueAllowBots());
		    },
		    [&](const MTPDprivacyValueDisallowBots &) {
			    result.push_back(MTP_inputPrivacyValueDisallowBots());
		    },
		    [&](const MTPDprivacyValueDisallowAll &) {
			    result.push_back(MTP_inputPrivacyValueDisallowAll());
		    },
		    [&](const MTPDprivacyValueDisallowContacts &) {
			    result.push_back(MTP_inputPrivacyValueDisallowContacts());
		    },
		    [&](const MTPDprivacyValueAllowUsers &v) {
			    result.push_back(
			        MTP_inputPrivacyValueAllowUsers(users(v.vusers())));
		    },
		    [&](const MTPDprivacyValueDisallowUsers &v) {
			    result.push_back(
			        MTP_inputPrivacyValueDisallowUsers(users(v.vusers())));
		    },
		    [&](const MTPDprivacyValueAllowChatParticipants &v) {
			    result.push_back(
			        MTP_inputPrivacyValueAllowChatParticipants(v.vchats()));
		    },
		    [&](const MTPDprivacyValueDisallowChatParticipants &v) {
			    result.push_back(
			        MTP_inputPrivacyValueDisallowChatParticipants(v.vchats()));
		    });
	}
	return MTP_vector<MTPInputPrivacyRule>(result);
}

[[nodiscard]] MTPInputPeerNotifySettings InputNotify(
    const MTPPeerNotifySettings &value) {
	const auto &data = value.data();
	using Flag = MTPDinputPeerNotifySettings::Flag;
	auto flags = MTPDinputPeerNotifySettings::Flags();
	if (data.vshow_previews()) {
		flags |= Flag::f_show_previews;
	}
	if (data.vsilent()) {
		flags |= Flag::f_silent;
	}
	if (data.vmute_until()) {
		flags |= Flag::f_mute_until;
	}
	if (data.vother_sound()) {
		flags |= Flag::f_sound;
	}
	if (data.vstories_muted()) {
		flags |= Flag::f_stories_muted;
	}
	if (data.vstories_hide_sender()) {
		flags |= Flag::f_stories_hide_sender;
	}
	if (data.vstories_other_sound()) {
		flags |= Flag::f_stories_sound;
	}
	const auto fallbackBool = MTPBool(MTP_bool(false));
	const auto fallbackSound = MTPNotificationSound(
		MTP_notificationSoundDefault());
	return MTP_inputPeerNotifySettings(
		MTP_flags(flags),
		(data.vshow_previews() ? *data.vshow_previews() : fallbackBool),
		(data.vsilent() ? *data.vsilent() : fallbackBool),
		(data.vmute_until() ? *data.vmute_until() : MTP_int(0)),
		(data.vother_sound() ? *data.vother_sound() : fallbackSound),
		(data.vstories_muted() ? *data.vstories_muted() : fallbackBool),
		(data.vstories_hide_sender()
			? *data.vstories_hide_sender()
			: fallbackBool),
		(data.vstories_other_sound()
			? *data.vstories_other_sound()
			: fallbackSound));
}

void Next(const std::shared_ptr<Job> &job);
void RestoreFolders(
    const std::shared_ptr<Job> &job, QJsonArray folders, int index);
void RestoreExceptions(
    const std::shared_ptr<Job> &job, QJsonArray entries, int index);

void Finish(const std::shared_ptr<Job> &job) {
	if (job->finished) {
		return;
	}
	job->finished = true;
	Busy = false;
	job->timer.cancel();
	auto done = std::move(job->done);
	crl::on_main([job] { job->sender.reset(); });
	done(std::move(job->result), std::move(job->errors));
}

void Advance(
    const std::shared_ptr<Job> &job, QJsonValue value = QJsonValue::Undefined) {
	if (job->finished) {
		return;
	}
	job->timer.cancel();
	if (!job->restoring && !value.isUndefined()) {
		const auto session = job->sessions[job->account].get();
		if (session) {
			const auto id = QString::number(session->uniqueId());
			auto account = job->result.value(id).toObject();
			account.insert(StepName(job->step), value);
			job->result.insert(id, account);
		}
	}
	++job->step;
	crl::on_main([job] { Next(job); });
}

void Failed(const std::shared_ptr<Job> &job, const QString &reason) {
	if (job->finished) {
		return;
	}
	job->errors.push_back(u"account %1 / %2: %3"_q.arg(job->account + 1)
	        .arg(StepName(job->step), reason));
	Advance(job);
}

void Next(const std::shared_ptr<Job> &job) {
	if (job->finished) {
		return;
	}
	if (job->step == kStepCount) {
		job->step = 0;
		++job->account;
		job->sender.reset();
	}
	if (job->account == job->sessions.size()) {
		Finish(job);
		return;
	}
	const auto session = job->sessions[job->account].get();
	if (!session) {
		job->step = kStepCount;
		crl::on_main([job] { Next(job); });
		return;
	}
	if (!job->sender) {
		job->sender = std::make_unique<MTP::Sender>(&session->mtp());
		session->lifetime().add([weak = std::weak_ptr<Job>(job)] {
			if (const auto locked = weak.lock(); locked && !locked->finished) {
				locked->sender.reset();
				locked->errors.push_back(
				    u"Account signed out during cloud operation."_q);
				Finish(locked);
			}
		});
	}
	const auto id = QString::number(session->uniqueId());
	const auto value
	    = job->result.value(id).toObject().value(StepName(job->step));
	if (job->restoring && value.isUndefined()) {
		Advance(job);
		return;
	}
	job->timer.setCallback([weak = std::weak_ptr<Job>(job)] {
		if (const auto locked = weak.lock()) {
			locked->errors.push_back(
			    u"Cloud operation timed out. Some updates may have reached the server."_q);
			Finish(locked);
			locked->sender.reset();
		}
	});
	job->timer.callOnce(45000);
	const auto fail
	    = [job](const MTP::Error &error) { Failed(job, error.type()); };
	const auto boolDone = [job](const MTPBool &result) {
		if (mtpIsTrue(result)) {
			Advance(job);
		} else {
			Failed(job, u"Server rejected the update."_q);
		}
	};
	if (job->step < 14) {
		const auto key = PrivacyKeys()[job->step];
		if (job->restoring) {
			job->sender
			    ->request(MTPaccount_SetPrivacy(
			        key, *Decode<MTPVector<MTPInputPrivacyRule>>(value)))
			    .done([job] { Advance(job); })
			    .fail(fail)
			    .handleAllErrors()
			    .send();
		} else {
			job->sender->request(MTPaccount_GetPrivacy(key))
			    .done([job](const MTPaccount_PrivacyRules &v) {
				    const auto session = job->sessions[job->account].get();
				    if (!session) {
					    Failed(job, u"Account signed out"_q);
					    return;
				    }
				    session->data().processUsers(v.data().vusers());
				    session->data().processChats(v.data().vchats());
				    Advance(
				        job, Encode(InputRules(session, v.data().vrules())));
			    })
			    .fail(fail)
			    .handleAllErrors()
			    .send();
		}
	} else if (job->step < 17) {
		const auto peer = NotifyPeer(job->step - 14);
		if (job->restoring) {
			job->sender
			    ->request(MTPaccount_UpdateNotifySettings(
			        peer, *Decode<MTPInputPeerNotifySettings>(value)))
			    .done(boolDone)
			    .fail(fail)
			    .handleAllErrors()
			    .send();
		} else {
			job->sender->request(MTPaccount_GetNotifySettings(peer))
			    .done([job](const MTPPeerNotifySettings &v) {
				    Advance(job, Encode(InputNotify(v)));
			    })
			    .fail(fail)
			    .handleAllErrors()
			    .send();
		}
	} else if (job->step == 17) {
		if (job->restoring) {
			job->sender
			    ->request(MTPaccount_SetGlobalPrivacySettings(
			        *Decode<MTPGlobalPrivacySettings>(value)))
			    .done([job] { Advance(job); })
			    .fail(fail)
			    .handleAllErrors()
			    .send();
		} else {
			job->sender->request(MTPaccount_GetGlobalPrivacySettings())
			    .done([job](const MTPGlobalPrivacySettings &v) {
				    Advance(job, Encode(v));
			    })
			    .fail(fail)
			    .handleAllErrors()
			    .send();
		}
	} else if (job->step == 18 && job->restoring) {
		RestoreFolders(job, value.toArray(), 0);
	} else if (job->step == 18) {
		job->sender->request(MTPmessages_GetDialogFilters())
		    .done([job](const MTPmessages_DialogFilters &v) {
			    auto folders = QJsonArray();
			    for (const auto &filter : v.data().vfilters().v) {
				    folders.push_back(Encode(filter));
			    }
			    Advance(job, folders);
		    })
		    .fail(fail)
		    .handleAllErrors()
		    .send();
	} else if (job->step == 19) {
		if (job->restoring) {
			job->sender
			    ->request(MTPaccount_SetReactionsNotifySettings(
			        *Decode<MTPReactionsNotifySettings>(value)))
			    .done([job] { Advance(job); })
			    .fail(fail)
			    .handleAllErrors()
			    .send();
		} else {
			job->sender->request(MTPaccount_GetReactionsNotifySettings())
			    .done([job](const MTPReactionsNotifySettings &v) {
				    Advance(job, Encode(v));
			    })
			    .fail(fail)
			    .handleAllErrors()
			    .send();
		}
	} else if (job->step == 20) {
		if (job->restoring) {
			job->sender
			    ->request(MTPmessages_SetDefaultHistoryTTL(
			        Decode<MTPDefaultHistoryTTL>(value)->data().vperiod()))
			    .done(boolDone)
			    .fail(fail)
			    .handleAllErrors()
			    .send();
		} else {
			job->sender->request(MTPmessages_GetDefaultHistoryTTL())
			    .done([job](const MTPDefaultHistoryTTL &v) {
				    Advance(job, Encode(v));
			    })
			    .fail(fail)
			    .handleAllErrors()
			    .send();
		}
	} else if (job->restoring) {
		RestoreExceptions(job, value.toArray(), 0);
	} else {
		job->sender
		    ->request(MTPaccount_GetNotifyExceptions(
		        MTP_flags(
		            MTPaccount_GetNotifyExceptions::Flag::f_compare_sound
		            | MTPaccount_GetNotifyExceptions::Flag::f_compare_stories),
		        MTP_inputNotifyUsers()))
		    .done([job](const MTPUpdates &updates) {
			    const auto session = job->sessions[job->account].get();
			    if (!session) {
				    Failed(job, u"Account signed out"_q);
				    return;
			    }
			    auto entries = QJsonArray();
			    const auto consume = [&](const auto &data) {
				    session->data().processUsers(data.vusers());
				    session->data().processChats(data.vchats());
				    for (const auto &update : data.vupdates().v) {
					    if (update.type() != mtpc_updateNotifySettings) {
						    continue;
					    }
					    const auto &v = update.c_updateNotifySettings();
					    if (v.vpeer().type() != mtpc_notifyPeer
					        && v.vpeer().type() != mtpc_notifyForumTopic) {
						    continue;
					    }
					    const auto peer = v.vpeer().match(
					        [&](const MTPDnotifyPeer &v) -> MTPInputNotifyPeer {
						        return MTP_inputNotifyPeer(session->data()
						                .peer(peerFromMTP(v.vpeer()))
						                ->input());
					        },
					        [&](const MTPDnotifyForumTopic &v)
					            -> MTPInputNotifyPeer {
						        return MTP_inputNotifyForumTopic(
						            session->data()
						                .peer(peerFromMTP(v.vpeer()))
						                ->input(),
						            v.vtop_msg_id());
					        },
					        [](const auto &) -> MTPInputNotifyPeer {
						        return MTP_inputNotifyUsers();
					        });
					    entries.push_back(QJsonObject{
					        { u"peer"_q, Encode(peer) },
					        { u"settings"_q,
					            Encode(InputNotify(v.vnotify_settings())) },
					    });
				    }
			    };
			    auto supported = true;
			    updates.match([&](const MTPDupdates &v) { consume(v); },
			        [&](const MTPDupdatesCombined &v) { consume(v); },
			        [&](const auto &) { supported = false; });
			    if (supported) {
				    Advance(job, entries);
			    } else {
				    Failed(job, u"Unsupported notification response."_q);
			    }
		    })
		    .fail(fail)
		    .handleAllErrors()
		    .send();
	}
}

void RestoreFolders(
    const std::shared_ptr<Job> &job, QJsonArray folders, int index) {
	if (job->finished) {
		return;
	}
	if (index == folders.size()) {
		auto order = QVector<MTPint>();
		for (const auto &entry : folders) {
			const auto filter = *Decode<MTPDialogFilter>(entry);
			if (filter.type() == mtpc_dialogFilter) {
				order.push_back(filter.c_dialogFilter().vid());
			} else if (filter.type() == mtpc_dialogFilterDefault) {
				order.push_back(MTP_int(0));
			}
		}
		if (order.isEmpty()) {
			Advance(job);
			return;
		}
		job->sender
		    ->request(
		        MTPmessages_UpdateDialogFiltersOrder(MTP_vector<MTPint>(order)))
		    .done([job](const MTPBool &result) {
			    if (mtpIsTrue(result)) {
				    Advance(job);
			    } else {
				    Failed(job, u"Folder order was not restored."_q);
			    }
		    })
		    .fail([job](const MTP::Error &error) { Failed(job, error.type()); })
		    .handleAllErrors()
		    .send();
		return;
	}
	const auto filter = *Decode<MTPDialogFilter>(folders[index]);
	if (filter.type() != mtpc_dialogFilter) {
		if (filter.type() != mtpc_dialogFilterDefault) {
			job->errors.push_back(u"Shared folder was skipped."_q);
		}
		RestoreFolders(job, std::move(folders), index + 1);
		return;
	}
	job->sender
	    ->request(MTPmessages_UpdateDialogFilter(
	        MTP_flags(MTPmessages_UpdateDialogFilter::Flag::f_filter),
	        filter.c_dialogFilter().vid(),
	        filter))
	    .done([job, folders, index](const MTPBool &result) {
		    if (!mtpIsTrue(result)) {
			    job->errors.push_back(u"Folder was not restored."_q);
		    }
		    RestoreFolders(job, folders, index + 1);
	    })
	    .fail([job](const MTP::Error &error) { Failed(job, error.type()); })
	    .handleAllErrors()
	    .send();
}

void RestoreExceptions(
    const std::shared_ptr<Job> &job, QJsonArray entries, int index) {
	if (job->finished) {
		return;
	}
	if (index == entries.size()) {
		Advance(job);
		return;
	}
	const auto entry = entries[index].toObject();
	job->sender
	    ->request(MTPaccount_UpdateNotifySettings(
	        *Decode<MTPInputNotifyPeer>(entry.value(u"peer"_q)),
	        *Decode<MTPInputPeerNotifySettings>(entry.value(u"settings"_q))))
	    .done([job, entries, index](const MTPBool &result) {
		    if (!mtpIsTrue(result)) {
			    job->errors.push_back(
			        u"Notification exception was not restored."_q);
		    }
		    RestoreExceptions(job, entries, index + 1);
	    })
	    .fail([job, entries, index](const MTP::Error &error) {
		    job->errors.push_back(error.type());
		    RestoreExceptions(job, entries, index + 1);
	    })
	    .handleAllErrors()
	    .send();
}

void Start(
    QJsonObject data, bool restoring, Fn<void(QJsonObject, QStringList)> done) {
	if (Busy) {
		done({}, { u"Another cloud settings operation is running."_q });
		return;
	}
	Busy = true;
	auto job = std::make_shared<Job>();
	job->restoring = restoring;
	job->result
	    = restoring ? data.value(u"accounts"_q).toObject() : QJsonObject();
	job->done = std::move(done);
	for (const auto &entry : Core::App().domain().accounts()) {
		if (entry.account->sessionExists()) {
			job->sessions.push_back(base::make_weak(&entry.account->session()));
		}
	}
	if (restoring) {
		for (const auto &id : job->result.keys()) {
			const auto found
			    = ranges::any_of(job->sessions, [&](const auto &weak) {
				      return weak && QString::number(weak->uniqueId()) == id;
			      });
			if (!found) {
				job->errors.push_back(
				    u"A backed-up account is not signed in."_q);
			}
		}
	}
	Next(job);
}

} // namespace

void Collect(Fn<void(QJsonObject, QStringList)> done) {
	Start({},
	    false,
	    [done = std::move(done)](QJsonObject accounts, QStringList errors) {
		    done(
		        QJsonObject{
		            { u"layer"_q, int(MTP::details::kCurrentLayer) },
		            { u"accounts"_q, accounts },
		        },
		        std::move(errors));
	    });
}

bool Validate(const QJsonObject &data) {
	if (data.value(u"layer"_q) != int(MTP::details::kCurrentLayer)
	    || !data.value(u"accounts"_q).isObject()) {
		return false;
	}
	const auto accounts = data.value(u"accounts"_q).toObject();
	if (accounts.size() > Main::Domain::kMaxAccounts) {
		return false;
	}
	for (auto i = accounts.begin(); i != accounts.end(); ++i) {
		if (!i->isObject()) {
			return false;
		}
		const auto account = i->toObject();
		for (auto step = 0; step != kStepCount; ++step) {
			const auto value = account.value(StepName(step));
			if (value.isUndefined()) {
				continue;
			}
			if (step < 14 && !Decode<MTPVector<MTPInputPrivacyRule>>(value)) {
				return false;
			}
			if (step >= 14 && step < 17
			    && !Decode<MTPInputPeerNotifySettings>(value)) {
				return false;
			}
			if (step == 17 && !Decode<MTPGlobalPrivacySettings>(value)) {
				return false;
			}
			if (step == 19 && !Decode<MTPReactionsNotifySettings>(value)) {
				return false;
			}
			if (step == 20) {
				const auto ttl = Decode<MTPDefaultHistoryTTL>(value);
				if (!ttl || ttl->data().vperiod().v < 0) {
					return false;
				}
			}
			if (step == 21) {
				if (!value.isArray() || value.toArray().size() > 10000) {
					return false;
				}
				for (const auto &entry : value.toArray()) {
					if (!entry.isObject()) {
						return false;
					}
					const auto object = entry.toObject();
					if (!Decode<MTPInputNotifyPeer>(object.value(u"peer"_q))
					    || !Decode<MTPInputPeerNotifySettings>(
					        object.value(u"settings"_q))) {
						return false;
					}
				}
			}
			if (step == 18) {
				if (!value.isArray() || value.toArray().size() > 100) {
					return false;
				}
				for (const auto &filter : value.toArray()) {
					if (!Decode<MTPDialogFilter>(filter)) {
						return false;
					}
				}
			}
		}
	}
	return true;
}

void Restore(QJsonObject data, Fn<void(QStringList)> done) {
	if (!Validate(data)) {
		done({ u"Invalid cloud settings."_q });
		return;
	}
	Start(std::move(data),
	    true,
	    [done = std::move(done)](
	        QJsonObject, QStringList errors) { done(std::move(errors)); });
}

} // namespace Api::SettingsBackup
