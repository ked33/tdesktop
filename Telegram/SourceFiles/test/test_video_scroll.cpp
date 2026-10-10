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
#include <QtCore/QStringList>
#include <QtGui/QFocusEvent>
#include <QtGui/QMoveEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QWheelEvent>

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Test {
namespace {

constexpr auto kCaptureWindow = crl::time(15000);
constexpr auto kSampleInterval = crl::time(250);
constexpr auto kCauseWindow = crl::time(30000);
constexpr auto kBurstLimit = 24;
constexpr auto kPhaseLimit = 96;
constexpr auto kEvidenceLimit = 192;
constexpr auto kTransitionLimit = 96;
constexpr auto kEvidenceBurst = 8;
constexpr auto kCycleLimit = 8;
constexpr auto kCauseLimit = 1024;
constexpr auto kRowLimit = 4096;
constexpr auto kLayoutDepthLimit = 4;

struct ResizeCause {
	const History *history = nullptr;
	FullMsgId id;
	std::source_location source;
	const char *event = "";
	crl::time first = 0;
	crl::time last = 0;
	int count = 1;
};

struct RowGeometry {
	const History *history = nullptr;
	const HistoryView::Element *view = nullptr;
	int top = 0;
	QSize message;
	QSize media;
	QSize optimal;
	QSize declared;
	QSize measured;
	uint64 document = 0;
	bool pending = false;
};

struct LayoutSnapshot {
	std::map<FullMsgId, RowGeometry> rows;
	int height = 0;
	int skipped = 0;
	int serial = 0;
};

struct Evidence {
	const char *event = "";
	const char *phase = "";
	QString state;
	crl::time time = 0;
};

std::deque<ResizeCause> RecentCauses;
auto CausesEvicted = 0;

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
	~VideoScrollTrace() override;

	[[nodiscard]] bool matches(HistoryInner *list) const;
	[[nodiscard]] bool matches(not_null<HistoryView::Element*> view) const;
	[[nodiscard]] bool matches(const History *history) const;
	[[nodiscard]] bool awaitingViewer(FullMsgId context) const;
	[[nodiscard]] bool collecting() const;
	void record(
		const char *event,
		const QString &detail = {},
		bool evidence = false);
	void layout(bool before);
	void cause(const ResizeCause &cause);
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
	[[nodiscard]] LayoutSnapshot rows() const;
	[[nodiscard]] QString rowDetails(const RowGeometry &row) const;
	[[nodiscard]] QString origins(const History *history, FullMsgId id) const;
	void rowChanges(
		const LayoutSnapshot &before,
		const LayoutSnapshot &after,
		const char *phase);
	void sample();
	void startPhase(const char *phase);
	void write(const char *event, const QString &detail);
	void flushEvidence();

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
	std::vector<LayoutSnapshot> _layouts;
	std::optional<LayoutSnapshot> _lastRows;
	std::deque<Evidence> _evidence;
	QString _firstLimited;
	QString _lastLimited;
	const char *_phase = "open";
	crl::time _until = 0;
	crl::time _burstAt = 0;
	int _burst = 0;
	int _written = 0;
	int _evidenceWritten = 0;
	int _transitions = 0;
	int _layoutSerial = 0;
	int _layoutOverflow = 0;
	int _rowsChanged = 0;
	int _duplicates = 0;
	int _limited = 0;
	int _minTop = 0;
	int _maxTop = 0;
	bool _closing = false;

};

QPointer<VideoScrollTrace> CurrentTrace;
auto CyclesStarted = 0;
auto CycleLimitReported = false;

[[nodiscard]] QString Origin(std::source_location source) {
	const auto path = std::string_view(source.file_name());
	const auto slash = path.find_last_of("/\\");
	const auto file = path.substr(slash == path.npos ? 0 : slash + 1);
	return u"%1:%2 [%3]"_q
		.arg(QString::fromUtf8(file.data(), int(file.size())))
		.arg(source.line())
		.arg(QString::fromUtf8(source.function_name()).left(300));
}

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

VideoScrollTrace::~VideoScrollTrace() {
	if (!_evidence.empty() && Media::Streaming::PlaybackDebugLogsEnabled()) {
		write("evidence-abandoned", u"queued=%1 first={%2} last={%3}"_q
			.arg(int(_evidence.size()))
			.arg(_evidence.front().state).arg(_evidence.back().state));
	}
}

bool VideoScrollTrace::matches(HistoryInner *list) const {
	return list == _list.get();
}

bool VideoScrollTrace::matches(
		not_null<HistoryView::Element*> view) const {
	const auto history = view->data()->history();
	return matches(history)
		&& (_list->viewByItem(view->data()) == view);
}

bool VideoScrollTrace::matches(const History *history) const {
	return history == _history || history == _migrated;
}

bool VideoScrollTrace::collecting() const {
	return _until
		&& crl::now() <= _until
		&& Media::Streaming::PlaybackDebugLogsEnabled();
}

bool VideoScrollTrace::awaitingViewer(FullMsgId context) const {
	return collecting() && !_viewer && context == _context;
}

LayoutSnapshot VideoScrollTrace::rows() const {
	auto result = LayoutSnapshot();
	for (const auto history : { _history.get(), _migrated }) {
		if (!history) {
			continue;
		}
		result.height += history->height();
		const auto historyTop = (history == _history)
			? _list->historyTop()
			: _list->migratedTop();
		for (const auto &block : history->blocks) {
			for (const auto &view : block->messages) {
				if (int(result.rows.size()) >= kRowLimit) {
					++result.skipped;
					continue;
				}
				const auto media = view->media();
				const auto document = media ? media->getDocument() : nullptr;
				const auto video = document ? document->video() : nullptr;
				result.rows.emplace(view->data()->fullId(), RowGeometry{
					.history = history,
					.view = view.get(),
					.top = historyTop + block->y() + view->y(),
					.message = view->currentSize(),
					.media = media ? media->currentSize() : QSize(),
					.optimal = media ? media->optimalSize() : QSize(),
					.declared = document ? document->dimensions : QSize(),
					.measured = video ? video->realVideoSize : QSize(),
					.document = document ? document->id : 0,
					.pending = view->pendingResize(),
				});
			}
		}
	}
	return result;
}

QString VideoScrollTrace::rowDetails(const RowGeometry &row) const {
	return u"history=%1 top=%2 message=%3 media=%4 optimal=%5 "
		"doc=%6 declared=%7 measured=%8 pending=%9"_q
		.arg(row.history == _history ? u"main"_q : u"migrated"_q)
		.arg(row.top)
		.arg(Size(row.message))
		.arg(Size(row.media))
		.arg(Size(row.optimal))
		.arg(qulonglong(row.document))
		.arg(Size(row.declared))
		.arg(Size(row.measured))
		.arg(row.pending);
}

QString VideoScrollTrace::origins(const History *history, FullMsgId id) const {
	auto result = QStringList();
	const auto now = crl::now();
	for (const auto specific : { true, false }) {
		auto remaining = specific ? 4 : 2;
		auto included = std::vector<std::source_location>();
		for (auto i = RecentCauses.rbegin(); i != RecentCauses.rend(); ++i) {
			if (i->history != history
				|| (specific ? i->id != id : bool(i->id))
				|| now - i->last > kCauseWindow) {
				continue;
			}
			if (std::any_of(included.begin(), included.end(), [&](const auto &s) {
				return s.line() == i->source.line()
					&& std::string_view(s.file_name()) == i->source.file_name();
			})) {
				continue;
			}
			included.push_back(i->source);
			result.push_back(u"%1@%2..%3 count=%4 %5"_q
				.arg(QString::fromLatin1(i->event))
				.arg(i->first - _started)
				.arg(i->last - _started)
				.arg(i->count)
				.arg(Origin(i->source)));
			if (!--remaining) {
				break;
			}
		}
	}
	return result.empty() ? u"unobserved"_q : result.join(u" | "_q);
}

void VideoScrollTrace::rowChanges(
		const LayoutSnapshot &before,
		const LayoutSnapshot &after,
		const char *phase) {
	auto heightDelta = 0;
	auto changed = 0;
	auto shifted = 0;
	for (const auto &[id, row] : before.rows) {
		if (!after.rows.contains(id)) {
			++changed;
			heightDelta -= row.message.height();
			record(after.skipped ? "row-unobserved" : "row-removed",
				u"layout=%1 pass=%2 msg=%3 old={%4} causes={%5}"_q
				.arg(before.serial).arg(QString::fromLatin1(phase))
				.arg(id.msg.bare).arg(rowDetails(row))
				.arg(origins(row.history, id)), true);
		}
	}
	for (const auto &[id, row] : after.rows) {
		const auto i = before.rows.find(id);
		if (i == before.rows.end()) {
			++changed;
			heightDelta += row.message.height();
			record(before.skipped ? "row-observed" : "row-added",
				u"layout=%1 pass=%2 msg=%3 new={%4} causes={%5}"_q
				.arg(before.serial).arg(QString::fromLatin1(phase))
				.arg(id.msg.bare).arg(rowDetails(row))
				.arg(origins(row.history, id)), true);
			continue;
		}
		const auto &old = i->second;
		shifted += (old.top != row.top);
		if (old.view == row.view
			&& old.message == row.message
			&& old.media == row.media
			&& old.optimal == row.optimal
			&& old.document == row.document
			&& old.declared == row.declared
			&& old.measured == row.measured) {
			continue;
		}
		++changed;
		const auto delta = row.message.height() - old.message.height();
		heightDelta += delta;
		record("row-change", u"layout=%1 pass=%2 msg=%3 dy=%4 dh=%5 "
			"replaced=%6 old={%7} new={%8} causes={%9}"_q
			.arg(before.serial).arg(QString::fromLatin1(phase))
			.arg(id.msg.bare).arg(row.top - old.top).arg(delta)
			.arg(old.view != row.view).arg(rowDetails(old)).arg(rowDetails(row))
			.arg(origins(row.history, id)), true);
	}
	_rowsChanged += changed;
	if (changed || shifted || before.height != after.height
		|| before.skipped || after.skipped) {
		record("layout-diff", u"layout=%1 pass=%2 changed=%3 shifted=%4 "
			"heightDelta=%5 rowHeightDelta=%6 unmatchedDelta=%7 "
			"rows=%8..%9 skipped=%10..%11"_q
			.arg(before.serial).arg(QString::fromLatin1(phase))
			.arg(changed).arg(shifted).arg(after.height - before.height)
			.arg(heightDelta).arg(after.height - before.height - heightDelta)
			.arg(int(before.rows.size())).arg(int(after.rows.size()))
			.arg(before.skipped).arg(after.skipped), true);
	}
}

void VideoScrollTrace::layout(bool before) {
	if (!collecting()) {
		return;
	}
	if (before) {
		if (int(_layouts.size()) >= kLayoutDepthLimit || _layoutOverflow) {
			++_layoutOverflow;
			record("layout-depth-limit");
			return;
		}
		auto current = rows();
		current.serial = ++_layoutSerial;
		if (_lastRows && _layouts.empty()) {
			rowChanges(*_lastRows, current, "before-layout");
		}
		_layouts.push_back(std::move(current));
	} else if (_layoutOverflow) {
		--_layoutOverflow;
	} else if (!_layouts.empty()) {
		auto current = rows();
		current.serial = _layouts.back().serial;
		rowChanges(_layouts.back(), current, "layout");
		_layouts.pop_back();
		_lastRows = std::move(current);
	} else {
		record("layout-unpaired");
		_lastRows = rows();
	}
}

void VideoScrollTrace::cause(const ResizeCause &cause) {
	record("cause", u"kind=%1 msg=%2 count=%3 first=%4 source={%5}"_q
		.arg(QString::fromLatin1(cause.event))
		.arg(cause.id.msg.bare).arg(cause.count).arg(cause.first - _started)
		.arg(Origin(cause.source)));
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

void VideoScrollTrace::record(
		const char *event,
		const QString &detail,
		bool evidence) {
	if (!collecting()) {
		return;
	}
	const auto now = crl::now();
	const auto name = std::string_view(event);
	const auto transition = name.starts_with("viewer-")
		|| name == "scroll-position"
		|| name == "scroll-event"
		|| name == "scroll-request"
		|| name == "scroll-applied"
		|| name == "geometry-target"
		|| name == "geometry-exit";
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
	const auto limited = transition
		? (_transitions >= kTransitionLimit)
		: evidence
		? (_evidenceWritten >= kEvidenceLimit)
		: (_written >= kPhaseLimit || _burst >= kBurstLimit);
	if (limited) {
		const auto skipped = u"t=%1 event=%2 %3"_q
			.arg(now - _started).arg(QString::fromLatin1(event)).arg(state);
		if (_firstLimited.isEmpty()) {
			_firstLimited = skipped;
		}
		_lastLimited = skipped;
		++_limited;
		return;
	}
	if (transition || evidence) {
		if (transition) {
			++_transitions;
		} else {
			++_evidenceWritten;
		}
		_evidence.push_back({ event, _phase, state, now - _started });
	} else {
		++_burst;
		++_written;
		write(event, state);
	}
}

void VideoScrollTrace::flushEvidence() {
	if (!Media::Streaming::PlaybackDebugLogsEnabled()) {
		_evidence.clear();
		return;
	}
	for (auto count = 0; count != kEvidenceBurst && !_evidence.empty(); ++count) {
		const auto entry = std::move(_evidence.front());
		_evidence.pop_front();
		LOG((u"Video Scroll: cycle=%1 phase=%2 t=%3 event=%4 buffered=1 %5"_q)
			.arg(_cycle).arg(QString::fromLatin1(entry.phase))
			.arg(entry.time).arg(QString::fromLatin1(entry.event))
			.arg(entry.state));
	}
}

void VideoScrollTrace::startPhase(const char *phase) {
	_phase = phase;
	_until = crl::now() + kCaptureWindow;
	_burstAt = crl::now();
	_burst = _written = _duplicates = _limited = 0;
	_evidenceWritten = _transitions = _rowsChanged = 0;
	_layouts.clear();
	_layoutOverflow = 0;
	_minTop = _maxTop = _scroll->scrollTop();
	_lastEvents.clear();
	_firstLimited.clear();
	_lastLimited.clear();
	_lastRows = rows();
	write("begin", snapshot());
	write("capture", u"version=2 duration_ms=%1 rows=%2 skipped=%3 "
		"recentCauses=%4 causesEvicted=%5"_q
		.arg(kCaptureWindow).arg(int(_lastRows->rows.size()))
		.arg(_lastRows->skipped)
		.arg(int(RecentCauses.size())).arg(CausesEvicted));
	_timer.callEach(kSampleInterval);
}

void VideoScrollTrace::finish(const char *reason) {
	if (!_until) {
		return;
	}
	_until = 0;
	if (_evidence.empty()) {
		_timer.cancel();
	}
	if (Media::Streaming::PlaybackDebugLogsEnabled()) {
		if (_limited) {
			write("first-limited", _firstLimited);
			write("last-limited", _lastLimited);
		}
		write(reason, u"written=%1 duplicates=%2 limited=%3 range=%4..%5 "
			"evidence=%6 rowsChanged=%7 causesEvicted=%8 queued=%9 "
			"transitions=%10 %11"_q
			.arg(_written).arg(_duplicates).arg(_limited)
			.arg(_minTop).arg(_maxTop).arg(_evidenceWritten)
			.arg(_rowsChanged).arg(CausesEvicted)
			.arg(int(_evidence.size())).arg(_transitions).arg(snapshot()));
	}
}

void VideoScrollTrace::sample() {
	flushEvidence();
	if (!collecting()) {
		finish("window-end");
		if (_evidence.empty()) {
			_timer.cancel();
		}
		return;
	}
	const auto state = snapshot();
	if (_lastEvents["sample"].state != u' ' + state) {
		record("sample");
		if (_layouts.empty()) {
			auto current = rows();
			if (_lastRows) {
				rowChanges(*_lastRows, current, "sample");
			}
			_lastRows = std::move(current);
		}
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
		auto detail = QString();
		switch (event->type()) {
		case QEvent::Move: {
			const auto moved = static_cast<QMoveEvent*>(event);
			detail = u"old=%1,%2 new=%3,%4"_q
				.arg(moved->oldPos().x()).arg(moved->oldPos().y())
				.arg(moved->pos().x()).arg(moved->pos().y());
		} break;
		case QEvent::Resize: {
			const auto resized = static_cast<QResizeEvent*>(event);
			detail = u"old=%1 new=%2"_q
				.arg(Size(resized->oldSize())).arg(Size(resized->size()));
		} break;
		case QEvent::FocusIn:
		case QEvent::FocusOut: {
			detail = u"reason=%1"_q
				.arg(int(static_cast<QFocusEvent*>(event)->reason()));
		} break;
		case QEvent::Wheel: {
			const auto wheel = static_cast<QWheelEvent*>(event);
			detail = u"angleY=%1 pixelY=%2 phase=%3"_q
				.arg(wheel->angleDelta().y()).arg(wheel->pixelDelta().y())
				.arg(int(wheel->phase()));
		} break;
		case QEvent::WindowActivate:
		case QEvent::WindowDeactivate:
		case QEvent::KeyPress:
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonRelease:
		case QEvent::TouchBegin:
		case QEvent::TouchEnd:
			break;
		default:
			return false;
		}
		record("widget-event", u"object=%1 type=%2 spontaneous=%3 %4"_q
			.arg(object == _list.get()
				? u"list"_q
				: object == _scroll.get()
				? u"viewport"_q
				: object == _scroll->window()
				? u"window"_q
				: u"ancestor"_q)
			.arg(int(event->type())).arg(event->spontaneous()).arg(detail));
	}
	return false;
}

} // namespace

void VideoScrollSettings() {
	static auto previous = std::optional<bool>();
	const auto enabled = Media::Streaming::PlaybackDebugLogsEnabled();
	if (previous == enabled) {
		return;
	}
	previous = enabled;
	LOG(("Video Scroll: config version=2 enabled=%1 "
		"window_ms=%2 cycle_limit=%3 phase_limit=%4 evidence_limit=%5 "
		"evidence_burst=%6 cause_window_ms=%7 row_limit=%8 transition_limit=%9.")
		.arg(enabled).arg(kCaptureWindow).arg(kCycleLimit)
		.arg(kPhaseLimit).arg(kEvidenceLimit).arg(kEvidenceBurst)
		.arg(kCauseWindow).arg(kRowLimit).arg(kTransitionLimit));
	if (!enabled) {
		RecentCauses.clear();
	}
}

void VideoScrollCause(
		const char *event,
		not_null<History*> history,
		const HistoryItem *item,
		std::source_location source) {
	if (!Media::Streaming::PlaybackDebugLogsEnabled()) {
		return;
	}
	const auto now = crl::now();
	while (!RecentCauses.empty()
		&& now - RecentCauses.front().last > kCauseWindow) {
		RecentCauses.pop_front();
	}
	const auto id = item ? item->fullId() : FullMsgId();
	auto checked = 0;
	for (auto i = RecentCauses.end(); i != RecentCauses.begin() && checked < 64;) {
		--i;
		++checked;
		if (i->history == history && i->id == id
			&& i->source.line() == source.line()
			&& std::string_view(i->source.file_name()) == source.file_name()
			&& std::string_view(i->event) == event) {
			auto repeated = *i;
			repeated.last = now;
			++repeated.count;
			RecentCauses.erase(i);
			RecentCauses.push_back(repeated);
			return;
		}
	}
	if (int(RecentCauses.size()) >= kCauseLimit) {
		RecentCauses.pop_front();
		++CausesEvicted;
	}
	RecentCauses.push_back({ history, id, source, event, now, now });
	if (CurrentTrace
		&& CurrentTrace->collecting()
		&& CurrentTrace->matches(history)) {
		CurrentTrace->cause(RecentCauses.back());
	}
}

void VideoScrollGeometryState(
		HistoryInner *list,
		bool initial,
		bool loadedDown,
		bool inited,
		bool loading,
		bool animating,
		bool required,
		bool updating) {
	if (CurrentTrace
		&& CurrentTrace->collecting()
		&& CurrentTrace->matches(list)) {
		CurrentTrace->record("geometry-state", u"initial=%1 loadedDown=%2 "
			"inited=%3 loading=%4 animating=%5 required=%6 updating=%7"_q
			.arg(initial).arg(loadedDown).arg(inited).arg(loading)
			.arg(animating).arg(required).arg(updating));
	}
}

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
		if (!CycleLimitReported) {
			CycleLimitReported = true;
			LOG(("Video Scroll: cycle-limit reached=%1 restart-required=1.")
				.arg(kCycleLimit));
		}
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
		int extra,
		std::source_location source) {
	if (CurrentTrace
		&& CurrentTrace->collecting()
		&& CurrentTrace->matches(list)) {
		if (std::string_view(event) == "list-size-before") {
			CurrentTrace->layout(true);
		} else if (std::string_view(event) == "list-size-after") {
			CurrentTrace->layout(false);
		}
		CurrentTrace->record(event, u"value=%1 extra=%2 source={%3}"_q
			.arg(value).arg(extra).arg(Origin(source)));
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
