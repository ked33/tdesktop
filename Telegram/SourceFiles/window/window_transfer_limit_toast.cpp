#include "window/window_transfer_limit_toast.h"

#include "chat_helpers/compose/compose_show.h"
#include "core/application.h"
#include "core/enhanced_settings.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/view/media_view_overlay_widget.h"
#include "mtproto/mtp_instance.h"
#include "ui/toast/toast.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

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

	const not_null<Main::Session*> _session;
	base::weak_ptr<Ui::Toast::Instance> _toast;
	MTP::TransferLimitInfo _last;
	crl::time _lastShown = 0;
	rpl::lifetime _lifetime;

};

TransferLimitToast::TransferLimitToast(not_null<Main::Session*> session)
: _session(session) {
}

TransferLimitToast::~TransferLimitToast() {
	if (const auto toast = _toast.get()) {
		toast->hide();
	}
}

void TransferLimitToast::start() {
	EnhancedSettings::DownloadLimitToastsChanges(
	) | rpl::on_next([=](bool enabled) {
		if (!enabled && !_last.upload) {
			if (const auto toast = _toast.get()) {
				toast->hide();
			}
		}
	}, _lifetime);
	_session->mtp().transferLimits(
	) | rpl::on_next([=](const MTP::TransferLimitInfo &info) {
		crl::on_main(this, [=] { show(info); });
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
	if (info.repeated
		|| (!info.upload && !EnhancedSettings::DownloadLimitToastsEnabled())) {
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

} // namespace

void SetupTransferLimitToasts(not_null<Main::Session*> session) {
	const auto toast = session->lifetime().make_state<TransferLimitToast>(session);
	toast->start();
}

} // namespace Window
