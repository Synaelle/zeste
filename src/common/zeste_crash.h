// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste's own crashes (not the emulated game's): on Windows, an unhandled exception in zeste.exe
// writes zeste_crash.dmp (a minidump to open in Visual Studio or WinDbg next to zeste.pdb) and
// zeste_crash.txt (the exception and the stack with function names) into the run folder, then
// calls the frontend's callback so the run summary says Zeste crashed.

#pragma once

#include <functional>
#include <string>

namespace Zeste::Crash {

/// `on_crash` gets the exception code and a one-line description. It runs inside the crash
/// handler, so it must be short and is best effort.
void Install(std::function<void(unsigned int code, const std::string& what)> on_crash);

} // namespace Zeste::Crash
