// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <array>
#include <cmath>
#include <boost/container/static_vector.hpp>
#include "common/settings.h"
#include "common/zeste_report.h"
#include "core/memory.h"
#include "video_core/pica/regs_internal.h"
#include "video_core/pica/zeste_pica.h"

namespace Pica::Zeste {

using ::Zeste::Category;
using ::Zeste::ReportF;

namespace {

std::string Where(const DrawLocation& where) {
    return fmt::format("cmdlist 0x{:08X}+0x{:X} draw #{}", where.cmdlist_addr,
                       where.cmdlist_offset, where.draw_index);
}

/// Dedup key per draw site: the same draw in later frames is counted, not logged again.
std::string Site(std::string_view what, const DrawLocation& where) {
    return fmt::format("{}@{:08X}+{:X}", what, where.cmdlist_addr, where.cmdlist_offset);
}

bool IsPow2(u32 v) {
    return v != 0 && (v & (v - 1)) == 0;
}

u32 TextureBitsPerPixel(TexturingRegs::TextureFormat format) {
    using F = TexturingRegs::TextureFormat;
    switch (format) {
    case F::RGBA8:
        return 32;
    case F::RGB8:
        return 24;
    case F::RGB5A1:
    case F::RGB565:
    case F::RGBA4:
    case F::IA8:
    case F::RG8:
        return 16;
    case F::I8:
    case F::A8:
    case F::IA4:
    case F::ETC1A4:
        return 8;
    case F::I4:
    case F::A4:
    case F::ETC1:
        return 4;
    default:
        return 32;
    }
}

u32 ColorBytesPerPixel(FramebufferRegs::ColorFormat format) {
    using F = FramebufferRegs::ColorFormat;
    switch (format) {
    case F::RGBA8:
        return 4;
    case F::RGB8:
        return 3;
    default:
        return 2;
    }
}

bool IsFinite(const f24& v) {
    return std::isfinite(v.ToFloat32());
}

template <typename Vec>
bool AllFinite(const Vec& vec, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (!IsFinite(vec[i])) {
            return false;
        }
    }
    return true;
}

struct ClipVertex {
    float x, y, z, w;
};

constexpr float NearEpsilon = 0.00001f;

/// Clips the triangle against the w = epsilon plane only, as the PICA200 does.
boost::container::static_vector<ClipVertex, 4> ClipNear(const std::array<ClipVertex, 3>& in) {
    boost::container::static_vector<ClipVertex, 4> out;
    for (std::size_t i = 0; i < 3; ++i) {
        const ClipVertex& a = in[i];
        const ClipVertex& b = in[(i + 1) % 3];
        const bool a_in = a.w >= NearEpsilon;
        const bool b_in = b.w >= NearEpsilon;
        if (a_in) {
            out.push_back(a);
        }
        if (a_in != b_in) {
            const float t = (NearEpsilon - a.w) / (b.w - a.w);
            out.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                           NearEpsilon});
        }
    }
    return out;
}

struct ScreenBounds {
    float min_x, min_y, max_x, max_y;
    bool near_clipped;
    bool empty;
};

ScreenBounds ProjectTriangle(const RegsInternal& regs, const OutputVertex& v0,
                             const OutputVertex& v1, const OutputVertex& v2) {
    const std::array<ClipVertex, 3> in = {{
        {v0.pos.x.ToFloat32(), v0.pos.y.ToFloat32(), v0.pos.z.ToFloat32(), v0.pos.w.ToFloat32()},
        {v1.pos.x.ToFloat32(), v1.pos.y.ToFloat32(), v1.pos.z.ToFloat32(), v1.pos.w.ToFloat32()},
        {v2.pos.x.ToFloat32(), v2.pos.y.ToFloat32(), v2.pos.z.ToFloat32(), v2.pos.w.ToFloat32()},
    }};
    ScreenBounds bounds{1e30f, 1e30f, -1e30f, -1e30f, false, true};
    bounds.near_clipped = std::any_of(in.begin(), in.end(),
                                      [](const ClipVertex& v) { return v.w < NearEpsilon; });
    const auto clipped = ClipNear(in);
    if (clipped.size() < 3) {
        return bounds;
    }
    const auto& r = regs.rasterizer;
    const float half_x = f24::FromRaw(r.viewport_size_x).ToFloat32();
    const float half_y = f24::FromRaw(r.viewport_size_y).ToFloat32();
    const float off_x = static_cast<float>(r.viewport_corner.x);
    const float off_y = static_cast<float>(r.viewport_corner.y);
    for (const auto& v : clipped) {
        const float sx = (v.x / v.w + 1.0f) * half_x + off_x;
        const float sy = (v.y / v.w + 1.0f) * half_y + off_y;
        bounds.min_x = std::min(bounds.min_x, sx);
        bounds.min_y = std::min(bounds.min_y, sy);
        bounds.max_x = std::max(bounds.max_x, sx);
        bounds.max_y = std::max(bounds.max_y, sy);
    }
    bounds.empty = false;
    return bounds;
}

bool OutsideGuardBand(const ScreenBounds& b) {
    return !(b.min_x >= -GuardBand && b.max_x < GuardBand && b.min_y >= -GuardBand &&
             b.max_y < GuardBand) ||
           !std::isfinite(b.min_x) || !std::isfinite(b.max_x) || !std::isfinite(b.min_y) ||
           !std::isfinite(b.max_y);
}

/// Byte offset of pixel (x, y) in a PICA framebuffer (8x8 tiles, Morton order inside a tile;
/// the tile's base is enough for a range estimate).
u64 TiledRowOffset(u32 y, u32 width, u32 bytes_per_pixel) {
    return static_cast<u64>(y / 8) * (width / 8) * 64 * bytes_per_pixel;
}

} // Anonymous namespace

bool IsGpuReachable(PAddr addr, u32 size) {
    const u64 end = static_cast<u64>(addr) + size;
    if (addr >= Memory::VRAM_PADDR && end <= Memory::VRAM_PADDR_END) {
        return true;
    }
    const u64 fcram_end = Settings::values.is_new_3ds.GetValue() ? Memory::FCRAM_N3DS_PADDR_END
                                                                 : Memory::FCRAM_PADDR_END;
    return addr >= Memory::FCRAM_PADDR && end <= fcram_end;
}

void CheckDrawState(const RegsInternal& regs, Memory::MemorySystem& memory, bool is_indexed,
                    const DrawLocation& where) {
    // --- Framebuffer ---
    const auto& fb = regs.framebuffer.framebuffer;
    const u32 fb_w = fb.GetWidth();
    const u32 fb_h = fb.GetHeight();
    const u32 color_bpp = ColorBytesPerPixel(fb.color_format.Value());
    const PAddr color_addr = fb.GetColorBufferPhysicalAddress();
    const u32 color_size = fb_w * fb_h * color_bpp;
    if (fb.allow_color_write != 0 && !IsGpuReachable(color_addr, color_size)) {
        ReportF(Category::PicaState, Site("fb-color", where),
                "color buffer 0x{:08X} ({}x{}, {} bytes) is outside VRAM/FCRAM | {}", color_addr,
                fb_w, fb_h, color_size, Where(where));
    }
    if (fb.allow_depth_stencil_write != 0) {
        const PAddr depth_addr = fb.GetDepthBufferPhysicalAddress();
        const u32 depth_size =
            fb_w * fb_h * FramebufferRegs::BytesPerDepthPixel(fb.depth_format.Value());
        if (!IsGpuReachable(depth_addr, depth_size)) {
            ReportF(Category::PicaState, Site("fb-depth", where),
                    "depth buffer 0x{:08X} ({}x{}) is outside VRAM/FCRAM | {}", depth_addr, fb_w,
                    fb_h, Where(where));
        }
    }
    if (fb_w == 0 || fb_w > 1024 || fb_h > 1024 || (fb_w % 8) != 0 || (fb_h % 8) != 0) {
        ReportF(Category::PicaState, Site("fb-size", where),
                "framebuffer size {}x{} is invalid (must be a multiple of 8, at most 1024) | {}",
                fb_w, fb_h, Where(where));
    }

    // Viewport larger than the framebuffer: the hardware does not clip to the framebuffer, the
    // pixels outside it land in the memory after (or before) the buffer.
    {
        const auto& r = regs.rasterizer;
        const float vp_w = f24::FromRaw(r.viewport_size_x).ToFloat32() * 2.0f;
        const float vp_h = f24::FromRaw(r.viewport_size_y).ToFloat32() * 2.0f;
        const s32 vx = r.viewport_corner.x;
        const s32 vy = r.viewport_corner.y;
        const bool scissor_include = r.scissor_test.mode == RasterizerRegs::ScissorMode::Include &&
                                     r.scissor_test.x2 < fb_w && r.scissor_test.y2 < fb_h;
        if (!scissor_include && fb_w != 0 &&
            (vx < 0 || vy < 0 || vx + vp_w > fb_w + 0.5f || vy + vp_h > fb_h + 0.5f)) {
            const u32 rows = static_cast<u32>(std::max(0.0f, vy + vp_h));
            const u64 end = color_addr + TiledRowOffset(rows + 7, fb_w, color_bpp);
            ReportF(Category::PicaBounds, Site("viewport", where),
                    "viewport ({},{}) {:.0f}x{:.0f} exceeds the {}x{} framebuffer at 0x{:08X}; "
                    "estimated out-of-bounds color range 0x{:08X}-0x{:08X} | {}",
                    vx, vy, vp_w, vp_h, fb_w, fb_h, color_addr, color_addr + color_size,
                    static_cast<u32>(std::max<u64>(end, color_addr + color_size)), Where(where));
        }
    }

    // --- Textures ---
    const auto textures = regs.texturing.GetTextures();
    for (std::size_t unit = 0; unit < textures.size(); ++unit) {
        const auto& tex = textures[unit];
        if (!tex.enabled) {
            continue;
        }
        const auto& cfg = tex.config;
        if (unit == 0 && cfg.type == TexturingRegs::TextureConfig::Disabled) {
            continue;
        }
        const u32 w = cfg.width;
        const u32 h = cfg.height;
        const PAddr addr = cfg.GetPhysicalAddress();
        const std::string unit_key = fmt::format("tex{}", unit);
        if (addr == 0) {
            ReportF(Category::PicaState, Site(unit_key + "-null", where),
                    "texture unit {} is enabled with address 0 (C3D_TexBind with a NULL or freed "
                    "texture?) | {}",
                    unit, Where(where));
        }
        if (w < 8 || h < 8 || w > 1024 || h > 1024 || !IsPow2(w) || !IsPow2(h)) {
            ReportF(Category::PicaState, Site(unit_key + "-size", where),
                    "texture unit {} is {}x{}: sizes must be powers of two between 8 and 1024 | "
                    "{}",
                    unit, w, h, Where(where));
        }
        const u32 max_level = cfg.lod.max_level;
        if (max_level > 0 && (std::min(w, h) >> max_level) < 8) {
            ReportF(Category::PicaState, Site(unit_key + "-mip", where),
                    "texture unit {} ({}x{}) uses mip level {} of {}x{}: levels below 8 texels "
                    "are not supported | {}",
                    unit, w, h, max_level, std::max(1u, w >> max_level),
                    std::max(1u, h >> max_level), Where(where));
        }
        u64 size = static_cast<u64>(w) * h * TextureBitsPerPixel(tex.format) / 8;
        for (u32 level = 1; level <= max_level; ++level) {
            size += static_cast<u64>(std::max(1u, w >> level)) * std::max(1u, h >> level) *
                    TextureBitsPerPixel(tex.format) / 8;
        }
        if (addr != 0 && !IsGpuReachable(addr, static_cast<u32>(size))) {
            ReportF(Category::PicaState, Site(unit_key + "-addr", where),
                    "texture unit {} data 0x{:08X}-0x{:08X} is outside VRAM/FCRAM | {}", unit,
                    addr, static_cast<u32>(addr + size), Where(where));
        }
    }

    // --- Vertex and index buffers ---
    const auto& pipeline = regs.pipeline;
    const auto& attribs = pipeline.vertex_attributes;
    const PAddr base = attribs.GetPhysicalBaseAddress();
    u32 max_vertex = pipeline.vertex_offset + std::max(1u, static_cast<u32>(pipeline.num_vertices));
    if (is_indexed) {
        const bool u16_index = pipeline.index_array.format != 0;
        const PAddr index_addr = base + pipeline.index_array.offset;
        const u32 index_size = pipeline.num_vertices * (u16_index ? 2 : 1);
        if (!IsGpuReachable(index_addr, index_size)) {
            ReportF(Category::PicaState, Site("index", where),
                    "index buffer 0x{:08X} (base 0x{:08X} + offset 0x{:07X}, {} bytes) is outside "
                    "VRAM/FCRAM: offsets are 28-bit and relative to the attribute base | {}",
                    index_addr, base, static_cast<u32>(pipeline.index_array.offset), index_size,
                    Where(where));
        } else if (const u8* indices = memory.GetPhysicalPointer(index_addr)) {
            max_vertex = 0;
            for (u32 i = 0; i < pipeline.num_vertices; ++i) {
                u32 index;
                if (u16_index) {
                    u16 v;
                    std::memcpy(&v, indices + i * 2, sizeof(v));
                    index = v;
                } else {
                    index = indices[i];
                }
                max_vertex = std::max(max_vertex, index + 1);
            }
        }
    }
    for (std::size_t i = 0; i < 12; ++i) {
        const auto& loader = attribs.attribute_loaders[i];
        if (loader.component_count == 0 || loader.byte_count == 0) {
            continue;
        }
        const PAddr addr = base + loader.data_offset;
        const u32 size = loader.byte_count * max_vertex;
        if (!IsGpuReachable(addr, size)) {
            ReportF(Category::PicaState, Site(fmt::format("attr{}", i), where),
                    "attribute loader {} reads 0x{:08X}-0x{:08X} (base 0x{:08X} + offset 0x{:07X}) "
                    "outside VRAM/FCRAM: offsets are 28-bit, so buffers more than 256 MB above the "
                    "base cannot be reached | {}",
                    i, addr, addr + size, base, static_cast<u32>(loader.data_offset),
                    Where(where));
        }
    }
}

void CollectDrawRanges(const RegsInternal& regs, Memory::MemorySystem& memory, bool is_indexed,
                       std::vector<DrawRange>& reads, std::vector<DrawRange>& writes) {
    const auto& fb = regs.framebuffer.framebuffer;
    const u32 pixels = fb.GetWidth() * fb.GetHeight();
    if (fb.allow_color_write != 0) {
        writes.push_back({fb.GetColorBufferPhysicalAddress(),
                          pixels * ColorBytesPerPixel(fb.color_format.Value()), "color buffer"});
    }
    if (fb.allow_depth_stencil_write != 0) {
        writes.push_back({fb.GetDepthBufferPhysicalAddress(),
                          pixels * FramebufferRegs::BytesPerDepthPixel(fb.depth_format.Value()),
                          "depth buffer"});
    }

    const auto& pipeline = regs.pipeline;
    const auto& attribs = pipeline.vertex_attributes;
    const PAddr base = attribs.GetPhysicalBaseAddress();
    u32 first_vertex = pipeline.vertex_offset;
    u32 end_vertex = pipeline.vertex_offset + pipeline.num_vertices;
    if (is_indexed) {
        const bool u16_index = pipeline.index_array.format != 0;
        const PAddr index_addr = base + pipeline.index_array.offset;
        const u32 index_size = pipeline.num_vertices * (u16_index ? 2 : 1);
        reads.push_back({index_addr, index_size, "GPU index buffer"});
        first_vertex = 0xFFFFFFFF;
        end_vertex = 0;
        if (const u8* indices = memory.GetPhysicalPointer(index_addr);
            indices && IsGpuReachable(index_addr, index_size)) {
            for (u32 i = 0; i < pipeline.num_vertices; ++i) {
                u32 index;
                if (u16_index) {
                    u16 v;
                    std::memcpy(&v, indices + i * 2, sizeof(v));
                    index = v;
                } else {
                    index = indices[i];
                }
                first_vertex = std::min(first_vertex, index);
                end_vertex = std::max(end_vertex, index + 1);
            }
        }
    }
    if (end_vertex > first_vertex) {
        for (std::size_t i = 0; i < 12; ++i) {
            const auto& loader = attribs.attribute_loaders[i];
            if (loader.component_count == 0 || loader.byte_count == 0) {
                continue;
            }
            reads.push_back({base + loader.data_offset + loader.byte_count * first_vertex,
                             loader.byte_count * (end_vertex - first_vertex),
                             "GPU vertex buffer"});
        }
    }

    const auto textures = regs.texturing.GetTextures();
    for (std::size_t unit = 0; unit < textures.size(); ++unit) {
        const auto& tex = textures[unit];
        if (!tex.enabled || tex.config.GetPhysicalAddress() == 0 ||
            (unit == 0 && tex.config.type == TexturingRegs::TextureConfig::Disabled)) {
            continue;
        }
        const u32 w = tex.config.width;
        const u32 h = tex.config.height;
        u64 size = 0;
        for (u32 level = 0; level <= tex.config.lod.max_level; ++level) {
            size += static_cast<u64>(std::max(1u, w >> level)) * std::max(1u, h >> level) *
                    TextureBitsPerPixel(tex.format) / 8;
        }
        reads.push_back({tex.config.GetPhysicalAddress(),
                         static_cast<u32>(std::min<u64>(size, 0x400000)), "GPU texture"});
    }
}

void CheckTriangle(const RegsInternal& regs, const OutputVertex& v0, const OutputVertex& v1,
                   const OutputVertex& v2, const DrawLocation& where) {
    const std::array<const OutputVertex*, 3> vs = {&v0, &v1, &v2};
    for (const auto* v : vs) {
        if (!AllFinite(v->pos, 4)) {
            ReportF(Category::PicaVertex, Site("nan-pos", where),
                    "vertex position is NaN/Inf ({}, {}, {}, {}) | {}", v->pos.x.ToFloat32(),
                    v->pos.y.ToFloat32(), v->pos.z.ToFloat32(), v->pos.w.ToFloat32(),
                    Where(where));
            return;
        }
        if (!AllFinite(v->color, 4) || !AllFinite(v->tc0, 2) || !AllFinite(v->tc1, 2) ||
            !AllFinite(v->tc2, 2) || !IsFinite(v->tc0_w)) {
            ReportF(Category::PicaVertex, Site("nan-attr", where),
                    "vertex attribute (color or texcoord) is NaN/Inf | {}", Where(where));
        }
    }

    const ScreenBounds b = ProjectTriangle(regs, v0, v1, v2);
    if (b.empty) {
        return;
    }
    const bool outside = OutsideGuardBand(b);
    if (outside) {
        ReportF(Category::PicaVertex, Site("range", where),
                "{}primitive reaches screen ({:.0f},{:.0f})-({:.0f},{:.0f}), outside the "
                "fixed-point rasterizer range (+-{:.0f} px): the PICA200 drops it or draws it "
                "wrong | {}",
                b.near_clipped ? "near-plane clipped " : "", b.min_x, b.min_y, b.max_x, b.max_y,
                GuardBand, Where(where));

        const auto& fb = regs.framebuffer.framebuffer;
        const u32 fb_w = fb.GetWidth();
        const u32 fb_h = fb.GetHeight();
        if (fb_w != 0) {
            // The coordinates wrap in the 12.4 fixed-point format: estimate which rows of memory
            // after the framebuffer a wrapped primitive could touch.
            const u32 bpp = ColorBytesPerPixel(fb.color_format.Value());
            const PAddr color = fb.GetColorBufferPhysicalAddress();
            const u32 fb_size = fb_w * fb_h * bpp;
            const u64 worst = TiledRowOffset(4095, fb_w, bpp);
            ReportF(Category::PicaBounds, Site("oob", where),
                    "out-of-range primitive on the {}x{} framebuffer at 0x{:08X}: wrapped "
                    "coordinates can write 0x{:08X}-0x{:08X} (past the buffer end) | {}",
                    fb_w, fb_h, color, color + fb_size, static_cast<u32>(color + worst),
                    Where(where));
        }
    } else if (b.near_clipped &&
               (b.max_x - b.min_x > HugeExtent || b.max_y - b.min_y > HugeExtent)) {
        ReportF(Category::PicaVertex, Site("near-huge", where),
                "near-plane clipped primitive spans {:.0f}x{:.0f} px on screen (precision loss "
                "in the fixed-point rasterizer) | {}",
                b.max_x - b.min_x, b.max_y - b.min_y, Where(where));
    }
}

bool FixedPointRasterAccepts(const RegsInternal& regs, const OutputVertex& v0,
                             const OutputVertex& v1, const OutputVertex& v2) {
    if (!AllFinite(v0.pos, 4) || !AllFinite(v1.pos, 4) || !AllFinite(v2.pos, 4)) {
        return false;
    }
    const ScreenBounds b = ProjectTriangle(regs, v0, v1, v2);
    return b.empty || !OutsideGuardBand(b);
}

} // namespace Pica::Zeste
