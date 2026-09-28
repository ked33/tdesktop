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

#include <vector>

#endif // Qt >= 6.7

namespace Media::View {

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)

using Microsoft::WRL::ComPtr;

namespace {

// Decoded frames rotate through a few GPU textures, keep all of them open.
constexpr auto kMaxOpenedFrames = 8;

struct OpenedFrame {
	std::weak_ptr<void> owner;
	void *handle = nullptr;
	ComPtr<ID3D11Texture2D> opened;
	QRhiTexture *texture = nullptr;
	QSize size;
	quint64 used = 0;
};

void Destroy(OpenedFrame &frame) {
	delete base::take(frame.texture);
	frame.opened.Reset();
}

[[nodiscard]] bool SameOwner(
		const std::weak_ptr<void> &a,
		const std::shared_ptr<void> &b) {
	return !a.owner_before(b) && !b.owner_before(a);
}

} // namespace

struct D3D11SharedTexture::State {
	std::vector<OpenedFrame> frames;
	QRhi *rhi = nullptr;
	quint64 counter = 0;

	void reset() {
		for (auto &frame : frames) {
			Destroy(frame);
		}
		frames.clear();
		rhi = nullptr;
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
		const std::shared_ptr<void> &owner,
		QSize size) {
	if (!rhi
		|| !sharedHandle
		|| !owner
		|| size.isEmpty()
		|| rhi->backend() != QRhi::D3D11) {
		return nullptr;
	}
	if (_state->rhi != rhi) {
		_state->reset();
		_state->rhi = rhi;
	}
	auto &frames = _state->frames;
	for (auto i = frames.begin(); i != frames.end();) {
		if (i->owner.expired()) {
			Destroy(*i);
			i = frames.erase(i);
		} else {
			++i;
		}
	}
	const auto used = ++_state->counter;
	const auto i = ranges::find_if(frames, [&](const OpenedFrame &frame) {
		return SameOwner(frame.owner, owner);
	});
	if (i != frames.end()) {
		if (i->handle == sharedHandle && i->size == size) {
			i->used = used;
			return i->texture;
		}
		Destroy(*i);
		frames.erase(i);
	}
	if (int(frames.size()) >= kMaxOpenedFrames) {
		const auto oldest = ranges::min_element(
			frames,
			ranges::less(),
			&OpenedFrame::used);
		Destroy(*oldest);
		frames.erase(oldest);
	}
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
	frames.push_back({
		.owner = owner,
		.handle = sharedHandle,
		.opened = std::move(opened),
		.texture = texture,
		.size = size,
		.used = used,
	});
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

QRhiTexture *D3D11SharedTexture::import(
		QRhi *,
		void *,
		const std::shared_ptr<void> &,
		QSize) {
	return nullptr;
}

#endif // Qt >= 6.7

} // namespace Media::View
