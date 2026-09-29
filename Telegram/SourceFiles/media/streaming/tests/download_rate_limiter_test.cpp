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
	TestBurstAndSeek();
	TestWaitAndRecovery();
	TestLongIdleAndIndependentServers();
	TestSharedBudget();
	std::cout << "PASS: targets, burst, seek, wait, recovery, idle, shared/DC budgets\n";
}
