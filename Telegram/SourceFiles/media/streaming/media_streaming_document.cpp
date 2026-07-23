/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_document.h"

#include "media/streaming/media_streaming_instance.h"
#include "media/streaming/media_streaming_debug.h"
#include "media/streaming/media_streaming_loader.h"
#include "media/streaming/media_streaming_reader.h"
#include "media/streaming/media_streaming_source.h"
#include "data/data_session.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "main/main_session.h"
#include "storage/file_download.h" // Storage::kMaxFileInMemory.
#include "styles/style_widgets.h"

#include <QtCore/QBuffer>

namespace Media {
namespace Streaming {
namespace {

constexpr auto kWaitingFastDuration = crl::time(200);
constexpr auto kWaitingShowDuration = crl::time(500);
constexpr auto kWaitingShowDelay = crl::time(500);
constexpr auto kGoodThumbQuality = 87;
constexpr auto kSwitchQualityUpPreloadedThreshold = 4 * crl::time(1000);
constexpr auto kSwitchQualityUpSpeedMultiplier = 1.2;
constexpr auto kSwitchQualityDownBufferThreshold = 12 * crl::time(1000);
constexpr auto kSwitchQualityDownRiskDuration = 3 * crl::time(1000);
constexpr auto kSwitchQualityDownRequestCooldown = 30 * crl::time(1000);
constexpr auto kSwitchQualityDownSpeedMultiplier = 1.15;

} // namespace

Document::Document(
	not_null<DocumentData*> document,
	std::shared_ptr<FileSource> source,
	std::vector<QualityDescriptor> otherQualities)
: Document(std::move(source), document, {}, std::move(otherQualities)) {
	_player.fullInCache(
	) | rpl::on_next([=](bool fullInCache) {
		_document->setLoadedInMediaCache(fullInCache);
	}, _player.lifetime());
}

Document::Document(
	not_null<DocumentData*> document,
	std::shared_ptr<Reader> reader,
	std::vector<QualityDescriptor> otherQualities)
: Document(
	document,
	MakeFileSource(std::move(reader)),
	std::move(otherQualities)) {
}

Document::Document(
	not_null<PhotoData*> photo,
	std::shared_ptr<FileSource> source,
	std::vector<QualityDescriptor> otherQualities)
: Document(std::move(source), {}, photo, std::move(otherQualities)) {
}

Document::Document(
	not_null<PhotoData*> photo,
	std::shared_ptr<Reader> reader,
	std::vector<QualityDescriptor> otherQualities)
: Document(photo, MakeFileSource(std::move(reader)), std::move(otherQualities)) {
}

Document::Document(std::unique_ptr<Loader> loader)
: Document(std::make_shared<Reader>(std::move(loader)), {}, {}, {}) {
}

Document::Document(
	std::shared_ptr<FileSource> source,
	DocumentData *document,
	PhotoData *photo,
	std::vector<QualityDescriptor> otherQualities)
: _document(document)
, _photo(photo)
, _player(std::move(source))
, _radial(
	[=] { waitingCallback(); },
	st::defaultInfiniteRadialAnimation)
, _otherQualities(std::move(otherQualities)) {
	resubscribe();
}

Document::Document(
	std::shared_ptr<Reader> reader,
	DocumentData *document,
	PhotoData *photo,
	std::vector<QualityDescriptor> otherQualities)
: Document(
	MakeFileSource(std::move(reader)),
	document,
	photo,
	std::move(otherQualities)) {
}

void Document::resubscribe() {
	_subscription = _player.updates(
	) | rpl::on_next_error([=](Update &&update) {
		handleUpdate(std::move(update));
	}, [=](Streaming::Error &&error) {
		handleError(std::move(error));
		resubscribe();
	});
}

Player &Document::player() {
	return _player;
}

const Player &Document::player() const {
	return _player;
}

const Information &Document::info() const {
	return _info;
}

void Document::play(const PlaybackOptions &options) {
	if (_document && (_document->isVideoFile() || _document->isVideoMessage())) {
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Start document stream doc=%1 size=%2 mime=%3 mode=%4 position=%5 durationOverride=%6 speed=%7.")
			.arg(qulonglong(_document->id))
			.arg(qlonglong(_document->size))
			.arg(_document->mimeString())
			.arg(int(options.mode))
			.arg(qlonglong(options.position))
			.arg(qlonglong(options.durationOverride))
			.arg(options.speed, 0, 'f', 2));
	}
	_player.play(options);
	_info.audio.state.position
		= _info.video.state.position
		= options.position;
	waitingChange(true);
}

void Document::saveFrameToCover() {
	_info.video.cover = _player.ready()
		? _player.currentFrameImage()
		: _info.video.cover;
}

void Document::registerInstance(not_null<Instance*> instance) {
	_instances.emplace(instance);
}

void Document::unregisterInstance(not_null<Instance*> instance) {
	_instances.remove(instance);
	_player.unregisterInstance(instance);
	refreshPlayerPriority();
}

void Document::refreshPlayerPriority() {
	if (_instances.empty()) {
		return;
	}
	const auto max = ranges::max_element(
		_instances,
		ranges::less(),
		&Instance::priority);
	_player.setLoaderPriority((*max)->priority());
}

bool Document::waitingShown() const {
	if (!_fading.animating() && !_waiting) {
		_radial.stop(anim::type::instant);
		return false;
	}
	return _radial.animating();
}

float64 Document::waitingOpacity() const {
	return _fading.value(_waiting ? 1. : 0.);
}

Ui::RadialState Document::waitingState() const {
	return _radial.computeState();
}

rpl::producer<int> Document::switchQualityRequests() const {
	return _switchQualityRequests.events();
}

void Document::handleUpdate(Update &&update) {
	v::match(update.data, [&](Information &update) {
		ready(std::move(update));
	}, [&](PreloadedVideo update) {
		_info.video.state.receivedTill = update.till;
		checkSwitchToHigherQuality();
	}, [&](UpdateVideo update) {
		_info.video.state.position = update.position;
	}, [&](PreloadedAudio update) {
		_info.audio.state.receivedTill = update.till;
	}, [&](UpdateAudio update) {
		_info.audio.state.position = update.position;
	}, [&](WaitingForData update) {
		waitingChange(update.waiting);
	}, [&](SpeedEstimate update) {
		checkForQualitySwitch(update);
	}, [](MutedByOther) {
	}, [&](Finished) {
		const auto finishTrack = [](TrackState &state) {
			state.position = state.receivedTill = state.duration;
		};
		finishTrack(_info.audio.state);
		finishTrack(_info.video.state);
	});
}

void Document::setOtherQualities(std::vector<QualityDescriptor> value) {
	_otherQualities = std::move(value);
	checkForQualitySwitch(_lastSpeedEstimate);
}

void Document::checkForQualitySwitch(SpeedEstimate estimate) {
	_lastSpeedEstimate = estimate;
	if (!estimate.unreliable && estimate.bytesPerSecond > 0) {
		constexpr auto kAlpha = 0.35;
		_qualityThroughputEma = _qualityThroughputEma
			? (_qualityThroughputEma * (1. - kAlpha))
				+ (double(estimate.bytesPerSecond) * kAlpha)
			: double(estimate.bytesPerSecond);
	}
	if (!checkSwitchToLowerQuality()) {
		checkSwitchToHigherQuality();
	}
}

bool Document::checkSwitchToHigherQuality() {
	const auto now = crl::now();
	if (_otherQualities.empty()
		|| (_info.video.state.duration == kTimeUnknown)
		|| (_info.video.state.duration == kDurationUnavailable)
		|| (_info.video.state.position == kTimeUnknown)
		|| (_info.video.state.receivedTill == kTimeUnknown)
		|| !_lastSpeedEstimate.bytesPerSecond
		|| _lastSpeedEstimate.unreliable
		|| _qualityRiskSince
		|| (_lastQualitySwitchRequest
			&& now < _lastQualitySwitchRequest
				+ kSwitchQualityDownRequestCooldown)
		|| (_info.video.state.receivedTill
			< std::min(
				_info.video.state.duration,
				(_info.video.state.position
					+ kSwitchQualityUpPreloadedThreshold)))) {
		return false;
	}
	const auto size = _player.fileSize();
	Assert(size >= 0 && size <= std::numeric_limits<uint32>::max());
	auto to = QualityDescriptor{ .sizeInBytes = uint32(size) };
	const auto duration = _info.video.state.duration / 1000.;
	const auto speed = _player.speed();
	const auto multiplier = speed * kSwitchQualityUpSpeedMultiplier;
	const auto latency = std::max(_lastSpeedEstimate.latencyMs, 1);
	const auto jitterRatio = std::clamp(
		double(_lastSpeedEstimate.jitterMs) / latency,
		0.,
		1.);
	const auto available = _qualityThroughputEma
		? (_qualityThroughputEma * (0.85 - 0.2 * jitterRatio))
		: double(_lastSpeedEstimate.bytesPerSecond);
	for (const auto &descriptor : _otherQualities) {
		const auto perSecond = descriptor.sizeInBytes / duration;
		if (descriptor.sizeInBytes > to.sizeInBytes
			&& available >= perSecond * multiplier) {
			to = descriptor;
		}
	}
	if (!to.height) {
		return false;
	}
	_lastQualitySwitchRequest = now;
	_switchQualityRequests.fire_copy(to.height);
	return true;
}

bool Document::checkSwitchToLowerQuality() {
	if (_otherQualities.empty()
		|| !_lastSpeedEstimate.bytesPerSecond) {
		return false;
	}
	const auto size = _player.fileSize();
	Assert(size >= 0 && size <= std::numeric_limits<uint32>::max());
	const auto waiting = _waiting && _radial.animating();
	const auto &state = _info.video.state;
	const auto predictive = [&] {
		if (waiting
			|| !_player.smartStreamingEnabled()
			|| !_document
			|| _document->mimeString() != u"video/mp4"_q
			|| _lastSpeedEstimate.unreliable
			|| !_qualityThroughputEma
			|| state.duration <= 1
			|| state.duration == kDurationUnavailable
			|| state.position == kTimeUnknown
			|| state.receivedTill == kTimeUnknown) {
			return false;
		}
		const auto duration = state.duration / 1000.;
		const auto currentRate = double(size) / duration * _player.speed();
		const auto latency = std::max(_lastSpeedEstimate.latencyMs, 1);
		const auto jitterRatio = std::clamp(
			double(_lastSpeedEstimate.jitterMs) / latency,
			0.,
			1.);
		const auto safety = 0.85 - 0.2 * jitterRatio;
		const auto safeThroughput = _qualityThroughputEma * safety;
		const auto buffered = std::max(
			state.receivedTill - state.position,
			crl::time(0));
		return safeThroughput
			< currentRate * kSwitchQualityDownSpeedMultiplier
			&& buffered <= kSwitchQualityDownBufferThreshold;
	}();
	const auto now = crl::now();
	if (!waiting && !predictive) {
		_qualityRiskSince = 0;
		return false;
	} else if (predictive && !_qualityRiskSince) {
		_qualityRiskSince = now;
		return false;
	} else if (predictive
		&& now < _qualityRiskSince + kSwitchQualityDownRiskDuration) {
		return false;
	} else if (predictive
		&& _lastQualitySwitchRequest
		&& now < _lastQualitySwitchRequest
			+ kSwitchQualityDownRequestCooldown) {
		return false;
	}
	const auto duration = (state.duration > 1
		&& state.duration != kDurationUnavailable)
		? (state.duration / 1000.)
		: 0.;
	const auto latency = std::max(_lastSpeedEstimate.latencyMs, 1);
	const auto jitterRatio = std::clamp(
		double(_lastSpeedEstimate.jitterMs) / latency,
		0.,
		1.);
	const auto safeThroughput = _qualityThroughputEma
		* (0.85 - 0.2 * jitterRatio);
	auto to = QualityDescriptor();
	auto lowest = QualityDescriptor();
	for (const auto &descriptor : _otherQualities) {
		if (!descriptor.mp4 || descriptor.sizeInBytes >= size) {
			continue;
		}
		if (!lowest.height
			|| descriptor.sizeInBytes < lowest.sizeInBytes) {
			lowest = descriptor;
		}
		const auto sustainable = waiting
			|| !duration
			|| (double(descriptor.sizeInBytes) / duration
				* _player.speed() * kSwitchQualityDownSpeedMultiplier
				<= safeThroughput);
		if (sustainable && descriptor.sizeInBytes > to.sizeInBytes) {
			to = descriptor;
		}
	}
	if (!to.height) {
		to = lowest;
	}
	if (!to.height) {
		return false;
	}
	_lastQualitySwitchRequest = now;
	_qualityRiskSince = 0;
	const auto buffered = (state.position != kTimeUnknown
		&& state.receivedTill != kTimeUnknown)
		? std::max(state.receivedTill - state.position, crl::time(0))
		: crl::time(0);
	VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: quality downgrade "
		"reason=%1 currentSize=%2 targetSize=%3 targetHeight=%4 "
		"throughput=%5 safe=%6 latency=%7 jitter=%8 bufferMs=%9.")
		.arg(waiting ? u"waiting"_q : u"predicted"_q)
		.arg(qlonglong(size))
		.arg(to.sizeInBytes)
		.arg(to.height)
		.arg(_lastSpeedEstimate.bytesPerSecond)
		.arg(int(safeThroughput))
		.arg(_lastSpeedEstimate.latencyMs)
		.arg(_lastSpeedEstimate.jitterMs)
		.arg(qlonglong(buffered)));
	_switchQualityRequests.fire_copy(to.height);
	return true;
}

void Document::handleError(Error &&error) {
	if (_document && (_document->isVideoFile() || _document->isVideoMessage())) {
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Document stream error doc=%1 size=%2 mime=%3 error=%4 supports=%5 useLoader=%6 remote=%7 inappFailed=%8.")
			.arg(qulonglong(_document->id))
			.arg(qlonglong(_document->size))
			.arg(_document->mimeString())
			.arg(PlaybackErrorDebugString(error))
			.arg(_document->supportsStreaming())
			.arg(_document->useStreamingLoader())
			.arg(_document->hasRemoteLocation())
			.arg(_document->inappPlaybackFailed()));
	}
	if (_document) {
		if (error == Error::OpenFailed) {
			_document->setInappPlaybackFailed();
		}
	} else if (_photo) {
		if (error == Error::NotStreamable || error == Error::OpenFailed) {
			_photo->setVideoPlaybackFailed();
		}
	}
	waitingChange(false);
}

void Document::ready(Information &&info) {
	_info = std::move(info);
	if (_document && (_document->isVideoFile() || _document->isVideoMessage())) {
		VIDEO_PLAYBACK_DEBUG_LOG(("Video Playback: Document stream ready doc=%1 size=%2 mime=%3 video=%4x%5 videoDuration=%6 audioDuration=%7 cover=%8 alpha=%9.")
			.arg(qulonglong(_document->id))
			.arg(qlonglong(_document->size))
			.arg(_document->mimeString())
			.arg(_info.video.size.width())
			.arg(_info.video.size.height())
			.arg(qlonglong(_info.video.state.duration))
			.arg(qlonglong(_info.audio.state.duration))
			.arg(!_info.video.cover.isNull())
			.arg(_info.video.alpha));
	}
	validateGoodThumbnail();
	waitingChange(false);
}

void Document::waitingChange(bool waiting) {
	if (_waiting == waiting) {
		return;
	}
	_waiting = waiting;
	const auto fade = [=](crl::time duration) {
		if (!_radial.animating()) {
			_radial.start(
				st::defaultInfiniteRadialAnimation.sineDuration);
		}
		_fading.start([=] {
			waitingCallback();
		}, _waiting ? 0. : 1., _waiting ? 1. : 0., duration);

		checkSwitchToLowerQuality();
	};
	if (waiting) {
		if (_radial.animating()) {
			_timer.cancel();
			fade(kWaitingFastDuration);
		} else {
			_timer.callOnce(kWaitingShowDelay);
			_timer.setCallback([=] {
				fade(kWaitingShowDuration);
			});
		}
	} else {
		_timer.cancel();
		if (_radial.animating()) {
			fade(kWaitingFastDuration);
		}
	}
}

void Document::validateGoodThumbnail() {
	if (_info.video.cover.isNull()
		|| !_document
		|| _document->goodThumbnailChecked()) {
		return;
	}
	const auto sticker = (_document->sticker() != nullptr);
	const auto document = _document;
	const auto information = _info.video;
	const auto key = document->goodThumbnailCacheKey();
	const auto guard = base::make_weak(&document->session());
	document->owner().cache().get(key, [=](QByteArray value) {
		if (!value.isEmpty()) {
			return;
		}
		const auto image = [&] {
			auto result = information.cover;
			if (information.rotation != 0) {
				auto transform = QTransform();
				transform.rotate(information.rotation);
				result = result.transformed(transform);
			}
			if (result.size() != information.size) {
				result = result.scaled(
					information.size,
					Qt::IgnoreAspectRatio,
					Qt::SmoothTransformation);
			}
			if (!sticker && information.alpha) {
				result = Images::Opaque(std::move(result));
			}
			return result;
		}();
		auto bytes = QByteArray();
		{
			auto buffer = QBuffer(&bytes);
			image.save(&buffer, sticker ? "WEBP" : "JPG", kGoodThumbQuality);
		}
		const auto length = bytes.size();
		if (!length || length > Storage::kMaxFileInMemory) {
			LOG(("App Error: Bad thumbnail data for saving to cache."));
			bytes = "(failed)"_q;
		}
		crl::on_main(guard, [=] {
			if (const auto active = document->activeMediaView()) {
				active->setGoodThumbnail(image);
			}
			if (bytes != "(failed)"_q) {
				document->setGoodThumbnailChecked(true);
			}
			document->owner().cache().putIfEmpty(
				document->goodThumbnailCacheKey(),
				Storage::Cache::Database::TaggedValue(
					base::duplicate(bytes),
					Data::kImageCacheTag));
		});
	});
}

void Document::waitingCallback() {
	for (const auto &instance : _instances) {
		instance->callWaitingCallback();
	}
}

} // namespace Streaming
} // namespace Media
