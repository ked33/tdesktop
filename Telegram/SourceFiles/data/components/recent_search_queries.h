/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

namespace Main {
class Session;
} // namespace Main

namespace Data {

[[nodiscard]] QString NormalizeRecentSearchQuery(const QString &query);

class RecentSearchQueries final {
public:
	explicit RecentSearchQueries(not_null<Main::Session*> session);
	~RecentSearchQueries();

	[[nodiscard]] const std::vector<QString> &list() const;
	[[nodiscard]] rpl::producer<> updates() const;

	void bump(const QString &query);
	void remove(const QString &query);
	void clear();

	[[nodiscard]] QByteArray serialize() const;
	void applyLocal(QByteArray serialized);

private:
	const not_null<Main::Session*> _session;

	std::vector<QString> _list;
	rpl::event_stream<> _updates;

};

} // namespace Data
