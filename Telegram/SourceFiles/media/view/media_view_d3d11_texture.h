/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QSize>

#include <memory>

class QRhi;
class QRhiTexture;

namespace Media::View {

class D3D11SharedTexture final {
public:
	D3D11SharedTexture();
	~D3D11SharedTexture();

	[[nodiscard]] QRhiTexture *import(
		QRhi *rhi,
		void *sharedHandle,
		void *owner,
		QSize size);
	void reset();

private:
	struct State;
	std::unique_ptr<State> _state;

};

} // namespace Media::View
