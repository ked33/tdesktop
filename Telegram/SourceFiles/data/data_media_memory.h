/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QSet>
#include <QtCore/QString>

class ImageLocation;

namespace Data {

class MediaMemoryCompactor final {
public:
	MediaMemoryCompactor();

	void compact(QString &value);
	void compact(QByteArray &value);
	void compact(ImageLocation &location);

	[[nodiscard]] int sharedStrings() const;
	[[nodiscard]] int sharedByteArrays() const;
	[[nodiscard]] bool limitReached() const;

private:
	QSet<QString> _strings;
	QSet<QByteArray> _bytes;
	int _sharedStrings = 0;
	int _sharedByteArrays = 0;
	bool _limitReached = false;

};

} // namespace Data
