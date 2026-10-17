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

/// Memory a draw reads (vertex attributes, indices, textures) and writes (color/depth buffers).
struct DrawRange {
    PAddr addr;
    u32 size;
    const char* what;
};
void CollectDrawRanges(const RegsInternal& regs, Memory::MemorySystem& memory, bool is_indexed,
                       std::vector<DrawRange>& reads, std::vector<DrawRange>& writes);

/// True when the physical address is in memory the GPU can reach (VRAM or FCRAM).
bool IsGpuReachable(PAddr addr, u32 size);

} // namespace Pica::Zeste
