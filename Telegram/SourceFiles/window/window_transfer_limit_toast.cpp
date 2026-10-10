#include "window/window_transfer_limit_toast.h"

#include "base/flat_map.h"
#include "base/timer.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/application.h"
#include "core/enhanced_settings.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/view/media_view_overlay_widget.h"
#include "mtproto/mtp_instance.h"
#include "storage/download_manager_mtproto.h"
#include "ui/toast/toast.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "window/window_transfer_limit_countdown.h"

#include <algorithm>
#include <QtCore/QPointer>

#include "styles/style_window.h"

namespace Window {
namespace {

constexpr auto kTransferLimitToastDuration = crl::time(4000);
constexpr auto kTransferLimitToastCoalesce = crl::time(1000);

class TransferLimitToast final : public base::has_weak_ptr {
public:
	explicit TransferLimitToast(not_null<Main::Session*> session);
	~TransferLimitToast();
	void start();

private:
	[[nodiscard]] std::shared_ptr<ChatHelpers::Show> resolveShow() const;
	void show(const MTP::TransferLimitInfo &info);
	void pruneCountdowns();
	[[nodiscard]] TextWithEntities thumbnailRetryText(crl::time now) const;
	void updateCountdown();
	void hideCountdown();
	void modeChanged();

	const not_null<Main::Session*> _session;
	base::weak_ptr<Ui::Toast::Instance> _toast;
	base::weak_ptr<Ui::Toast::Instance> _countdownToast;
	QPointer<Ui::FlatLabel> _countdownLabel;
	base::flat_map<mtpRequestId, MTP::TransferLimitInfo> _countdowns;
	base::flat_map<Storage::DownloadMtprotoTask*, Storage::ThumbnailRetryInfo>
		_thumbnailRetries;
	base::Timer _countdownTimer;
	MTP::TransferLimitInfo _last;
	MTP::TransferLimitInfo _lastDownloadLimit;
	crl::time _lastShown = 0;
	rpl::lifetime _lifetime;

};

TransferLimitToast::TransferLimitToast(not_null<Main::Session*> session)
: _session(session)
, _countdownTimer([=] { updateCountdown(); }) {
}

TransferLimitToast::~TransferLimitToast() {
	hideCountdown();
	if (const auto toast = _toast.get()) {
		toast->hide();
	}
}

void TransferLimitToast::start() {
	EnhancedSettings::DownloadLimitToastsChanges(
	) | rpl::on_next([=](bool enabled) {
		if (!enabled) {
			_countdowns.clear();
			hideCountdown();
			if (!_last.upload) {
				if (const auto toast = _toast.get()) {
					toast->hide();
				}
				_toast = nullptr;
			}
			updateCountdown();
		}
	}, _lifetime);
	EnhancedSettings::DownloadLimitToastModeChanges(
	) | rpl::on_next([=] { modeChanged(); }, _lifetime);
	Core::App().passcodeLockChanges(
	) | rpl::on_next([=](bool locked) {
		if (locked) {
			hideCountdown();
		} else {
			updateCountdown();
		}
	}, _lifetime);
	_session->mtp().transferLimits(
	) | rpl::on_next([=](const MTP::TransferLimitInfo &info) {
		crl::on_main(this, [=] { show(info); });
	}, _lifetime);
	_session->downloader().thumbnailRetries(
	) | rpl::on_next([=](const Storage::ThumbnailRetryInfo &info) {
		crl::on_main(this, [=] {
			if (info.attempt) {
				_thumbnailRetries[info.task] = info;
			} else {
				_thumbnailRetries.remove(info.task);
			}
			updateCountdown();
		});
	}, _lifetime);
}

std::shared_ptr<ChatHelpers::Show> TransferLimitToast::resolveShow() const {
	if (Core::App().passcodeLocked()) {
		return nullptr;
	}
	const auto overlay = Core::App().mediaView();
	if (overlay && !overlay->isHidden() && !overlay->isMinimized()) {
		const auto show = overlay->uiShow();
		if (show->valid() && &show->session() == _session) {
			return show;
		}
	}
	const auto active = Core::App().activeWindow();
	const auto controller = active ? active->sessionController() : nullptr;
	if (controller && &controller->session() == _session) {
		return controller->uiShow();
	}
	for (const auto window : _session->windows()) {
		if (window->isPrimary()) {
			return window->uiShow();
		}
	}
	return _session->windows().empty()
		? nullptr
		: _session->windows().front()->uiShow();
}

void TransferLimitToast::show(const MTP::TransferLimitInfo &info) {
	if (!info.upload && !EnhancedSettings::DownloadLimitToastsEnabled()) {
		return;
	}
	if (!info.upload) {
		_lastDownloadLimit = info;
	}
	if (!info.upload
		&& (EnhancedSettings::DownloadLimitToastsMode()
			== EnhancedSettings::DownloadLimitToastMode::Countdown
			|| !_thumbnailRetries.empty())
		&& info.waitSeconds >= 0) {
		_countdowns[info.requestId] = info;
		updateCountdown();
		return;
	}
	if (info.repeated) {
		return;
	}
	const auto now = crl::now();
	if (_toast
		&& now - _lastShown < kTransferLimitToastCoalesce
		&& _last.dcId == info.dcId
		&& _last.upload == info.upload
		&& _last.type == info.type) {
		return;
	}
	const auto show = resolveShow();
	if (!show || !show->valid()) {
		return;
	}
	if (const auto toast = _toast.get()) {
		toast->hide();
	}
	_last = info;
	_lastShown = now;
	_toast = show->showToast({
		.title = (info.upload
			? tr::lng_transfer_limit_upload
			: tr::lng_transfer_limit_download)(tr::now),
		.text = (info.waitSeconds >= 0)
			? tr::lng_transfer_limit_wait(
				tr::now,
				lt_seconds,
				tr::marked(QString::number(info.waitSeconds)),
				tr::marked)
			: tr::lng_transfer_limit_wait_unknown(tr::now, tr::marked),
		.attach = RectPart::Top,
		.duration = kTransferLimitToastDuration,
	});
	LOG(("Transfer limit toast: direction=%1 error=%2 wait_seconds=%3 "
		"dc=%4 request=%5")
		.arg(info.upload ? u"upload"_q : u"download"_q)
		.arg(info.type)
		.arg(info.waitSeconds)
		.arg(info.dcId)
		.arg(info.requestId));
}

void TransferLimitToast::hideCountdown() {
	_countdownTimer.cancel();
	if (const auto toast = _countdownToast.get()) {
		toast->hide();
	}
	_countdownToast = nullptr;
	_countdownLabel = nullptr;
}

void TransferLimitToast::pruneCountdowns() {
	const auto now = crl::now();
	for (auto i = _countdowns.begin(); i != _countdowns.end();) {
		if (i->second.retryAt <= now || !_session->mtp().hasCallback(i->first)) {
			i = _countdowns.erase(i);
		} else {
			++i;
		}
	}
}

TextWithEntities TransferLimitToast::thumbnailRetryText(crl::time now) const {
	const auto nearest = std::min_element(
		_thumbnailRetries.begin(),
		_thumbnailRetries.end(),
		[=](const auto &a, const auto &b) {
			const auto waitingA = a.second.retryAt > now;
			const auto waitingB = b.second.retryAt > now;
			return (waitingA != waitingB)
				? waitingA
				: a.second.retryAt < b.second.retryAt;
		});
	const auto &info = nearest->second;
	const auto amount = QString::number(qlonglong(_thumbnailRetries.size()));
	const auto attempt = QString::number(info.attempt);
	const auto total = QString::number(info.total);
	const auto seconds = TransferLimitRemainingSeconds(info.retryAt, now);
	return tr::marked(seconds
		? tr::lng_thumbnail_retry_wait(
			tr::now,
			lt_amount,
			amount,
			lt_ready,
			attempt,
			lt_total,
			total,
			lt_seconds,
			QString::number(qlonglong(seconds)))
		: tr::lng_thumbnail_retry_active(
			tr::now,
			lt_amount,
			amount,
			lt_ready,
			attempt,
			lt_total,
			total));
}

void TransferLimitToast::updateCountdown() {
	pruneCountdowns();
	const auto showLimit = !_countdowns.empty()
		&& EnhancedSettings::DownloadLimitToastsEnabled()
		&& (EnhancedSettings::DownloadLimitToastsMode()
			== EnhancedSettings::DownloadLimitToastMode::Countdown
			|| !_thumbnailRetries.empty());
	if (!showLimit && _thumbnailRetries.empty()) {
		hideCountdown();
		return;
	}
	const auto now = crl::now();
	auto text = TextWithEntities();
	auto tick = crl::time(1000);
	auto limit = MTP::TransferLimitInfo();
	if (showLimit) {
		const auto latest = std::max_element(
			_countdowns.begin(),
			_countdowns.end(),
			[](const auto &a, const auto &b) {
				return a.second.retryAt < b.second.retryAt;
			});
		limit = latest->second;
		const auto until = limit.retryAt;
		text = tr::lng_transfer_limit_countdown(
			tr::now,
			lt_seconds,
			tr::marked(QString::number(qlonglong(
				TransferLimitRemainingSeconds(until, now)))),
			tr::marked);
		tick = std::min(tick, TransferLimitNextTick(until, now));
	}
	if (!_thumbnailRetries.empty()) {
		if (!text.empty()) {
			text.append(u"\n"_q);
		}
		text.append(thumbnailRetryText(now));
		for (const auto &[task, info] : _thumbnailRetries) {
			if (info.retryAt > now) {
				tick = std::min(tick, TransferLimitNextTick(info.retryAt, now));
			}
		}
	}
	const auto show = resolveShow();
	if (!show || !show->valid()) {
		hideCountdown();
		if (!Core::App().passcodeLocked()) {
			_countdownTimer.callOnce(
				std::max(tick, crl::time(1)),
				Qt::PreciseTimer);
		}
		return;
	}
	const auto toast = _countdownToast.get();
	if (!toast
		|| !_countdownLabel
		|| toast->widget()->parentWidget() != show->toastParent().get()) {
		hideCountdown();
		if (!_last.upload) {
			if (const auto fixed = _toast.get()) {
				fixed->hide();
			}
			_toast = nullptr;
		}
		auto content = object_ptr<Ui::FlatLabel>(
			nullptr,
			st::transferLimitToastLabel);
		content->setMarkedText(text);
		_countdownLabel = content.data();
		_countdownToast = Ui::Toast::Show(show->toastParent(), {
			.content = std::move(content),
			.attach = RectPart::Top,
			.infinite = true,
		});
		if (showLimit) {
			LOG(("Transfer limit toast: direction=download error=%1 "
				"wait_seconds=%2 dc=%3 request=%4 mode=countdown")
				.arg(limit.type)
				.arg(qlonglong(TransferLimitRemainingSeconds(limit.retryAt, now)))
				.arg(limit.dcId)
				.arg(limit.requestId));
		}
	} else {
		_countdownLabel->setMarkedText(text);
	}
	_countdownTimer.callOnce(
		std::max(tick, crl::time(1)),
		Qt::PreciseTimer);
}

void TransferLimitToast::modeChanged() {
	_countdowns[_lastDownloadLimit.requestId] = _lastDownloadLimit;
	pruneCountdowns();
	hideCountdown();
	if (_countdowns.empty()
		|| !EnhancedSettings::DownloadLimitToastsEnabled()) {
		updateCountdown();
		return;
	}
	const auto latest = std::max_element(
		_countdowns.begin(),
		_countdowns.end(),
		[](const auto &a, const auto &b) {
			return a.second.retryAt < b.second.retryAt;
		});
	auto info = latest->second;
	if (!_last.upload) {
		if (const auto toast = _toast.get()) {
			toast->hide();
		}
		_toast = nullptr;
	}
	info.repeated = false;
	info.waitSeconds = int(TransferLimitRemainingSeconds(
		info.retryAt,
		crl::now()));
	show(info);
}

} // namespace

void SetupTransferLimitToasts(not_null<Main::Session*> session) {
	const auto toast = session->lifetime().make_state<TransferLimitToast>(session);
	toast->start();
}

} // namespace Window
