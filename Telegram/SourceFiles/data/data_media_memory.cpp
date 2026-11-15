/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_media_memory.h"

#include "base/assertion.h"
#include "ui/image/image_location.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

namespace Data {
namespace {

constexpr auto kMaxUniqueValues = 512 * 1024;

template <typename Value>
bool CompactValue(
		QSet<Value> &values,
		Value &value,
		qsizetype total,
		bool &limitReached) {
	if (value.isEmpty()) {
		return false;
	}
	const auto i = values.constFind(value);
	if (i != values.cend()) {
		if (value.constData() == i->constData()) {
			return false;
		}
		value = *i;
		return true;
	}
	if (total >= kMaxUniqueValues) {
		limitReached = true;
		return false;
	}
	values.insert(value);
	return false;
}

} // namespace

MediaMemoryCompactor::MediaMemoryCompactor() {
	Expects(QCoreApplication::instance() != nullptr);
	Expects(QThread::currentThread()
		== QCoreApplication::instance()->thread());
}

void MediaMemoryCompactor::compact(QString &value) {
	if (CompactValue(
			_strings,
			value,
			_strings.size() + _bytes.size(),
			_limitReached)) {
		++_sharedStrings;
	}
}

void MediaMemoryCompactor::compact(QByteArray &value) {
	if (CompactValue(
			_bytes,
			value,
			_strings.size() + _bytes.size(),
			_limitReached)) {
		++_sharedByteArrays;
	}
}

void MediaMemoryCompactor::compact(ImageLocation &location) {
	auto reference = location.fileReference();
	compact(reference);
	location.shareFileReference(reference);
}

int MediaMemoryCompactor::sharedStrings() const {
	return _sharedStrings;
}

int MediaMemoryCompactor::sharedByteArrays() const {
	return _sharedByteArrays;
}

bool MediaMemoryCompactor::limitReached() const {
	return _limitReached;
}

} // namespace Data
