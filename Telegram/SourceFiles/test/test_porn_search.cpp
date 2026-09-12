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

struct SearchSource {
	int offsetId = 0;
	std::set<int> messages;
	bool started = false;
	bool failed = false;
	friend bool operator==(
		const SearchSource &,
		const SearchSource &) = default;
};

void CheckSourceSnapshot() {
	using Snapshot = Policy::SourceSnapshot<int, SearchSource>;
	auto sources = Snapshot();
	auto catalog = Snapshot::Entries();
	auto collections = 0;
	const auto collect = [&] {
		++collections;
		return catalog;
	};
	Check(!sources.ready(), "a new query waits for its chat catalog");
	for (auto peer = 1; peer <= 109; ++peer) {
		catalog.emplace(peer, SearchSource());
		Check(
			!sources.prepare(false, collect),
			"partial catalog pages cannot start supplemental searches");
	}
	Check(collections == 0, "the query does not collect partial chat catalogs");
	Check(sources.entries().empty(), "no source starts before the total is known");
	Check(sources.prepare(true, collect), "a complete catalog starts the query");
	Check(sources.ready(), "the search total is now known");
	Check(sources.entries().size() == 109, "all 109 sources are captured together");
	const auto &confirmed = sources;
	Check(
		confirmed.entries() == catalog,
		"published progress reads the same confirmed sources");

	auto &paged = sources.entries().at(1);
	paged.offsetId = 40;
	paged.messages = { 40, 60, 80 };
	paged.failed = true;
	catalog.erase(1);
	catalog.emplace(110, SearchSource());
	catalog.emplace(111, SearchSource());
	for (const auto searched : std::array{ 3, 11, 45, 99, 109 }) {
		for (auto peer = 1; peer <= searched; ++peer) {
			sources.entries().at(peer).started = true;
		}
		const auto beforeRefresh = sources.entries();
		for (const auto complete : std::array{ false, true }) {
			Check(
				!sources.prepare(complete, collect),
				"background catalog invalidation and refresh cannot recapture sources");
			Check(sources.ready(), "a confirmed search total never becomes unknown");
			Check(sources.entries().size() == 109, "the confirmed total stays at 109");
			Check(
				sources.entries() == beforeRefresh,
				"refresh preserves search progress, failures, messages and page cursors");
			Check(
				std::count_if(
					sources.entries().begin(),
					sources.entries().end(),
					[](const auto &entry) { return entry.second.started; }) == searched,
				"the completed-chat counter cannot regress during a catalog refresh");
		}
	}
	paged.failed = false;
	Check(
		!sources.prepare(true, collect) && paged.offsetId == 40,
		"retrying a source resumes the saved page cursor");
	Check(collections == 1, "the active query captures its catalog only once");

	auto nextQuery = Snapshot();
	Check(
		nextQuery.prepare(true, collect),
		"a new query can use the refreshed catalog");
	Check(
		nextQuery.entries() == catalog && sources.entries().size() == 109,
		"new queries see membership changes without altering active queries");
	Check(!nextQuery.entries().contains(1), "the new query excludes removed chats");
	Check(
		nextQuery.entries().contains(111),
		"the new query includes newly added chats");

	auto empty = Snapshot();
	catalog.clear();
	Check(empty.prepare(true, collect), "an empty complete catalog is confirmed");
	catalog.emplace(200, SearchSource());
	Check(
		!empty.prepare(true, collect) && empty.ready() && empty.entries().empty(),
		"a confirmed empty query also keeps its original scope");
}

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

void CheckAdjustableInterval() {
	auto gate = Policy::RequestGate();
	gate.started(0);
	gate.setInterval(100);
	Check(gate.delay(50) == 50, "shortening the interval updates a queued request");
	Check(gate.canStart(100, 2, 3), "a shorter interval takes effect immediately");
	gate.setInterval(5000);
	Check(gate.delay(100) == 4900, "lengthening the interval updates a queued request");
	gate.setInterval(0);
	Check(gate.canStart(100, 2, 3), "zero interval removes the fixed pacing delay");
	Check(!gate.canStart(100, 3, 3), "zero interval still enforces concurrency");
	gate.pause(100, 10);
	gate.setInterval(500);
	gate.setInterval(0);
	Check(!gate.canStart(500, 0, 20), "changing the interval cannot bypass a flood wait");
	Check(gate.canStart(10100, 0, 20), "zero interval can resume after the flood wait");
}

std::int64_t QueueDuration(int limit, std::int64_t interval) {
	auto gate = Policy::RequestGate();
	gate.setInterval(interval);
	auto completions = std::vector<std::int64_t>();
	auto started = 0;
	for (auto now = std::int64_t(0);; now += 50) {
		Check(now < 150 * 2000, "short-latency queue eventually completes");
		std::erase_if(completions, [&](auto at) { return at <= now; });
		if (started == 150 && completions.empty()) {
			return now;
		}
		while (started != 150 && gate.canStart(now, completions.size(), limit)) {
			gate.started(now);
			completions.push_back(now + 1000);
			++started;
		}
		Check(
			completions.size() <= std::size_t(limit),
			"an adjustable interval never exceeds the concurrent request limit");
	}
}

void CheckConcurrencyThroughput() {
	const auto spacedThree = QueueDuration(3, 500);
	const auto spacedTwenty = QueueDuration(20, 500);
	const auto unspacedThree = QueueDuration(3, 0);
	const auto unspacedTwenty = QueueDuration(20, 0);
	Check(
		spacedThree == spacedTwenty,
		"a fixed interval can hide concurrency gains for fast responses");
	Check(
		unspacedThree < spacedThree,
		"removing the fixed interval improves throughput at the same concurrency");
	Check(
		unspacedTwenty < unspacedThree,
		"higher concurrency can improve throughput without fixed pacing");
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

void CheckFloodNotices() {
	auto gate = Policy::RequestGate();
	Check(!gate.takePauseNotice(0), "normal request queue needs no flood toast");
	gate.started(0);
	Check(!gate.pauseRemaining(0), "request spacing is not a server flood wait");
	gate.pause(100, 10);
	gate.pause(200, 20);
	Check(
		gate.takePauseNotice(300) == 19900,
		"simultaneous flood replies produce one notice for the longest wait");
	Check(!gate.takePauseNotice(400), "only one window consumes the flood notice");
	gate.pause(500, 30);
	Check(!gate.takePauseNotice(600), "extending the same pause does not spam toasts");
	Check(gate.pauseRemaining(600) == 29900, "extended wait still delays all searches");
	Check(gate.finishPause(30500), "the extended pause expires at its deadline");
	gate.pause(40000, 5);
	Check(gate.takePauseNotice(40000) == 5000, "a later flood gets a new notice");
	gate.pause(50000, 5);
	Check(!gate.takePauseNotice(55000), "expired notices are not shown");
}

void CheckSearchTiming() {
	auto elapsed = Policy::ElapsedTime();
	Check(elapsed.elapsed(5000) == 0, "a new search has no elapsed time");
	elapsed.setRunning(5000, true);
	elapsed.setRunning(6000, true);
	Check(elapsed.elapsed(7000) == 2000, "progress updates do not restart the clock");
	elapsed.setRunning(45000, false);
	Check(
		elapsed.elapsed(65000) == 40000,
		"reading results between pages does not increase search time");
	elapsed.setRunning(65000, true);
	Check(elapsed.elapsed(85000) == 60000, "pagination resumes the same elapsed time");
	elapsed.setRunning(85000, true);
	Check(
		elapsed.elapsed(105000) == 80000,
		"searching and automatic flood waiting use continuous elapsed time");
	elapsed.setRunning(105000, false);
	Check(elapsed.elapsed(110000) == 80000, "completion freezes elapsed time");
	Check(!elapsed.running(), "the repaint timer can stop when idle");
	Check(Policy::FormatDuration(0) == "0s", "zero-second format");
	Check(Policy::FormatDuration(40) == "40s", "seconds-only format");
	Check(Policy::FormatDuration(59) == "59s", "last second before a minute");
	Check(Policy::FormatDuration(60) == "1m0s", "exact-minute format");
	Check(Policy::FormatDuration(80) == "1m20s", "minutes-and-seconds format");
	elapsed = {};
	Check(elapsed.elapsed(110000) == 0, "a replacement query resets elapsed time");
}

void CheckExactCounts() {
	Check(Policy::ExactCountReached(0, 0), "empty exact result is complete");
	Check(Policy::ExactCountReached(1, 1), "one hit needs no confirming empty page");
	Check(Policy::ExactCountReached(50, 50), "an exact full page can also be complete");
	Check(!Policy::ExactCountReached(20, 100), "a short page alone does not prove completion");
	Check(!Policy::ExactCountReached(50, 100), "more matches still need pagination");
	Check(!Policy::ExactCountReached(100, -1), "inexact totals cannot end pagination early");
	const auto duplicates = std::vector<Message>{
		{ { 1, 5 }, 10 },
		{ { 1, 5 }, 10 },
	};
	Check(
		!Policy::ExactCountReached(Merge({}, duplicates).size(), 2),
		"duplicate hits do not satisfy the exact total");
}

} // namespace

int main() {
	CheckSourceSnapshot();
	CheckMergedPages();
	CheckRequestGate();
	CheckAdjustableInterval();
	CheckConcurrencyThroughput();
	CheckFloodNotices();
	CheckSearchTiming();
	CheckExactCounts();
	for (const auto limit : std::array{ 1, 3, 10, 20, 32 }) {
		CheckLargeQueue(limit);
	}
	std::cout << "Supplemental search regression: "
		<< TotalChecks << " checks passed.\n";
	return 0;
}
