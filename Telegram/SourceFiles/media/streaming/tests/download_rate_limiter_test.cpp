#include "storage/download_rate_limiter.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

constexpr auto kPart = 128 * 1024;

void Require(bool condition, const char *name) {
	if (!condition) {
		std::cerr << "FAILED: " << name << '\n';
		std::exit(1);
	}
}

void TestTarget() {
	using Limiter = Storage::DownloadRateLimiter;
	Require(Limiter::Target(0, 0) == 512 * 1024, "unknown bitrate");
	Require(Limiter::Target(310925, 0) == 388656, "log 11 target");
	Require(Limiter::Target(1750348, 0) == 2187935, "high bitrate headroom");
	Require(Limiter::Target(1, 0) == kPart, "automatic minimum");
	Require(Limiter::Target(1750348, 1024) == 1024 * 1024, "manual ceiling");
	Require(Limiter::Target(0, 1) == 1024, "low manual ceiling");
	Require(
		Limiter::Target(std::numeric_limits<int>::max(), 0)
			== 64 * 1024 * 1024,
		"overflow safe target");
}

void TestManualTargetsAndBudget() {
	using Limiter = Storage::DownloadRateLimiter;
	constexpr auto maximumKiB = 3906;
	constexpr auto target = maximumKiB * 1024;
	for (const auto playback : { 0, 1, 310925, 1750348, 8 * 1024 * 1024 }) {
		Require(Limiter::Target(playback, maximumKiB, false, true) == target,
			"manual target ignores video bitrate");
		Require(Limiter::Target(playback, maximumKiB, true, true) == target,
			"manual target cannot be boosted");
		Require(Limiter::Target(playback, 0, false, true)
			== Limiter::Target(playback, 0), "invalid manual target falls back");
		auto limiter = Limiter();
		auto sent = std::int64_t(0);
		for (auto now = 0; now <= 60000; ++now) {
			const auto rate = Limiter::Target(playback, maximumKiB, false, true);
			limiter.configure(rate, 2 * kPart, now, rate, true);
			while (!limiter.delay(kPart, now)) {
				limiter.consume(kPart, now);
				sent += kPart;
			}
			Require(sent * 1000 <= std::int64_t(2 * kPart) * 1000
				+ std::int64_t(target) * now, "manual sustained rate stays bounded");
			Require(!limiter.catchingUp(), "manual mode has no catch-up phase");
		}
		Require(sent >= std::int64_t(target) * 60,
			"pending downloads reach manual target even for low bitrate video");
		limiter.penalize(63000, 123000);
		limiter.configure(target, 2 * kPart, 60000);
		Require(limiter.rate() == target * 3 / 4,
			"server penalty overrides manual target");
		Require(limiter.delay(kPart, 60000) > 3000,
			"manual mode preserves server wait and empty budget");
		limiter.configure(target, 0, 60000, target, true);
		Require(limiter.rate() == 0 && limiter.delay(kPart, 60000) == 0,
			"zero burst disables manual target as well");
	}
	Require(Limiter::Target(0, 1, false, true) == 1024,
		"manual target can be below automatic minimum");
	Require(Limiter::Target(0, std::numeric_limits<int>::max(), false, true)
		== 64 * 1024 * 1024, "manual target multiplication cannot overflow");
}

void TestLargeBurst() {
	auto limiter = Storage::DownloadRateLimiter();
	constexpr auto capacity = std::int64_t(50000) * kPart;
	limiter.configure(kPart, capacity, 0);
	for (auto i = 0; i != 50000; ++i) {
		Require(limiter.delay(kPart, 0) == 0, "large burst has no overflow");
		limiter.consume(kPart, 0);
	}
	Require(limiter.delay(kPart, 0) == 1000, "large burst still exhausts");
	limiter.configure(kPart, capacity, 1000000000);
	for (auto i = 0; i != 50000; ++i) {
		limiter.consume(kPart, 1000000000);
	}
	Require(limiter.delay(kPart, 1000000000) == 1000,
		"long idle refills only the configured large burst");
}

void TestDisabled() {
	auto limiter = Storage::DownloadRateLimiter();
	const auto target = Storage::DownloadRateLimiter::Target(1750348, 1);
	limiter.configure(target, 0, 0);
	Require(limiter.rate() == 0, "zero burst disables manual rate ceiling");
	for (auto i = 0; i != 10000; ++i) {
		limiter.consume(kPart, 0);
		Require(limiter.delay(kPart, 0) == 0, "disabled budget cannot exhaust");
	}
	limiter.penalize(3000, 63000);
	limiter.suspend(5000, 65000);
	Require(limiter.ceiling() == 0, "disabled limiter ignores pacing penalty");
	Require(limiter.delay(kPart, 0) == 0, "disabled limiter adds no delay");
	limiter.configure(target, kPart, 0);
	Require(limiter.delay(kPart, 0) == 0, "reenabled budget starts full");
	limiter.consume(kPart, 0);
	Require(limiter.delay(kPart, 0) == 128000, "reenabled manual ceiling");
	limiter.penalize(3000, 63000);
	limiter.configure(target, 0, 1);
	Require(limiter.rate() == 0, "disable an active limiter");
	Require(limiter.ceiling() == 0, "disable clears pacing penalty");
	Require(limiter.delay(kPart, 1) == 0, "disable clears pending pacing wait");
	limiter.configure(512 * 1024, 2 * kPart, 1);
	limiter.consume(kPart, 1);
	limiter.consume(kPart, 1);
	Require(limiter.delay(kPart, 1) == 250, "reenabled budget has no old debt");
}

void TestBurstAndSeek() {
	auto limiter = Storage::DownloadRateLimiter();
	constexpr auto rate = 512 * 1024;
	limiter.configure(rate, 2 * kPart, 0);
	Require(limiter.delay(kPart, 0) == 0, "initial part");
	limiter.consume(kPart, 0);
	limiter.consume(kPart, 0);
	Require(limiter.delay(kPart, 0) == 250, "burst exhausted");
	for (auto i = 0; i != 100; ++i) {
		limiter.configure(rate, 2 * kPart, 0);
	}
	Require(limiter.delay(kPart, 0) == 250, "seek cannot refill credit");
	Require(limiter.delay(kPart, 249) == 1, "no early dispatch");
	Require(limiter.delay(kPart, 250) == 0, "deadline dispatch");
	limiter.consume(kPart, 250);
	Require(limiter.delay(kPart, 250) == 250, "cancellation cannot refund");
	limiter.configure(rate / 2, 2 * kPart, 250);
	Require(limiter.delay(kPart, 250) == 500, "lowering rate preserves debt");
	limiter.configure(rate / 2, 2 * kPart, 100000);
	limiter.consume(kPart, 100000);
	limiter.consume(kPart, 100000);
	Require(limiter.delay(kPart, 100000) == 500, "idle burst remains bounded");
}

void TestWaitAndRecovery() {
	auto limiter = Storage::DownloadRateLimiter();
	constexpr auto rate = 512 * 1024;
	limiter.configure(rate, 2 * kPart, 0);
	limiter.penalize(3000, 63000);
	Require(limiter.rate() == rate * 3 / 4, "server backoff");
	Require(limiter.delay(kPart, 0) == 3334, "server wait plus pacing");
	limiter.configure(rate, 2 * kPart, 3000);
	Require(limiter.delay(kPart, 3000) == 334, "no burst after wait");
	limiter.suspend(5000, 65000);
	Require(limiter.rate() == rate * 3 / 4, "coalesced wait no double backoff");
	limiter.configure(rate, 2 * kPart, 5000);
	Require(limiter.delay(kPart, 5000) == 334, "extended wait stays empty");
	limiter.configure(rate, 2 * kPart, 64000);
	Require(limiter.rate() == rate * 3 / 4, "recovery hold");
	limiter.configure(rate, 2 * kPart, 65000);
	const auto recovered = limiter.rate();
	Require(recovered > rate * 3 / 4 && recovered < rate, "gradual recovery");
	limiter.configure(rate, 2 * kPart, 65000);
	Require(limiter.rate() == recovered, "no repeated immediate recovery");
	limiter.penalize(68000, 128000);
	Require(limiter.rate() < recovered, "repeated server backoff");
	limiter.configure(1024, 2 * kPart, 68000);
	Require(limiter.rate() == 1024, "manual ceiling survives penalty");
}

void TestLongIdleAndIndependentServers() {
	auto first = Storage::DownloadRateLimiter();
	auto second = Storage::DownloadRateLimiter();
	first.configure(1024, 32 * kPart, 0);
	second.configure(1024, 32 * kPart, 0);
	for (auto i = 0; i != 32; ++i) {
		first.consume(kPart, 0);
	}
	Require(first.delay(kPart, 0) == 128000, "slow manual pacing");
	Require(second.delay(kPart, 0) == 0, "independent DC budget");
	Require(first.delay(kPart, 128000) == 0, "low rate timer deadline");
	first.configure(1024, 32 * kPart, 1000000000);
	for (auto i = 0; i != 32; ++i) {
		Require(first.delay(kPart, 1000000000) == 0, "long idle refills capacity");
		first.consume(kPart, 1000000000);
	}
	Require(first.delay(kPart, 1000000000) == 128000, "long idle cannot exceed capacity");
}

void TestCatchUpTargets() {
	using Limiter = Storage::DownloadRateLimiter;
	Require(Limiter::Target(0, 0, true) == 1536 * 1024, "header catch-up");
	Require(Limiter::Target(1222440, 0, true) == 4584150, "log 12 catch-up");
	Require(Limiter::Target(1222440, 2048, true) == 2 * 1024 * 1024,
		"catch-up respects manual ceiling");
	Require(Limiter::Target(4 * 1024 * 1024, 0, true) == 8 * 1024 * 1024,
		"catch-up rate is bounded");
	Require(Limiter::Target(8 * 1024 * 1024, 0, true) == 10 * 1024 * 1024,
		"high normal rate is not reduced or boosted");
	Require(Limiter::Target(std::numeric_limits<int>::max(), 0, true)
		== 64 * 1024 * 1024, "catch-up target cannot overflow");
}

void TestCatchUpWindowAndTaper() {
	auto limiter = Storage::DownloadRateLimiter();
	constexpr auto base = 512 * 1024;
	constexpr auto peak = 3 * base;
	limiter.configure(base, 2 * kPart, 0);
	Require(limiter.rate() == base && !limiter.catchingUp(), "switch off");
	limiter.configure(base, 2 * kPart, 0, peak, true);
	Require(limiter.rate() == peak && limiter.catchingUp(), "catch-up starts");
	for (auto now = 1; now != 8000; ++now) {
		limiter.configure(base, 2 * kPart, now, peak, true);
	}
	limiter.configure(base, 2 * kPart, 8000, peak, true);
	Require(limiter.rate() == base, "repeated seeks cannot extend window");
	limiter.configure(base, 2 * kPart, 29999, peak, true);
	Require(limiter.rate() == base, "catch-up cooldown");
	limiter.configure(base, 2 * kPart, 30000, peak, true);
	Require(limiter.rate() == peak, "catch-up may resume after cooldown");
	limiter.configure(base, 2 * kPart, 30500, peak, false);
	Require(limiter.rate() == peak, "short buffer changes do not flap rate");
	limiter.configure(base, 2 * kPart, 31000, peak, false);
	Require(limiter.rate() == peak * 3 / 4, "buffer recovery tapers rate");
	limiter.configure(base, 2 * kPart, 34000, peak, false);
	Require(limiter.rate() == base, "taper returns to steady rate");
	limiter.configure(base, 2 * kPart, 35000, peak, true);
	Require(limiter.rate() == peak, "pressure can resume within same window");
	limiter.configure(base, 2 * kPart, 38000, peak, true);
	Require(limiter.rate() == base, "resumed pressure preserves deadline");
	limiter.configure(base, 0, 60000, peak, true);
	Require(limiter.rate() == 0 && limiter.delay(kPart, 60000) == 0,
		"zero burst takes precedence over catch-up");
}

void TestCatchUpByteBudget() {
	auto limiter = Storage::DownloadRateLimiter();
	constexpr auto base = 4 * 1024 * 1024;
	constexpr auto peak = 8 * 1024 * 1024;
	auto boostedBytes = 0;
	for (auto now = 0; now != 8000; ++now) {
		limiter.configure(base, 32 * kPart, now, peak, true);
		while (!limiter.delay(kPart, now)) {
			if (limiter.catchingUp()) {
				boostedBytes += kPart;
			}
			limiter.consume(kPart, now);
			limiter.configure(base, 32 * kPart, now, peak, true);
		}
	}
	Require(boostedBytes == 16 * 1024 * 1024, "catch-up byte budget");
	Require(limiter.rate() == base, "exhausted budget ends boost");
	limiter.configure(base, 32 * kPart, 29999, peak, true);
	Require(limiter.rate() == base, "exhausted budget cannot refill on seek");
}

void TestCatchUpPreservesCredit() {
	auto limiter = Storage::DownloadRateLimiter();
	constexpr auto base = 512 * 1024;
	constexpr auto peak = 3 * base;
	limiter.configure(base, 2 * kPart, 0);
	limiter.consume(kPart, 0);
	limiter.consume(kPart, 0);
	for (auto i = 0; i != 100; ++i) {
		limiter.configure(base, 2 * kPart, 0, peak, true);
		limiter.configure(base, 2 * kPart, 0, peak, false);
	}
	Require(limiter.delay(kPart, 0) == 84,
		"pressure and seek transitions cannot refill burst credit");
	limiter.configure(base, 2 * kPart, 0, base, true);
	Require(limiter.rate() == base && limiter.delay(kPart, 0) == 250,
		"lower ceiling applies immediately without refilling credit");
}

void TestCatchUpServerLimits() {
	auto limiter = Storage::DownloadRateLimiter();
	constexpr auto base = 512 * 1024;
	constexpr auto peak = 3 * base;
	limiter.configure(base, 2 * kPart, 0, peak, true);
	limiter.penalize(3000, 63000);
	Require(limiter.rate() == base * 3 / 4,
		"penalty is based on steady rate, not boosted rate");
	Require(limiter.delay(kPart, 0) == 3334, "server wait survives catch-up");
	limiter.configure(base, 2 * kPart, 3000, peak, true);
	Require(!limiter.catchingUp(), "no catch-up during server recovery");
	Require(limiter.delay(kPart, 3000) == 334, "no free credit after wait");
	limiter.suspend(5000, 65000);
	limiter.configure(base, 2 * kPart, 64000, peak, true);
	Require(limiter.rate() == base * 3 / 4, "extended server recovery holds");
	limiter.configure(base, 2 * kPart, 65000, peak, true);
	Require(limiter.rate() <= limiter.ceiling(), "recovery ceiling wins");
	auto recoveredBoost = false;
	for (auto now = 95000; now <= 365000; now += 30000) {
		limiter.configure(base, 2 * kPart, now, peak, true);
		Require(limiter.rate() <= limiter.ceiling(), "boost never exceeds ceiling");
		recoveredBoost = recoveredBoost || limiter.catchingUp();
	}
	Require(recoveredBoost, "server recovery does not disable catch-up forever");
	limiter.configure(1024, 2 * kPart, 365000, 1024, true);
	Require(limiter.rate() == 1024, "low manual ceiling cannot be boosted");
}

void TestSharedBudget() {
	for (const auto playback : {310925, 911470, 1750348, 2816045}) {
		auto limiter = Storage::DownloadRateLimiter();
		const auto rate = Storage::DownloadRateLimiter::Target(playback, 0);
		auto sent = std::int64_t(0);
		for (auto now = 0; now <= 60000; ++now) {
			limiter.configure(rate, 2 * kPart, now);
			for (auto loader = 0; loader != 8; ++loader) {
				if (!limiter.delay(kPart, now)) {
					limiter.consume(kPart, now);
					sent += kPart;
				}
			}
			Require(
				sent * 1000 <= std::int64_t(2 * kPart) * 1000
					+ std::int64_t(rate) * now,
				"shared byte budget invariant");
		}
		Require(sent >= std::int64_t(rate) * 60, "no scheduling starvation");
	}
}

} // namespace

int main() {
	TestTarget();
	TestManualTargetsAndBudget();
	TestLargeBurst();
	TestDisabled();
	TestBurstAndSeek();
	TestWaitAndRecovery();
	TestLongIdleAndIndependentServers();
	TestCatchUpTargets();
	TestCatchUpWindowAndTaper();
	TestCatchUpByteBudget();
	TestCatchUpPreservesCredit();
	TestCatchUpServerLimits();
	TestSharedBudget();
	std::cout << "PASS: targets, manual throughput, disable, large burst, seek, wait, recovery, idle, catch-up, shared/DC budgets\n";
}
