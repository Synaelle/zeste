// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// Zeste strict PICA mode: per-draw checks of what the real PICA200 mishandles, and the
// near-plane-only clipping model used by the software renderer's "hardware-like clipping".

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

/// Assumed range of the PICA200 rasterizer's fixed-point screen coordinates, in pixels.
/// The software rasterizer models them as 12.4 fixed point; with only near-plane clipping, the
/// usable range around the viewport is modelled as [-GuardBand, GuardBand). This is an
/// assumption (see docs/ZESTE_CHANGES.md), not a measured hardware constant.
constexpr float GuardBand = 4096.0f;

/// Near-plane clipped primitives whose screen bounding box is larger than this (in pixels) are
/// reported even when they stay inside the guard band.
constexpr float HugeExtent = 2048.0f;

/// Draw tags: an application marks its GPU work by writing the tag id to this register with an
/// empty byte mask (the write changes nothing on hardware), and registers the text with
/// svcOutputDebugString("ZESTE:tag <id> <text>"). 0x130 is GPUREG_FRAGOP_SHADOW.
constexpr u32 TagRegister = 0x130;

/// Wrap model for out-of-range primitives (see ZESTE_GUIDE.md, "Out-of-range primitives"):
/// screen coordinates are signed 13.4 fixed point, so a coordinate outside
/// [-GuardBand, GuardBand) wraps around by 2 * GuardBand.
float WrapCoordinate(float pixels);

/// Byte offset of pixel (x, y), relative to the start of a color buffer, in the PICA layout
/// (8x8 Morton tiles, rows stored bottom-up), for any position, including positions outside the
/// buffer (negative = before the buffer). `height_field` is the raw register value (height - 1).
s64 PixelByteOffset(s32 x, s32 y, u32 width, u32 height_field, u32 bytes_per_pixel);

/// A primitive after near-plane clipping, in screen pixels, before and after the wrap.
struct ScreenPolygon {
    u32 count = 0;
    std::array<std::array<float, 2>, 4> points{};
    std::array<std::array<float, 2>, 4> wrapped{};
};
/// Near-plane-only clipping and viewport transform of a triangle (false when nothing is left).
bool ProjectNearClipped(const RegsInternal& regs, const OutputVertex& v0, const OutputVertex& v1,
                        const OutputVertex& v2, ScreenPolygon& out);

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
