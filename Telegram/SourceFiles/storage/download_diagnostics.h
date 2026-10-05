#pragma once

#include "base/timer.h"

#include <atomic>
#include <map>
#include <memory>
#include <tuple>

namespace Storage {

struct DownloadTrace {
	enum class Stage {
		Created,
		CacheLookup,
		CacheDecodeQueued,
		CacheDecode,
		CacheDelivery,
		Queued,
		Request,
		Receiving,
		Reference,
		Retry,
		Idle,
		Complete,
		Cancelled,
		Failed,
	};

	void setStage(Stage value);
	void setError(QString value);
	void finish(Stage value);
	[[nodiscard]] bool terminal() const;
	[[nodiscard]] QString snapshot(crl::time now) const;

	uint64 id = 0;
	uint64 scope = 0;
	int dc = 0;
	QString kind = u"file"_q;
	int cacheTag = -1;
	bool automatic = false;
	bool streaming = false;
	std::atomic<Stage> stage = Stage::Created;
	crl::time created = crl::now();
	crl::time finishedAt = 0;
	crl::time progressAt = created;
	crl::time reportedAt = 0;
	crl::time retryAt = 0;
	crl::time oldestSent = 0;
	crl::time referenceAt = 0;
	crl::time referenceMs = 0;
	crl::time firstSent = 0;
	crl::time firstReceived = 0;
	crl::time cacheMs = -1;
	int64 bytes = 0;
	int sent = 0;
	int received = 0;
	int pending = 0;
	int deferred = 0;
	int references = 0;
	int refreshed = 0;
	int retries = 0;
	int requestId = 0;
	int priority = 0;
	QString error;
};

class DownloadDiagnostics final {
public:
	explicit DownloadDiagnostics(Fn<QString(int)> queueSnapshot);
	[[nodiscard]] std::shared_ptr<DownloadTrace> create();
	void watch(const std::shared_ptr<DownloadTrace> &trace);

private:
	void check();
	bool report(const DownloadTrace &trace, const char *event, crl::time now);

	const uint64 _id = 0;
	const Fn<QString(int)> _queueSnapshot;
	std::map<uint64, std::shared_ptr<DownloadTrace>> _traces;
	std::map<std::tuple<int, QString, bool>, crl::time> _successSamples;
	base::Timer _timer;
	crl::time _summaryAt = 0;
	int _started = 0;
	int _completed = 0;
	int _failed = 0;
	int _cancelled = 0;
	int _untracked = 0;
	int _suppressed = 0;

};

} // namespace Storage
