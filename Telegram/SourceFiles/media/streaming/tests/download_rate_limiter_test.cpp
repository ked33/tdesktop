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

void TestAutomaticTargetPercent() {
	using Limiter = Storage::DownloadRateLimiter;
	constexpr auto playback = 1023070;
	Require(Limiter::Target(playback, 0, false, false, 125) == 1278837,
		"125 percent preserves log 13 target");
	Require(Limiter::Target(playback, 0, false, false, 100) == playback,
		"100 percent targets playback consumption");
	Require(Limiter::Target(playback, 0, false, false, 200) == 2046140,
		"200 percent doubles playback consumption");
	Require(Limiter::Target(playback, 0, false, false, 500) == 5115350,
		"500 percent is five times playback consumption");
	Require(Limiter::Target(playback, 2929, false, false, 500) == 2999296,
		"three MB ceiling still caps higher automatic multiplier");
	Require(Limiter::Target(playback, 0, true, false, 200) == 6138420,
		"catch-up uses the configured normal target");
	Require(Limiter::Target(playback, 0, true, false, 500) == 8 * 1024 * 1024,
		"higher automatic multiplier cannot bypass catch-up maximum");
	Require(Limiter::Target(playback, 2929, true, false, 500) == 2999296,
		"catch-up also respects the user ceiling");
	Require(Limiter::Target(playback, 0, false, false, 0) == playback,
		"invalid percentage is bounded at the minimum");
	Require(Limiter::Target(playback, 0, false, false, 1001) == playback * 10,
		"invalid percentage is bounded at the maximum");
	Require(Limiter::Target(std::numeric_limits<int>::max(), 0,
		false, false, 1000) == 64 * 1024 * 1024,
		"large bitrate and percentage cannot overflow");
	for (const auto percent : { 100, 125, 200, 500, 1000 }) {
		Require(Limiter::Target(0, 0, false, false, percent) == 512 * 1024,
			"unknown bitrate fallback does not scale");
		Require(Limiter::Target(1, 0, false, false, percent) == kPart,
			"automatic floor is preserved");
		Require(Limiter::Target(playback, 1, false, false, percent) == 1024,
			"low user ceiling wins over the automatic target");
		Require(Limiter::Target(playback, 3906, false, true, percent)
			== 3906 * 1024, "manual mode ignores automatic percentage");
		auto limiter = Limiter();
		const auto rate = Limiter::Target(playback, 0, false, false, percent);
		limiter.configure(rate, 2 * kPart, 0);
		limiter.penalize(3000, 63000);
		limiter.configure(rate, 2 * kPart, 3000);
		Require(limiter.rate() == rate * 3 / 4,
			"configured automatic percentage obeys server backoff");
		Require(limiter.delay(kPart, 3000) > 0,
			"server backoff leaves no free burst");
		limiter.configure(rate, 0, 3000);
		Require(limiter.rate() == 0 && limiter.delay(kPart, 3000) == 0,
			"zero burst disables configured automatic targets");
	}
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

void TestImmediateRecovery() {
	using Limiter = Storage::DownloadRateLimiter;
	auto limiter = Limiter();
	const auto target = Limiter::Target(1196775, 3613, false, true);
	auto now = std::int64_t(0);
	limiter.configure(target, 2 * kPart, now);
	for (auto round = 0; round != 3; ++round) {
		const auto until = now + 6000;
		const auto extended = until + 3000;
		limiter.penalize(
			until,
			until + 120000,
			Limiter::Recovery::Immediate);
		Require(
			limiter.rate() == target && limiter.ceiling() == 0,
			"immediate recovery preserves the configured target");
		limiter.configure(target, 2 * kPart, until - 1);
		Require(
			limiter.delay(kPart, until - 1) > 1,
			"immediate recovery cannot dispatch before the server deadline");
		limiter.suspend(extended, extended + 120000);
		limiter.configure(target, 2 * kPart, until);
		Require(
			limiter.delay(kPart, until) > extended - until,
			"coalesced waits extend the deadline without bypassing it");
		limiter.configure(target, 2 * kPart, extended);
		Require(
			limiter.delay(kPart, extended) > 0,
			"waiting cannot accumulate a free burst");
		auto sent = std::int64_t(0);
		for (now = extended; now <= extended + 2000; ++now) {
			limiter.configure(target, 2 * kPart, now);
			for (auto loader = 0; loader != 8; ++loader) {
				if (!limiter.delay(kPart, now)) {
					limiter.consume(kPart, now);
					sent += kPart;
				}
			}
			Require(
				sent * 1000 <= std::int64_t(target) * (now - extended),
				"shared downloads stay within the manual ceiling after a wait");
			Require(
				limiter.rate() == target && limiter.ceiling() == 0,
				"repeated server waits do not compound a disabled rate penalty");
		}
		Require(
			sent >= std::int64_t(target) * 2 - kPart,
			"pending data reaches the manual target without a recovery hold");
	}
	limiter.configure(target, 2 * kPart, now + 180000);
	Require(
		limiter.rate() == target && limiter.ceiling() == 0,
		"an expired recovery period cannot raise the manual target");
}

void TestImmediateAutomaticRecovery() {
	using Limiter = Storage::DownloadRateLimiter;
	for (const auto maximumKiB : { 0, 1, 1024 }) {
		auto limiter = Limiter();
		const auto target = Limiter::Target(1196775, maximumKiB);
		const auto peak = Limiter::Target(1196775, maximumKiB, true);
		limiter.configure(target, 2 * kPart, 0, peak, true);
		limiter.penalize(6000, 126000, Limiter::Recovery::Immediate);
		limiter.configure(target, 2 * kPart, 6000, peak, true);
		Require(
			limiter.rate() == target && limiter.ceiling() == 0,
			"automatic recovery keeps its normal playback-based target");
		Require(
			!limiter.catchingUp() && limiter.delay(kPart, 6000) > 0,
			"immediate recovery keeps boost cooldown and an empty budget");
		limiter.configure(1024, 2 * kPart, 6001);
		Require(
			limiter.rate() == 1024,
			"a lower target remains effective after immediate recovery");
	}
}

void TestRecoveryPolicySwitch() {
	using Limiter = Storage::DownloadRateLimiter;
	auto limiter = Limiter();
	constexpr auto target = 4 * kPart;
	limiter.configure(target, 2 * kPart, 0);
	limiter.penalize(3000, 63000);
	limiter.configure(target, 2 * kPart, 4000);
	Require(
		limiter.rate() < target,
		"gradual policy starts with a rate penalty");
	limiter.penalize(7000, 127000, Limiter::Recovery::Immediate);
	limiter.configure(target, 2 * kPart, 7000);
	Require(
		limiter.rate() == target && limiter.ceiling() == 0,
		"immediate policy clears a previous rate penalty");
	Require(
		limiter.delay(kPart, 7000) > 0,
		"changing recovery policy does not refill the budget");
	limiter.penalize(10000, 130000, Limiter::Recovery::Gradual);
	limiter.configure(target, 2 * kPart, 10000);
	Require(
		limiter.rate() == target * 3 / 4,
		"gradual recovery remains available independently of the target");
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
	TestAutomaticTargetPercent();
	TestManualTargetsAndBudget();
	TestLargeBurst();
	TestDisabled();
	TestBurstAndSeek();
	TestWaitAndRecovery();
	TestImmediateRecovery();
	TestImmediateAutomaticRecovery();
	TestRecoveryPolicySwitch();
	TestLongIdleAndIndependentServers();
	TestCatchUpTargets();
	TestCatchUpWindowAndTaper();
	TestCatchUpByteBudget();
	TestCatchUpPreservesCredit();
	TestCatchUpServerLimits();
	TestSharedBudget();
	std::cout << "PASS: targets, automatic percentages, manual throughput, disable, large burst, seek, wait, gradual/immediate recovery, idle, catch-up, shared/DC budgets\n";
}
