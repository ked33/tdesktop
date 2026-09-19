/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/core_types.h"

namespace MTP::details {

[[nodiscard]] QString DumpToJson(const mtpPrime *from, const mtpPrime *end);

} // namespace MTP::details
