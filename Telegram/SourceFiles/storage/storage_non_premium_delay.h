#pragma once

#include "base/timer.h"

namespace Storage {

struct NonPremiumDelayInfo {
	int serverWaitSeconds = 0;
	int overrideWaitMs = -1;
	int appliedWaitMs = 0;
};

struct NonPremiumDelayState {
	crl::time limitedUntil = 0;
	crl::time recoveryUntil = 0;
};

} // namespace Storage
