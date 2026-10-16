// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste CPU data cache detection.
//
// Azahar does not emulate the ARM11 data cache: whatever the CPU writes is immediately visible to
// the GPU, the DSP and the transfer engine. On hardware, data the CPU wrote stays in the data
// cache until it is flushed (GSPGPU_FlushDataCache, DSP_FlushDataCache, GX flush commands), and
// data a device wrote can be shadowed by stale cache lines until the CPU invalidates them.
//
// Instead of emulating the cache, Zeste tracks 32-byte lines (the ARM11 line size) of the memory
// devices read:
//   - a CPU flush (or a device/system write, or a fresh allocation) takes a snapshot of the lines;
//   - when a device reads memory, every line must have been flushed or written by a device, and
//     must still match its snapshot. Lines that differ were written by the CPU after their last
//     flush: on hardware the device may see stale data. Each such read is reported.
//   - lines a device wrote are marked; when the CPU reads them through the renderer cache path
//     before an InvalidateDataCache, that is reported too.
// Everything is a no-op when zeste_cache_detection is off.

#pragma once

#include <optional>
#include "common/common_types.h"

namespace Memory {
class MemorySystem;
}

namespace Zeste::Cache {

bool Enabled();

void Reset();

/// Physical address of a linear heap / VRAM / N3DS extra RAM virtual address (fixed mappings).
std::optional<PAddr> LinearToPhysical(VAddr vaddr);

/// The CPU flushed (or stored) a range from its data cache.
void OnCpuFlush(Memory::MemorySystem& memory, PAddr addr, u32 size);

/// The CPU invalidated a range of its data cache.
void OnCpuInvalidate(PAddr addr, u32 size);

/// The GPU or the transfer engine wrote a range. With hardware renderers the guest memory copy
/// may be stale, so these lines are trusted for device reads and checked on CPU reads.
void OnDeviceWrite(Memory::MemorySystem& memory, PAddr addr, u32 size);

/// A system agent wrote a range outside the application's data cache (kernel zero-fill of new
/// allocations, service writes such as file reads, DMA). The lines become clean.
void OnSystemWrite(Memory::MemorySystem& memory, PAddr addr, u32 size);

/// A device reads a range. Reports lines the CPU wrote without a flush. `what` names the reader
/// ("GPU vertex buffer", "DSP sample buffer"...).
void CheckDeviceRead(Memory::MemorySystem& memory, PAddr addr, u32 size, const char* what);

/// The CPU reads a range that the renderer cache holds (GPU output). Reports reads of
/// device-written lines that were not invalidated first.
void CheckCpuRead(PAddr addr, u32 size);

} // namespace Zeste::Cache
