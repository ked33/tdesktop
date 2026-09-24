/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/view/media_view_d3d11_texture.h"

#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)

#include <d3d11_1.h>
#include <wrl/client.h>

#include <rhi/qrhi.h>

#endif // Qt >= 6.7

namespace Media::View {

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)

using Microsoft::WRL::ComPtr;

struct D3D11SharedTexture::State {
	void *handle = nullptr;
	void *owner = nullptr;
	ComPtr<ID3D11Texture2D> opened;
	QRhiTexture *texture = nullptr;
	QSize size;

	void reset() {
		delete texture;
		texture = nullptr;
		opened.Reset();
		handle = nullptr;
		owner = nullptr;
		size = QSize();
	}
};

D3D11SharedTexture::D3D11SharedTexture()
: _state(std::make_unique<State>()) {
}

D3D11SharedTexture::~D3D11SharedTexture() {
	reset();
}

void D3D11SharedTexture::reset() {
	if (_state) {
		_state->reset();
	}
}

QRhiTexture *D3D11SharedTexture::import(
		QRhi *rhi,
		void *sharedHandle,
		void *owner,
		QSize size) {
	if (!rhi
		|| !sharedHandle
		|| !owner
		|| size.isEmpty()
		|| rhi->backend() != QRhi::D3D11) {
		return nullptr;
	}
	if (_state->texture
		&& _state->owner == owner
		&& _state->size == size) {
		return _state->texture;
	}
	_state->reset();
	const auto *handles = static_cast<const QRhiD3D11NativeHandles *>(
		rhi->nativeHandles());
	if (!handles || !handles->dev) {
		return nullptr;
	}
	auto device = ComPtr<ID3D11Device1>();
	if (FAILED(static_cast<ID3D11Device *>(handles->dev)->QueryInterface(
			IID_PPV_ARGS(&device)))) {
		return nullptr;
	}
	auto opened = ComPtr<ID3D11Texture2D>();
	if (FAILED(device->OpenSharedResource1(
			sharedHandle,
			IID_PPV_ARGS(&opened)))
		|| !opened) {
		return nullptr;
	}
	auto *texture = rhi->newTexture(QRhiTexture::BGRA8, size);
	auto native = QRhiTexture::NativeTexture{};
	native.object = quint64(quintptr(opened.Get()));
	native.layout = 0;
	if (!texture->createFrom(native)) {
		delete texture;
		return nullptr;
	}
	_state->handle = sharedHandle;
	_state->owner = owner;
	_state->opened = std::move(opened);
	_state->texture = texture;
	_state->size = size;
	return texture;
}

#else // Qt >= 6.7

struct D3D11SharedTexture::State {
};

D3D11SharedTexture::D3D11SharedTexture()
: _state(std::make_unique<State>()) {
}

D3D11SharedTexture::~D3D11SharedTexture() = default;

void D3D11SharedTexture::reset() {
}

QRhiTexture *D3D11SharedTexture::import(QRhi *, void *, void *, QSize) {
	return nullptr;
}

#endif // Qt >= 6.7

} // namespace Media::View
