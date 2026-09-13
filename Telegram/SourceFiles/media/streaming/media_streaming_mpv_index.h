/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "media/streaming/media_streaming_mp4_header.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

class QProcess;
class QString;

namespace Media::Streaming {

class Reader;

} // namespace Media::Streaming

namespace Media::Streaming::Mpv {

enum class IndexState {
	Unknown,
	Preparing,
	Ready,
	OnDemand,
	Unavailable,
};

struct IndexControl {
	std::function<IndexState()> state;
	std::function<std::uint64_t(std::int64_t)> request;
	std::function<bool(std::uint64_t)> ready;
	std::function<void()> settled;
	std::function<void()> abandoned;
};

class PreparedIndex final {
public:
	PreparedIndex();
	~PreparedIndex();

	[[nodiscard]] IndexState state() const;
	[[nodiscard]] bool prepare();
	void start(std::shared_ptr<Reader> reader, std::int64_t durationMs);
	void cancel();
	[[nodiscard]] std::uint64_t request(std::int64_t positionMs);
	void settle();
	[[nodiscard]] bool ready(std::uint64_t revision) const;
	[[nodiscard]] std::shared_ptr<const std::vector<Mp4::HeaderPatch>> patches(
		std::uint64_t revision) const;
	[[nodiscard]] std::size_t copy(
		std::int64_t offset,
		std::span<char> buffer) const;

private:
	struct State;
	static void Prepare(
		std::shared_ptr<State> state,
		std::shared_ptr<Reader> reader,
		std::int64_t durationMs);

	const std::shared_ptr<State> _state;

};

void ManageIndexReload(
	QProcess *process,
	IndexControl control,
	std::int64_t durationMs);

[[nodiscard]] IndexControl ControlIndex(std::weak_ptr<PreparedIndex> index);

[[nodiscard]] QString PlaybackDemuxerOptions(bool fastOpen);

} // namespace Media::Streaming::Mpv
