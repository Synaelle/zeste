// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste strict PICA mode: per-draw checks of what the real PICA200 mishandles, and the
// near-plane-only clipping model used by the software renderer's "hardware-like clipping".

#pragma once

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

/// Assumed range of the PICA200 rasterizer's fixed-point screen coordinates, in pixels.
/// The software rasterizer models them as 12.4 fixed point; with only near-plane clipping, the
/// usable range around the viewport is modelled as [-GuardBand, GuardBand). This is an
/// assumption (see docs/ZESTE_CHANGES.md), not a measured hardware constant.
constexpr float GuardBand = 4096.0f;

/// Near-plane clipped primitives whose screen bounding box is larger than this (in pixels) are
/// reported even when they stay inside the guard band.
constexpr float HugeExtent = 2048.0f;

/// Where the draw comes from, for the report.
struct DrawLocation {
    u32 cmdlist_addr = 0;  ///< Physical address of the command list
    u32 cmdlist_offset = 0; ///< Byte offset of the draw command in that list
    u32 draw_index = 0;     ///< Draw number within the current frame
};

/// Checks the register state at a draw (textures, framebuffer, buffers, addresses).
void CheckDrawState(const RegsInternal& regs, Memory::MemorySystem& memory, bool is_indexed,
                    const DrawLocation& where);

/// Checks one post-vertex-shader triangle (NaN/Inf, fixed-point range, near-plane extents,
/// framebuffer bounds).
void CheckTriangle(const RegsInternal& regs, const OutputVertex& v0, const OutputVertex& v1,
                   const OutputVertex& v2, const DrawLocation& where);

/// Hardware-like clipping for the software renderer: returns false when the triangle must be
/// dropped because, after clipping against the near plane only, a vertex leaves the fixed-point
/// range (the hardware draws it wrong or not at all; Zeste drops it and reports).
bool FixedPointRasterAccepts(const RegsInternal& regs, const OutputVertex& v0,
                             const OutputVertex& v1, const OutputVertex& v2);

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
