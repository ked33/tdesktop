#include "storage/media_preview_limits.h"

#include <array>
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
	Require(
		!raw.concurrent && !raw.requestsPerSecond && !raw.burst,
		"default values stay zero");
	Require(!raw.enabled(), "default settings add no download limits");
	const auto mixed = Storage::MediaPreviewLimits{ 3, 0, 1 }.normalized();
	Require(
		mixed.concurrent == 3 && !mixed.requestsPerSecond && mixed.burst == 1,
		"zero does not select a nonzero fallback");
	Require(mixed.enabled(), "concurrency can be enabled without pacing");
	Require(
		!Storage::MediaPreviewLimits{ 0, 0, 2 }.enabled(),
		"burst allowance needs a positive request rate");
	for (const auto invalid : {
		-1,
		std::numeric_limits<int>::min(),
		std::numeric_limits<int>::max(),
	}) {
		const auto value = Storage::MediaPreviewLimits{
			invalid, invalid, invalid,
		}.normalized();
		Require(
			!value.concurrent && !value.requestsPerSecond && !value.burst,
			"invalid stored values use the unrestricted defaults");
	}
	const auto maximum = Storage::MediaPreviewLimits{ 32, 120, 32 }.normalized();
	Require(
		maximum.concurrent == 32 && maximum.requestsPerSecond == 120
			&& maximum.burst == 32,
		"maximum values remain configurable");
}

void TestZeroCombinations() {
	struct Scenario {
		Storage::MediaPreviewLimits limits;
		int allowed = 0;
	};
	const auto scenarios = std::array{
		Scenario{ { 0, 0, 0 }, 16 },
		Scenario{ { 2, 0, 0 }, 2 },
		Scenario{ { 0, 4, 0 }, 1 },
		Scenario{ { 0, 0, 2 }, 16 },
		Scenario{ { 2, 4, 0 }, 1 },
		Scenario{ { 2, 0, 2 }, 2 },
		Scenario{ { 0, 4, 2 }, 2 },
		Scenario{ { 2, 4, 2 }, 2 },
	};
	for (const auto &scenario : scenarios) {
		auto limiter = Storage::MediaPreviewRequestLimiter();
		limiter.configure(scenario.limits, 0);
		auto sent = 0;
		while (sent < 16 && limiter.hasCapacity() && !limiter.delay(0)) {
			limiter.requestStarted(0);
			++sent;
		}
		Require(sent == scenario.allowed, "all eight zero/nonzero combinations");
		Require(
			limiter.rateLimited() == (scenario.limits.requestsPerSecond > 0),
			"zero rate preserves the original playback pacing path");
		while (limiter.active()) {
			limiter.requestFinished();
		}
	}
}

void TestUnlimitedDefaults() {
	auto limiter = Storage::MediaPreviewRequestLimiter();
	limiter.configure({}, 0);
	for (auto i = 0; i != 200; ++i) {
		Require(
			limiter.hasCapacity() && !limiter.delay(0),
			"zero values add neither concurrency nor rate limits");
		limiter.requestStarted(0);
	}
	Require(limiter.active() == 200, "unlimited requests remain accounted for");
	limiter.configure({ 2, 0, 0 }, 0);
	Require(!limiter.hasCapacity(), "enabling a cap counts existing requests");
	for (auto i = 0; i != 199; ++i) {
		limiter.requestFinished();
	}
	Require(limiter.hasCapacity(), "new work resumes below the enabled cap");
	limiter.requestFinished();
	limiter.suspend(5000);
	Require(!limiter.delay(0), "zero rate adds no pacing wait");
}

void TestRateWithoutBurstCap() {
	auto limiter = Storage::MediaPreviewRequestLimiter();
	limiter.configure({ 0, 4, 0 }, 0);
	limiter.requestStarted(0);
	limiter.requestFinished();
	Require(limiter.delay(249) == 1, "zero burst keeps the configured rate");
	Require(!limiter.delay(250), "four per second adds one allowance in 250 ms");
	limiter.requestStarted(250);
	limiter.requestFinished();
	limiter.configure({ 0, 4, 0 }, 2750);
	for (auto i = 0; i != 10; ++i) {
		Require(!limiter.delay(2750), "zero burst does not cap idle allowance");
		limiter.requestStarted(2750);
		limiter.requestFinished();
	}
	Require(limiter.delay(2750) == 250, "pacing resumes after saved allowance");
	limiter.configure({ 0, 4, 0 }, 5250);
	limiter.configure({ 0, 4, 2 }, 5250);
	for (auto i = 0; i != 2; ++i) {
		limiter.requestStarted(5250);
		limiter.requestFinished();
	}
	Require(
		limiter.delay(5250) == 250,
		"enabling a burst cap trims previously unlimited allowance");
}

void TestBurstAndCancellation() {
	auto limiter = Storage::MediaPreviewRequestLimiter();
	limiter.configure({ 2, 4, 2 }, 0);
	Require(limiter.hasCapacity() && !limiter.delay(0), "initial allowance");
	limiter.requestStarted(0);
	limiter.requestStarted(0);
	Require(
		!limiter.hasCapacity() && limiter.active() == 2,
		"requests share the concurrency cap");
	limiter.requestFinished();
	Require(limiter.hasCapacity(), "completion or cancellation frees a slot");
	Require(limiter.delay(249) == 1, "cancellation does not refund rate credit");
	Require(!limiter.delay(250), "next request resumes at the rate deadline");
	limiter.requestStarted(250);
	limiter.requestFinished();
	limiter.requestFinished();
	for (auto i = 0; i != 10; ++i) {
		limiter.configure({ 2, 4, 2 }, 250);
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
	limiter.configure({ 1, 4, 2 }, 100);
	Require(
		limiter.active() == 4 && !limiter.hasCapacity(),
		"lowering a limit preserves existing requests");
	for (auto i = 0; i != 3; ++i) {
		limiter.requestFinished();
	}
	Require(!limiter.hasCapacity(), "new requests wait for the lower cap");
	limiter.configure({ 0, 4, 2 }, 100);
	Require(
		limiter.hasCapacity() && limiter.delay(100) == 250,
		"disabling concurrency preserves the rate limit");
	limiter.configure({ 0, 0, 2 }, 100);
	Require(
		limiter.hasCapacity() && !limiter.delay(100),
		"disabling the rate takes effect immediately");
	for (auto i = 0; i != 8; ++i) {
		limiter.requestStarted(100);
	}
	limiter.configure({ 2, 0, 0 }, 100);
	Require(
		limiter.active() == 9 && !limiter.hasCapacity(),
		"re-enabling concurrency preserves unlimited in-flight requests");
	for (auto i = 0; i != 9; ++i) {
		limiter.requestFinished();
	}
	Require(limiter.hasCapacity() && !limiter.delay(100), "all slots released");
}

void TestServerWait() {
	auto limiter = Storage::MediaPreviewRequestLimiter();
	limiter.configure({ 2, 4, 2 }, 100);
	limiter.requestStarted(100);
	limiter.suspend(5000);
	Require(limiter.active() == 1, "server wait keeps the original request");
	limiter.configure({ 2, 4, 2 }, 5000);
	Require(limiter.delay(5000) == 250, "server wait does not bank a burst");
	Require(!limiter.delay(5250), "pacing resumes after server wait");
	limiter.suspend(6000);
	Require(limiter.delay(6249) == 1, "extended server wait is respected");
	Require(!limiter.delay(6250), "extended wait eventually recovers");
	limiter.requestFinished();
	Require(!limiter.active(), "server retry completion releases its slot");
	limiter.suspend(7000);
	limiter.configure({ 2, 0, 2 }, 6250);
	Require(!limiter.delay(6250), "disabled pacing leaves server waits to MTProto");
	limiter.configure({ 2, 4, 2 }, 6300);
	Require(
		limiter.delay(7000) == 250,
		"re-enabling pacing during a wait cannot refill a burst");
}

void TestSharedBudgetAndIndependentServers() {
	auto first = Storage::MediaPreviewRequestLimiter();
	auto second = Storage::MediaPreviewRequestLimiter();
	first.configure({ 2, 4, 2 }, 0);
	second.configure({ 2, 4, 2 }, 0);
	first.requestStarted(0);
	first.requestStarted(0);
	Require(
		second.hasCapacity() && !second.delay(0),
		"accounts and servers have independent state");
	first.requestFinished();
	first.requestFinished();
	first.configure({ 2, 4, 2 }, 60000);
	first.requestStarted(60000);
	first.requestStarted(60000);
	Require(first.delay(60000) == 250, "long idle only restores the burst cap");
	first.requestFinished();
	first.requestFinished();

	auto shared = Storage::MediaPreviewRequestLimiter();
	auto sent = 0;
	for (auto now = 0; now <= 120000; ++now) {
		shared.configure({ 2, 4, 2 }, now);
		while (shared.hasCapacity() && !shared.delay(now)) {
			shared.requestStarted(now);
			shared.requestFinished();
			++sent;
		}
		Require(sent * 1000 <= 2000 + 4 * now, "shared sustained rate invariant");
	}
	Require(sent == 482, "queued requests resume without starvation");
}

void TestUnlimitedAllowanceOverflow() {
	auto limiter = Storage::MediaPreviewRequestLimiter();
	limiter.configure({ 0, 120, 0 }, 0);
	limiter.requestStarted(0);
	limiter.requestFinished();
	const auto now = std::numeric_limits<std::int64_t>::max();
	limiter.configure({ 0, 120, 0 }, now);
	for (auto i = 0; i != 64; ++i) {
		Require(!limiter.delay(now), "unlimited idle allowance saturates safely");
		limiter.requestStarted(now);
		limiter.requestFinished();
	}
	limiter.configure({ 0, 120, 1 }, now);
	limiter.requestStarted(now);
	limiter.requestFinished();
	Require(limiter.delay(now) == 9, "fractional request intervals round up");
	limiter.suspend(now);
	Require(limiter.delay(0) == now, "wait duration saturates without overflow");
}

} // namespace

int main() {
	TestDefaults();
	TestZeroCombinations();
	TestUnlimitedDefaults();
	TestRateWithoutBurstCap();
	TestBurstAndCancellation();
	TestLiveChanges();
	TestServerWait();
	TestSharedBudgetAndIndependentServers();
	TestUnlimitedAllowanceOverflow();
	std::cout << "Media preview zero defaults, independent limits, allowance, "
		"cancellation, live changes, server waits and shared budgets passed.\n";
}
