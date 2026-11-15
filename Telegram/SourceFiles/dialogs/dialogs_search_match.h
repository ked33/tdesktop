/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/text/text_entity.h"

#include <QtCore/QStringView>

#include <algorithm>
#include <utility>
#include <vector>

namespace Dialogs {

[[nodiscard]] inline bool SearchTextMatches(QStringView text, QStringView query) {
	return query.isEmpty() || text.contains(query, Qt::CaseInsensitive);
}

template <typename Callback>
void ForEachSearchMatch(QStringView text, QStringView query, Callback callback) {
	if (query.isEmpty()) {
		return;
	}
	auto from = text.indexOf(query, 0, Qt::CaseInsensitive);
	while (from >= 0) {
		auto till = from + query.size();
		auto next = text.indexOf(query, from + 1, Qt::CaseInsensitive);
		while (next >= 0 && next < till) {
			till = next + query.size();
			next = text.indexOf(query, next + 1, Qt::CaseInsensitive);
		}
		callback(int(from), int(till - from));
		from = next;
	}
}

[[nodiscard]] inline int HighlightSearchText(
		TextWithEntities &text,
		QStringView query,
		int start = 0) {
	struct Range {
		int from = 0;
		int till = 0;
	};
	auto matches = std::vector<Range>();
	ForEachSearchMatch(
		QStringView(text.text).mid(start),
		query,
		[&](int from, int length) {
			matches.push_back({ from + start, from + start + length });
		});
	if (matches.empty()) {
		return -1;
	}
	auto entities = EntitiesInText();
	entities.reserve(text.entities.size() + matches.size());
	for (const auto &entity : text.entities) {
		if (entity.type() != EntityType::Colorized) {
			entities.push_back(entity);
			continue;
		}
		auto from = entity.offset();
		const auto till = from + entity.length();
		const auto append = [&](int end) {
			if (end > from) {
				entities.push_back(EntityInText(
					EntityType::Colorized,
					from,
					end - from,
					entity.data()));
			}
		};
		for (const auto &match : matches) {
			if (match.till <= from) {
				continue;
			} else if (match.from >= till) {
				break;
			}
			append(match.from);
			from = std::min(match.till, till);
		}
		append(till);
	}
	for (const auto &match : matches) {
		entities.push_back(EntityInText(
			EntityType::Colorized,
			match.from,
			match.till - match.from,
			QString(QChar(1))));
	}
	std::stable_sort(
		entities.begin(),
		entities.end(),
		[](const auto &a, const auto &b) { return a.offset() < b.offset(); });
	text.entities = std::move(entities);
	return matches.front().from;
}

} // namespace Dialogs
