/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/dev/LEGAL
*/
#pragma once

#include <array>

#include <QtCore/QString>

namespace Media::Streaming {

struct BoostProfile {
	int requestsLimit = 8;
	int preloadPartsAhead = 8;
	int tailPrefetchParts = 0;
	bool seekCancelEnabled = false;
	int seekCancelJumpParts = 32;
	int seekCancelGuardParts = 4;
	int loadInAdvanceMs = 32000;
	int waitingBufferMs = 3000;
	int startWaitedParts = 4;
	int maxWaitedParts = 16;
	int startSessions = 1;
	int maxSessions = 8;
	int mpvTailPrefetchParts = 0;
	int mpvCacheMaxMb = 0;
	int mpvCacheBackMb = 0;
	int nonPremiumPreloadLimit = 12;
	int smartMinimumPreload = 8;
	int smartMinimumRequests = 4;
	int smartMaximumPreload = 20;
	int smartInitialRequestLimit = 8;
	int smartMinimumRequestLimit = 2;
	int smartMaximumRequestLimit = 10;
	int smartCapacityMinimumRequestLimit = 4;
};

using BoostProfiles = std::array<BoostProfile, 7>;

[[nodiscard]] const BoostProfiles &DefaultBoostProfiles();
[[nodiscard]] BoostProfiles LoadBoostProfiles();
[[nodiscard]] QString SerializeBoostProfiles(const BoostProfiles &profiles);
[[nodiscard]] const BoostProfile &BoostProfileFor(int level);

} // namespace Media::Streaming
