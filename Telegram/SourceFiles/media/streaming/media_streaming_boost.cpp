/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/dev/LEGAL
*/
#include "media/streaming/media_streaming_boost.h"

#include "settings.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <algorithm>

namespace Media::Streaming {
namespace {

constexpr auto kProfilesKey = "net_download_speed_boost_profiles";
constexpr auto kHighBitrateBytesPerSecond = 1024 * 1024;
constexpr auto kPlaybackRateMaximum = 64 * 1024 * 1024;

[[nodiscard]] int ReadInt(
		const QJsonObject &object,
		const char *key,
		int fallback,
		int minimum,
		int maximum) {
	const auto value = object.value(QString::fromLatin1(key));
	return value.isDouble()
		? std::clamp(value.toInt(), minimum, maximum)
		: fallback;
}

[[nodiscard]] bool ReadBool(
		const QJsonObject &object,
		const char *key,
		bool fallback) {
	const auto value = object.value(QString::fromLatin1(key));
	return value.isBool() ? value.toBool() : fallback;
}

void WriteProfile(QJsonObject &object, const BoostProfile &profile) {
	object.insert("requestsLimit", profile.requestsLimit);
	object.insert("preloadPartsAhead", profile.preloadPartsAhead);
	object.insert("tailPrefetchParts", profile.tailPrefetchParts);
	object.insert("seekCancelEnabled", profile.seekCancelEnabled);
	object.insert("seekCancelJumpParts", profile.seekCancelJumpParts);
	object.insert("seekCancelGuardParts", profile.seekCancelGuardParts);
	object.insert("loadInAdvanceMs", profile.loadInAdvanceMs);
	object.insert("waitingBufferMs", profile.waitingBufferMs);
	object.insert("startWaitedParts", profile.startWaitedParts);
	object.insert("maxWaitedParts", profile.maxWaitedParts);
	object.insert("startSessions", profile.startSessions);
	object.insert("maxSessions", profile.maxSessions);
	object.insert("mpvTailPrefetchParts", profile.mpvTailPrefetchParts);
	object.insert("mpvCacheMaxMb", profile.mpvCacheMaxMb);
	object.insert("mpvCacheBackMb", profile.mpvCacheBackMb);
	object.insert("nonPremiumPreloadLimit", profile.nonPremiumPreloadLimit);
	object.insert("smartMinimumPreload", profile.smartMinimumPreload);
	object.insert("smartMinimumRequests", profile.smartMinimumRequests);
	object.insert("smartMaximumPreload", profile.smartMaximumPreload);
	object.insert("smartInitialRequestLimit", profile.smartInitialRequestLimit);
	object.insert("smartMinimumRequestLimit", profile.smartMinimumRequestLimit);
	object.insert("smartMaximumRequestLimit", profile.smartMaximumRequestLimit);
	object.insert(
		"smartCapacityMinimumRequestLimit",
		profile.smartCapacityMinimumRequestLimit);
}

} // namespace

const BoostProfiles &DefaultBoostProfiles() {
	static const auto result = [] {
		return BoostProfiles{
			BoostProfile{
				.requestsLimit = 8,
				.preloadPartsAhead = 8,
				.tailPrefetchParts = 0,
				.seekCancelEnabled = false,
				.seekCancelJumpParts = 32,
				.seekCancelGuardParts = 4,
				.loadInAdvanceMs = 32000,
				.waitingBufferMs = 3000,
				.startWaitedParts = 4,
				.maxWaitedParts = 16,
				.startSessions = 1,
				.maxSessions = 8,
				.mpvTailPrefetchParts = 0,
				.mpvCacheMaxMb = 0,
				.mpvCacheBackMb = 0,
				.nonPremiumPreloadLimit = 12,
				.smartMinimumPreload = 8,
				.smartMinimumRequests = 4,
				.smartMaximumPreload = 20,
				.smartInitialRequestLimit = 8,
				.smartMinimumRequestLimit = 2,
				.smartMaximumRequestLimit = 10,
				.smartCapacityMinimumRequestLimit = 4,
			},
			BoostProfile{
				.requestsLimit = 12,
				.preloadPartsAhead = 12,
				.tailPrefetchParts = 2,
				.seekCancelEnabled = true,
				.loadInAdvanceMs = 40000,
				.waitingBufferMs = 2600,
				.startWaitedParts = 8,
				.maxWaitedParts = 24,
				.startSessions = 2,
				.maxSessions = 8,
				.mpvTailPrefetchParts = 2,
				.mpvCacheMaxMb = 512,
				.mpvCacheBackMb = 128,
			},
			BoostProfile{
				.requestsLimit = 16,
				.preloadPartsAhead = 16,
				.tailPrefetchParts = 2,
				.seekCancelEnabled = true,
				.loadInAdvanceMs = 48000,
				.waitingBufferMs = 2300,
				.startWaitedParts = 10,
				.maxWaitedParts = 32,
				.startSessions = 2,
				.maxSessions = 10,
				.mpvTailPrefetchParts = 2,
				.mpvCacheMaxMb = 512,
				.mpvCacheBackMb = 128,
			},
			BoostProfile{
				.requestsLimit = 20,
				.preloadPartsAhead = 24,
				.tailPrefetchParts = 3,
				.seekCancelEnabled = true,
				.loadInAdvanceMs = 56000,
				.waitingBufferMs = 2100,
				.startWaitedParts = 12,
				.maxWaitedParts = 40,
				.startSessions = 3,
				.maxSessions = 12,
				.mpvTailPrefetchParts = 3,
				.mpvCacheMaxMb = 512,
				.mpvCacheBackMb = 128,
			},
			BoostProfile{
				.requestsLimit = 24,
				.preloadPartsAhead = 32,
				.tailPrefetchParts = 3,
				.seekCancelEnabled = true,
				.loadInAdvanceMs = 64000,
				.waitingBufferMs = 1900,
				.startWaitedParts = 14,
				.maxWaitedParts = 48,
				.startSessions = 4,
				.maxSessions = 14,
				.mpvTailPrefetchParts = 3,
				.mpvCacheMaxMb = 512,
				.mpvCacheBackMb = 128,
			},
			BoostProfile{
				.requestsLimit = 32,
				.preloadPartsAhead = 48,
				.tailPrefetchParts = 4,
				.seekCancelEnabled = true,
				.loadInAdvanceMs = 80000,
				.waitingBufferMs = 1700,
				.startWaitedParts = 16,
				.maxWaitedParts = 64,
				.startSessions = 5,
				.maxSessions = 16,
				.mpvTailPrefetchParts = 4,
				.mpvCacheMaxMb = 512,
				.mpvCacheBackMb = 128,
			},
			BoostProfile{
				.requestsLimit = 12,
				.preloadPartsAhead = 16,
				.tailPrefetchParts = 2,
				.seekCancelEnabled = true,
				.loadInAdvanceMs = 40000,
				.waitingBufferMs = 2600,
				.startWaitedParts = 8,
				.maxWaitedParts = 24,
				.startSessions = 2,
				.maxSessions = 8,
				.mpvTailPrefetchParts = 2,
				.mpvCacheMaxMb = 512,
				.mpvCacheBackMb = 128,
				.smartMinimumPreload = 8,
				.smartMinimumRequests = 4,
				.smartMaximumPreload = 20,
				.smartInitialRequestLimit = 8,
				.smartMinimumRequestLimit = 2,
				.smartMaximumRequestLimit = 10,
				.smartCapacityMinimumRequestLimit = 4,
			},
		};
	}();
	return result;
}

BoostProfiles LoadBoostProfiles() {
	auto result = DefaultBoostProfiles();
	const auto document = QJsonDocument::fromJson(
		GetEnhancedString(QString::fromLatin1(kProfilesKey)).toUtf8());
	if (!document.isArray()) {
		return result;
	}
	const auto array = document.array();
	const auto count = std::min(int(result.size()), int(array.size()));
	for (auto i = 0; i != count; ++i) {
		if (!array.at(i).isObject()) {
			continue;
		}
		const auto object = array.at(i).toObject();
		auto &profile = result[i];
		const auto &defaults = DefaultBoostProfiles()[i];
		profile.requestsLimit = ReadInt(
			object,
			"requestsLimit",
			defaults.requestsLimit,
			1,
			32);
		profile.preloadPartsAhead = ReadInt(
			object,
			"preloadPartsAhead",
			defaults.preloadPartsAhead,
			1,
			64);
		profile.tailPrefetchParts = ReadInt(
			object,
			"tailPrefetchParts",
			defaults.tailPrefetchParts,
			0,
			16);
		profile.seekCancelEnabled = ReadBool(object, "seekCancelEnabled", defaults.seekCancelEnabled);
		profile.seekCancelJumpParts = ReadInt(
			object,
			"seekCancelJumpParts",
			defaults.seekCancelJumpParts,
			1,
			256);
		profile.seekCancelGuardParts = ReadInt(
			object,
			"seekCancelGuardParts",
			defaults.seekCancelGuardParts,
			0,
			64);
		profile.loadInAdvanceMs = ReadInt(
			object,
			"loadInAdvanceMs",
			defaults.loadInAdvanceMs,
			0,
			300000);
		profile.waitingBufferMs = ReadInt(
			object,
			"waitingBufferMs",
			defaults.waitingBufferMs,
			0,
			30000);
		profile.startWaitedParts = ReadInt(
			object,
			"startWaitedParts",
			defaults.startWaitedParts,
			1,
			128);
		profile.maxWaitedParts = ReadInt(
			object,
			"maxWaitedParts",
			defaults.maxWaitedParts,
			1,
			256);
		profile.startSessions = ReadInt(
			object,
			"startSessions",
			defaults.startSessions,
			1,
			32);
		profile.maxSessions = ReadInt(
			object,
			"maxSessions",
			defaults.maxSessions,
			1,
			64);
		profile.mpvTailPrefetchParts = ReadInt(
			object,
			"mpvTailPrefetchParts",
			defaults.mpvTailPrefetchParts,
			0,
			16);
		profile.mpvCacheMaxMb = ReadInt(
			object,
			"mpvCacheMaxMb",
			defaults.mpvCacheMaxMb,
			0,
			4096);
		profile.mpvCacheBackMb = ReadInt(
			object,
			"mpvCacheBackMb",
			defaults.mpvCacheBackMb,
			0,
			1024);
		profile.nonPremiumPreloadLimit = ReadInt(
			object,
			"nonPremiumPreloadLimit",
			defaults.nonPremiumPreloadLimit,
			1,
			128);
		profile.smartMinimumPreload = ReadInt(
			object,
			"smartMinimumPreload",
			defaults.smartMinimumPreload,
			1,
			64);
		profile.smartMinimumRequests = ReadInt(
			object,
			"smartMinimumRequests",
			defaults.smartMinimumRequests,
			1,
			32);
		profile.smartMaximumPreload = ReadInt(
			object,
			"smartMaximumPreload",
			defaults.smartMaximumPreload,
			1,
			64);
		profile.smartInitialRequestLimit = ReadInt(
			object,
			"smartInitialRequestLimit",
			defaults.smartInitialRequestLimit,
			1,
			32);
		profile.smartMinimumRequestLimit = ReadInt(
			object,
			"smartMinimumRequestLimit",
			defaults.smartMinimumRequestLimit,
			1,
			32);
		profile.smartMaximumRequestLimit = ReadInt(
			object,
			"smartMaximumRequestLimit",
			defaults.smartMaximumRequestLimit,
			1,
			32);
		profile.smartCapacityMinimumRequestLimit = ReadInt(
			object,
			"smartCapacityMinimumRequestLimit",
			defaults.smartCapacityMinimumRequestLimit,
			1,
			32);
		profile.maxWaitedParts = std::max(profile.maxWaitedParts, profile.startWaitedParts);
		profile.maxSessions = std::max(profile.maxSessions, profile.startSessions);
		profile.mpvCacheBackMb = std::min(
			profile.mpvCacheBackMb,
			profile.mpvCacheMaxMb);
		profile.smartMaximumPreload = std::max(
			profile.smartMaximumPreload,
			profile.smartMinimumPreload);
		profile.smartMaximumRequestLimit = std::max(
			profile.smartMaximumRequestLimit,
			profile.smartMinimumRequestLimit);
		profile.smartInitialRequestLimit = std::clamp(
			profile.smartInitialRequestLimit,
			profile.smartMinimumRequestLimit,
			profile.smartMaximumRequestLimit);
		profile.smartCapacityMinimumRequestLimit = std::clamp(
			profile.smartCapacityMinimumRequestLimit,
			profile.smartMinimumRequestLimit,
			profile.smartMaximumRequestLimit);
	}
	return result;
}

QString SerializeBoostProfiles(const BoostProfiles &profiles) {
	auto array = QJsonArray();
	for (const auto &profile : profiles) {
		auto object = QJsonObject();
		WriteProfile(object, profile);
		array.append(object);
	}
	return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

const BoostProfile &BoostProfileFor(int level) {
	static const auto profiles = LoadBoostProfiles();
	return profiles[std::clamp(level, 0, int(profiles.size() - 1))];
}

int AveragePlaybackBytesPerSecond(int64 size, int64 duration) {
	if (size <= 0 || duration <= 1) {
		return 0;
	}
	return int(std::clamp(
		double(size) * 1000. / double(duration),
		0.,
		double(kPlaybackRateMaximum)));
}

bool IsHighBitratePlaybackRate(int bytesPerSecond) {
	return bytesPerSecond >= kHighBitrateBytesPerSecond;
}

bool IsHighBitrateVideo(int64 size, int64 duration) {
	return IsHighBitratePlaybackRate(
		AveragePlaybackBytesPerSecond(size, duration));
}

} // namespace Media::Streaming
