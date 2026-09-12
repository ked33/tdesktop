/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

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
	Unavailable,
};

class PreparedIndex final {
public:
	PreparedIndex();
	~PreparedIndex();

	[[nodiscard]] IndexState state() const;
	[[nodiscard]] bool prepare();
	void start(std::shared_ptr<Reader> reader);
	void cancel();
	[[nodiscard]] std::size_t copy(
		std::int64_t offset,
		std::span<char> buffer) const;

private:
	struct State;
	static void Prepare(
		std::shared_ptr<State> state,
		std::shared_ptr<Reader> reader);

	const std::shared_ptr<State> _state;

};

void ManageIndexReload(
	QProcess *process,
	std::function<IndexState()> state,
	std::int64_t durationMs,
	std::function<void()> abandoned);

[[nodiscard]] QString PlaybackDemuxerOptions(bool fastOpen);

} // namespace Media::Streaming::Mpv
