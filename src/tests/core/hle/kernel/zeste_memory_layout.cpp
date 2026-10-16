// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <catch2/catch_test_macros.hpp>
#include "core/hle/kernel/memory.h"
#include "core/memory.h"

TEST_CASE("Zeste hardware layout puts the 178 MB linear heap above 0x28000000",
          "[kernel][zeste]") {
    // APPLICATION region of the New 3DS 178 MB mode (memory mode 7).
    Kernel::MemoryRegionInfo region;
    region.Reset(0, 0x0B200000);

    SECTION("Azahar policy: linear heap at the start of FCRAM") {
        region.zeste_hw_layout = false;
        REQUIRE(region.HeapAllocate(96 * 1024 * 1024).size() != 0);
        const auto linear = region.LinearAllocate(32 * 1024 * 1024);
        REQUIRE(linear.has_value());
        REQUIRE(Memory::FCRAM_PADDR + *linear < 0x28000000);
    }

    SECTION("Zeste policy: heap from the bottom, linear heap from the top") {
        region.zeste_hw_layout = true;
        const auto heap = region.HeapAllocate(96 * 1024 * 1024);
        REQUIRE(heap.size() != 0);
        REQUIRE(heap.begin()->lower() == 0);
        const auto linear = region.RLinearAllocate(32 * 1024 * 1024);
        REQUIRE(linear.has_value());
        REQUIRE(Memory::FCRAM_PADDR + *linear >= 0x28000000);
        REQUIRE(*linear + 32 * 1024 * 1024 == 0x0B200000);
    }
}
