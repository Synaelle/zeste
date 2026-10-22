// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste memory budget: once a second, how full the game's malloc heap is (Zeste::Heap::Walk). The
// heap has the size it would have on the console for that memory mode (libctru maps nearly all of
// the APPLICATION region at start), so a full heap here is a full heap there. Warns once at 85%
// and once at 95% (zeste_memory_warnings, on by default) and puts the peaks in the run summary.

#pragma once

#include <string>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Zeste::MemoryBudget {

/// Samples the heap once a second. Called at every VBlank.
void OnVBlank(Core::System& system);

/// "- malloc heap: peak 80.2 MB used of 90.0 MB..." lines for the run summary, empty before the
/// first sample.
std::string SummaryText();

void Reset();

} // namespace Zeste::MemoryBudget
