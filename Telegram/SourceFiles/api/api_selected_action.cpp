
#include "api/api_selected_action.h"
#include "data/data_peer.h"
#include "base/call_delayed.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"
#include "mtproto/mtp_instance.h"
#include "ui/layers/show.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "styles/style_window.h"

#include <map>

namespace Api {
namespace {

constexpr auto kResultDuration = crl::time(4000);
uint64 NextOperationId = 0;

class ProgressContent final : public Ui::RpWidget {
public:
	ProgressContent(QWidget *parent, not_null<QWidget*> window)
	: RpWidget(parent)
	, _window(window)
	, _scroll(this)
	, _label(_scroll->setOwnedWidget(object_ptr<Ui::FlatLabel>(
		nullptr, st::selectedActionToastLabel))) {
		setNaturalWidth(st::selectedActionToast.maxWidth);
		_scroll->show();
	}

	void setText(TextWithEntities text) {
		_label->setMarkedText(std::move(text));
		resizeToWidth(width());
	}

protected:
	int resizeGetHeight(int width) override {
		_label->resizeToWidth(std::max(width - st::selectedActionScrollSkip, 1));
		const auto available = _window
			? (_window->height() / 2
				- st::selectedActionToast.padding.top()
				- st::selectedActionToast.padding.bottom()
				- st::selectedActionToast.margin.top()
				- st::selectedActionToast.margin.bottom())
			: st::selectedActionToast.maxWidth;
		const auto height = std::min(_label->height(), std::max(available, 1));
		_scroll->setGeometry(0, 0, width, height);
		return height;
	}

private:
	QPointer<QWidget> _window;
	object_ptr<Ui::ScrollArea> _scroll;
	QPointer<Ui::FlatLabel> _label;

};

} // namespace

class SelectedActionDisplay final
	: public std::enable_shared_from_this<SelectedActionDisplay> {
public:
	explicit SelectedActionDisplay(std::shared_ptr<Ui::Show> show)
	: _show(std::move(show))
	, _timer([=] { nextError(); }) {
	}

	~SelectedActionDisplay() {
		if (const auto toast = _toast.get()) {
			toast->hide();
		}
		if (const auto toast = _errorToast.get()) {
			toast->hide();
		}
	}

	void update(uint64 id, TextWithEntities text) {
		_entries[id] = std::move(text);
		render();
	}

	void remove(uint64 id) {
		_entries.erase(id);
		render();
	}

	void error(QString text) {
		_keepAlive = shared_from_this();
		_errors.push_back(std::move(text));
		if (!_timer.isActive()) {
			nextError();
		}
	}

private:
	void render() {
		if (!_show || !_show->valid()) {
			return;
		}
		if (_entries.empty()) {
			if (const auto toast = _toast.get()) {
				toast->hideAnimated();
			}
			_toast = nullptr;
			_content = nullptr;
			return;
		}
		auto text = TextWithEntities();
		for (const auto &[id, entry] : _entries) {
			if (!text.empty()) {
				text.append(u"\n\n"_q);
			}
			text.append(entry);
		}
		if (!_toast || !_content) {
			auto content = object_ptr<ProgressContent>(nullptr, _show->toastParent());
			_content = content.data();
			content->setText(std::move(text));
			_toast = Ui::Toast::Show(_show->toastParent(), {
				.content = std::move(content),
				.st = &st::selectedActionToast,
				.attach = RectPart::Top,
				.acceptinput = true,
				.infinite = true,
			});
		} else {
			_content->setText(std::move(text));
		}
	}

	void nextError() {
		const auto keep = shared_from_this();
		if (_errors.empty() || !_show || !_show->valid()) {
			_keepAlive = nullptr;
			return;
		}
		auto content = object_ptr<ProgressContent>(nullptr, _show->toastParent());
		content->setText({ _errors.front() });
		_errors.erase(_errors.begin());
		_errorToast = Ui::Toast::Show(_show->toastParent(), {
			.content = std::move(content),
			.st = &st::selectedActionToast,
			.attach = RectPart::Bottom,
			.acceptinput = true,
			.duration = kResultDuration,
		});
		_timer.callOnce(kResultDuration);
	}

	std::shared_ptr<SelectedActionDisplay> _keepAlive;
	std::shared_ptr<Ui::Show> _show;
	std::map<uint64, TextWithEntities> _entries;
	std::vector<QString> _errors;
	base::weak_ptr<Ui::Toast::Instance> _toast;
	base::weak_ptr<Ui::Toast::Instance> _errorToast;
	QPointer<ProgressContent> _content;
	base::Timer _timer;

};

SelectedActionPtr SelectedAction::Start(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		QString title,
		int selected) {
	static auto displays = std::map<
		QWidget*,
		std::weak_ptr<SelectedActionDisplay>>();
	for (auto i = displays.begin(); i != displays.end();) {
		if (i->second.expired()) {
			i = displays.erase(i);
		} else {
			++i;
		}
	}
	const auto parent = show && show->valid()
		? show->toastParent().get()
		: nullptr;
	auto &weakDisplay = displays[parent];
	auto display = weakDisplay.lock();
	if (!display) {
		display = std::make_shared<SelectedActionDisplay>(std::move(show));
		weakDisplay = display;
	}
	auto result = SelectedActionPtr(new SelectedAction(
		display, std::move(title), selected));
	session->lifetime().add([weak = std::weak_ptr(result)] {
		if (const auto strong = weak.lock()) {
			strong->cancel();
		}
	});
	result->_keepAlive = result;
	result->render();
	return result;
}

SelectedAction::SelectedAction(
		std::shared_ptr<SelectedActionDisplay> display,
		QString title,
		int selected)
: _display(std::move(display))
, _title(std::move(title))
, _id(++NextOperationId)
, _selected(selected)
, _finishTimer([=] { tryFinish(); })
, _refreshTimer([=] { render(); }) {
}

SelectedActionBatchPtr SelectedAction::add(
		QString stage,
		int count,
		QString details) {
	if (_finished) {
		return nullptr;
	}
	const auto id = int(_batches.size());
	_batches.push_back({ std::move(stage), std::move(details), count });
	refresh();
	return std::make_shared<SelectedActionBatch>(shared_from_this(), id);
}

void SelectedAction::start(int id) {
	_current = id;
	refresh();
}

void SelectedAction::complete(int id, const QString &error, bool silent) {
	auto &batch = _batches[id];
	if (batch.finished || _finished) {
		return;
	}
	batch.finished = true;
	if (error.isEmpty()) {
		_success += batch.count;
	} else {
		_failed += std::max(batch.count, 1);
		report(id, error, silent);
	}
	refresh();
	if (_sealed) {
		_finishTimer.callOnce(1);
	}
}

void SelectedAction::report(int id, const QString &error, bool silent) {
	const auto &batch = _batches[id];
	LOG(("SelectedAction: operation=%1 title=%2 phase=%3 batch=%4 count=%5 context=%6 error=%7")
		.arg(_id).arg(_title).arg(batch.stage).arg(id + 1)
		.arg(batch.count).arg(batch.details).arg(error));
	if (!silent && _display) {
		_display->error(_title + u"\n\n"_q + batch.stage
			+ u"\n"_q + error + u"\n"_q + batch.details);
	}
}

void SelectedAction::skip(int count, const QString &reason) {
	_skipped += count;
	LOG(("SelectedAction: operation=%1 skipped=%2 reason=%3")
		.arg(_id).arg(count).arg(reason));
	refresh();
}

bool SelectedAction::failed() const {
	return _failed != 0;
}

void SelectedAction::afterRequests(Fn<void()> callback) {
	_afterRequests = std::move(callback);
}

void SelectedAction::finish() {
	_sealed = true;
	_finishTimer.callOnce(1);
}

void SelectedAction::tryFinish() {
	if (_finished || !_sealed) {
		return;
	}
	for (const auto &batch : _batches) {
		if (!batch.finished) {
			return;
		}
	}
	if (_afterRequests) {
		const auto callback = base::take(_afterRequests);
		callback();
		_finishTimer.callOnce(1);
		return;
	}
	_finished = true;
	refresh();
	const auto keep = shared_from_this();
	base::call_delayed(kResultDuration, [keep] { keep->cancel(); });
}

void SelectedAction::cancel() {
	const auto keep = shared_from_this();
	_finished = true;
	_finishTimer.cancel();
	_refreshTimer.cancel();
	_afterRequests = nullptr;
	_keepAlive = nullptr;
	if (_display) {
		_display->remove(_id);
		_display = nullptr;
	}
}

void SelectedAction::refresh() {
	if (_finished) {
		_refreshTimer.cancel();
		render();
	} else {
		_refreshTimer.callOnce(100);
	}
}

void SelectedAction::render() {
	if (!_display) {
		return;
	}
	auto total = 0;
	auto batches = 0;
	auto completed = 0;
	for (const auto &batch : _batches) {
		total += batch.count;
		if (batch.count) {
			++batches;
			completed += batch.finished ? 1 : 0;
		}
	}
	auto text = Ui::Text::Bold(_title);
	text.append(u"\n\n"_q);
	if (_finished) {
		text.append(_failed ? tr::lng_selected_action_partial(tr::now)
			: _skipped ? tr::lng_selected_action_skipped(tr::now)
			: tr::lng_selected_action_done(tr::now));
	} else {
		text.append(_current >= 0 ? _batches[_current].stage
			: tr::lng_selected_action_preparing(tr::now));
		if (_current >= 0 && !_batches[_current].details.isEmpty()) {
			text.append(u"\n"_q).append(_batches[_current].details);
		}
	}
	if (!_finished) {
		auto waiting = false;
		for (const auto &batch : _batches) {
			if (!batch.finished && batch.retryAt > crl::now()) {
				waiting = true;
				text.append(u"\n"_q).append(tr::lng_selected_action_wait(
					tr::now, lt_seconds,
					QString::number((batch.retryAt - crl::now() + 999) / 1000)));
			}
		}
		if (waiting) {
			_refreshTimer.callOnce(1000);
		}
	}
	text.append(u"\n\n"_q).append(tr::lng_selected_action_progress(
		tr::now, lt_count, QString::number(_selected),
		lt_ready, QString::number(_success + _failed),
		lt_total, QString::number(total)));
	text.append(u"\n"_q).append(tr::lng_selected_action_batches(
		tr::now, lt_ready, QString::number(completed),
		lt_total, QString::number(batches)));
	text.append(u"\n\n"_q).append(tr::lng_selected_action_results(
		tr::now, lt_count, QString::number(_success),
		lt_failed, QString::number(_failed),
		lt_skipped, QString::number(_skipped)));
	if (_finished) {
		auto details = QStringList();
		for (const auto &batch : _batches) {
			if (!batch.details.isEmpty() && !details.contains(batch.details)) {
				details.push_back(batch.details);
			}
		}
		if (!details.empty()) {
			text.append(QString(2, QChar(10))).append(details.join(QChar(10)));
		}
	}
	_display->update(_id, std::move(text));
}

SelectedActionBatch::SelectedActionBatch(SelectedActionPtr owner, int id)
: _owner(std::move(owner))
, _id(id) {
}

void SelectedActionBatch::start() {
	_owner->start(_id);
}

void SelectedActionBatch::done() {
	if (!_finished) {
		_finished = true;
		_lifetime.destroy();
		_owner->complete(_id, QString());
	}
}

void SelectedActionBatch::fail(const MTP::Error &error) {
	if (!_finished) {
		_finished = true;
		_lifetime.destroy();
		_owner->complete(_id, u"%1 (%2): %3"_q.arg(error.type())
			.arg(error.code()).arg(error.description()), MTP::IgnoreError(error));
	}
}

void SelectedActionBatch::fail(const QString &error) {
	if (!_finished) {
		_finished = true;
		_lifetime.destroy();
		_owner->complete(_id, !_errorDetails.isEmpty() ? _errorDetails
			: error.isEmpty() ? u"UNKNOWN_ERROR"_q : error, _errorCode == 406);
	}
}

void SelectedActionBatch::retry(const MTP::Error &error) {
	_owner->report(_id, u"%1 (%2): %3"_q.arg(error.type())
		.arg(error.code()).arg(error.description()), MTP::IgnoreError(error));
}

void SelectedActionBatch::observe(
		not_null<Main::Session*> session,
		mtpRequestId requestId) {
	_lifetime.destroy();
	session->mtp().requestErrors() | rpl::on_next([=](const MTP::RequestRetryInfo &info) {
		if (_finished || info.requestId != requestId) {
			return;
		}
		_errorCode = info.code;
		_errorDetails = u"%1 (%2): %3"_q.arg(info.type)
			.arg(info.code).arg(info.description);
	}, _lifetime);
	session->mtp().requestRetries() | rpl::on_next([=](const MTP::RequestRetryInfo &info) {
		if (_finished || info.requestId != requestId) {
			return;
		}
		_owner->_batches[_id].retryAt = info.retryAt;
		_owner->report(_id, u"%1 (%2): %3; request=%4"_q.arg(info.type)
			.arg(info.code).arg(info.description).arg(requestId), false);
		_owner->refresh();
	}, _lifetime);
}

void ReportSelectedActionError(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		const QString &title,
		const QString &error) {
	const auto progress = SelectedAction::Start(session, std::move(show), title, 0);
	const auto batch = progress->add(tr::lng_selected_action_preparing(tr::now), 1);
	batch->fail(error);
	progress->finish();
}

SelectedActionBatchPtr TrackSelectedAction(
		const SelectedActionPtr &action,
		const QString &stage,
		int count,
		const QString &details) {
	return action ? action->add(stage, count, details) : nullptr;
}

QString SelectedActionPeer(not_null<PeerData*> peer) {
	return u"%1 [%2]"_q.arg(peer->name()).arg(peer->id.value);
}

} // namespace Api
