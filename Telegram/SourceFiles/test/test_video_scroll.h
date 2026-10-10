#pragma once

#include "base/basic_types.h"

#include <QtCore/QSize>

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
	int extra = 0);
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
