#include "window/window_transfer_limit_countdown.h"

#include <array>
#include <cassert>
#include <iostream>
#include <limits>

int main() {
	using Window::TransferLimitNextTick;
	using Window::TransferLimitRemainingSeconds;
	constexpr auto deadline = std::int64_t(50000);
	const auto times = std::array<std::int64_t, 9>{
		0, 1, 999, 1000, 1001, 47003, 49000, 49999, 50000,
	};
	const auto seconds = std::array<std::int64_t, 9>{
		50, 50, 50, 49, 49, 3, 1, 1, 0,
	};
	for (auto i = std::size_t(0); i != times.size(); ++i) {
		assert(TransferLimitRemainingSeconds(deadline, times[i]) == seconds[i]);
		const auto delay = TransferLimitNextTick(deadline, times[i]);
		assert(delay >= 0 && delay <= 1000);
		if (seconds[i] > 0) {
			assert(delay > 0);
			assert(TransferLimitRemainingSeconds(deadline, times[i] + delay)
				== seconds[i] - 1);
		}
	}
	assert(TransferLimitRemainingSeconds(deadline, deadline + 30000) == 0);
	assert(TransferLimitNextTick(deadline, deadline + 30000) == 0);
	assert(TransferLimitRemainingSeconds(0, 0) == 0);
	assert(TransferLimitNextTick(0, 0) == 0);
	const auto extended = deadline + 32000;
	assert(TransferLimitRemainingSeconds(extended, deadline) == 32);
	assert(TransferLimitNextTick(extended, deadline + 750) == 250);
	const auto longest = std::int64_t(std::numeric_limits<int>::max()) * 1000;
	assert(TransferLimitRemainingSeconds(longest, 0)
		== std::numeric_limits<int>::max());
	assert(TransferLimitNextTick(longest, 1) == 999);
	std::cout << "Countdown deadline and tick checks passed.\n";
}
