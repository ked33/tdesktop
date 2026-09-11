/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_porn_search_policy.h"

#include <array>
#include <compare>
#include <cstdlib>
#include <iostream>

namespace {

namespace Policy = Api::PornSearchPolicy;

auto TotalChecks = 0;

void Check(bool condition, const char *name) {
	++TotalChecks;
	if (!condition) {
		std::cerr << "FAILED: " << name << '\n';
		std::exit(1);
	}
}

struct MessageId {
	int peer = 0;
	int msg = 0;
	friend auto operator<=>(const MessageId &, const MessageId &) = default;
};

struct Message {
	MessageId id;
	int date = 0;
	friend bool operator==(const Message &, const Message &) = default;
};

std::vector<Message> Merge(
		const std::vector<Message> &native,
		const std::vector<Message> &additional,
		bool enabled = true) {
	return Policy::MergeResults(
		native,
		additional,
		enabled,
		[](const Message &message) { return message.id; },
		[](const Message &message) { return message.date; });
}

void CheckMergedPages() {
	const auto native = std::vector<Message>{
		{ { 2, 100 }, 400 },
		{ { 1, 10 }, 300 },
		{ { 4, 9 }, 100 },
	};
	const auto pages = std::array{
		std::vector<Message>{ { { 1, 10 }, 300 }, { { 1, 8 }, 200 } },
		std::vector<Message>{ { { 3, 10 }, 500 }, { { 3, 9 }, 300 } },
		std::vector<Message>{ { { 1, 12 }, 300 }, { { 1, 7 }, 100 } },
	};
	const auto expected = std::vector<Message>{
		{ { 3, 10 }, 500 },
		{ { 2, 100 }, 400 },
		{ { 1, 12 }, 300 },
		{ { 1, 10 }, 300 },
		{ { 3, 9 }, 300 },
		{ { 1, 8 }, 200 },
		{ { 1, 7 }, 100 },
		{ { 4, 9 }, 100 },
	};
	auto order = std::array{ 0, 1, 2 };
	do {
		auto additional = std::vector<Message>();
		for (const auto index : order) {
			additional.insert(
				additional.end(),
				pages[index].begin(),
				pages[index].end());
			const auto partial = Merge(native, additional);
			Check(
				std::is_sorted(partial.begin(), partial.end(), [](auto a, auto b) {
					return a.date > b.date;
				}),
				"every arriving page preserves descending message time");
		}
		Check(
			Merge(native, additional) == expected,
			"page order does not affect results");
		Check(
			Merge(additional, native) == expected,
			"a late native page keeps supplemental results");
		Check(
			Merge(native, additional, false) == native,
			"disabling restores native results");
		additional.insert(additional.end(), pages[0].begin(), pages[0].end());
		Check(
			Merge(native, additional) == expected,
			"repeated pages and cross-source duplicates collapse");
	} while (std::next_permutation(order.begin(), order.end()));
	Check(Merge({}, {}) == std::vector<Message>(), "empty sources remain empty");
	const auto nativeOrder = std::vector<Message>{ native[2], native[0], native[2] };
	Check(
		Merge(nativeOrder, pages[0], false)
			== std::vector<Message>{ native[2], native[0] },
		"disabled mode retains native order and unique message identities");
	Check(Policy::PageAdvanced(0, 100), "first nonempty page establishes its cursor");
	Check(Policy::PageAdvanced(100, 50), "older page advances its cursor");
	Check(!Policy::PageAdvanced(100, 100), "duplicate page stops automatic pagination");
	Check(
		!Policy::PageAdvanced(100, 101),
		"backward cursor cannot restart newer messages");
	Check(!Policy::PageAdvanced(100, 0), "missing cursor cannot restart at page one");
	Check(!Policy::PageAdvanced(0, -1), "invalid message ids cannot advance pagination");
}

void CheckRequestGate() {
	auto gate = Policy::RequestGate();
	Check(gate.canStart(0, 0, 3), "idle queue starts immediately");
	gate.started(0);
	Check(!gate.canStart(499, 0, 20), "high concurrency still observes request spacing");
	Check(gate.canStart(500, 0, 3), "next request starts after the spacing interval");
	Check(!gate.canStart(500, 3, 3), "default concurrency bounds in-flight requests");
	Check(gate.canStart(500, 3, 10), "raising concurrency takes effect immediately");
	Check(!gate.canStart(500, 10, 3), "lowering concurrency waits for existing requests");
	Check(gate.canStart(500, 2, 3), "queue resumes below the lowered limit");
	gate.pause(500, 10);
	gate.pause(1000, 1);
	Check(gate.delay(1000) == 9500, "a shorter flood response cannot shorten the pause");
	Check(!gate.canStart(10499, 0, 20), "flood pause applies even to a new query");
	Check(!gate.finishPause(10499), "pause cannot clear before its deadline");
	Check(gate.finishPause(10500), "expired pause can update the UI once");
	Check(!gate.finishPause(10500), "expired pause is not published repeatedly");
	Check(gate.canStart(10500, 0, 20), "queue resumes after flood wait");
	gate.pause(11000, 0);
	Check(gate.delay(11000) == 1000, "invalid flood duration has a nonzero fallback");
	gate.pause(11000, 20);
	Check(gate.delay(11000) == 20000, "a longer flood wait extends the pause");
	Check(!gate.canStart(40000, 0, 0), "invalid concurrency cannot start requests");
}

void CheckLargeQueue(int limit) {
	auto gate = Policy::RequestGate();
	auto completions = std::vector<std::int64_t>();
	auto started = 0;
	auto peak = std::size_t(0);
	auto previous = std::int64_t(-500);
	for (auto now = std::int64_t(0); started != 150; now += 250) {
		Check(now < 150 * 20000, "150-chat queue eventually completes");
		std::erase_if(completions, [&](auto at) { return at <= now; });
		if (gate.canStart(now, completions.size(), limit)) {
			Check(now - previous >= 500, "large queue never sends a burst");
			previous = now;
			gate.started(now);
			completions.push_back(now + 20000);
			++started;
			peak = std::max(peak, completions.size());
		}
		Check(
			completions.size() <= std::size_t(limit),
			"large queue respects configured concurrency");
	}
	Check(
		peak == std::size_t(limit),
		"configured concurrency is attainable for slow responses");
}

} // namespace

int main() {
	CheckMergedPages();
	CheckRequestGate();
	for (const auto limit : std::array{ 1, 3, 10, 20, 32 }) {
		CheckLargeQueue(limit);
	}
	std::cout << "Supplemental search regression: "
		<< TotalChecks << " checks passed.\n";
	return 0;
}
