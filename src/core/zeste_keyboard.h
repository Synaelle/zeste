// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste headless software keyboard. When the game opens the keyboard (swkbdInputText), nothing is
// shown: the request is logged and printed on stdout, and the game waits (emulation keeps running)
// until a script or RPC command answers it:
//   keyboard <text>     type <text> and press OK
//   keyboard-cancel     press Cancel (only when the keyboard has a Cancel button)
// Answers sent before the request are queued and used in order.

#pragma once

#include <memory>
#include <string>

namespace Core {
class System;
}

namespace Zeste::Keyboard {

/// Registers the headless keyboard as the system's software keyboard.
void RegisterHeadless(Core::System& system);

/// Answers the current keyboard request, or queues the answer for the next one.
void Type(const std::string& text);
void Cancel();

/// True while the game waits for an answer to a keyboard request (headless).
bool IsWaiting();

} // namespace Zeste::Keyboard
