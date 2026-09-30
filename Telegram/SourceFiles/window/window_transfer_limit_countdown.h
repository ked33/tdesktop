#pragma once

#include <algorithm>
#include <cstdint>

namespace Window {

[[nodiscard]] inline std::int64_t TransferLimitRemainingSeconds(
		std::int64_t retryAt,
		std::int64_t now) {
	const auto remaining = std::max(retryAt - now, std::int64_t(0));
	return remaining / 1000 + (remaining % 1000 != 0);
}

[[nodiscard]] inline std::int64_t TransferLimitNextTick(
		std::int64_t retryAt,
		std::int64_t now) {
	const auto remaining = std::max(retryAt - now, std::int64_t(0));
	return remaining ? (remaining - 1) % 1000 + 1 : 0;
}

} // namespace Window
