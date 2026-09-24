/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <memory>

class QImage;

struct AVFrame;

namespace Media::Streaming {

struct NativeFrame;

// Copy a D3D11 decoder texture to a shared BGRA texture on the GPU.
// `retained` is reused when the frame size has not changed.
[[nodiscard]] bool RetainD3D11Frame(
	AVFrame *decoded,
	std::shared_ptr<void> retained,
	NativeFrame *out);

[[nodiscard]] QImage ReadD3D11Frame(const NativeFrame &frame);

} // namespace Media::Streaming
