// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste race exposure and starvation reporting.
//
// Azahar's scheduler already follows strict priorities inside a core, but the emulated cores run
// one after the other in slices of about 1.1 million cycles (4 ms), so two threads on different
// cores almost never interleave inside a short critical section, and a high-priority thread's
// wake-ups land only at slice boundaries. Zeste can shrink the interleaving granularity:
//   mode 1: every SVC ends the core's slice (the other cores then run as far);
//   mode 2: every slice gets a seeded random length (200 cycles to the normal maximum).
// Runs with the same seed, inputs and settings take the same scheduling decisions.

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Zeste::Sched {

/// Re-seeds the random preemption generator (called on boot).
void Reset();

/// Slice length to use for the next round of all cores (mode 2 randomises it).
s64 AdjustSlice(s64 max_slice);

/// Mode 2: number of single instructions (0-7) to execute past the end of a JIT slice, so the
/// preemption point falls inside a JIT block (between a load and its store) instead of always on
/// a block boundary. 0 in the other modes.
u32 ExtraSteps();

/// Called after every SVC (mode 1 ends the slice there).
void OnSvc(Core::System& system);

/// Called at every VBlank: reports threads that stayed ready without running for a long time
/// while higher-priority threads ran on their core (strict-priority starvation).
void OnVBlank(Core::System& system);

} // namespace Zeste::Sched
