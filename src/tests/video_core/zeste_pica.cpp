// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <cmath>
#include <catch2/catch_test_macros.hpp>
#include "common/zeste_report.h"
#include "video_core/pica/regs_internal.h"
#include "video_core/pica/zeste_pica.h"

namespace {

Pica::OutputVertex MakeVertex(float x, float y, float z, float w) {
    Pica::OutputVertex v{};
    v.pos = Common::MakeVec(Pica::f24::FromFloat32(x), Pica::f24::FromFloat32(y), Pica::f24::FromFloat32(z),
                            Pica::f24::FromFloat32(w));
    v.color = Common::MakeVec(Pica::f24::One(), Pica::f24::One(), Pica::f24::One(), Pica::f24::One());
    return v;
}

Pica::RegsInternal MakeRegs() {
    Pica::RegsInternal regs{};
    // 400x240 viewport (half sizes as f24), 240x400 framebuffer.
    regs.rasterizer.viewport_size_x.Assign(0x45E000); // f24 120.0 (same raw value as citro3d)
    regs.rasterizer.viewport_size_y.Assign(0x469000); // f24 200.0
    regs.framebuffer.framebuffer.width.Assign(240);
    regs.framebuffer.framebuffer.height.Assign(399);
    return regs;
}

} // Anonymous namespace

TEST_CASE("Zeste strict PICA: on-screen triangle is clean", "[video_core][zeste]") {
    ::Zeste::Reset();
    const auto regs = MakeRegs();
    Pica::Zeste::CheckTriangle(regs, MakeVertex(-0.5f, -0.5f, -0.5f, 1.0f),
                         MakeVertex(0.5f, -0.5f, -0.5f, 1.0f), MakeVertex(0.0f, 0.5f, -0.5f, 1.0f),
                         {});
    REQUIRE(::Zeste::Count(::Zeste::Category::PicaVertex) == 0);
    REQUIRE(::Zeste::Count(::Zeste::Category::PicaBounds) == 0);
    REQUIRE(Pica::Zeste::FixedPointRasterAccepts(regs, MakeVertex(-0.5f, -0.5f, -0.5f, 1.0f),
                                           MakeVertex(0.5f, -0.5f, -0.5f, 1.0f),
                                           MakeVertex(0.0f, 0.5f, -0.5f, 1.0f)));
}

TEST_CASE("Zeste strict PICA: near-plane triangle leaves the fixed-point range",
          "[video_core][zeste]") {
    ::Zeste::Reset();
    const auto regs = MakeRegs();
    // One vertex barely in front of the camera, two far behind it.
    const auto v0 = MakeVertex(0.0f, 0.0f, -0.01f, 0.001f);
    const auto v1 = MakeVertex(-50.0f, -1.0f, 10.0f, -10.0f);
    const auto v2 = MakeVertex(50.0f, -1.0f, 10.0f, -10.0f);
    Pica::Zeste::CheckTriangle(regs, v0, v1, v2, {0x20000000, 0x40, 1});
    REQUIRE(::Zeste::Count(::Zeste::Category::PicaVertex) == 1);
    REQUIRE(::Zeste::Count(::Zeste::Category::PicaBounds) == 1);
    REQUIRE_FALSE(Pica::Zeste::FixedPointRasterAccepts(regs, v0, v1, v2));
}

TEST_CASE("Zeste strict PICA: NaN position", "[video_core][zeste]") {
    ::Zeste::Reset();
    const auto regs = MakeRegs();
    Pica::Zeste::CheckTriangle(regs, MakeVertex(NAN, 0.0f, -0.5f, 1.0f),
                         MakeVertex(0.5f, -0.5f, -0.5f, 1.0f), MakeVertex(0.0f, 0.5f, -0.5f, 1.0f),
                         {});
    REQUIRE(::Zeste::Count(::Zeste::Category::PicaVertex) == 1);
    ::Zeste::Reset();
}

TEST_CASE("Zeste strict PICA: GPU reachable memory", "[video_core][zeste]") {
    REQUIRE(Pica::Zeste::IsGpuReachable(0x18000000, 0x1000));
    REQUIRE(Pica::Zeste::IsGpuReachable(0x20000000, 0x1000));
    REQUIRE_FALSE(Pica::Zeste::IsGpuReachable(0x0000000C, 4));
    REQUIRE_FALSE(Pica::Zeste::IsGpuReachable(0x185FF000, 0x2000)); // crosses the end of VRAM
}

TEST_CASE("Zeste wrap model: signed 13.4 coordinates", "[video_core][zeste]") {
    REQUIRE(Pica::Zeste::WrapCoordinate(100.0f) == 100.0f);
    REQUIRE(Pica::Zeste::WrapCoordinate(-100.0f) == -100.0f);
    REQUIRE(Pica::Zeste::WrapCoordinate(4096.0f) == -4096.0f);
    REQUIRE(Pica::Zeste::WrapCoordinate(5000.0f) == 5000.0f - 8192.0f);
    REQUIRE(Pica::Zeste::WrapCoordinate(-5000.0f) == -5000.0f + 8192.0f);
}

TEST_CASE("Zeste pixel addressing matches the PICA layout", "[video_core][zeste]") {
    // 240x400 RGBA8 buffer: the row stored first is the bottom one (y = height - 1).
    REQUIRE(Pica::Zeste::PixelByteOffset(0, 399, 240, 399, 4) == 0);
    // One tile row higher on screen = one 8-pixel band lower in memory.
    REQUIRE(Pica::Zeste::PixelByteOffset(0, 391, 240, 399, 4) == 8 * 240 * 4);
    // Above the buffer top (y < 0 on screen in flipped space) lands after the buffer.
    REQUIRE(Pica::Zeste::PixelByteOffset(0, -8, 240, 399, 4) >= 240 * 400 * 4);
    // Below the bottom row lands before the buffer.
    REQUIRE(Pica::Zeste::PixelByteOffset(0, 407, 240, 399, 4) < 0);
}
