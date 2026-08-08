/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lang/translate_protect.h"

namespace Lang::TranslateProtect {
namespace {

constexpr auto kMaxProtectTextLength = 800;
constexpr auto kMaxProtectEntities = 12;
constexpr auto kMaxPlaceholders = 8;

[[nodiscard]] QString PlaceholderFor(int index) {
	return u"\u27E6TG"_q + QString::number(index) + u"\u27E7"_q;
}

[[nodiscard]] bool RangesOverlap(
		int aOffset,
		int aLength,
		int bOffset,
		int bLength) {
	return aOffset < (bOffset + bLength) && bOffset < (aOffset + aLength);
}

} // namespace

bool IsProtectedType(EntityType type) {
	switch (type) {
	case EntityType::Url:
	case EntityType::CustomUrl:
	case EntityType::Email:
	case EntityType::Code:
	case EntityType::Pre:
		return true;
	default:
		return false;
	}
}

Protected Protect(const TextWithEntities &original) {
	Protected result;
	result.text = original.text;
	if (original.text.isEmpty()
		|| original.entities.isEmpty()
		|| original.text.size() > kMaxProtectTextLength) {
		return result;
	}

	struct Candidate {
		int offset = 0;
		int length = 0;
		EntityType type = EntityType::Invalid;
		QString data;
		QString original;
	};
	auto candidates = std::vector<Candidate>();
	candidates.reserve(original.entities.size());
	for (const auto &entity : original.entities) {
		if (!IsProtectedType(entity.type())) {
			continue;
		}
		const auto offset = entity.offset();
		const auto length = entity.length();
		if (offset < 0
			|| length <= 0
			|| offset >= original.text.size()
			|| (offset + length) > original.text.size()) {
			continue;
		}
		candidates.push_back({
			.offset = offset,
			.length = length,
			.type = entity.type(),
			.data = entity.data(),
			.original = original.text.mid(offset, length),
		});
	}
	if (candidates.empty()
		|| int(candidates.size()) > kMaxProtectEntities) {
		return result;
	}

	ranges::sort(candidates, ranges::less(), &Candidate::offset);
	auto selected = std::vector<Candidate>();
	selected.reserve(std::min(int(candidates.size()), kMaxPlaceholders));
	for (const auto &candidate : candidates) {
		if (int(selected.size()) >= kMaxPlaceholders) {
			break;
		}
		const auto overlaps = ranges::any_of(
			selected,
			[&](const Candidate &existing) {
				return RangesOverlap(
					existing.offset,
					existing.length,
					candidate.offset,
					candidate.length);
			});
		if (!overlaps) {
			selected.push_back(candidate);
		}
	}
	if (selected.empty()) {
		return result;
	}

	result.spans.reserve(selected.size());
	for (auto i = 0; i != int(selected.size()); ++i) {
		result.spans.push_back({
			.index = i,
			.type = selected[i].type,
			.data = selected[i].data,
			.original = selected[i].original,
			.placeholder = PlaceholderFor(i),
		});
	}

	for (auto i = int(selected.size()); i != 0;) {
		--i;
		result.text.replace(
			selected[i].offset,
			selected[i].length,
			result.spans[i].placeholder);
	}
	result.used = true;
	return result;
}

TextWithEntities Restore(
		const QString &translated,
		const std::vector<Span> &spans) {
	if (spans.empty() || translated.isEmpty()) {
		return { translated };
	}

	struct Hit {
		int offset = 0;
		int placeholderLength = 0;
		EntityType type = EntityType::Invalid;
		QString data;
		QString original;
	};

	auto text = translated;
	auto hits = std::vector<Hit>();
	hits.reserve(spans.size());

	for (const auto &span : spans) {
		const auto pos = text.indexOf(span.placeholder);
		if (pos < 0) {
			continue;
		}
		const auto overlaps = ranges::any_of(
			hits,
			[&](const Hit &existing) {
				return RangesOverlap(
					existing.offset,
					existing.placeholderLength,
					pos,
					span.placeholder.size());
			});
		if (overlaps) {
			continue;
		}
		hits.push_back({
			.offset = pos,
			.placeholderLength = span.placeholder.size(),
			.type = span.type,
			.data = span.data,
			.original = span.original,
		});
	}
	if (hits.empty()) {
		return { translated };
	}

	ranges::sort(hits, ranges::less(), &Hit::offset);
	auto entities = EntitiesInText();
	entities.reserve(int(hits.size()));

	// Right-to-left replace keeps lower offsets stable.
	for (auto i = int(hits.size()); i != 0;) {
		--i;
		const auto &hit = hits[i];
		text.replace(hit.offset, hit.placeholderLength, hit.original);
		entities.push_back({
			hit.type,
			hit.offset,
			hit.original.size(),
			hit.data,
		});
	}

	ranges::sort(entities, ranges::less(), [](const EntityInText &entity) {
		return entity.offset();
	});
	return { text, std::move(entities) };
}

} // namespace Lang::TranslateProtect
