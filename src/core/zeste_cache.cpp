// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <array>
#include <bitset>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "common/settings.h"
#include "common/zeste_report.h"
#include "core/memory.h"
#include "core/zeste_cache.h"

namespace Zeste::Cache {

namespace {

constexpr u32 LineSize = 32; ///< ARM11 MPCore L1 data cache line
constexpr u32 PageSize = 0x1000;
constexpr u32 LinesPerPage = PageSize / LineSize;

struct PageState {
    std::array<u8, PageSize> snapshot{};
    std::bitset<LinesPerPage> clean;  ///< Flushed (or written by a device): snapshot is valid
    std::bitset<LinesPerPage> device; ///< Written by a device, not invalidated by the CPU since
};

struct State {
    std::mutex mutex;
    std::unordered_map<u32, std::unique_ptr<PageState>> pages;
};

State& GetState() {
    static State state;
    return state;
}

PageState& GetPage(State& s, u32 page) {
    auto& entry = s.pages[page];
    if (!entry) {
        entry = std::make_unique<PageState>();
    }
    return *entry;
}

/// Calls f(page_index, first_line, last_line_exclusive) for each page overlapped by the range.
template <typename F>
void ForEachPage(PAddr addr, u32 size, F&& f) {
    if (size == 0) {
        return;
    }
    const u64 end = static_cast<u64>(addr) + size;
    u64 cursor = addr & ~static_cast<u64>(LineSize - 1);
    while (cursor < end) {
        const u32 page = static_cast<u32>(cursor / PageSize);
        const u64 page_end = std::min<u64>(static_cast<u64>(page + 1) * PageSize, end);
        const u32 first = static_cast<u32>((cursor % PageSize) / LineSize);
        const u32 last = static_cast<u32>(((page_end - 1) % PageSize) / LineSize) + 1;
        f(page, first, last);
        cursor = static_cast<u64>(page + 1) * PageSize;
    }
}

void Snapshot(Memory::MemorySystem& memory, PageState& state, u32 page, u32 first, u32 last) {
    const u8* src = memory.GetPhysicalPointer(page * PageSize);
    if (!src) {
        return;
    }
    std::memcpy(state.snapshot.data() + first * LineSize, src + first * LineSize,
                (last - first) * LineSize);
}

} // Anonymous namespace

bool Enabled() {
    return Settings::values.zeste_cache_detection.GetValue();
}

void Reset() {
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    s.pages.clear();
}

std::optional<PAddr> LinearToPhysical(VAddr vaddr) {
    if (vaddr >= Memory::LINEAR_HEAP_VADDR && vaddr < Memory::LINEAR_HEAP_VADDR_END) {
        return Memory::FCRAM_PADDR + (vaddr - Memory::LINEAR_HEAP_VADDR);
    }
    if (vaddr >= Memory::NEW_LINEAR_HEAP_VADDR && vaddr < Memory::NEW_LINEAR_HEAP_VADDR_END) {
        return Memory::FCRAM_PADDR + (vaddr - Memory::NEW_LINEAR_HEAP_VADDR);
    }
    if (vaddr >= Memory::VRAM_VADDR && vaddr < Memory::VRAM_VADDR_END) {
        return Memory::VRAM_PADDR + (vaddr - Memory::VRAM_VADDR);
    }
    if (vaddr >= Memory::N3DS_EXTRA_RAM_VADDR && vaddr < Memory::N3DS_EXTRA_RAM_VADDR_END) {
        return Memory::N3DS_EXTRA_RAM_PADDR + (vaddr - Memory::N3DS_EXTRA_RAM_VADDR);
    }
    return std::nullopt;
}

void OnCpuFlush(Memory::MemorySystem& memory, PAddr addr, u32 size) {
    if (!Enabled()) {
        return;
    }

    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    ForEachPage(addr, size, [&](u32 page, u32 first, u32 last) {
        auto& state = GetPage(s, page);
        Snapshot(memory, state, page, first, last);
        for (u32 line = first; line < last; ++line) {
            state.clean.set(line);
        }
    });
}

void OnCpuInvalidate(PAddr addr, u32 size) {
    if (!Enabled()) {
        return;
    }
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    ForEachPage(addr, size, [&](u32 page, u32 first, u32 last) {
        const auto it = s.pages.find(page);
        if (it == s.pages.end()) {
            return;
        }
        for (u32 line = first; line < last; ++line) {
            it->second->device.reset(line);
        }
    });
}

void OnDeviceWrite(Memory::MemorySystem& memory, PAddr addr, u32 size) {
    if (!Enabled()) {
        return;
    }
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    ForEachPage(addr, size, [&](u32 page, u32 first, u32 last) {
        auto& state = GetPage(s, page);
        Snapshot(memory, state, page, first, last);
        for (u32 line = first; line < last; ++line) {
            state.clean.set(line);
            state.device.set(line);
        }
    });
}

void OnSystemWrite(Memory::MemorySystem& memory, PAddr addr, u32 size) {
    OnCpuFlush(memory, addr, size);
}

void CheckDeviceRead(Memory::MemorySystem& memory, PAddr addr, u32 size, const char* what) {
    if (!Enabled() || size == 0) {
        return;
    }
    u32 never_flushed = 0;
    u32 modified = 0;
    u32 total = 0;
    PAddr first_bad = 0;
    {
        auto& s = GetState();
        std::scoped_lock lock{s.mutex};
        ForEachPage(addr, size, [&](u32 page, u32 first, u32 last) {
            total += last - first;
            const auto it = s.pages.find(page);
            const u8* current = memory.GetPhysicalPointer(page * PageSize);
            for (u32 line = first; line < last; ++line) {
                bool bad = false;
                if (it == s.pages.end() || !it->second->clean.test(line)) {
                    ++never_flushed;
                    bad = true;
                } else if (!it->second->device.test(line) && current &&
                           std::memcmp(it->second->snapshot.data() + line * LineSize,
                                       current + line * LineSize, LineSize) != 0) {
                    ++modified;
                    bad = true;
                }
                if (bad && first_bad == 0) {
                    first_bad = page * PageSize + line * LineSize;
                }
            }
        });
    }
    if (never_flushed + modified == 0) {
        return;
    }
    ReportF(Category::CacheCpuToGpu, fmt::format("{}@{:08X}", what, addr),
            "{} reads 0x{:08X}-0x{:08X}: {} of {} cache lines were written by the CPU without a "
            "data cache flush before this read ({} never flushed, {} changed since the last "
            "flush; first at 0x{:08X}). On hardware the device can read stale data.",
            what, addr, addr + size, never_flushed + modified, total, never_flushed, modified,
            first_bad);
}

void CheckCpuRead(PAddr addr, u32 size) {
    if (!Enabled()) {
        return;
    }
    bool device_written = false;
    {
        auto& s = GetState();
        std::scoped_lock lock{s.mutex};
        ForEachPage(addr, size, [&](u32 page, u32 first, u32 last) {
            const auto it = s.pages.find(page);
            if (it == s.pages.end()) {
                return;
            }
            for (u32 line = first; line < last; ++line) {
                device_written |= it->second->device.test(line);
            }
        });
    }
    if (device_written) {
        ReportF(Category::CacheGpuToCpu, fmt::format("cpu-read@{:08X}", addr & ~0xFFFu),
                "CPU read of 0x{:08X} ({} bytes), written by the GPU/transfer engine, without "
                "GSPGPU_InvalidateDataCache: stale cache lines can shadow the GPU's data on "
                "hardware",
                addr, size);
    }
}

} // namespace Zeste::Cache
