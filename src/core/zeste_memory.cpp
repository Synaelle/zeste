// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <iostream>
#include <mutex>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/zeste_run.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/memory.h"
#include "core/zeste_core.h"
#include "core/zeste_heap.h"
#include "core/zeste_memory.h"

namespace Zeste::MemoryBudget {

namespace {

constexpr u64 SampleEveryVBlanks = 60;
constexpr u32 Thresholds[] = {85, 95};

std::mutex g_mutex;
u64 g_vblanks = 0;
bool g_started = false;
bool g_is_3dsx = false;
Kernel::MemoryMode g_mode = Kernel::MemoryMode::Prod;
u32 g_region_size = 0;
u32 g_region_peak = 0;
u32 g_heap_mapped = 0;
u32 g_heap_peak = 0;
u64 g_heap_peak_frame = 0;
u32 g_warned = 0; ///< Thresholds already reported

double Mb(u32 bytes) {
    return bytes / (1024.0 * 1024.0);
}

std::string ModeText(Kernel::MemoryMode mode) {
    switch (mode) {
    case Kernel::MemoryMode::Prod:
        return "64 MB mode";
    case Kernel::MemoryMode::Dev1:
        return "96 MB mode";
    case Kernel::MemoryMode::Dev2:
        return "80 MB mode";
    case Kernel::MemoryMode::Dev3:
        return "72 MB mode";
    case Kernel::MemoryMode::Dev4:
        return "32 MB mode";
    case Kernel::MemoryMode::NewProd:
        return "New 3DS 124 MB mode";
    case Kernel::MemoryMode::NewDev1:
        return "New 3DS 178 MB mode";
    }
    return "unknown mode";
}

/// What the console would give this game, when Zeste gives it something else.
std::string ConsoleNote() {
    if (g_is_3dsx && Settings::values.is_new_3ds.GetValue() &&
        g_mode != Kernel::MemoryMode::NewProd) {
        return " Under the Homebrew Launcher a New 3DS gives .3dsx files 124 MB "
               "(hw_memory_layout).";
    }
    return {};
}

} // Anonymous namespace

void OnVBlank(Core::System& system) {
    if (!system.IsPoweredOn()) {
        return;
    }
    {
        std::scoped_lock lock{g_mutex};
        if (++g_vblanks % SampleEveryVBlanks != 0) {
            return;
        }
        if (!g_started) {
            g_started = true;
            const std::string path = system.GetFilePath();
            g_is_3dsx = path.size() >= 5 && path.compare(path.size() - 5, 5, ".3dsx") == 0;
            g_mode = system.Kernel().GetMemoryMode();
        }
        const auto region = system.Kernel().GetMemoryRegion(Kernel::MemoryRegion::APPLICATION);
        if (region) {
            g_region_size = region->size;
            g_region_peak = std::max(g_region_peak, region->used);
        }
    }

    // libctru maps nearly all the free memory as the heap at start, so the region is always
    // close to full: what can run out is malloc's share of that heap.
    const Heap::Summary heap = Heap::Walk(system);
    if (!heap.valid || heap.heap_mapped == 0) {
        return;
    }
    const u64 frame = CurrentFrame(system);
    std::string warning;
    {
        std::scoped_lock lock{g_mutex};
        g_heap_mapped = heap.heap_mapped;
        if (heap.used > g_heap_peak) {
            g_heap_peak = heap.used;
            g_heap_peak_frame = frame;
        }
        const u32 percent = static_cast<u32>(u64{heap.used} * 100 / heap.heap_mapped);
        for (u32 i = 0; i < std::size(Thresholds); ++i) {
            if (percent >= Thresholds[i] && (g_warned & (1u << i)) == 0) {
                g_warned |= (1u << (i + 1)) - 1; // the lower thresholds are covered too
                warning = fmt::format(
                    "Zeste memory: malloc uses {:.1f} MB of the {:.1f} MB heap ({}%) at frame {}: "
                    "close to running out on the console ({}).{}",
                    Mb(heap.used), Mb(heap.heap_mapped), percent, frame, ModeText(g_mode),
                    ConsoleNote());
            }
        }
    }
    if (!warning.empty() && Settings::values.zeste_memory_warnings.GetValue()) {
        LOG_WARNING(Zeste, "{}", warning);
        std::cout << warning << std::endl;
        Run::Note("memory warning", warning);
    }
}

std::string SummaryText() {
    std::scoped_lock lock{g_mutex};
    std::string out;
    if (g_heap_mapped != 0) {
        out += fmt::format("- malloc heap: peak {:.1f} MB used of {:.1f} MB ({}%) at frame {}.\n",
                           Mb(g_heap_peak), Mb(g_heap_mapped),
                           u64{g_heap_peak} * 100 / g_heap_mapped, g_heap_peak_frame);
    }
    if (g_region_size != 0) {
        out += fmt::format("- APPLICATION memory mapped (code, heap, linear heap, stacks): peak "
                           "{:.1f} MB of {:.1f} MB, {}.{}\n",
                           Mb(g_region_peak), Mb(g_region_size), ModeText(g_mode), ConsoleNote());
    }
    return out;
}

void Reset() {
    std::scoped_lock lock{g_mutex};
    g_vblanks = 0;
    g_started = false;
    g_region_size = 0;
    g_region_peak = 0;
    g_heap_mapped = 0;
    g_heap_peak = 0;
    g_heap_peak_frame = 0;
    g_warned = 0;
}

} // namespace Zeste::MemoryBudget
