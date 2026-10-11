#include "storage/media_preview_limits.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void Require(bool condition, const char *name) {
	if (!condition) {
		std::cerr << name << '\n';
		std::exit(1);
	}
}

void TestDefaults() {
	const auto raw = Storage::MediaPreviewLimits().normalized();
	Require(!raw.concurrent && !raw.requestsPerSecond && !raw.burst,
		"stored zero values retain default selection");
	const auto defaults = raw.resolved();
	Require(defaults.concurrent == 2, "default concurrency");
	Require(defaults.requestsPerSecond == 4, "default request rate");
	Require(defaults.burst == 2, "default burst");
	const auto mixed = Storage::MediaPreviewLimits{ 3, 0, 1 }.resolved();
	Require(mixed.concurrent == 3 && mixed.requestsPerSecond == 4
		&& mixed.burst == 1, "independent default selection");
	for (const auto invalid : {
		-1,
		std::numeric_limits<int>::min(),
		std::numeric_limits<int>::max(),
	}) {
		const auto value = Storage::MediaPreviewLimits{
			invalid, invalid, invalid,
		}.resolved();
		Require(value.concurrent == 2 && value.requestsPerSecond == 4
			&& value.burst == 2, "invalid stored values fall back safely");
	}
	const auto maximum = Storage::MediaPreviewLimits{ 32, 120, 32 }.resolved();
	Require(maximum.concurrent == 32 && maximum.requestsPerSecond == 120
		&& maximum.burst == 32, "maximum values remain configurable");
}

void TestBurstAndCancellation() {
	auto limiter = Storage::MediaPreviewRequestLimiter();
	limiter.configure({}, 0);
	Require(limiter.hasCapacity() && !limiter.delay(0), "initial allowance");
	limiter.requestStarted(0);
	limiter.requestStarted(0);
	Require(!limiter.hasCapacity() && limiter.active() == 2,
		"requests share the concurrency cap");
	limiter.requestFinished();
	Require(limiter.hasCapacity(), "completion or cancellation frees a slot");
	Require(limiter.delay(249) == 1, "cancellation does not refund rate credit");
	Require(!limiter.delay(250), "next request resumes at the rate deadline");
	limiter.requestStarted(250);
	limiter.requestFinished();
	limiter.requestFinished();
	for (auto i = 0; i != 10; ++i) {
		limiter.configure({}, 250);
	}
	Require(limiter.delay(250) == 250, "reconfiguration cannot refill a burst");
	Require(!limiter.active(), "all slots released");
}

void TestLiveChanges() {
	auto limiter = Storage::MediaPreviewRequestLimiter();
	limiter.configure({ 4, 8, 4 }, 100);
	for (auto i = 0; i != 4; ++i) {
		limiter.requestStarted(100);
	}
	limiter.configure({ 1, 0, 0 }, 100);
	Require(limiter.active() == 4 && !limiter.hasCapacity(),
		"lowering a limit preserves existing requests");
	for (auto i = 0; i != 3; ++i) {
		limiter.requestFinished();
	}
	Require(!limiter.hasCapacity(), "new requests wait for the lower cap");
	limiter.requestFinished();
	Require(limiter.hasCapacity() && limiter.delay(100) == 250,
		"restoring default rate does not bypass pacing");
	Require(!limiter.delay(350), "live rate change takes effect");
}

void TestServerWait() {
	auto limiter = Storage::MediaPreviewRequestLimiter();
	limiter.configure({}, 100);
	limiter.requestStarted(100);
	limiter.suspend(5000, 65000);
	Require(limiter.active() == 1, "server wait keeps the original request");
	limiter.configure({}, 5000);
	Require(limiter.delay(5000) == 250, "server wait does not bank a burst");
	Require(!limiter.delay(5250), "pacing resumes after server wait");
	limiter.suspend(6000, 66000);
	Require(limiter.delay(6249) == 1, "extended server wait is respected");
	Require(!limiter.delay(6250), "extended wait eventually recovers");
	limiter.requestFinished();
	Require(!limiter.active(), "server retry completion releases its slot");
}

void TestSharedBudgetAndIndependentServers() {
	auto first = Storage::MediaPreviewRequestLimiter();
	auto second = Storage::MediaPreviewRequestLimiter();
	first.configure({}, 0);
	second.configure({}, 0);
	first.requestStarted(0);
	first.requestStarted(0);
	Require(second.hasCapacity() && !second.delay(0),
		"accounts and servers have independent state");
	first.requestFinished();
	first.requestFinished();
	first.configure({}, 60000);
	first.requestStarted(60000);
	first.requestStarted(60000);
	Require(first.delay(60000) == 250, "long idle only restores the burst cap");
	first.requestFinished();
	first.requestFinished();

	auto shared = Storage::MediaPreviewRequestLimiter();
	auto sent = 0;
	for (auto now = 0; now <= 120000; ++now) {
		shared.configure({}, now);
		while (shared.hasCapacity() && !shared.delay(now)) {
			shared.requestStarted(now);
			shared.requestFinished();
			++sent;
		}
		Require(sent * 1000 <= 2000 + 4 * now, "shared sustained rate invariant");
	}
	Require(sent == 482, "queued requests resume without starvation");
}

} // namespace

int main() {
	TestDefaults();
	TestBurstAndCancellation();
	TestLiveChanges();
	TestServerWait();
	TestSharedBudgetAndIndependentServers();
	std::cout << "Media preview defaults, concurrency, pacing, cancellation, "
		"live changes, server waits and shared budgets passed.\n";
}
