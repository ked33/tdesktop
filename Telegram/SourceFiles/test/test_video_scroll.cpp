#include "test/test_video_scroll.h"

#include "base/timer.h"
#include "data/data_document.h"
#include "data/data_file_click_handler.h"
#include "data/data_session.h"
#include "history/view/media/history_view_media.h"
#include "history/view/history_view_element.h"
#include "history/history.h"
#include "history/history_inner_widget.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_debug.h"
#include "ui/widgets/elastic_scroll.h"
#include "logs.h"

#include <QtCore/QEvent>
#include <QtCore/QPointer>

#include <algorithm>
#include <array>
#include <map>
#include <string>

namespace Test {
namespace {

constexpr auto kCaptureWindow = crl::time(5000);
constexpr auto kSampleInterval = crl::time(250);
constexpr auto kBurstLimit = 24;
constexpr auto kPhaseLimit = 96;
constexpr auto kCycleLimit = 8;

// This observer also runs in cloud Release artifacts; the existing playback
// logging switch is its opt-in, independent of the disposable test agent.
class VideoScrollTrace final : public QObject {
public:
	VideoScrollTrace(
		not_null<HistoryInner*> list,
		not_null<Ui::ElasticScroll*> scroll,
		not_null<History*> history,
		History *migrated,
		FullMsgId context,
		int cycle);
	~VideoScrollTrace();

	[[nodiscard]] bool matches(HistoryInner *list) const;
	[[nodiscard]] bool matches(not_null<HistoryView::Element*> view) const;
	[[nodiscard]] bool awaitingViewer(FullMsgId context) const;
	[[nodiscard]] bool collecting() const;
	void record(const char *event, const QString &detail = {});
	void viewer(
		VideoScrollViewerEvent event,
		not_null<QObject*> viewer,
		HistoryItem *item);
	void finish(const char *reason);

protected:
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	struct LastEvent {
		QString state;
		crl::time time = 0;
	};

	[[nodiscard]] QString snapshot() const;
	void sample();
	void startPhase(const char *phase);
	void write(const char *event, const QString &detail);

	const not_null<HistoryInner*> _list;
	const not_null<Ui::ElasticScroll*> _scroll;
	const not_null<History*> _history;
	History *const _migrated = nullptr;
	const FullMsgId _context;
	const int _cycle = 0;
	const crl::time _started = crl::now();
	QPointer<QObject> _viewer;
	base::Timer _timer;
	rpl::lifetime _lifetime;
	std::map<std::string, LastEvent> _lastEvents;
	QString _firstLimited;
	QString _lastLimited;
	const char *_phase = "open";
	crl::time _until = 0;
	crl::time _burstAt = 0;
	int _burst = 0;
	int _written = 0;
	int _duplicates = 0;
	int _limited = 0;
	int _minTop = 0;
	int _maxTop = 0;
	bool _closing = false;

};

QPointer<VideoScrollTrace> CurrentTrace;
auto CyclesStarted = 0;

[[nodiscard]] QString Size(QSize size) {
	return u"%1x%2"_q.arg(size.width()).arg(size.height());
}

[[nodiscard]] QString Rect(QRect rect) {
	return u"%1,%2,%3x%4"_q
		.arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height());
}

[[nodiscard]] QString Anchor(History *history) {
	const auto view = history ? history->scrollTopItem : nullptr;
	return view
		? u"%1,%2,%3"_q
			.arg(view->data()->id.bare)
			.arg(history->scrollTopOffset)
			.arg(history->height())
		: u"none,0,%1"_q.arg(history ? history->height() : 0);
}

[[nodiscard]] int SavedTop(
		not_null<HistoryInner*> list,
		History *history,
		History *migrated) {
	const auto top = list->historyTop();
	const auto migratedTop = list->migratedTop();
	const auto view = (top >= 0 && history->scrollTopItem)
		? history->scrollTopItem
		: (migratedTop >= 0 && migrated)
		? migrated->scrollTopItem
		: nullptr;
	if (!view || !view->block()) {
		return -1;
	}
	const auto owner = view->history();
	return ((owner == history) ? top : migratedTop)
		+ view->block()->y() + view->y() + owner->scrollTopOffset;
}

VideoScrollTrace::VideoScrollTrace(
	not_null<HistoryInner*> list,
	not_null<Ui::ElasticScroll*> scroll,
	not_null<History*> history,
	History *migrated,
	FullMsgId context,
	int cycle)
: QObject(list)
, _list(list)
, _scroll(scroll)
, _history(history)
, _migrated(migrated)
, _context(context)
, _cycle(cycle)
, _timer([=] { sample(); }) {
	_list->installEventFilter(this);
	_scroll->installEventFilter(this);
	if (const auto parent = _scroll->parentWidget()) {
		parent->installEventFilter(this);
		if (parent->window() != parent) {
			parent->window()->installEventFilter(this);
		}
	}
	_scroll->scrolls() | rpl::on_next([=] {
		record("scroll-event");
	}, _lifetime);
	_scroll->positionValue() | rpl::on_next([=] {
		record("scroll-position");
	}, _lifetime);
	startPhase("open");
}

VideoScrollTrace::~VideoScrollTrace() = default;

bool VideoScrollTrace::matches(HistoryInner *list) const {
	return list == _list.get();
}

bool VideoScrollTrace::matches(
		not_null<HistoryView::Element*> view) const {
	const auto history = view->data()->history();
	return (history == _history || history == _migrated)
		&& (_list->viewByItem(view->data()) == view);
}

bool VideoScrollTrace::collecting() const {
	return _until
		&& crl::now() <= _until
		&& Media::Streaming::PlaybackDebugLogsEnabled();
}

bool VideoScrollTrace::awaitingViewer(FullMsgId context) const {
	return collecting() && !_viewer && context == _context;
}

QString VideoScrollTrace::snapshot() const {
	const auto item = _list->session().data().message(_context);
	const auto view = item ? _list->viewByItem(item) : nullptr;
	const auto media = view ? view->media() : nullptr;
	const auto top = (view && view->block()) ? _list->itemTop(view) : -1;
	const auto position = _scroll->position();
	return u"scroll=%1 max=%2 over=%3 movement=%4 viewport=%5 list=%6 "
		"listGlobalY=%7 historyTop=%8 savedTop=%9 anchor=%10 migrated=%11 "
		"pending=%12,%13 target=%14 top=%15 screenY=%16 message=%17 "
		"media=%18 optimal=%19 viewPending=%20"_q
		.arg(_scroll->scrollTop())
		.arg(_scroll->scrollTopMax())
		.arg(position.overscroll)
		.arg(int(_scroll->movement()))
		.arg(Rect(_scroll->geometry()))
		.arg(Rect(_list->geometry()))
		.arg(_list->mapToGlobal(QPoint()).y())
		.arg(_list->historyTop())
		.arg(SavedTop(_list, _history, _migrated))
		.arg(Anchor(_history))
		.arg(Anchor(_migrated))
		.arg(_history->hasPendingResizedItems())
		.arg(_migrated && _migrated->hasPendingResizedItems())
		.arg(_context.msg.bare)
		.arg(top)
		.arg(top >= 0 ? _list->mapToGlobal(QPoint(0, top)).y() : -1)
		.arg(Size(view ? view->currentSize() : QSize()))
		.arg(Size(media ? media->currentSize() : QSize()))
		.arg(Size(media ? media->optimalSize() : QSize()))
		.arg(view && view->pendingResize());
}

void VideoScrollTrace::write(const char *event, const QString &detail) {
	LOG((u"Video Scroll: cycle=%1 phase=%2 t=%3 event=%4 %5"_q)
		.arg(_cycle)
		.arg(QString::fromLatin1(_phase))
		.arg(crl::now() - _started)
		.arg(QString::fromLatin1(event))
		.arg(detail));
}

void VideoScrollTrace::record(const char *event, const QString &detail) {
	if (!collecting()) {
		return;
	}
	const auto now = crl::now();
	const auto top = _scroll->scrollTop();
	_minTop = std::min(_minTop, top);
	_maxTop = std::max(_maxTop, top);
	const auto state = detail + u' ' + snapshot();
	auto &last = _lastEvents[event];
	if (last.state == state && now - last.time < kSampleInterval) {
		++_duplicates;
		return;
	}
	last = { state, now };
	if (now - _burstAt >= kSampleInterval) {
		_burstAt = now;
		_burst = 0;
	}
	if (_written >= kPhaseLimit || _burst >= kBurstLimit) {
		const auto skipped = u"t=%1 event=%2 %3"_q
			.arg(now - _started).arg(QString::fromLatin1(event)).arg(state);
		if (_firstLimited.isEmpty()) {
			_firstLimited = skipped;
		}
		_lastLimited = skipped;
		++_limited;
		return;
	}
	++_burst;
	++_written;
	write(event, state);
}

void VideoScrollTrace::startPhase(const char *phase) {
	_phase = phase;
	_until = crl::now() + kCaptureWindow;
	_burstAt = crl::now();
	_burst = _written = _duplicates = _limited = 0;
	_minTop = _maxTop = _scroll->scrollTop();
	_lastEvents.clear();
	_firstLimited.clear();
	_lastLimited.clear();
	write("begin", snapshot());
	_timer.callEach(kSampleInterval);
}

void VideoScrollTrace::finish(const char *reason) {
	if (!_until) {
		return;
	}
	_until = 0;
	_timer.cancel();
	if (Media::Streaming::PlaybackDebugLogsEnabled()) {
		if (_limited) {
			write("first-limited", _firstLimited);
			write("last-limited", _lastLimited);
		}
		write(reason, u"written=%1 duplicates=%2 limited=%3 range=%4..%5 %6"_q
			.arg(_written).arg(_duplicates).arg(_limited)
			.arg(_minTop).arg(_maxTop).arg(snapshot()));
	}
}

void VideoScrollTrace::sample() {
	if (!collecting()) {
		finish("window-end");
		return;
	}
	const auto state = snapshot();
	if (_lastEvents["sample"].state != u' ' + state) {
		record("sample");
	}
}

void VideoScrollTrace::viewer(
		VideoScrollViewerEvent event,
		not_null<QObject*> viewer,
		HistoryItem *item) {
	if (event == VideoScrollViewerEvent::Open) {
		if (!item
			|| &item->history()->owner() != &_history->owner()
			|| item->fullId() != _context) {
			return;
		}
		_viewer = viewer.get();
	} else if (_viewer != viewer.get()) {
		return;
	}
	if (event == VideoScrollViewerEvent::Closing && !_closing) {
		finish("before-close");
		_closing = true;
		startPhase("close");
	}
	const auto names = std::array{
		"viewer-open", "viewer-shown", "viewer-ready",
		"viewer-player-locked", "viewer-clear-stream",
		"viewer-closing", "viewer-hidden",
	};
	record(names[int(event)]);
}

bool VideoScrollTrace::eventFilter(QObject *object, QEvent *event) {
	if (collecting()) {
		switch (event->type()) {
		case QEvent::Move:
		case QEvent::Resize:
		case QEvent::FocusIn:
		case QEvent::FocusOut:
		case QEvent::WindowActivate:
		case QEvent::WindowDeactivate:
			record("widget-event", u"object=%1 type=%2"_q
				.arg(object == _list.get()
					? u"list"_q
					: object == _scroll.get()
					? u"viewport"_q
					: u"ancestor"_q)
				.arg(int(event->type())));
			break;
		}
	}
	return false;
}

} // namespace

void VideoScrollClick(
		not_null<HistoryInner*> list,
		not_null<Ui::ElasticScroll*> scroll,
		not_null<History*> history,
		History *migrated,
		const ClickHandler *handler) {
	if (!Media::Streaming::PlaybackDebugLogsEnabled()) {
		return;
	}
	if (const auto open = dynamic_cast<const DocumentOpenClickHandler*>(handler)) {
		VideoScrollBegin(
			list,
			scroll,
			history,
			migrated,
			open->document(),
			open->context());
	}
}

void VideoScrollBegin(
		not_null<HistoryInner*> list,
		not_null<Ui::ElasticScroll*> scroll,
		not_null<History*> history,
		History *migrated,
		not_null<DocumentData*> document,
		FullMsgId context) {
	if (!Media::Streaming::PlaybackDebugLogsEnabled()
		|| !document->isVideoFile()) {
		return;
	}
	if (CurrentTrace) {
		if (CurrentTrace->matches(list)
			&& CurrentTrace->awaitingViewer(context)) {
			CurrentTrace->record("open-dispatch");
			return;
		}
		CurrentTrace->finish("next-open");
		delete CurrentTrace.data();
	}
	if (CyclesStarted >= kCycleLimit) {
		return;
	}
	CurrentTrace = new VideoScrollTrace(
		list,
		scroll,
		history,
		migrated,
		context,
		++CyclesStarted);
	CurrentTrace->record("document", u"doc=%1 declared=%2 cyclesLeft=%3"_q
		.arg(qulonglong(document->id))
		.arg(Size(document->dimensions))
		.arg(kCycleLimit - CyclesStarted));
}

void VideoScrollDetach(not_null<HistoryInner*> list) {
	if (CurrentTrace && CurrentTrace->matches(list)) {
		CurrentTrace->finish("list-destroyed");
		delete CurrentTrace.data();
	}
}

void VideoScrollHistory(
		const char *event,
		HistoryInner *list,
		int value,
		int extra) {
	if (CurrentTrace
		&& CurrentTrace->collecting()
		&& CurrentTrace->matches(list)) {
		CurrentTrace->record(event, u"value=%1 extra=%2"_q.arg(value).arg(extra));
	}
}

void VideoScrollView(
		const char *event,
		not_null<HistoryView::Element*> view,
		QSize size,
		int value,
		QSize frame) {
	if (CurrentTrace
		&& CurrentTrace->collecting()
		&& CurrentTrace->matches(view)) {
		CurrentTrace->record(event, u"causeMsg=%1 size=%2 value=%3 frame=%4"_q
			.arg(view->data()->id.bare)
			.arg(Size(size))
			.arg(value)
			.arg(Size(frame)));
	}
}

void VideoScrollViewer(
		VideoScrollViewerEvent event,
		not_null<QObject*> viewer,
		HistoryItem *item) {
	if (CurrentTrace && Media::Streaming::PlaybackDebugLogsEnabled()) {
		CurrentTrace->viewer(event, viewer, item);
	}
}

} // namespace Test
