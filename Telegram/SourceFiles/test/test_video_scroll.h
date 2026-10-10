#pragma once

#include "base/basic_types.h"

#include <QtCore/QSize>

#include <source_location>

class ClickHandler;
class DocumentData;
class History;
class HistoryInner;
class HistoryItem;
class QObject;
struct FullMsgId;

namespace HistoryView {
class Element;
} // namespace HistoryView

namespace Ui {
class ElasticScroll;
} // namespace Ui

namespace Test {

enum class VideoScrollViewerEvent {
	Open,
	Shown,
	Ready,
	Locked,
	ClearStream,
	Closing,
	Hidden,
};

void VideoScrollSettings();
void VideoScrollCause(
	const char *event,
	not_null<History*> history,
	const HistoryItem *item,
	std::source_location source = std::source_location::current());
void VideoScrollGeometryState(
	HistoryInner *list,
	bool initial,
	bool loadedDown,
	bool inited,
	bool loading,
	bool animating,
	bool required,
	bool updating);
void VideoScrollClick(
	not_null<HistoryInner*> list,
	not_null<Ui::ElasticScroll*> scroll,
	not_null<History*> history,
	History *migrated,
	const ClickHandler *handler);
void VideoScrollBegin(
	not_null<HistoryInner*> list,
	not_null<Ui::ElasticScroll*> scroll,
	not_null<History*> history,
	History *migrated,
	not_null<DocumentData*> document,
	FullMsgId context);
void VideoScrollDetach(not_null<HistoryInner*> list);
void VideoScrollHistory(
	const char *event,
	HistoryInner *list,
	int value = 0,
	int extra = 0,
	std::source_location source = std::source_location::current());
void VideoScrollView(
	const char *event,
	not_null<HistoryView::Element*> view,
	QSize size = {},
	int value = -1,
	QSize frame = {});
void VideoScrollViewer(
	VideoScrollViewerEvent event,
	not_null<QObject*> viewer,
	HistoryItem *item = nullptr);

} // namespace Test
