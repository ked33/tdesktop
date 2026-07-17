#pragma once

#include "base/timer.h"

#include <algorithm>

namespace Storage {

inline constexpr auto kNonPremiumInitialRequestLimit = 8;
inline constexpr auto kNonPremiumRecoveryFirstStep = 15 * crl::time(1000);
inline constexpr auto kNonPremiumRecoverySecondStep = 45 * crl::time(1000);
inline constexpr auto kNonPremiumRecoveryBaseDuration = 60 * crl::time(1000);
inline constexpr auto kNonPremiumRecoveryPenaltyStep = 30 * crl::time(1000);
inline constexpr auto kNonPremiumRecoveryMaxDuration = 120 * crl::time(1000);

struct NonPremiumDelayInfo {
	int serverWaitSeconds = 0;
	int overrideWaitMs = -1;
	int appliedWaitMs = 0;
};

struct NonPremiumDelayState {
	crl::time limitedUntil = 0;
	crl::time recoveryUntil = 0;
	int penalty = 0;
};

[[nodiscard]] inline crl::time NonPremiumRecoveryDuration(int penalty) {
	return std::min(
		kNonPremiumRecoveryBaseDuration
			+ std::max(penalty - 1, 0)
				* kNonPremiumRecoveryPenaltyStep,
		kNonPremiumRecoveryMaxDuration);
}

[[nodiscard]] inline int NonPremiumRequestLimit(
		const NonPremiumDelayState &state,
		crl::time now) {
	if (now < state.limitedUntil) {
		return 0;
	} else if (!state.recoveryUntil || now >= state.recoveryUntil) {
		return kNonPremiumInitialRequestLimit;
	}
	const auto elapsed = std::max(
		now - state.limitedUntil,
		crl::time(0));
	const auto repeated = (state.penalty >= 2);
	if (elapsed < kNonPremiumRecoveryFirstStep) {
		return repeated ? 2 : 4;
	} else if (elapsed < kNonPremiumRecoverySecondStep) {
		return repeated ? 4 : 6;
	}
	return repeated ? 6 : kNonPremiumInitialRequestLimit;
}

[[nodiscard]] inline crl::time NonPremiumNextStateChange(
		const NonPremiumDelayState &state,
		crl::time now) {
	if (state.limitedUntil > now) {
		return state.limitedUntil;
	} else if (state.recoveryUntil <= now) {
		return 0;
	}
	const auto first = state.limitedUntil
		+ kNonPremiumRecoveryFirstStep;
	const auto second = state.limitedUntil
		+ kNonPremiumRecoverySecondStep;
	if (first > now) {
		return first;
	} else if (second > now) {
		return second;
	}
	return state.recoveryUntil;
}

} // namespace Storage
