// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "common/settings.h"
#include "common/zeste_report.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "core/zeste_gx.h"

namespace Zeste::Gx {

namespace Detail {
std::atomic<u32> g_pending_count{0};
}

namespace {

struct Pending {
    u64 id;
    PAddr paddr;
    VAddr vaddr;
    u32 size;
    const char* what;
    s64 done_ticks;
    std::weak_ptr<Kernel::Process> process;
    std::vector<VAddr> trapped_pages; ///< Pages given a watchpoint (one count each)
    bool reported = false;
};

std::mutex g_mutex;
std::vector<Pending> g_pending;
Core::System* g_system = nullptr;

/// Virtual address of a GPU-visible physical address in the application's fixed mappings.
std::optional<VAddr> PhysicalToVirtual(const Kernel::Process& process, PAddr addr) {
    if (addr >= Memory::VRAM_PADDR && addr < Memory::VRAM_PADDR_END) {
        return Memory::VRAM_VADDR + (addr - Memory::VRAM_PADDR);
    }
    if (addr >= Memory::N3DS_EXTRA_RAM_PADDR && addr < Memory::N3DS_EXTRA_RAM_PADDR_END) {
        return Memory::N3DS_EXTRA_RAM_VADDR + (addr - Memory::N3DS_EXTRA_RAM_PADDR);
    }
    if (addr >= Memory::FCRAM_PADDR && addr < Memory::FCRAM_N3DS_PADDR_END) {
        const VAddr area = process.GetLinearHeapAreaAddress();
        const u32 area_size = area == Memory::LINEAR_HEAP_VADDR ? Memory::LINEAR_HEAP_SIZE
                                                                 : Memory::NEW_LINEAR_HEAP_SIZE;
        const u32 offset = addr - Memory::FCRAM_PADDR;
        if (offset < area_size) {
            return area + offset;
        }
    }
    return std::nullopt;
}

bool CanTrap(Memory::PageType type) {
    // Unmapped pages cannot hold a watchpoint (MemorySystem::RegisterWatchpoint needs memory).
    return type != Memory::PageType::Unmapped;
}

} // Anonymous namespace

bool Enabled() {
    return Settings::values.zeste_async_gx.GetValue();
}

void AddPending(Core::System& system, u64 id, PAddr addr, u32 size, const char* what,
                s64 done_ticks) {
    if (size == 0) {
        return;
    }
    const auto process = system.Kernel().GetCurrentProcess();
    Pending pending{id, addr, 0, size, what, done_ticks, process, {}, false};
    if (process) {
        if (const auto vaddr = PhysicalToVirtual(*process, addr)) {
            pending.vaddr = *vaddr;
            auto& page_table = *process->vm_manager.page_table;
            const u64 end = static_cast<u64>(*vaddr) + size;
            for (u64 page = *vaddr & ~static_cast<u64>(Memory::CITRA_PAGE_MASK); page < end;
                 page += Memory::CITRA_PAGE_SIZE) {
                if (CanTrap(page_table.attributes[page >> Memory::CITRA_PAGE_BITS])) {
                    system.Memory().RegisterWatchpoint(*process, static_cast<VAddr>(page), 1);
                    pending.trapped_pages.push_back(static_cast<VAddr>(page));
                }
            }
        }
    }
    std::scoped_lock lock{g_mutex};
    g_system = &system;
    g_pending.push_back(std::move(pending));
    Detail::g_pending_count.store(static_cast<u32>(g_pending.size()), std::memory_order_relaxed);
}

void RemovePending(Core::System& system, u64 id) {
    Pending pending{};
    {
        std::scoped_lock lock{g_mutex};
        const auto it = std::find_if(g_pending.begin(), g_pending.end(),
                                     [id](const Pending& p) { return p.id == id; });
        if (it == g_pending.end()) {
            return;
        }
        pending = std::move(*it);
        g_pending.erase(it);
        Detail::g_pending_count.store(static_cast<u32>(g_pending.size()),
                                      std::memory_order_relaxed);
    }
    if (const auto process = pending.process.lock()) {
        for (const VAddr page : pending.trapped_pages) {
            system.Memory().UnregisterWatchpoint(*process, page, 1);
        }
    }
}

void Reset() {
    std::scoped_lock lock{g_mutex};
    g_pending.clear();
    g_system = nullptr;
    Detail::g_pending_count.store(0, std::memory_order_relaxed);
}

void OnCpuRead(VAddr vaddr, u32 size) {
    Pending hit{};
    s64 now = 0;
    {
        std::scoped_lock lock{g_mutex};
        if (!g_system) {
            return;
        }
        const u64 end = static_cast<u64>(vaddr) + size;
        for (auto& p : g_pending) {
            if (p.vaddr == 0 || vaddr >= static_cast<u64>(p.vaddr) + p.size || end <= p.vaddr) {
                continue;
            }
            if (p.reported) {
                return;
            }
            p.reported = true;
            hit = p;
            now = g_system->CoreTiming().GetTicks();
            break;
        }
    }
    if (hit.what == nullptr) {
        return;
    }
    const double us_left =
        static_cast<double>(hit.done_ticks - now) * 1e6 / BASE_CLOCK_RATE_ARM11;
    ReportF(Category::Timing, fmt::format("gx-pending-read@{:08X}", hit.paddr),
            "CPU read of 0x{:08X} ({} bytes) inside the destination of a {} that is still "
            "running (0x{:08X}-0x{:08X}, physical 0x{:08X}, done in {:.1f} us): on hardware this "
            "reads the old contents or a half-finished copy. gspWaitForPPF/gspWaitForPSC0, also "
            "inside C3D_SyncTextureCopy/C3D_SyncDisplayTransfer/C3D_SyncMemoryFill outside a "
            "frame, return at once when that event was raised earlier (citro3d's frames raise "
            "PPF and PSC0); wait for this operation itself, e.g. gxCmdQueueWait on the queue it "
            "went through",
            vaddr, size, hit.what, hit.vaddr, hit.vaddr + hit.size, hit.paddr, us_left);
}

} // namespace Zeste::Gx
