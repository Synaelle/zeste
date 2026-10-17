// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <boost/container/static_vector.hpp>
#include "common/settings.h"
#include "common/zeste_report.h"
#include "core/memory.h"
#include "video_core/pica/regs_internal.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/pica/zeste_pica.h"
#include "video_core/utils.h"

namespace Pica::Zeste {

using ::Zeste::Category;
using ::Zeste::ReportF;

namespace {

std::string Where(const DrawLocation& where) {
    const std::string tag = ::Zeste::TagText(where.tag);
    return fmt::format("cmdlist 0x{:08X}+0x{:X} draw #{}{}", where.cmdlist_addr,
                       where.cmdlist_offset, where.draw_index,
                       tag.empty() ? std::string{} : fmt::format(" tag \"{}\"", tag));
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

bool DrawBuffersReachable(const RegsInternal& regs, bool is_indexed, const DrawLocation& where) {
    const auto& pipeline = regs.pipeline;
    const auto& attribs = pipeline.vertex_attributes;
    const PAddr base = attribs.GetPhysicalBaseAddress();
    const auto unreachable = [&](PAddr addr, const char* what, u32 offset) {
        ReportF(Category::PicaState, Site(fmt::format("unreachable-{}", what), where),
                "{} at 0x{:08X} (base 0x{:08X} + 28-bit offset 0x{:07X}) is outside VRAM/FCRAM: "
                "the draw is skipped (on hardware the GPU hangs or reads garbage; a buffer more "
                "than 256 MB above the base wraps around) | {}",
                what, addr, base, offset, Where(where));
        return false;
    };
    if (is_indexed && !IsGpuReachable(base + pipeline.index_array.offset, 1)) {
        return unreachable(base + pipeline.index_array.offset, "index buffer",
                           pipeline.index_array.offset);
    }
    for (std::size_t i = 0; i < 12; ++i) {
        const auto& loader = attribs.attribute_loaders[i];
        if (loader.component_count == 0 || loader.byte_count == 0) {
            continue;
        }
        if (!IsGpuReachable(base + loader.data_offset, loader.byte_count)) {
            return unreachable(base + loader.data_offset, "vertex attribute buffer",
                               loader.data_offset);
        }
    }
    return true;
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

    // 24-bit float range: the emulator computed these in 32-bit floats. Only the shader's outputs
    // are checked, not its intermediate values.
    for (const auto* v : vs) {
        const std::array<float, 4> pos = {v->pos.x.ToFloat32(), v->pos.y.ToFloat32(),
                                          v->pos.z.ToFloat32(), v->pos.w.ToFloat32()};
        const auto too_big = [](float f) { return std::abs(f) > F24Max; };
        if (std::any_of(pos.begin(), pos.end(), too_big)) {
            ReportF(Category::PicaVertex, Site("f24-overflow", where),
                    "vertex position ({:.4g}, {:.4g}, {:.4g}, {:.4g}) exceeds the 24-bit float "
                    "range (+-{:.3g}): Inf on hardware, finite in the emulator | {}",
                    pos[0], pos[1], pos[2], pos[3], F24Max, Where(where));
            break;
        }
        if (pos[3] != 0.0f && std::abs(pos[3]) < F24MinNormal) {
            ReportF(Category::PicaVertex, Site("f24-underflow", where),
                    "vertex w = {:.4g} is below the smallest 24-bit float ({:.3g}): 0 on hardware "
                    "| {}",
                    pos[3], F24MinNormal, Where(where));
            break;
        }
        const std::array<float, 8> attr = {v->color.r().ToFloat32(), v->color.g().ToFloat32(),
                                           v->color.b().ToFloat32(), v->color.a().ToFloat32(),
                                           v->tc0.u().ToFloat32(),   v->tc0.v().ToFloat32(),
                                           v->tc1.u().ToFloat32(),   v->tc1.v().ToFloat32()};
        if (std::any_of(attr.begin(), attr.end(), too_big)) {
            ReportF(Category::PicaVertex, Site("f24-overflow-attr", where),
                    "vertex color or texcoord exceeds the 24-bit float range (+-{:.3g}): Inf on "
                    "hardware, finite in the emulator | {}",
                    F24Max, Where(where));
            break;
        }
    }

    // Hardware calibration (tools/zeste/calib, ZESTE_GUIDE.md section 13): primitives far outside
    // the screen draw exactly like float clipping, up to the magnitudes measured. Only vertices
    // beyond those are reported, as unverified. Vertices outside the depth range are clipped
    // away before rasterization and do not count.
    const auto& r = regs.rasterizer;
    const float half_x = f24::FromRaw(r.viewport_size_x).ToFloat32();
    const float half_y = f24::FromRaw(r.viewport_size_y).ToFloat32();
    float worst = 0.0f;
    for (const auto* v : vs) {
        const float w = v->pos.w.ToFloat32();
        const float z = v->pos.z.ToFloat32();
        if (!(w > 0.0f) || z > 0.0f || z < -w) {
            continue;
        }
        const float sx = std::abs((v->pos.x.ToFloat32() / w + 1.0f) * half_x);
        const float sy = std::abs((v->pos.y.ToFloat32() / w + 1.0f) * half_y);
        worst = std::max({worst, sx, sy});
    }
    if (worst > MeasuredRange) {
        ReportF(Category::PicaVertex, Site("range", where),
                "vertex reaches {:.3g} px on screen, beyond what was measured on hardware "
                "(+-{:.0g} px drew correctly): not verified | {}",
                worst, MeasuredRange, Where(where));
    }
}

TriangleDump DescribeTriangle(const RegsInternal& regs, const OutputVertex& v0,
                              const OutputVertex& v1, const OutputVertex& v2) {
    using Polygon = boost::container::static_vector<ClipVertex, 9>;
    const std::array<const OutputVertex*, 3> vs = {&v0, &v1, &v2};
    TriangleDump out;
    Polygon poly;
    for (const auto* v : vs) {
        poly.push_back({v->pos.x.ToFloat32(), v->pos.y.ToFloat32(), v->pos.z.ToFloat32(),
                        v->pos.w.ToFloat32()});
        out.text += fmt::format(" ({:.6g}, {:.6g}, {:.6g}, {:.6g}) uv ({:.4g}, {:.4g})",
                                poly.back().x, poly.back().y, poly.back().z, poly.back().w,
                                v->tc0.x.ToFloat32(), v->tc0.y.ToFloat32());
    }
    // Planes as distance functions: w >= epsilon, z <= 0 (near), z >= -w (far).
    const std::array<float (*)(const ClipVertex&), 3> planes = {
        [](const ClipVertex& c) { return c.w - NearEpsilon; },
        [](const ClipVertex& c) { return -c.z; },
        [](const ClipVertex& c) { return c.z + c.w; },
    };
    for (const auto plane : planes) {
        Polygon in = poly;
        poly.clear();
        for (std::size_t i = 0; i < in.size(); ++i) {
            const ClipVertex& a = in[i];
            const ClipVertex& b = in[(i + 1) % in.size()];
            const float da = plane(a), db = plane(b);
            if (da >= 0) {
                poly.push_back(a);
            } else {
                out.cut = true;
            }
            if ((da >= 0) != (db >= 0) && poly.size() < poly.capacity()) {
                const float t = da / (da - db);
                poly.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                                a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
            }
        }
        if (poly.size() < 3) {
            out.outside = true;
            out.text += " | clipped away";
            return out;
        }
    }
    const auto& r = regs.rasterizer;
    const float half_x = f24::FromRaw(r.viewport_size_x).ToFloat32();
    const float half_y = f24::FromRaw(r.viewport_size_y).ToFloat32();
    const float off_x = static_cast<float>(r.viewport_corner.x);
    const float off_y = static_cast<float>(r.viewport_corner.y);
    out.min_x = out.min_y = std::numeric_limits<float>::max();
    out.max_x = out.max_y = std::numeric_limits<float>::lowest();
    out.text += out.cut ? fmt::format(" | cut, {} vertices on screen:", poly.size()) : " | screen:";
    for (const auto& c : poly) {
        const float sx = (c.x / c.w + 1.0f) * half_x + off_x;
        const float sy = (c.y / c.w + 1.0f) * half_y + off_y;
        out.min_x = std::min(out.min_x, sx);
        out.min_y = std::min(out.min_y, sy);
        out.max_x = std::max(out.max_x, sx);
        out.max_y = std::max(out.max_y, sy);
        out.text += fmt::format(" ({:.6g}, {:.6g})", sx, sy);
    }
    return out;
}

} // namespace Pica::Zeste
