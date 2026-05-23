/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#include "history/view/media/history_view_preview_brightness.h"

#include "settings.h"

#include <QtGui/QColor>
#include <QtGui/QImage>

#include <algorithm>

namespace HistoryView {
namespace {

constexpr auto kDisabledKey = -1;
constexpr auto kDefaultPercent = 70;
constexpr auto kMinPercent = 10;
constexpr auto kMaxPercent = 100;

[[nodiscard]] int ClampPercent(int value) {
	return std::clamp(value, kMinPercent, kMaxPercent);
}

} // namespace

bool PreviewBrightnessEnabled() {
	return GetEnhancedBool("preview_brightness_enabled");
}

int PreviewBrightnessPercent() {
	const auto raw = GetEnhancedInt("preview_brightness");
	return raw > 0 ? ClampPercent(raw) : kDefaultPercent;
}

int PreviewBrightnessKey() {
	if (!PreviewBrightnessEnabled()) {
		return kDisabledKey;
	}
	const auto percent = PreviewBrightnessPercent();
	return (percent >= kMaxPercent) ? kDisabledKey : percent;
}

void ApplyPreviewBrightness(QImage &image) {
	const auto key = PreviewBrightnessKey();
	if (key == kDisabledKey || image.isNull()) {
		return;
	}
	if (image.format() != QImage::Format_ARGB32_Premultiplied
		&& image.format() != QImage::Format_RGB32) {
		image = image.convertToFormat(
			QImage::Format_ARGB32_Premultiplied);
	}

	uchar lut[256];
	for (auto i = 0; i != 256; ++i) {
		lut[i] = uchar(std::min(255, (i * key + 50) / 100));
	}

	const auto height = image.height();
	const auto width = image.width();
	const auto stride = image.bytesPerLine();
	auto bytes = image.bits();
	for (auto y = 0; y != height; ++y) {
		auto line = reinterpret_cast<QRgb*>(bytes + y * stride);
		for (auto x = 0; x != width; ++x) {
			const auto c = line[x];
			line[x] = qRgba(
				lut[qRed(c)],
				lut[qGreen(c)],
				lut[qBlue(c)],
				qAlpha(c));
		}
	}
}

QColor PreviewBrightnessColor(QColor color) {
	const auto key = PreviewBrightnessKey();
	if (key == kDisabledKey) {
		return color;
	}
	const auto brightnessAlpha = std::clamp(
		((kMaxPercent - key) * 256 + 50) / 100,
		0,
		255);
	if (!color.alpha()) {
		return QColor(0, 0, 0, brightnessAlpha);
	}
	const auto colorAlpha = color.alpha();
	const auto kept = ((256 - brightnessAlpha) * (256 - colorAlpha)
		+ 128) / 256;
	const auto combinedAlpha = std::clamp(256 - kept, 0, 255);
	if (!combinedAlpha) {
		return QColor(0, 0, 0, 0);
	}
	const auto scale = [&](int value) {
		return std::clamp(
			(value * key * colorAlpha + 50 * combinedAlpha)
				/ (kMaxPercent * combinedAlpha),
			0,
			255);
	};
	return QColor(
		scale(color.red()),
		scale(color.green()),
		scale(color.blue()),
		combinedAlpha);
}

} // namespace HistoryView
