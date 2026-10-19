// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste sampling CPU profiler (a debugging tool, not an accuracy feature). Every N microseconds
// of emulated time, a timing event on each CPU core records the core's PC, LR, thread and
// process. The report lists the hottest functions per core and thread (names from --elf) and a
// collapsed-stack file for flame-graph tools. Emulated time is not console time: Azahar runs
// instructions at a flat rate (no caches, no memory waits), so the report shows where the
// instructions go, not where the console spends its time.

#pragma once

#include <string>
#include <vector>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Zeste::Profiler {

constexpr u32 DefaultIntervalUs = 100;

/// Starts sampling every `interval_us` microseconds of emulated time (restarts with a new
/// interval and empty counts when already running).
void Start(Core::System& system, u32 interval_us = DefaultIntervalUs);

/// Stops sampling and writes `<base>.md` and `<base>.folded`. Returns the .md path ("" when
/// nothing was sampled).
std::string Stop(Core::System& system, const std::string& base_path);

bool Active();

/// Registers the sampling event on the current Timing (a save state load creates a new one before
/// restoring its queue, which may hold profiler events).
void RegisterEvent(Core::System& system);

/// Every frame: re-arms the sampling events when a save state load replaced the event queue.
void OnVBlank(Core::System& system);

/// At shutdown: a profile still running is stopped and written to the run folder (profile.md).
void OnShutdown(Core::System& system);

/// Reports written during this run (for the run summary).
std::vector<std::string> Reports();

} // namespace Zeste::Profiler
