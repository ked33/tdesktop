/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/components/recent_search_queries.h"

#include "core/version.h"
#include "main/main_session.h"
#include "storage/serialize_common.h"
#include "storage/storage_account.h"

namespace Data {
namespace {

constexpr auto kLimit = 20;
constexpr auto kMaxLength = 256;

[[nodiscard]] bool SameQuery(const QString &a, const QString &b) {
	return a.compare(b, Qt::CaseInsensitive) == 0;
}

} // namespace

QString NormalizeRecentSearchQuery(const QString &query) {
	auto result = query.trimmed();
	if (result.size() > kMaxLength) {
		result.resize(kMaxLength);
		if (result.at(result.size() - 1).isHighSurrogate()) {
			result.chop(1);
		}
	}
	if (result.isEmpty()
		|| result == u"#"_q
		|| result == u"@"_q
		|| result == u"from:"_q) {
		return {};
	}
	return result;
}

RecentSearchQueries::RecentSearchQueries(not_null<Main::Session*> session)
: _session(session) {
}

RecentSearchQueries::~RecentSearchQueries() = default;

const std::vector<QString> &RecentSearchQueries::list() const {
	_session->local().readSearchSuggestions();

	return _list;
}

rpl::producer<> RecentSearchQueries::updates() const {
	return _updates.events();
}

void RecentSearchQueries::bump(const QString &entry) {
	const auto query = NormalizeRecentSearchQuery(entry);
	if (query.isEmpty()) {
		return;
	}
	_session->local().readSearchSuggestions();

	auto i = ranges::find_if(_list, [&](const QString &other) {
		return SameQuery(other, query);
	});
	if (i != end(_list)) {
		if (i == begin(_list) && *i == query) {
			return;
		}
		_list.erase(i);
	}
	_list.insert(begin(_list), query);
	if (int(_list.size()) > kLimit) {
		_list.resize(kLimit);
	}
	_updates.fire({});

	_session->local().writeSearchSuggestionsDelayed();
}

void RecentSearchQueries::remove(const QString &entry) {
	const auto query = NormalizeRecentSearchQuery(entry);
	if (query.isEmpty()) {
		return;
	}
	_session->local().readSearchSuggestions();

	const auto i = ranges::find_if(_list, [&](const QString &other) {
		return SameQuery(other, query);
	});
	if (i == end(_list)) {
		return;
	}
	_list.erase(i);
	_updates.fire({});

	_session->local().writeSearchSuggestionsDelayed();
}

void RecentSearchQueries::clear() {
	_session->local().readSearchSuggestions();
	if (_list.empty()) {
		return;
	}
	_list.clear();
	_updates.fire({});

	_session->local().writeSearchSuggestionsDelayed();
}

QByteArray RecentSearchQueries::serialize() const {
	_session->local().readSearchSuggestions();

	if (_list.empty()) {
		return {};
	}
	const auto count = std::min(int(_list.size()), kLimit);
	auto size = 2 * int(sizeof(quint32));
	for (auto i = 0; i != count; ++i) {
		size += Serialize::stringSize(_list[i]);
	}
	auto stream = Serialize::ByteArrayWriter(size);
	stream
		<< quint32(AppVersion)
		<< quint32(count);
	for (auto i = 0; i != count; ++i) {
		stream << _list[i];
	}
	return std::move(stream).result();
}

void RecentSearchQueries::applyLocal(QByteArray serialized) {
	_list.clear();
	if (serialized.isEmpty()) {
		return;
	}
	auto stream = Serialize::ByteArrayReader(serialized);
	auto version = quint32();
	auto count = quint32();
	stream >> version >> count;
	if (!stream.ok()) {
		return;
	}
	count = std::min(count, quint32(kLimit * 2));
	_list.reserve(std::min(int(count), kLimit));
	for (auto i = quint32(0); i != count; ++i) {
		auto value = QString();
		stream >> value;
		if (!stream.ok()) {
			_list.clear();
			return;
		}
		value = NormalizeRecentSearchQuery(value);
		if (value.isEmpty()
			|| ranges::find_if(_list, [&](const QString &entry) {
				return SameQuery(entry, value);
			}) != end(_list)) {
			continue;
		}
		_list.push_back(std::move(value));
		if (int(_list.size()) >= kLimit) {
			break;
		}
	}
}

} // namespace Data
