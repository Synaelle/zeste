// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "core/zeste_heap.h"

namespace Zeste::Heap {

namespace {

constexpr u32 PrevInUse = 1;
constexpr u32 SizeMask = ~7u;
constexpr u32 MinChunk = 16;

std::string Bytes(u64 n) {
    if (n >= 1024 * 1024) {
        return fmt::format("{:.2f} MB ({} B)", n / 1048576.0, n);
    }
    if (n >= 1024) {
        return fmt::format("{:.1f} KB ({} B)", n / 1024.0, n);
    }
    return fmt::format("{} B", n);
}

} // Anonymous namespace

Summary Walk(Core::System& system) {
    Summary out;
    auto process = system.Kernel().GetCurrentProcess();
    if (!process) {
        out.error = "no running application";
        return out;
    }
    auto& memory = system.Memory();
    out.heap_start = Memory::HEAP_VADDR;
    for (VAddr a = Memory::HEAP_VADDR; a < Memory::HEAP_VADDR_END; a += Memory::CITRA_PAGE_SIZE) {
        if (!memory.IsValidVirtualAddress(*process, a)) {
            break;
        }
        out.heap_mapped += Memory::CITRA_PAGE_SIZE;
    }
    if (out.heap_mapped == 0) {
        out.error = "nothing is mapped at the heap address 0x08000000";
        return out;
    }
    const VAddr end = out.heap_start + out.heap_mapped;
    // malloc's first chunk is not always at the start of the heap mapping (libctru/newlib keep
    // the first pages for themselves: 32 KB in the test ROM). The arena starts at the first
    // 8-byte-aligned position from which at least 8 valid chunks chain, with PREV_INUSE set on
    // the first one (malloc marks the space before its first chunk as in use).
    const auto chain_length = [&](VAddr start, u32 max) {
        u32 count = 0;
        for (VAddr c = start; count < max && c + 8 <= end; ++count) {
            const u32 size = memory.Read32(c + 4) & SizeMask;
            if (size < MinChunk || c + size > end) {
                break;
            }
            c += size;
        }
        return count;
    };
    VAddr chunk = 0;
    const VAddr scan_end = std::min<VAddr>(end, out.heap_start + 0x200000);
    for (VAddr c = out.heap_start; c + 8 <= scan_end; c += 8) {
        const u32 word = memory.Read32(c + 4);
        if ((word & PrevInUse) != 0 && (word & SizeMask) >= MinChunk &&
            chain_length(c, 8) >= 8) {
            chunk = c;
            break;
        }
    }
    if (chunk == 0) {
        out.error = "no malloc chunk chain found in the first 2 MB of the heap (not a newlib "
                    "malloc heap, or malloc not used yet?)";
        return out;
    }
    out.arena_start = chunk;
    u32 last_size = 0;
    bool last_in_use = true;
    while (chunk + 8 <= end) {
        const u32 size = memory.Read32(chunk + 4) & SizeMask;
        if (size < MinChunk || chunk + size > end) {
            break; // past the top chunk (unused heap is zero)
        }
        const VAddr next = chunk + size;
        // A chunk is in use when the next chunk's PREV_INUSE bit says so. The top chunk is the
        // last one; it is free.
        const bool next_valid = next + 8 <= end && (memory.Read32(next + 4) & SizeMask) >= MinChunk;
        const bool in_use = next_valid && (memory.Read32(next + 4) & PrevInUse) != 0;
        if (in_use) {
            out.used += size;
            ++out.used_chunks;
        } else {
            out.free += size;
            ++out.free_chunks;
            out.largest_free = std::max(out.largest_free, size);
            static constexpr u32 Limits[7] = {64, 256, 1024, 4096, 16384, 65536, 1048576};
            u32 bucket = 7;
            for (u32 i = 0; i < 7; ++i) {
                if (size < Limits[i]) {
                    bucket = i;
                    break;
                }
            }
            ++out.free_histogram[bucket];
        }
        last_size = size;
        last_in_use = in_use;
        out.arena += size;
        chunk = next;
        if (!next_valid) {
            break;
        }
    }
    if (out.arena == 0) {
        out.error = "no malloc chunk found at the start of the heap (not a newlib malloc heap?)";
        return out;
    }
    out.top = last_in_use ? 0 : last_size;
    // Largest free chunk that is not the top chunk.
    out.largest_inner_free = 0;
    {
        VAddr c = out.arena_start;
        for (u32 walked = 0; walked + (out.top ? out.top : 0) < out.arena;) {
            const u32 size = memory.Read32(c + 4) & SizeMask;
            const VAddr n = c + size;
            if ((memory.Read32(n + 4) & PrevInUse) == 0) {
                out.largest_inner_free = std::max(out.largest_inner_free, size);
            }
            walked += size;
            c = n;
        }
    }
    out.valid = true;
    return out;
}

void WriteReport(Core::System& system, const std::string& path) {
    const Summary h = Walk(system);
    std::string text;
    if (!h.valid) {
        text = fmt::format("Zeste heap report: {}\n", h.error);
    } else {
        const u32 unclaimed = h.heap_mapped - h.arena - (h.arena_start - h.heap_start);
        text = fmt::format(
            "Zeste heap report (newlib malloc at 0x{:08X})\n\n"
            "heap mapped           {}{}\n"
            "taken by malloc       {} (mallinfo arena)\n"
            "  allocated           {} in {} chunks (uordblks)\n"
            "  free                {} in {} chunks (fordblks), top chunk {} (keepcost)\n"
            "not yet taken         {} (malloc can still take it through sbrk)\n\n"
            "largest free block inside the heap: {}\n"
            "largest single allocation possible: {} (top chunk + not yet taken)\n\n"
            "free chunks by size: <64 B {}, <256 B {}, <1 KB {}, <4 KB {}, <16 KB {}, <64 KB {}, "
            "<1 MB {}, larger {}\n\n"
            "Sizes include the 8-byte chunk headers and alignment.\n",

            h.arena_start, Bytes(h.heap_mapped), "", Bytes(h.arena), Bytes(h.used),
            h.used_chunks, Bytes(h.free), h.free_chunks, Bytes(h.top), Bytes(unclaimed),
            Bytes(h.largest_inner_free),
            Bytes(std::max<u64>(h.largest_inner_free, u64{h.top} + unclaimed)),
            h.free_histogram[0], h.free_histogram[1], h.free_histogram[2], h.free_histogram[3],
            h.free_histogram[4], h.free_histogram[5], h.free_histogram[6], h.free_histogram[7]);
    }
    FileUtil::WriteStringToFile(true, path, text);
    LOG_INFO(Zeste, "heap report: {} -> {}",
             h.valid ? fmt::format("arena {} used {} free {} largest free {} unmapped {}",
                                   h.arena, h.used, h.free, h.largest_free,
                                   h.heap_mapped - h.arena)
                     : h.error,
             path);
}

} // namespace Zeste::Heap
