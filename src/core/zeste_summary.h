// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste run summary: one file per run (summary.md in the run folder) with why the run ended,
// the findings, the crash (registers and a stack scan, with function names when --elf is given),
// the last lines of the game's debug output and of the log, and the screenshots taken.

#pragma once

#include <string>
#include "common/common_types.h"

namespace Zeste::Summary {

/// The game's ELF (with symbols): crash addresses get function names.
void SetElfPath(const std::string& path);

/// "function+0x12" for an address, or "" when there is no ELF or no symbol covers it.
std::string Symbolize(u32 address);

/// The name of the function containing the address, or "" (no ELF, or no symbol covers it).
std::string FunctionName(u32 address);

/// True when an ELF with symbols was given (--elf) and loaded.
bool HasElf();

/// Remembers the emulated frame count at the end of emulation (the system is gone afterwards).
void SetFramesRun(u64 frames);

/// Writes <folder>summary.md. `reason` is used when no command or crash set one.
void Write(const std::string& folder, int exit_code, const std::string& reason);

} // namespace Zeste::Summary
