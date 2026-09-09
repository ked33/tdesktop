/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_read_stall.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using Media::Streaming::ReadStallPolicy;

auto TotalChecks = 0;

void Check(bool condition, const char *name) {
	++TotalChecks;
	if (!condition) {
		std::cerr << "FAILED: " << name << '\n';
		std::exit(1);
	}
}

void CheckRequiredRange() {
	auto policy = ReadStallPolicy();
	Check(policy.waitingFor(10000) == 0, "idle reader has no wait");
	Check(!policy.retryReady(10000, 0, 500, 50), "idle reader cannot retry");
	policy.setRead(1000, 5000, 0);
	Check(
		policy.contains(0, 131072),
		"part overlapping required bytes is eligible");
	Check(!policy.contains(6000, 100), "prefetch beyond read is not eligible");
	Check(!policy.contains(0, 1000), "part ending at read start is not eligible");
	Check(!policy.contains(-1, 131072), "negative offset is rejected");
	Check(!policy.contains(1000, 0), "empty part is rejected");
	policy.setRead(1000, 5000, 3500);
	Check(policy.waitingFor(3500) == 3500, "repeated fill preserves wait age");
	Check(!policy.retryReady(3999, 0, 500, 50), "normal latency is not retried");
	Check(policy.retryReady(4000, 0, 500, 50), "stalled required read can retry");
	Check(
		!policy.retryReady(4000, 1000, 500, 50),
		"freshly sent part is protected");
	Check(
		policy.retryReady(4000, 0, 500, 50),
		"failed transport attempt spends no budget");
	policy.retried(4000);
	Check(!policy.retryReady(100000, 0, 0, 0), "one retry per blocking read");
	policy.setRead(-1, 0, 100000);
	Check(policy.waitingFor(200000) == 0, "stop clears blocking read");
	Check(!policy.retryReady(200000, 0, 0, 0), "stop cannot trigger retries");
}

void CheckProgress() {
	auto policy = ReadStallPolicy();
	policy.setRead(1000, 5000, 0);
	policy.progress(100000, 131072, 3000);
	Check(
		policy.waitingFor(4000) == 4000,
		"unrelated prefetch does not hide a stall");
	policy.progress(1000, 1000, 3500);
	Check(policy.waitingFor(4000) == 500, "required data resets no-progress age");
	Check(
		!policy.retryReady(4000, 0, 500, 50),
		"slow progressing read is protected");
	Check(
		policy.retryReady(7500, 0, 500, 50),
		"remaining hole eventually becomes eligible");
	policy.retried(7500);
	policy.progress(2000, 1000, 8000);
	Check(!policy.retryReady(30000, 0, 0, 0), "progress does not reset retry budget");
}

void CheckDispatchPriority() {
	constexpr auto kPart = std::int64_t(131072);
	auto policy = ReadStallPolicy();
	const auto queued = std::array<std::int64_t, 4>{
		0, kPart, 100 * kPart, 101 * kPart,
	};
	Check(!policy.firstRequired(queued, kPart), "idle queue keeps normal priority");
	policy.setRead(100 * kPart + 100, 200, 0);
	Check(
		policy.firstRequired(queued, kPart) == 100 * kPart,
		"current read wins over earlier speculative offsets immediately");
	policy.setRead(101 * kPart - 20, 40, 0);
	Check(
		policy.firstRequired(queued, kPart) == 100 * kPart,
		"cross-part read schedules its first missing part");
	const auto remaining = std::array<std::int64_t, 3>{ 0, kPart, 101 * kPart };
	Check(
		policy.firstRequired(remaining, kPart) == 101 * kPart,
		"cross-part read schedules remaining hole ahead of prefetch");
	policy.setRead(kPart + 20, 100, 500);
	Check(
		policy.firstRequired(queued, kPart) == kPart,
		"new seek immediately replaces old read priority");
	policy.setRead(200 * kPart, 100, 1000);
	Check(
		!policy.firstRequired(queued, kPart),
		"unscheduled blocker is distinct from a slow request");
	policy.setRead(-1, 0, 1000);
	Check(!policy.firstRequired(queued, kPart), "stop releases read priority");
}

void CheckQueuedReadReplacement() {
	constexpr auto kPart = std::int64_t(131072);
	auto policy = ReadStallPolicy();
	policy.setRead(100 * kPart + 100, 200, 0);
	Check(
		!policy.replacementReady(0, kPart, 3999, 0, 0, 0),
		"queued read does not replace normal latency requests");
	Check(
		policy.replacementReady(0, kPart, 4000, 0, 0, 0),
		"queued blocker can replace an old unrelated request");
	Check(
		!policy.replacementReady(0, kPart, 4000, 1, 0, 0),
		"freshly dispatched work cannot be displaced");
	Check(
		!policy.replacementReady(100 * kPart, kPart, 4000, 0, 0, 0),
		"replacement never cancels bytes needed by the current read");
	policy.setRead(101 * kPart - 20, 40, 0);
	Check(
		!policy.replacementReady(101 * kPart, kPart, 4000, 0, 0, 0),
		"both sides of a crossing read are protected");
	Check(
		!policy.replacementReady(-1, kPart, 4000, 0, 0, 0),
		"invalid candidate offsets are rejected");
	policy.retried(4000);
	Check(
		!policy.replacementReady(0, kPart, 50000, 0, 0, 0),
		"only one recovery action is permitted for one blocking read");
	policy.setRead(200 * kPart, 200, 5000);
	Check(
		!policy.retryReady(13999, 5000, 0, 0),
		"replacement and request retry share the same cooldown");
	Check(
		policy.retryReady(14000, 5000, 0, 0),
		"next read recovers after the shared cooldown");
	policy.retried(14000);
	policy.setRead(300 * kPart, 200, 15000);
	Check(
		!policy.replacementReady(0, kPart, 24000, 15000, 0, 0),
		"replacement also respects the rolling recovery budget");
	Check(
		policy.replacementReady(0, kPart, 34000, 15000, 0, 0),
		"replacement budget becomes available after the window expires");
}

void CheckBudgetAcrossSeeks() {
	auto policy = ReadStallPolicy();
	policy.setRead(0, 131072, 0);
	Check(policy.retryReady(4000, 0, 0, 0), "first retry available");
	policy.retried(4000);
	policy.setRead(131072, 131072, 5000);
	Check(!policy.retryReady(13000, 5000, 0, 0), "seek does not bypass cooldown");
	Check(policy.retryReady(14000, 5000, 0, 0), "second retry after cooldown");
	policy.retried(14000);
	policy.setRead(-1, 0, 15000);
	policy.setRead(262144, 131072, 16000);
	Check(
		!policy.retryReady(24000, 16000, 0, 0),
		"stop and seek preserve window budget");
	Check(
		!policy.retryReady(33999, 16000, 0, 0),
		"third retry remains blocked in window");
	Check(
		policy.retryReady(34000, 16000, 0, 0),
		"budget recovers after thirty seconds");
	policy.retried(34000);
	Check(
		!policy.retryReady(70000, 16000, 0, 0),
		"window reset cannot retry same read twice");
}

void CheckRollingBudget() {
	auto policy = ReadStallPolicy();
	policy.setRead(0, 131072, 0);
	Check(policy.retryReady(4000, 0, 0, 0), "rolling window first retry");
	policy.retried(4000);
	policy.setRead(131072, 131072, 24000);
	Check(policy.retryReady(28000, 24000, 0, 0), "late second retry");
	policy.retried(28000);
	policy.setRead(262144, 131072, 34000);
	Check(policy.retryReady(38000, 34000, 0, 0), "oldest retry expires");
	policy.retried(38000);
	policy.setRead(393216, 131072, 44000);
	Check(
		!policy.retryReady(48000, 44000, 0, 0),
		"window boundary cannot allow three nearby retries");
	Check(
		!policy.retryReady(57999, 44000, 0, 0),
		"second oldest retry remains in rolling window");
	Check(
		policy.retryReady(58000, 44000, 0, 0),
		"rolling budget becomes available at exact boundary");
}

void CheckBoundaries() {
	Check(
		ReadStallPolicy::RetryDelay(0, 0) == 4000,
		"retry has a conservative floor");
	Check(
		ReadStallPolicy::RetryDelay(1500, 1000) == 8000,
		"retry adapts to latency and jitter");
	Check(
		ReadStallPolicy::RetryDelay(-100, -100) == 4000,
		"invalid latency uses floor");
	Check(
		ReadStallPolicy::RetryDelay(
			std::numeric_limits<int>::max(),
			std::numeric_limits<int>::max()) == 10000,
		"extreme latency cannot overflow");
	auto policy = ReadStallPolicy();
	policy.setRead(0, 131072, 0);
	Check(!policy.retryReady(7999, 0, 1500, 1000), "adaptive delay is respected");
	Check(
		policy.retryReady(8000, 0, 1500, 1000),
		"adaptive delay eventually expires");
	policy.setRead(131072, 131072, 10000);
	Check(policy.waitingFor(9000) == 0, "backward clock does not create a stall");
	policy.setRead(std::numeric_limits<std::int64_t>::max(), 1, 10000);
	Check(!policy.contains(0, 131072), "overflowing read range is rejected");
	policy.setRead(0, 0, 10000);
	Check(!policy.retryReady(20000, 0, 0, 0), "empty read cannot retry");
}

} // namespace

int main() {
	CheckRequiredRange();
	CheckProgress();
	CheckDispatchPriority();
	CheckQueuedReadReplacement();
	CheckBudgetAcrossSeeks();
	CheckRollingBudget();
	CheckBoundaries();
	std::cout << "Streaming read stall regression: "
		<< TotalChecks << " checks passed.\n";
	return 0;
}
