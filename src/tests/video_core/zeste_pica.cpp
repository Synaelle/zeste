// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
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
}

TEST_CASE("Zeste strict PICA: far off-screen primitives within the measured range are clean",
          "[video_core][zeste]") {
    // Measured on a New 3DS (tools/zeste/calib): these draw exactly like float clipping.
    ::Zeste::Reset();
    const auto regs = MakeRegs();
    // Unclipped, one vertex 1,000,000 px off screen.
    Pica::Zeste::CheckTriangle(regs, MakeVertex(-0.5f, -0.5f, -0.5f, 1.0f),
                               MakeVertex(8333.0f, -0.5f, -0.5f, 1.0f),
                               MakeVertex(-0.5f, 0.5f, -0.5f, 1.0f), {});
    // Near-plane clipped: one vertex just past the near plane at ~1e8 px, one behind the camera.
    Pica::Zeste::CheckTriangle(regs, MakeVertex(83333.0f, 0.0f, -0.0001f, 0.1001f),
                               MakeVertex(0.0f, 0.0f, 1.2f, -1.0f),
                               MakeVertex(0.0f, 0.5f, -1.0f, 2.0f), {0x20000000, 0x40, 1});
    REQUIRE(::Zeste::Count(::Zeste::Category::PicaVertex) == 0);
    REQUIRE(::Zeste::Count(::Zeste::Category::PicaBounds) == 0);
}

TEST_CASE("Zeste strict PICA: vertex beyond the measured range is reported",
          "[video_core][zeste]") {
    ::Zeste::Reset();
    const auto regs = MakeRegs();
    Pica::Zeste::CheckTriangle(regs, MakeVertex(1.0e7f, 0.0f, -0.0001f, 0.001f),
                               MakeVertex(0.0f, 0.0f, 1.2f, -1.0f),
                               MakeVertex(0.0f, 0.5f, -1.0f, 2.0f), {0x20000000, 0x40, 1});
    REQUIRE(::Zeste::Count(::Zeste::Category::PicaVertex) == 1);
    REQUIRE(::Zeste::Count(::Zeste::Category::PicaBounds) == 0);
    ::Zeste::Reset();
}

TEST_CASE("Zeste strict PICA: values outside the 24-bit float range", "[video_core][zeste]") {
    const auto regs = MakeRegs();
    // Finite in 32-bit floats, Inf on the PICA.
    ::Zeste::Reset();
    Pica::Zeste::CheckTriangle(regs, MakeVertex(-0.5f, -0.5f, -0.5f, 1.0f),
                               MakeVertex(0.5f, -0.5f, -0.5f, 1.0f),
                               MakeVertex(0.0f, 3.0e19f, -0.5f, 1.0f), {});
    REQUIRE(::Zeste::Count(::Zeste::Category::PicaVertex) >= 1);
    // w flushed to 0 on the PICA.
    ::Zeste::Reset();
    Pica::Zeste::CheckTriangle(regs, MakeVertex(-0.5f, -0.5f, -0.5f, 1.0f),
                               MakeVertex(0.5f, -0.5f, -0.5f, 1.0f),
                               MakeVertex(0.0f, 0.0f, 0.0f, 1.0e-20f), {});
    REQUIRE(::Zeste::Count(::Zeste::Category::PicaVertex) >= 1);
    // Large but in range: clean.
    ::Zeste::Reset();
    Pica::Zeste::CheckTriangle(regs, MakeVertex(-0.5f, -0.5f, -0.5f, 1.0f),
                               MakeVertex(0.5f, -0.5f, -0.5f, 1.0f),
                               MakeVertex(0.0f, 0.5f, -0.5f, 1.0e18f), {});
    REQUIRE(::Zeste::Count(::Zeste::Category::PicaVertex) == 0);
    ::Zeste::Reset();
}

TEST_CASE("Zeste strict PICA: covered pixels", "[video_core][zeste]") {
    const auto regs = MakeRegs();
    // Half of the 240x400 viewport.
    const float half = Pica::Zeste::CoveredPixels(regs, MakeVertex(-1.0f, -1.0f, -0.5f, 1.0f),
                                                  MakeVertex(1.0f, -1.0f, -0.5f, 1.0f),
                                                  MakeVertex(-1.0f, 1.0f, -0.5f, 1.0f));
    REQUIRE(std::abs(half - 48000.0f) < 1.0f);
    // A triangle far larger than the screen covers it once.
    const float full = Pica::Zeste::CoveredPixels(regs, MakeVertex(-100.0f, -100.0f, -0.5f, 1.0f),
                                                  MakeVertex(300.0f, -100.0f, -0.5f, 1.0f),
                                                  MakeVertex(-100.0f, 300.0f, -0.5f, 1.0f));
    REQUIRE(std::abs(full - 96000.0f) < 1.0f);
    // Entirely behind the camera.
    REQUIRE(Pica::Zeste::CoveredPixels(regs, MakeVertex(0.0f, 0.0f, 0.5f, -1.0f),
                                       MakeVertex(1.0f, 0.0f, 0.5f, -1.0f),
                                       MakeVertex(0.0f, 1.0f, 0.5f, -1.0f)) == 0.0f);
}

TEST_CASE("Zeste strict PICA: draw-mode switch reports", "[video_core][zeste]") {
    ::Zeste::Reset();
    auto regs = MakeRegs();
    const auto indexed = Pica::Zeste::CurrentDrawMode(regs, 1);
    const auto arrays = Pica::Zeste::CurrentDrawMode(regs, 0);
    REQUIRE_FALSE(indexed == arrays);
    Pica::Zeste::ModeRun run;
    run.draws = 1;
    run.pixels = 96000.0 * 300; // 300 screens
    run.heaviest_pixels = run.pixels;
    Pica::Zeste::ReportDrawModeSwitch(regs, indexed, arrays, run, {0x20000000, 0x40, 2});
    REQUIRE(::Zeste::Count(::Zeste::Category::PicaState) == 1);
    const auto issues = ::Zeste::GetIssues();
    REQUIRE(std::any_of(issues.begin(), issues.end(), [](const auto& issue) {
        return issue.message.find("covered the screen 300.0 times") != std::string::npos &&
               issue.message.find("still be in the pipeline") != std::string::npos;
    }));
    ::Zeste::Reset();
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
