// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste heap report: walks the game's newlib malloc heap in emulated memory (devkitARM newlib,
// dlmalloc-style chunks: 4-byte prev_size, 4-byte size with the PREV_INUSE bit, 8-byte alignment)
// starting at the application heap (0x08000000, where libctru's sbrk starts). Needs no symbols:
// it reads the same chunk headers malloc itself uses.

#pragma once

#include <string>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Zeste::Heap {

struct Summary {
    bool valid = false;
    std::string error;
    VAddr heap_start = 0;
    VAddr arena_start = 0; ///< First malloc chunk
    u32 heap_mapped = 0;  ///< Bytes mapped at the heap address (libctru's __ctru_heap_size)
    u32 arena = 0;        ///< Bytes malloc took from sbrk (mallinfo().arena)
    u32 used = 0;         ///< Bytes in allocated chunks, headers included (mallinfo().uordblks)
    u32 free = 0;         ///< Bytes in free chunks, the top chunk included (mallinfo().fordblks)
    u32 top = 0;          ///< The top chunk (mallinfo().keepcost)
    u32 largest_free = 0; ///< Largest free chunk, the top one included
    u32 largest_inner_free = 0; ///< Largest free chunk that is not the top chunk
    u32 used_chunks = 0;
    u32 free_chunks = 0;
    u32 free_histogram[8] = {}; ///< Free chunks by size: <64, <256, <1K, <4K, <16K, <64K, <1M, more
};

/// Walks the heap of the running application.
Summary Walk(Core::System& system);

/// Writes a readable report to `path` (and a one-line summary to the log).
void WriteReport(Core::System& system, const std::string& path);

} // namespace Zeste::Heap
