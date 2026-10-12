#include "storage/avatar_retry_policy.h"

#include <cstdlib>
#include <iostream>

namespace {

using Storage::AvatarRetryKey;
using Storage::AvatarRetryPolicy;

void Require(bool condition, const char *message) {
	if (!condition) {
		std::cerr << message << '\n';
		std::exit(1);
	}
}

void CheckRecordedFloodSequence() {
	auto policy = AvatarRetryPolicy();
	const auto key = AvatarRetryKey{ 1, 2, 3, 0 };
	Require(policy.notify(key, 1000), "first limit may show a short toast");
	const auto first = policy.failed(key, 1000, 37010);
	Require(!first.stopped, "first 36-second wait permits a retry");
	Require(policy.blockedUntil(key, 2000) == 37010,
		"a replacement loader must preserve the server deadline");
	Require(!policy.notify(key, 37020), "the same resource must not repeat its toast");
	const auto second = policy.failed(key, 37020, 89030);
	Require(!second.stopped && second.failures == 2,
		"second 52-second wait stays in the original budget");
	const auto third = policy.failed(key, 89040, 99050);
	Require(third.stopped && third.failures == 3,
		"third limit terminates the request without a third retry");
	Require(third.retryAt == 389040, "exhaustion starts a five-minute cooldown");
	Require(policy.blockedUntil(key, 100000) == third.retryAt,
		"recreating a loader must not bypass the cooldown");
	Require(!policy.notify(key, 100000), "recreating a toast must not reset notification state");
	Require(policy.blockedUntil(key, third.retryAt) == third.retryAt,
		"a fresh demand is allowed after cooldown");
	Require(policy.notify(key, third.retryAt), "a later round has its own notification budget");
}

void CheckLongWaitAndTimeout() {
	auto policy = AvatarRetryPolicy();
	const auto key = AvatarRetryKey{ 2, 3, 4, 0 };
	const auto longWait = policy.failed(key, 1000, 901000);
	Require(longWait.stopped, "a server wait beyond the window releases the loader");
	Require(longWait.retryAt == 901000, "local cooldown never shortens the server wait");
	Require(policy.blockedUntil(key, 301001) == 901000,
		"long server waits survive the default cooldown");
	policy.succeeded(key);
	const auto first = policy.failed(key, 1000, 2000);
	Require(first.deadline == 121000, "window is measured from the first failure");
	Require(policy.expire(key, first.deadline) == 421000,
		"a stalled in-flight retry is stopped at the window deadline");
	Require(policy.blockedUntil(key, 421000) == 421000,
		"expiry is temporary rather than a permanent image failure");
}

void CheckResourceIsolationAndRecovery() {
	auto policy = AvatarRetryPolicy();
	const auto small = AvatarRetryKey{ 7, 8, 9, 0 };
	const auto big = AvatarRetryKey{ 7, 8, 9, 1 };
	const auto changed = AvatarRetryKey{ 7, 8, 10, 0 };
	const auto otherPeer = AvatarRetryKey{ 8, 8, 9, 0 };
	const auto first = policy.failed(small, 1000, 2500);
	Require(!first.stopped, "a short wait is recoverable");
	Require(policy.blockedUntil(big, 1000) == 1000, "avatar sizes are isolated");
	Require(policy.blockedUntil(changed, 1000) == 1000, "a new photo version is independent");
	Require(policy.blockedUntil(otherPeer, 1000) == 1000, "other peers are unaffected");
	auto otherAccount = AvatarRetryPolicy();
	Require(otherAccount.blockedUntil(small, 1000) == 1000, "accounts own separate policies");
	policy.succeeded(small);
	Require(policy.blockedUntil(small, 1100) == 1100, "success clears the old wait");
	const auto recovered = policy.failed(small, 2000, 3000);
	Require(recovered.failures == 1, "success clears the failure budget");
	Require(policy.blockedUntil(small, 122000) == 422000,
		"loader reconstruction after the window enters cooldown");
}

void CheckCapacityAndClockBoundaries() {
	auto policy = AvatarRetryPolicy();
	const auto original = AvatarRetryKey{ 0, 0, 0, 0 };
	for (auto i = std::size_t(0); i != AvatarRetryPolicy::kCapacity; ++i) {
		const auto key = AvatarRetryKey{ std::uint64_t(i), 0, 0, 0 };
		const auto state = policy.failed(key, 0, 2000000);
		Require(state.stopped, "long waits occupy bounded cooldown records");
	}
	const auto extra = AvatarRetryKey{ 2000, 0, 0, 0 };
	const auto overflow = policy.failed(extra, 1, 3000000);
	Require(overflow.stopped && overflow.retryAt == 3000000,
		"overflow preserves an untracked server deadline");
	Require(policy.blockedUntil(original, 300001) == 2000000,
		"capacity pressure must not evict live server waits");
	Require(policy.blockedUntil(extra, 300001) == 3000000,
		"untracked avatars stay gated during overflow");
	Require(policy.blockedUntil(extra, 3000000) == 3000000,
		"overflow gating also expires");
	Require(policy.notify(extra, 3000000), "expired records are reclaimed");
	const auto atBoundary = policy.failed(extra, 3000000, 3120000);
	Require(atBoundary.stopped, "a retry at the budget deadline must not be sent");
}

} // namespace

int main() {
	CheckRecordedFloodSequence();
	CheckLongWaitAndTimeout();
	CheckResourceIsolationAndRecovery();
	CheckCapacityAndClockBoundaries();
	std::cout << "Avatar retry policy checks passed.\n";
}
