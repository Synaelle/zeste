// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste strict PICA mode: per-draw checks of what the real PICA200 mishandles.

#pragma once

#include <array>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "video_core/pica/output_vertex.h"

namespace Memory {
class MemorySystem;
}

namespace Pica {
struct RegsInternal;
struct ShaderSetup;
}

namespace Pica::Zeste {

/// Screen coordinates (pixels) up to which out-of-range primitives were measured on a New 3DS
/// (tools/zeste/calib): unclipped triangles up to +-1e6 px, near-plane clipped ones up to 1e9 px
/// and triangles with a vertex on the camera plane (w = 0, +-1e-7) drew exactly like float
/// clipping (no wrap, no drop, no GPU hang, no write outside the buffers). Vertices beyond this
/// are reported as unverified.
constexpr float MeasuredRange = 1e9f;

/// Range of the PICA's 24-bit floats (1 sign, 7 exponent bits with bias 63, 16 mantissa bits; the
/// top exponent is Inf/NaN, denormals are flushed). Azahar computes shaders in 32-bit floats, so a
/// value outside this range stays finite (or non-zero) in the emulator but is Inf (or 0) on
/// hardware.
constexpr float F24Max = 1.8446462e19f;        ///< 2^63 * (2 - 2^-16)
constexpr float F24MinNormal = 2.1684043e-19f; ///< 2^-62

/// Draw tags: an application marks its GPU work by writing the tag id to this register with an
/// empty byte mask (the write changes nothing on hardware), and registers the text with
/// svcOutputDebugString("ZESTE:tag <id> <text>"). 0x130 is GPUREG_FRAGOP_SHADOW.
constexpr u32 TagRegister = 0x130;

/// Where the draw comes from, for the report.
struct DrawLocation {
    u32 cmdlist_addr = 0;  ///< Physical address of the command list
    u32 cmdlist_offset = 0; ///< Byte offset of the draw command in that list
    u32 draw_index = 0;     ///< Draw number within the current frame
    u32 tag = 0;            ///< Draw tag id (0 = none)
};

/// Checks the register state at a draw (textures, framebuffer, buffers, addresses).
void CheckDrawState(const RegsInternal& regs, Memory::MemorySystem& memory, bool is_indexed,
                    const DrawLocation& where);

/// Returns false (and reports) when a vertex attribute or index buffer starts outside the memory
/// the GPU can reach. The draw must then be skipped: on hardware the GPU hangs or reads garbage,
/// and the host renderers would read through a null pointer.
bool DrawBuffersReachable(const RegsInternal& regs, bool is_indexed, const DrawLocation& where);

/// Checks one post-vertex-shader triangle (NaN/Inf, coordinates beyond the measured range).
void CheckTriangle(const RegsInternal& regs, const OutputVertex& v0, const OutputVertex& v1,
                   const OutputVertex& v2, const DrawLocation& where);

/// One triangle for the command-list dump: the vertex shader's output positions (clip space) and
/// texture coordinates, and the polygon left after clipping against w > 0 and the depth range
/// (near plane z = 0, far plane z = -w), in screen pixels. x/y are not clipped: the hardware
/// draws primitives far off screen correctly (MeasuredRange), so their reach is shown as is.
struct TriangleDump {
    std::string text;
    bool cut = false;      ///< A clip plane cut the triangle
    bool outside = false;  ///< Nothing left after clipping
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0; ///< Screen bounds after clipping
};
TriangleDump DescribeTriangle(const RegsInternal& regs, const OutputVertex& v0,
                              const OutputVertex& v1, const OutputVertex& v2);

/// Screen pixels a triangle covers after clipping (w > 0 and the depth range), face culling and
/// the viewport rectangle: the rasterizer's work for it.
float CoveredPixels(const RegsInternal& regs, const OutputVertex& v0, const OutputVertex& v1,
                    const OutputVertex& v2);

/// Draw-mode switch check. On hardware the command processor applies GPUREG_PRIMITIVE_CONFIG and
/// GPUREG_GEOSTAGE_CONFIG/CONFIG2 as soon as it reaches them, while the previous draws' vertices
/// may still be in the pipeline. An array draw right behind indexed geometry-mode draws that
/// covered the screen hundreds of times left the vertex stream one output out of step for the
/// rest of the run (OpenMW port, hardware tests #26-#33; fixed by one primitive mode per frame).
/// The emulator runs every draw to completion first, so the switch is only reported.
struct DrawMode {
    u8 kind = 0;       ///< 0 array draw, 1 indexed draw, 2 immediate-mode vertices
    u8 topology = 0;   ///< GPUREG_PRIMITIVE_CONFIG bits 8-9 (3 = geometry-primitive mode)
    u32 geostage = 0;  ///< GPUREG_GEOSTAGE_CONFIG (0x229), bits 0-1 and 8
    u32 geostage2 = 0; ///< GPUREG_GEOSTAGE_CONFIG2 (0x253), bits 0 and 8
    bool operator==(const DrawMode&) const = default;
};
DrawMode CurrentDrawMode(const RegsInternal& regs, u8 kind);
std::string DescribeDrawMode(const DrawMode& mode);

/// The draws of one mode since the last switch in the current command list.
struct ModeRun {
    u32 draws = 0;
    double pixels = 0;          ///< Covered pixels, all draws
    double heaviest_pixels = 0; ///< Covered pixels of the heaviest draw
    u32 heaviest_index = 0;     ///< Its draw number in the frame
    u32 heaviest_tag = 0;       ///< Its draw tag
};

/// Previous draws covering at least this many screens make a switch "heavy". The hardware broke
/// behind hundreds of screens; switches behind ordinary scene draws (around one screen) ran
/// through whole test runs without trouble. The threshold in between is not measured.
constexpr double HeavyModeRunScreens = 10.0;

void ReportDrawModeSwitch(const RegsInternal& regs, const DrawMode& previous,
                          const DrawMode& current, const ModeRun& run,
                          const DrawLocation& where);

/// Memory a draw reads (vertex attributes, indices, textures) and writes (color/depth buffers).
struct DrawRange {
    PAddr addr;
    u32 size;
    const char* what;
};
void CollectDrawRanges(const RegsInternal& regs, Memory::MemorySystem& memory, bool is_indexed,
                       std::vector<DrawRange>& reads, std::vector<DrawRange>& writes);

/// PICA cost model (zeste_pica_timing), in GPU cycles at 268 MHz. Calibrated on New 3DS
/// measurements (openmw3ds PORTING_NOTES): ~0.8 ms per 1000 triangles with a 260-instruction
/// vertex shader, ~1000 draws or ~200k short-shader triangles per 33 ms frame.
constexpr double CyclesPerDraw = 1500.0;
constexpr double CyclesPerShaderInstructionPerVertex = 0.8;
constexpr double CyclesPerTriangle = 30.0;
constexpr double CyclesPerFragment = 1.0;
constexpr double GpuClockHz = 268111856.0;

/// Number of vertex shader instructions from the entry point to the first END.
u32 VertexShaderLength(const RegsInternal& regs, const ShaderSetup& vs);
/// Cost of a draw call before rasterization (command overhead and vertex shading).
double DrawCostCycles(const RegsInternal& regs, const ShaderSetup& vs, u32 num_vertices);
/// Cost of one triangle: setup plus one cycle per covered pixel (near-plane clipped, bounded by
/// the framebuffer).
double TriangleCostCycles(const RegsInternal& regs, const OutputVertex& v0, const OutputVertex& v1,
                          const OutputVertex& v2);

/// True when the physical address is in memory the GPU can reach (VRAM or FCRAM).
bool IsGpuReachable(PAddr addr, u32 size);

} // namespace Pica::Zeste
