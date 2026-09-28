/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_d3d11_frame.h"

#include "media/streaming/media_streaming_common.h"
#include "logs.h"

#include <cstring>
#include <mutex>

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <libavutil/hwcontext_d3d11va.h>

#include <QtGui/QImage>

namespace Media::Streaming {
namespace {

using Microsoft::WRL::ComPtr;

struct GpuFrame {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11Texture2D> texture;
	HANDLE handle = nullptr;
	int width = 0;
	int height = 0;

	~GpuFrame() {
		texture.Reset();
		device.Reset();
		if (handle) {
			CloseHandle(handle);
			handle = nullptr;
		}
	}
};

struct Processor {
	ID3D11Device *device = nullptr;
	ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
	ComPtr<ID3D11VideoProcessor> processor;
	int width = 0;
	int height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	bool failed = false;

	Processor() = default;
	Processor(const Processor &) = delete;
	Processor &operator=(const Processor &) = delete;

	~Processor() {
		reset();
	}

	void reset() {
		enumerator.Reset();
		processor.Reset();
		if (device) {
			device->Release();
			device = nullptr;
		}
		width = 0;
		height = 0;
		format = DXGI_FORMAT_UNKNOWN;
		failed = false;
	}
};

Processor &SharedProcessor() {
	static Processor processor;
	return processor;
}

std::mutex &ProcessorMutex() {
	static auto mutex = std::mutex();
	return mutex;
}

struct FfmpegD3DLock {
	explicit FfmpegD3DLock(AVD3D11VADeviceContext *context)
	: _context(context) {
		if (_context && _context->lock) {
			_context->lock(_context->lock_ctx);
		}
	}
	FfmpegD3DLock(const FfmpegD3DLock &) = delete;
	FfmpegD3DLock &operator=(const FfmpegD3DLock &) = delete;
	~FfmpegD3DLock() {
		if (_context && _context->unlock) {
			_context->unlock(_context->lock_ctx);
		}
	}

	AVD3D11VADeviceContext *_context = nullptr;
};

[[nodiscard]] bool SupportedInputFormat(DXGI_FORMAT format) {
	return (format == DXGI_FORMAT_NV12)
		|| (format == DXGI_FORMAT_P010)
		|| (format == DXGI_FORMAT_P016);
}

[[nodiscard]] bool EnsureProcessor(
		AVD3D11VADeviceContext *d3d,
		int width,
		int height,
		DXGI_FORMAT format) {
	auto &processor = SharedProcessor();
	if (processor.device == d3d->device
		&& processor.width == width
		&& processor.height == height
		&& processor.format == format) {
		return processor.processor && !processor.failed;
	}
	processor.enumerator.Reset();
	processor.processor.Reset();
	processor.failed = false;
	if (processor.device && processor.device != d3d->device) {
		processor.device->Release();
		processor.device = nullptr;
	}
	if (!processor.device) {
		processor.device = d3d->device;
		processor.device->AddRef();
	}

	auto content = D3D11_VIDEO_PROCESSOR_CONTENT_DESC{};
	content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	content.InputWidth = UINT(width);
	content.InputHeight = UINT(height);
	content.OutputWidth = UINT(width);
	content.OutputHeight = UINT(height);
	content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
	if (FAILED(d3d->video_device->CreateVideoProcessorEnumerator(
			&content,
			processor.enumerator.GetAddressOf()))) {
		processor.failed = true;
		processor.width = width;
		processor.height = height;
		processor.format = format;
		return false;
	}
	auto inputSupport = UINT(0);
	auto outputSupport = UINT(0);
	if (FAILED(processor.enumerator->CheckVideoProcessorFormat(
			format,
			&inputSupport))
		|| !(inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)
		|| FAILED(processor.enumerator->CheckVideoProcessorFormat(
			DXGI_FORMAT_B8G8R8A8_UNORM,
			&outputSupport))
		|| !(outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)
		|| FAILED(d3d->video_device->CreateVideoProcessor(
			processor.enumerator.Get(),
			0,
			processor.processor.GetAddressOf()))) {
		processor.enumerator.Reset();
		processor.processor.Reset();
		processor.failed = true;
		processor.width = width;
		processor.height = height;
		processor.format = format;
		return false;
	}
	processor.width = width;
	processor.height = height;
	processor.format = format;
	return true;
}

[[nodiscard]] bool CreateSharedTexture(
		ID3D11Device *device,
		int width,
		int height,
		GpuFrame *frame) {
	auto desc = D3D11_TEXTURE2D_DESC{};
	desc.Width = UINT(width);
	desc.Height = UINT(height);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED
		| D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	auto texture = ComPtr<ID3D11Texture2D>();
	if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf()))) {
		return false;
	}
	auto resource = ComPtr<IDXGIResource1>();
	if (FAILED(texture.As(&resource))) {
		return false;
	}
	auto handle = HANDLE();
	if (FAILED(resource->CreateSharedHandle(
			nullptr,
			DXGI_SHARED_RESOURCE_READ,
			nullptr,
			&handle))
		|| !handle) {
		return false;
	}
	frame->texture = std::move(texture);
	frame->handle = handle;
	frame->device = device;
	frame->width = width;
	frame->height = height;
	return true;
}

[[nodiscard]] std::shared_ptr<GpuFrame> PrepareOutput(
		ID3D11Device *device,
		int width,
		int height,
		const std::shared_ptr<void> &retained) {
	auto existing = std::static_pointer_cast<GpuFrame>(retained);
	if (existing
		&& existing->texture
		&& existing->handle
		&& existing->width == width
		&& existing->height == height
		&& existing->device.Get() == device) {
		return existing;
	}
	auto created = std::make_shared<GpuFrame>();
	if (!CreateSharedTexture(device, width, height, created.get())) {
		return nullptr;
	}
	return created;
}

void ApplyColorSpace(
		AVD3D11VADeviceContext *d3d,
		AVFrame *decoded,
		int height) {
	const auto matrix709 = (decoded->colorspace != AVCOL_SPC_BT470BG)
		&& (decoded->colorspace != AVCOL_SPC_SMPTE170M);
	const auto fullRange = (decoded->color_range == AVCOL_RANGE_JPEG);
	auto input = D3D11_VIDEO_PROCESSOR_COLOR_SPACE{};
	input.Usage = 0;
	input.YCbCr_Matrix = matrix709 ? 1 : 0;
	input.Nominal_Range = fullRange ? 2 : 1;
	auto output = D3D11_VIDEO_PROCESSOR_COLOR_SPACE{};
	output.Usage = 0;
	output.RGB_Range = 0;
	output.Nominal_Range = 2;
	const auto processor = SharedProcessor().processor.Get();
	d3d->video_context->VideoProcessorSetStreamColorSpace(
		processor,
		0,
		&input);
	d3d->video_context->VideoProcessorSetOutputColorSpace(processor, &output);
	auto rect = RECT{ 0, 0, decoded->width, height };
	d3d->video_context->VideoProcessorSetStreamSourceRect(
		processor,
		0,
		TRUE,
		&rect);
	d3d->video_context->VideoProcessorSetStreamDestRect(
		processor,
		0,
		TRUE,
		&rect);
	d3d->video_context->VideoProcessorSetOutputTargetRect(
		processor,
		TRUE,
		&rect);
	d3d->video_context->VideoProcessorSetStreamAutoProcessingMode(
		processor,
		0,
		FALSE);
}

[[nodiscard]] ID3D11Texture2D *SourceTexture(
		AVFrame *decoded,
		AVD3D11VAFramesContext *frames,
		UINT *slice) {
	auto *texture = frames ? frames->texture : nullptr;
	auto index = static_cast<UINT>(
		reinterpret_cast<uintptr_t>(decoded->data[1]));
	if (!texture) {
		texture = reinterpret_cast<ID3D11Texture2D *>(decoded->data[0]);
	}
	if (!texture) {
		return nullptr;
	}
	auto desc = D3D11_TEXTURE2D_DESC{};
	texture->GetDesc(&desc);
	if (index >= desc.ArraySize
		&& decoded->data[0]
		&& decoded->data[0] != reinterpret_cast<uint8_t *>(texture)) {
		const auto descriptor = reinterpret_cast<AVD3D11FrameDescriptor *>(
			decoded->data[0]);
		if (descriptor->texture) {
			texture = descriptor->texture;
			index = static_cast<UINT>(descriptor->index);
			texture->GetDesc(&desc);
		}
	}
	if (index >= desc.ArraySize) {
		index = 0;
	}
	*slice = index;
	return texture;
}

} // namespace

bool RetainD3D11Frame(
		AVFrame *decoded,
		std::shared_ptr<void> retained,
		NativeFrame *out) {
	if (!decoded
		|| !out
		|| !decoded->hw_frames_ctx
		|| decoded->format != AV_PIX_FMT_D3D11
		|| decoded->width < 2
		|| decoded->height < 2
		|| (decoded->width % 2)
		|| (decoded->height % 2)) {
		return false;
	}
	const auto frames = reinterpret_cast<AVHWFramesContext *>(
		decoded->hw_frames_ctx->data);
	if (!frames || !frames->device_ctx || !frames->hwctx) {
		return false;
	}
	const auto d3d = reinterpret_cast<AVD3D11VADeviceContext *>(
		frames->device_ctx->hwctx);
	const auto frameContext = reinterpret_cast<AVD3D11VAFramesContext *>(
		frames->hwctx);
	if (!d3d
		|| !d3d->device
		|| !d3d->video_device
		|| !d3d->video_context) {
		return false;
	}

	const std::lock_guard<std::mutex> lock(ProcessorMutex());
	const FfmpegD3DLock ffmpegLock(d3d);
	auto slice = UINT(0);
	auto *source = SourceTexture(decoded, frameContext, &slice);
	if (!source) {
		return false;
	}
	auto sourceDesc = D3D11_TEXTURE2D_DESC{};
	source->GetDesc(&sourceDesc);
	if (!SupportedInputFormat(sourceDesc.Format)) {
		return false;
	}
	if (!EnsureProcessor(
			d3d,
			decoded->width,
			decoded->height,
			sourceDesc.Format)) {
		static auto logged = false;
		if (!logged) {
			logged = true;
			LOG(("Video Error: D3D11 video processor is unavailable."));
		}
		return false;
	}
	auto gpu = PrepareOutput(
		d3d->device,
		decoded->width,
		decoded->height,
		retained);
	if (!gpu) {
		static auto logged = false;
		if (!logged) {
			logged = true;
			LOG(("Video Error: Could not create a shared D3D11 video texture."));
		}
		return false;
	}

	auto inputDesc = D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC{};
	inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	inputDesc.Texture2D.MipSlice = 0;
	inputDesc.Texture2D.ArraySlice = slice;
	auto inputView = ComPtr<ID3D11VideoProcessorInputView>();
	auto outputDesc = D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC{};
	outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	outputDesc.Texture2D.MipSlice = 0;
	auto outputView = ComPtr<ID3D11VideoProcessorOutputView>();
	const auto processor = SharedProcessor().processor.Get();
	const auto enumerator = SharedProcessor().enumerator.Get();
	if (FAILED(d3d->video_device->CreateVideoProcessorInputView(
			source,
			enumerator,
			&inputDesc,
			inputView.GetAddressOf()))
		|| FAILED(d3d->video_device->CreateVideoProcessorOutputView(
			gpu->texture.Get(),
			enumerator,
			&outputDesc,
			outputView.GetAddressOf()))) {
		return false;
	}
	ApplyColorSpace(d3d, decoded, decoded->height);
	auto stream = D3D11_VIDEO_PROCESSOR_STREAM{};
	stream.Enable = TRUE;
	stream.pInputSurface = inputView.Get();
	if (FAILED(d3d->video_context->VideoProcessorBlt(
			processor,
			outputView.Get(),
			0,
			1,
			&stream))) {
		static auto logged = false;
		if (!logged) {
			logged = true;
			LOG(("Video Error: D3D11 video processor blit failed."));
		}
		return false;
	}
	// The renderer reads the texture through another D3D11 device.
	if (d3d->device_context) {
		d3d->device_context->Flush();
	}

	out->pixelBuffer = nullptr;
	out->sharedHandle = gpu->handle;
	out->retained = gpu;
	out->size = QSize(decoded->width, decoded->height);
	out->chromaSize = QSize(
		(decoded->width + 1) / 2,
		(decoded->height + 1) / 2);
	static auto logged = false;
	if (!logged) {
		logged = true;
		LOG(("Video Info: D3D11 frames stay on the GPU (%1x%2)."
			).arg(decoded->width
			).arg(decoded->height));
	}
	return true;
}

QImage ReadD3D11Frame(const NativeFrame &frame) {
	const auto gpu = std::static_pointer_cast<GpuFrame>(frame.retained);
	if (!gpu || !gpu->texture || !gpu->device || gpu->width < 1 || gpu->height < 1) {
		return {};
	}
	auto context = ComPtr<ID3D11DeviceContext>();
	gpu->device->GetImmediateContext(context.GetAddressOf());
	if (!context) {
		return {};
	}
	auto desc = D3D11_TEXTURE2D_DESC{};
	desc.Width = UINT(gpu->width);
	desc.Height = UINT(gpu->height);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	auto staging = ComPtr<ID3D11Texture2D>();
	if (FAILED(gpu->device->CreateTexture2D(
			&desc,
			nullptr,
			staging.GetAddressOf()))) {
		return {};
	}
	context->CopyResource(staging.Get(), gpu->texture.Get());
	auto mapped = D3D11_MAPPED_SUBRESOURCE{};
	if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))
		|| !mapped.pData) {
		return {};
	}
	auto image = QImage(gpu->width, gpu->height, QImage::Format_ARGB32);
	const auto rowBytes = gpu->width * 4;
	for (auto y = 0; y != gpu->height; ++y) {
		memcpy(
			image.scanLine(y),
			static_cast<const char *>(mapped.pData) + y * mapped.RowPitch,
			rowBytes);
	}
	context->Unmap(staging.Get(), 0);
	return image;
}

} // namespace Media::Streaming
