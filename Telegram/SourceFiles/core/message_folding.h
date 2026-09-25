#pragma once

#include "rpl/producer.h"

class HistoryItem;

namespace MessageFolding {

inline constexpr auto kMaxRuleLength = 65536;

[[nodiscard]] bool Enabled();
[[nodiscard]] uint64 Version();
[[nodiscard]] QString Keywords();
[[nodiscard]] QString UserIds();
[[nodiscard]] bool ValidUserIds(const QString &value);
[[nodiscard]] bool Save(const QString &keywords, const QString &userIds);
void SetEnabled(bool enabled);
void Reload();
[[nodiscard]] rpl::producer<> Changes();
[[nodiscard]] bool Matches(not_null<HistoryItem*> item);
void Invalidate(not_null<HistoryItem*> item);

} // namespace MessageFolding
