/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_mpv_http.h"

#include <cstdlib>
#include <iostream>

namespace {

namespace Http = Media::Streaming::Mpv::Http;
using Result = Http::ReadResult;

auto TotalChecks = 0;

void Check(bool condition, const char *name) {
	++TotalChecks;
	if (!condition) {
		std::cerr << "FAILED: " << name << '\n';
		std::exit(1);
	}
}

void CheckCachedRead() {
	auto reads = 0;
	auto waits = 0;
	const auto result = Http::ReadChunk([&] {
		++reads;
		return Result::Success;
	}, [] {
		return false;
	}, [&] {
		++waits;
		return true;
	});
	Check(result == Result::Success, "cached data is served immediately");
	Check(reads == 1 && waits == 0, "cached reads never enter the wait loop");
}

void CheckClosedRequest() {
	auto reads = 0;
	auto waits = 0;
	const auto result = Http::ReadChunk([&] {
		++reads;
		return Result::Waiting;
	}, [] {
		return true;
	}, [&] {
		++waits;
		return true;
	});
	Check(result == Result::Cancelled, "closed requests are cancelled");
	Check(reads == 0 && waits == 0, "closed requests do not start downloads");
}

void CheckDisconnectWithoutRemoteCompletion() {
	auto connected = true;
	auto reads = 0;
	auto waits = 0;
	const auto result = Http::ReadChunk([&] {
		++reads;
		return Result::Waiting;
	}, [&] {
		return !connected;
	}, [&] {
		++waits;
		connected = false;
		return true;
	});
	Check(result == Result::Cancelled, "disconnect does not need a remote response");
	Check(reads == 1 && waits == 1, "abandoned offset is not read again");
}

void CheckSupersededRequest() {
	auto generation = 1;
	const auto requestedGeneration = generation;
	auto oldReads = 0;
	const auto oldResult = Http::ReadChunk([&] {
		++oldReads;
		return Result::Waiting;
	}, [&] {
		return generation != requestedGeneration;
	}, [&] {
		++generation;
		return true;
	});
	Check(oldResult == Result::Cancelled, "new range supersedes a stalled request");
	Check(oldReads == 1, "supersession stops the old read loop");
	const auto newResult = Http::ReadChunk([] {
		return Result::Success;
	}, [] {
		return false;
	}, [] {
		Check(false, "new cached range must not wait for the old range");
		return false;
	});
	Check(newResult == Result::Success, "cached range can proceed after cancellation");
}

void CheckCacheMissDoesNotBlockBackground() {
	auto reads = 0;
	auto waits = 0;
	const auto result = Http::ReadChunk([&] {
		++reads;
		return Result::Waiting;
	}, [] {
		return false;
	}, [&] {
		++waits;
		return false;
	});
	Check(result == Result::Waiting, "background cache miss yields to live requests");
	Check(reads == 1 && waits == 1, "background miss stops without network polling");
}

void CheckRemoteCompletion() {
	auto ready = false;
	auto reads = 0;
	auto waits = 0;
	const auto result = Http::ReadChunk([&] {
		++reads;
		return ready ? Result::Success : Result::Waiting;
	}, [] {
		return false;
	}, [&] {
		++waits;
		ready = true;
		return true;
	});
	Check(result == Result::Success, "live request resumes when data arrives");
	Check(reads == 2 && waits == 1, "ready data is read without an extra wait");
}

void CheckDisconnectDuringRead() {
	auto connected = true;
	const auto result = Http::ReadChunk([&] {
		connected = false;
		return Result::Success;
	}, [&] {
		return !connected;
	}, [] {
		Check(false, "disconnect after fill must not wait");
		return false;
	});
	Check(result == Result::Cancelled, "late data is not sent to an abandoned request");
}

void CheckReadFailure() {
	const auto result = Http::ReadChunk([] {
		return Result::Failed;
	}, [] {
		return false;
	}, [] {
		Check(false, "failed reader must not enter a wait loop");
		return false;
	});
	Check(result == Result::Failed, "read failure remains distinct from cancellation");
}

} // namespace

int main() {
	CheckCachedRead();
	CheckClosedRequest();
	CheckDisconnectWithoutRemoteCompletion();
	CheckSupersededRequest();
	CheckCacheMissDoesNotBlockBackground();
	CheckRemoteCompletion();
	CheckDisconnectDuringRead();
	CheckReadFailure();
	std::cout << "MPV HTTP checks passed: " << TotalChecks << '\n';
}
