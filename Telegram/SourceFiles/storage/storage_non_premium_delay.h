#pragma once

namespace Storage {

struct NonPremiumDelayInfo {
	int serverWaitSeconds = 0;
	int overrideWaitMs = -1;
	int appliedWaitMs = 0;
};

} // namespace Storage
