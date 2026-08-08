/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lang/translate_cache.h"

#include <QtCore/QMutex>
#include <list>
#include <optional>
#include <unordered_map>

namespace Lang::TranslateCache {
namespace {

constexpr auto kMaxEntries = 256;

struct Entry {
	QString key;
	TextWithEntities value;
};

struct QStringHasher {
	size_t operator()(const QString &value) const noexcept {
		return size_t(qHash(value));
	}
};

class Store final {
public:
	[[nodiscard]] std::optional<TextWithEntities> get(const QString &key) {
		const auto lock = QMutexLocker(&_mutex);
		const auto i = _index.find(key);
		if (i == end(_index)) {
			return std::nullopt;
		}
		_list.splice(_list.begin(), _list, i->second);
		return i->second->value;
	}

	void put(const QString &key, TextWithEntities value) {
		const auto lock = QMutexLocker(&_mutex);
		if (const auto i = _index.find(key); i != end(_index)) {
			i->second->value = std::move(value);
			_list.splice(_list.begin(), _list, i->second);
			return;
		}
		_list.push_front(Entry{
			.key = key,
			.value = std::move(value),
		});
		_index.emplace(key, _list.begin());
		while (int(_list.size()) > kMaxEntries) {
			_index.erase(_list.back().key);
			_list.pop_back();
		}
	}

	void clear() {
		const auto lock = QMutexLocker(&_mutex);
		_index.clear();
		_list.clear();
	}

private:
	QMutex _mutex;
	std::list<Entry> _list;
	std::unordered_map<
		QString,
		std::list<Entry>::iterator,
		QStringHasher> _index;
};

[[nodiscard]] Store &Instance() {
	static auto store = Store();
	return store;
}

} // namespace

QString MakeKey(
		const QString &originalText,
		const QString &toLang,
		bool keepProtectedFormat) {
	return originalText
		+ QChar(0)
		+ toLang
		+ QChar(0)
		+ (keepProtectedFormat ? u"1"_q : u"0"_q);
}

std::optional<TextWithEntities> Get(const QString &key) {
	return Instance().get(key);
}

void Put(const QString &key, TextWithEntities value) {
	Instance().put(key, std::move(value));
}

void Clear() {
	Instance().clear();
}

} // namespace Lang::TranslateCache
