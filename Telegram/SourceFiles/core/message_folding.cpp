#include "core/message_folding.h"
#include "core/message_folding_matcher.h"

#include "core/enhanced_settings.h"
#include "data/data_peer.h"
#include "history/history_item.h"
#include "rpl/event_stream.h"
#include "settings.h"

#include <QtCore/QStringList>
#include <unordered_set>

namespace MessageFolding {
namespace {

struct MatchCache : RuntimeComponent<MatchCache, HistoryItem> {
	uint64 version = 0;
	PeerId sender;
	bool matched = false;
};

struct Rules {
	bool enabled = true;
	uint64 version = 0;
	QString keywords;
	QString userIds;
	KeywordMatcher matcher;
	std::unordered_set<uint64> users;
};

Rules Current;
rpl::event_stream<> Events;

QStringList Split(const QString &value) {
	auto normalized = value;
	normalized.replace(QChar(0xFF0C), QChar(','));
	auto result = QStringList();
	for (const auto &part : normalized.split(QChar(','), Qt::SkipEmptyParts)) {
		const auto word = part.trimmed();
		if (!word.isEmpty()) {
			result.push_back(word);
		}
	}
	result.removeDuplicates();
	return result;
}

QString Normalize(const QString &value) {
	return value.normalized(QString::NormalizationForm_C).toCaseFolded();
}

bool ParseIds(const QString &value, std::unordered_set<uint64> &result) {
	if (value.size() > kMaxRuleLength) {
		return false;
	}
	for (const auto &part : Split(value)) {
		const auto id = ParseUserId(part.toStdU16String());
		if (!id) {
			return false;
		}
		result.emplace(*id);
	}
	return true;
}

void Compile(Rules &rules) {
	auto words = std::vector<std::u16string>();
	for (const auto &word : Split(Normalize(rules.keywords))) {
		words.push_back(word.toStdU16String());
	}
	rules.matcher = KeywordMatcher(std::move(words));
}

bool MatchText(const QString &text) {
	if (text.isEmpty() || Current.matcher.empty()) {
		return false;
	}
	return Current.matcher.matches(Normalize(text).toStdU16String());
}

} // namespace

bool Enabled() {
	return Current.enabled;
}

uint64 Version() {
	return Current.version;
}

QString Keywords() {
	return Current.keywords;
}

QString UserIds() {
	return Current.userIds;
}

bool ValidUserIds(const QString &value) {
	auto ids = std::unordered_set<uint64>();
	return ParseIds(value, ids);
}

bool Save(const QString &keywords, const QString &userIds) {
	if (keywords.size() > kMaxRuleLength || !ValidUserIds(userIds)) {
		return false;
	}
	SetEnhancedValue(u"message_folding_keywords"_q, Split(keywords).join(','));
	SetEnhancedValue(u"message_folding_user_ids"_q, Split(userIds).join(','));
	Reload();
	EnhancedSettings::Write();
	return true;
}

void SetEnabled(bool enabled) {
	if (enabled == Current.enabled) {
		return;
	}
	SetEnhancedValue(u"message_folding_enabled"_q, enabled);
	Reload();
	EnhancedSettings::Write();
}

void Reload() {
	auto rules = Rules();
	rules.enabled = gEnhancedOptions.value(
		u"message_folding_enabled"_q, true).toBool();
	rules.keywords = GetEnhancedString(u"message_folding_keywords"_q);
	rules.userIds = GetEnhancedString(u"message_folding_user_ids"_q);
	if (rules.keywords.size() > kMaxRuleLength) {
		rules.keywords.clear();
	}
	if (!ParseIds(rules.userIds, rules.users)) {
		rules.users.clear();
	}
	if (Current.version
		&& rules.enabled == Current.enabled
		&& rules.keywords == Current.keywords
		&& rules.userIds == Current.userIds) {
		return;
	}
	rules.version = Current.version + 1;
	if (rules.enabled) {
		Compile(rules);
	}
	Current = std::move(rules);
	Events.fire({});
}

rpl::producer<> Changes() {
	return Events.events();
}

bool Matches(not_null<HistoryItem*> item) {
	if (!Enabled() || item->out() || item->isService()) {
		return false;
	} else if (Current.users.empty() && Current.matcher.empty()) {
		return false;
	}
	item->AddComponents(MatchCache::Bit());
	const auto cache = item->Get<MatchCache>();
	const auto sender = item->from()->id;
	if (cache->version != Current.version || cache->sender != sender) {
		cache->version = Current.version;
		cache->sender = sender;
		cache->matched = (sender.is<UserId>()
			&& Current.users.contains(peerToUser(sender).bare))
			|| MatchText(item->originalText().text);
		if (!cache->matched && !Current.matcher.empty()) {
			if (const auto media = item->media()) {
				cache->matched = MatchText(media->consumedMessageText().text);
			}
		}
	}
	return cache->matched;
}

void Invalidate(not_null<HistoryItem*> item) {
	if (const auto cache = item->Get<MatchCache>()) {
		cache->version = 0;
	}
}

} // namespace MessageFolding
