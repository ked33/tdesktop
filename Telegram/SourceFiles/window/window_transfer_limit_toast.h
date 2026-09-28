#pragma once

#include "base/basic_types.h"

namespace Main {
class Session;
} // namespace Main

namespace Window {

void SetupTransferLimitToasts(not_null<Main::Session*> session);

} // namespace Window
