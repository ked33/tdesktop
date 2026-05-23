/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#pragma once

class QImage;
class QColor;

namespace HistoryView {

[[nodiscard]] bool PreviewBrightnessEnabled();
[[nodiscard]] int PreviewBrightnessPercent();

[[nodiscard]] int PreviewBrightnessKey();

void ApplyPreviewBrightness(QImage &image);
[[nodiscard]] QColor PreviewBrightnessColor(QColor color);

} // namespace HistoryView
