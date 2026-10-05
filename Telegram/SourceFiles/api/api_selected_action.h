#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_types.h"
#include "rpl/lifetime.h"

#include <memory>
#include <vector>

namespace Main {
class Session;
} // namespace Main
namespace MTP {
class Error;
} // namespace MTP
namespace Ui {
class Show;
} // namespace Ui

namespace Api {

class SelectedAction;
class SelectedActionDisplay;
using SelectedActionPtr = std::shared_ptr<SelectedAction>;

class SelectedActionBatch final {
public:
	SelectedActionBatch(SelectedActionPtr owner, int id);
	void start();
	void setStage(QString stage);
	void beginFallback(QString stage);
	void done();
	void fail(const MTP::Error &error);
	void fail(const QString &error);
	void retry(const MTP::Error &error);
	void observe(not_null<Main::Session*> session, mtpRequestId requestId);

private:
	SelectedActionPtr _owner;
	int _id = 0;
	int _errorCode = 0;
	QString _errorDetails;
	bool _finished = false;
	rpl::lifetime _lifetime;

};
using SelectedActionBatchPtr = std::shared_ptr<SelectedActionBatch>;

class SelectedAction final
	: public std::enable_shared_from_this<SelectedAction> {
public:
	static SelectedActionPtr Start(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		QString title,
		int selected);
	SelectedActionBatchPtr add(
		QString stage,
		int count,
		QString details = QString());
	void skip(int count, const QString &reason);
	void keepSources(int count);
	void keepTemporaryCopies(int count);
	void finish();
	void afterRequests(Fn<void()> callback);
	void cancel();
	[[nodiscard]] bool failed() const;

private:
	friend class SelectedActionBatch;
	struct Batch {
		QString stage;
		QString details;
		int count = 0;
		bool finished = false;
		bool fallback = false;
		crl::time retryAt = 0;
	};
	SelectedAction(
		std::shared_ptr<SelectedActionDisplay> display,
		QString title,
		int selected);
	void start(int id);
	void complete(int id, const QString &error, bool silent = false);
	void report(int id, const QString &error, bool silent);
	void refresh();
	void render();
	void tryFinish();

	SelectedActionPtr _keepAlive;
	std::shared_ptr<SelectedActionDisplay> _display;
	QString _title;
	std::vector<Batch> _batches;
	uint64 _id = 0;
	int _selected = 0;
	int _current = -1;
	int _success = 0;
	int _failed = 0;
	int _skipped = 0;
	int _kept = 0;
	int _temporaryKept = 0;
	bool _sealed = false;
	bool _finished = false;
	Fn<void()> _afterRequests;
	base::Timer _finishTimer;
	base::Timer _refreshTimer;

};

void ReportSelectedActionError(
	not_null<Main::Session*> session,
	std::shared_ptr<Ui::Show> show,
	const QString &title,
	const QString &error);

[[nodiscard]] SelectedActionBatchPtr TrackSelectedAction(
	const SelectedActionPtr &action,
	const QString &stage,
	int count,
	const QString &details = QString());
[[nodiscard]] QString SelectedActionPeer(not_null<PeerData*> peer);

} // namespace Api
