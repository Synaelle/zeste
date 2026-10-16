// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste simulated system events for automation: HOME Menu suspend/resume, sleep (lid close),
// HOME Menu "close" and the Rosalina menu combination. Driven by script/RPC commands and
// advanced at every VBlank.

#pragma once

#include <string>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Zeste::SystemEvents {

/// Starts an event: "home" (amount = frames spent in the HOME Menu, default 120), "sleep"
/// (frames asleep, default 120), "close", "rosalina" (milliseconds paused, default 3000).
void Start(Core::System& system, const std::string& event, u64 amount);

/// Advances the running event. Called at every VBlank.
void OnVBlank(Core::System& system);

/// Buttons currently forced by an event (the Rosalina combination), HID pad bits.
u32 ForcedButtons();

/// Records the application's answer to the last sleep query (APT ReplySleepQuery).
void OnSleepQueryReply(u32 reply);

void Reset();

} // namespace Zeste::SystemEvents
