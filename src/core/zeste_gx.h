// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste asynchronous GX transfers (zeste_async_gx).
//
// Azahar performs display transfers, texture copies and memory fills at the register write. On
// hardware they run in the background and finish later, so a CPU that reads the destination too
// early (for example after a gspWaitForPPF that returned on an older event) sees old or half
// copied data. With the option on, the GPU (video_core/gpu.cpp) performs each operation at its
// modelled completion time. This file keeps the destinations that are still being written and
// reports CPU reads of them: their pages are trapped with the watchpoint page types, so guest
// loads take the slow path while an operation is pending.

#pragma once

#include <atomic>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Zeste::Gx {

bool Enabled();

/// A deferred operation `id` will write [addr, addr + size) (physical) at `done_ticks`.
/// `what` names it in reports ("texture copy", "display transfer", "memory fill").
void AddPending(Core::System& system, u64 id, PAddr addr, u32 size, const char* what,
                s64 done_ticks);

/// The operation completed (or was dropped): its destination is no longer trapped.
void RemovePending(Core::System& system, u64 id);

/// Forgets every pending destination without touching page tables (GPU created or destroyed).
void Reset();

namespace Detail {
extern std::atomic<u32> g_pending_count;
}

/// True while at least one destination is pending (cheap check for the memory read path).
inline bool AnyPending() {
    return Detail::g_pending_count.load(std::memory_order_relaxed) != 0;
}

/// The CPU reads `size` bytes at `vaddr` (current process). Reports the first read of each
/// pending destination.
void OnCpuRead(VAddr vaddr, u32 size);

} // namespace Zeste::Gx
