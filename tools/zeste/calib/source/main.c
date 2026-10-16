// Zeste PICA wrap calibration ROM - run on a real New 3DS.
//
// Draws primitives whose screen coordinates leave the PICA's fixed-point range, one case at a
// time, and records what the hardware did, so Zeste's strict_pica wrap model can be calibrated:
//   - what the LCDs showed (sdmc:/zeste-calib/<case>_top.bmp and _bottom.bmp, read from the LCD
//     framebuffer registers like Rosalina);
//   - whether memory outside the render target changed: canary patterns are written to VRAM
//     before the colour buffer, into the depth buffer, after the buffers and into a second render
//     target, and compared after each case;
//   - whether the GPU still completes work: every frame waits for the GPU with a 2 s timeout.
//     A case that breaks the GPU is reported on screen and in the log, and the run stops.
//
// Progress is saved before each case (sdmc:/zeste-calib/progress.txt). If a case freezes the
// console, restart it and run again: the case that did not finish is recorded as "froze" and the
// run continues with the next one.
//
// Files: log.txt (readable), results.csv (one line per case), <case>_top.bmp / _bottom.bmp.

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "vshader_shbin.h"

#define DIR "sdmc:/zeste-calib"
#define FRAMES_PER_CASE 180   // ~3 s at 60 fps
#define GPU_TIMEOUT_MS 2000
#define MAX_VERTICES 16384

#define DISPLAY_TRANSFER_FLAGS                                                                     \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |               \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// Keep heap sizes explicit (the .cia runs in the 178 MB mode).
u32 __ctru_heap_size = 64 * 1024 * 1024;
u32 __ctru_linear_heap_size = 24 * 1024 * 1024;
extern u32 __ctru_linear_heap;

// ----------------------------------------------------------------------------------------------
// Logging (every write is flushed and closed so nothing is lost if the console freezes)

static void append(const char* file, const char* fmt, ...) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", DIR, file);
    FILE* f = fopen(path, "a");
    if (!f)
        return;
    va_list va;
    va_start(va, fmt);
    vfprintf(f, fmt, va);
    va_end(va);
    fclose(f);
}

static void say(const char* fmt, ...) {
    char buf[256];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    printf("%s\n", buf);
    append("log.txt", "%s\n", buf);
}

// ----------------------------------------------------------------------------------------------
// Cases

typedef enum { K_TRI, K_NEAR, K_SPHERE, K_QUADS, K_FIRE } Kind;

typedef struct {
    Kind kind;
    double magnitude; // K_TRI / K_NEAR: screen coordinate reached by the far vertex, in pixels
    int sign;         // +1 / -1
    float scale;      // K_SPHERE / K_FIRE: model scale
    bool scissor, additive, depth, textured;
    char name[64];
} Case;

static Case cases[256];
static int case_count;

static void add_case(Kind kind, double magnitude, int sign, float scale, bool scissor,
                     bool additive, bool depth, bool textured) {
    Case* c = &cases[case_count++];
    c->kind = kind;
    c->magnitude = magnitude;
    c->sign = sign;
    c->scale = scale;
    c->scissor = scissor;
    c->additive = additive;
    c->depth = depth;
    c->textured = textured;
    const char* kind_name[] = {"tri", "near", "sphere", "quads", "fire"};
    char base[24];
    if (kind == K_TRI || kind == K_NEAR)
        snprintf(base, sizeof(base), "%s%c%.0f", kind_name[kind], sign > 0 ? 'p' : 'm', magnitude);
    else if (kind == K_SPHERE || kind == K_FIRE)
        snprintf(base, sizeof(base), "%s%.0f", kind_name[kind], scale);
    else
        snprintf(base, sizeof(base), "%s", kind_name[kind]);
    snprintf(c->name, sizeof(c->name), "%03d_%s%s%s%s%s", case_count - 1, base,
             scissor ? "_sc" : "", additive ? "_add" : "", depth ? "_dep" : "",
             textured ? "_tex" : "");
}

static void build_cases(bool quick) {
    case_count = 0;
    const double tri_mag[] = {4096, 8192, 16384, 65536, 1000000};
    const double near_mag[] = {4096, 65536, 1000000, 1000000000.0};
    for (int k = 0; k < 2; k++) {
        const Kind kind = k == 0 ? K_TRI : K_NEAR;
        const double* mags = k == 0 ? tri_mag : near_mag;
        const int count = k == 0 ? 5 : 4;
        for (int m = 0; m < count; m++) {
            // Plain and scissor, both signs.
            for (int sign = 1; sign >= -1; sign -= 2) {
                add_case(kind, mags[m], sign, 1, false, false, false, false);
                add_case(kind, mags[m], sign, 1, true, false, false, false);
            }
            if (quick)
                continue;
            // The other combinations, positive sign.
            for (int combo = 0; combo < 16; combo++) {
                const bool sc = combo & 1, add = combo & 2, dep = combo & 4, tex = combo & 8;
                if (!add && !dep && !tex)
                    continue; // plain / scissor-only done above
                add_case(kind, mags[m], 1, 1, sc, add, dep, tex);
            }
        }
    }
    const float scales[] = {1500, 44000};
    for (int s = 0; s < 2; s++) {
        add_case(K_SPHERE, 0, 1, scales[s], false, false, false, false);
        add_case(K_SPHERE, 0, 1, scales[s], false, true, false, false);
        if (!quick) {
            add_case(K_SPHERE, 0, 1, scales[s], false, true, true, false);
            add_case(K_SPHERE, 0, 1, scales[s], true, true, false, true);
        }
    }
    add_case(K_QUADS, 0, 1, 1, false, false, false, false);
    add_case(K_QUADS, 0, 1, 1, false, true, false, false);
    if (!quick) {
        add_case(K_QUADS, 0, 1, 1, false, true, true, false);
        add_case(K_QUADS, 0, 1, 1, true, true, false, true);
    }
    add_case(K_FIRE, 0, 1, 1500, false, true, false, false);
    add_case(K_FIRE, 0, 1, 44000, false, true, true, false);
    if (!quick)
        add_case(K_FIRE, 0, 1, 44000, false, true, true, true);
}

// ----------------------------------------------------------------------------------------------
// GPU objects

typedef struct {
    float pos[4];
    float col[4];
    float tc[2];
} Vertex;

static DVLB_s* vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection, uLoc_modelview;
static C3D_RenderTarget* target;  // the render target the cases draw into (top screen)
static C3D_RenderTarget* target2; // a second render target, used only as a canary
// One VRAM block, laid out by hand so the canaries are really next to the buffers:
// [canary pre][colour][canary mid][depth][canary post]
#define CANARY_SIZE (256 * 1024)
#define BUFFER_SIZE (240 * 400 * 4)
static u8* block;
static C3D_Tex texture;
static Vertex* vbo;
static int vbo_count;

typedef struct {
    const char* name;
    u32* addr;
    u32 size;
    u32 pattern;
} Region;

static Region regions[8];
static int region_count;

static u32 phys(const void* p) {
    return osConvertVirtToPhys(p);
}

static void gpu_init(void) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    // The case target uses a hand-laid block in VRAM bank A; the second target (a canary too)
    // goes to bank B. The physical addresses are logged.
    block = vramMemAlignAt(3 * CANARY_SIZE + 2 * BUFFER_SIZE, 0x1000, VRAM_ALLOC_A);
    u8* const color = block + CANARY_SIZE;
    u8* const depth = color + BUFFER_SIZE + CANARY_SIZE;
    target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_FrameBufColor(&target->frameBuf, color, GPU_RB_RGBA8);
    C3D_FrameBufDepth(&target->frameBuf, depth, GPU_RB_DEPTH24_STENCIL8);
    target2 = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    region_count = 0;
    regions[region_count++] = (Region){"pre", (u32*)block, CANARY_SIZE, 0x11C0FFEE};
    regions[region_count++] = (Region){"mid", (u32*)(color + BUFFER_SIZE), CANARY_SIZE,
                                       0x22BADF00};
    regions[region_count++] = (Region){"depth", (u32*)depth, BUFFER_SIZE, 0x00ABCDEF};
    regions[region_count++] = (Region){"post", (u32*)(depth + BUFFER_SIZE), CANARY_SIZE,
                                       0x55FACADE};
    regions[region_count++] = (Region){"rt2color", (u32*)target2->frameBuf.colorBuf, BUFFER_SIZE,
                                       0x33CAFE33};
    regions[region_count++] = (Region){"rt2depth", (u32*)target2->frameBuf.depthBuf, BUFFER_SIZE,
                                       0x0044DEAD};

    vshader_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);
    C3D_BindProgram(&program);
    uLoc_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
    uLoc_modelview = shaderInstanceGetUniformLocation(program.vertexShader, "modelview");

    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 4);
    AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2);

    // 8x8 checker texture (the tile order does not matter for a calibration pattern).
    C3D_TexInit(&texture, 8, 8, GPU_RGBA8);
    u32* texels = (u32*)texture.data;
    for (int i = 0; i < 64; i++)
        texels[i] = (i ^ (i >> 3)) & 1 ? 0xFFFFFFFF : 0x404040FF;
    C3D_TexFlush(&texture);
    C3D_TexSetFilter(&texture, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&texture, GPU_REPEAT, GPU_REPEAT);

    vbo = linearAlloc(sizeof(Vertex) * MAX_VERTICES);

    if (!block) {
        say("could not allocate the VRAM block");
        return;
    }
    say("VRAM layout (physical): canary pre 0x%08lX, colour 0x%08lX, canary mid 0x%08lX, depth "
        "0x%08lX, canary post 0x%08lX-0x%08lX, rt2 colour 0x%08lX, rt2 depth 0x%08lX",
        phys(block), phys(color), phys(color + BUFFER_SIZE), phys(depth),
        phys(depth + BUFFER_SIZE), phys(depth + BUFFER_SIZE + CANARY_SIZE),
        phys(target2->frameBuf.colorBuf), phys(target2->frameBuf.depthBuf));
}

// ----------------------------------------------------------------------------------------------
// Geometry

static void vtx(float x, float y, float z, float w, float r, float g, float b, float a, float u,
                float v) {
    if (vbo_count >= MAX_VERTICES)
        return;
    Vertex* p = &vbo[vbo_count++];
    p->pos[0] = x, p->pos[1] = y, p->pos[2] = z, p->pos[3] = w;
    p->col[0] = r, p->col[1] = g, p->col[2] = b, p->col[3] = a;
    p->tc[0] = u, p->tc[1] = v;
}

// Clip-space x/y for a screen position in pixels (render target 240 x 400).
static double clip_x(double px) {
    return px / 120.0 - 1.0;
}
static double clip_y(double py) {
    return py / 200.0 - 1.0;
}

static double clip_x_to_px(double x) {
    return (x + 1.0) * 120.0;
}
static double clip_y_to_px(double y) {
    return (y + 1.0) * 200.0;
}

static C3D_Mtx projection, modelview;

// View-space x/y of the point at `depth` in front of the camera that `projection` puts at screen
// pixel (px, py). Clip x/y are linear in view x/y, so solve the 2x2 system.
static void view_for_screen(double px, double py, float depth, float out[2]) {
    const C3D_FVec o = Mtx_MultiplyFVec4(&projection, FVec4_New(0, 0, -depth, 1));
    const C3D_FVec ex = Mtx_MultiplyFVec4(&projection, FVec4_New(1, 0, -depth, 1));
    const C3D_FVec ey = Mtx_MultiplyFVec4(&projection, FVec4_New(0, 1, -depth, 1));
    const double w = o.w; // w does not depend on view x/y in a perspective projection
    const double tx = clip_x(px) * w - o.x, ty = clip_y(py) * w - o.y;
    const double a = ex.x - o.x, b = ey.x - o.x, c = ex.y - o.y, d = ey.y - o.y;
    const double det = a * d - b * c;
    out[0] = (float)((tx * d - b * ty) / det);
    out[1] = (float)((a * ty - c * tx) / det);
}

static void sphere(float radius, float cx, float cy, float cz, float r, float g, float b, float a) {
    const int lat = 12, lon = 24;
    for (int i = 0; i < lat; i++) {
        const float t0 = M_PI * i / lat, t1 = M_PI * (i + 1) / lat;
        for (int j = 0; j < lon; j++) {
            const float p0 = 2 * M_PI * j / lon, p1 = 2 * M_PI * (j + 1) / lon;
            float q[4][3] = {
                {sinf(t0) * cosf(p0), cosf(t0), sinf(t0) * sinf(p0)},
                {sinf(t1) * cosf(p0), cosf(t1), sinf(t1) * sinf(p0)},
                {sinf(t1) * cosf(p1), cosf(t1), sinf(t1) * sinf(p1)},
                {sinf(t0) * cosf(p1), cosf(t0), sinf(t0) * sinf(p1)},
            };
            const int order[6] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 6; k++) {
                const float* v = q[order[k]];
                vtx(cx + radius * v[0], cy + radius * v[1], cz + radius * v[2], 1, r, g, b, a,
                    (float)j / 4, (float)i / 4);
            }
        }
    }
}

static void quads_around_camera(int count) {
    srand(1234);
    for (int i = 0; i < count; i++) {
        // Positions within 0.6 units of the camera: many straddle the near plane (0.1).
        const float x = ((rand() % 1000) / 1000.0f - 0.5f) * 1.2f;
        const float y = ((rand() % 1000) / 1000.0f - 0.5f) * 1.2f;
        const float z = ((rand() % 1000) / 1000.0f - 0.5f) * 1.2f;
        const float s = 0.15f;
        const float c = 0.3f + (i % 7) * 0.1f;
        vtx(x - s, y - s, z, 1, c, 0.4f, 0.1f, 1, 0, 0);
        vtx(x + s, y - s, z + s, 1, c, 0.4f, 0.1f, 1, 1, 0);
        vtx(x + s, y + s, z, 1, c, 0.4f, 0.1f, 1, 1, 1);
        vtx(x - s, y - s, z, 1, c, 0.4f, 0.1f, 1, 0, 0);
        vtx(x + s, y + s, z, 1, c, 0.4f, 0.1f, 1, 1, 1);
        vtx(x - s, y + s, z - s, 1, c, 0.4f, 0.1f, 1, 0, 1);
    }
}

// Builds the vertex buffer and matrices of a case; returns a description for the log.
static void build_geometry(const Case* c, char* desc, size_t desc_size) {
    vbo_count = 0;
    Mtx_Identity(&projection);
    Mtx_Identity(&modelview);
    const double m = c->magnitude * c->sign;
    switch (c->kind) {
    case K_TRI: {
        // Unclipped (w = 1): one vertex at screen x = 60 + m, one at y = 100 + m.
        const double x0 = clip_x(60), y0 = clip_y(100);
        const double x1 = clip_x(60 + m), y2 = clip_y(100 + m);
        const float tr = (float)(c->magnitude / 8);
        vtx(x0, y0, -0.5f, 1, 1, 0.2f, 0.2f, 1, 0, 0);
        vtx(x1, y0, -0.5f, 1, 0.2f, 1, 0.2f, 1, tr, 0);
        vtx(x0, y2, -0.5f, 1, 0.2f, 0.2f, 1, 1, 0, tr);
        snprintf(desc, desc_size,
                 "unclipped triangle, screen (60,100) (%.0f,100) (60,%.0f), clip x1=%g y2=%g",
                 60 + m, 100 + m, x1, y2);
        break;
    }
    case K_NEAR: {
        // A perspective camera (near plane 0.1) and a triangle in view space:
        //  A just in front of the near plane (depth 0.1001), placed so it lands at screen x 60 + m;
        //  B behind the camera; C in front, on screen at (60, 300).
        // The GPU clips the A-B and C-B edges at the near plane; A keeps its huge coordinate.
        Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(60), C3D_AspectRatioTop, 0.1f, 1000.0f,
                      false);
        const double ax = 60 + m, ay = 100;
        float a[2], cc[2];
        view_for_screen(ax, ay, 0.1001f, a);
        view_for_screen(60, 300, 2.0f, cc);
        vtx(a[0], a[1], -0.1001f, 1, 1, 0.6f, 0.1f, 1, 0, 0);
        vtx(0.0f, 0.0f, 1.0f, 1, 1, 0.6f, 0.1f, 1, 4, 0);
        vtx(cc[0], cc[1], -2.0f, 1, 1, 0.6f, 0.1f, 1, 0, 4);
        const C3D_FVec clip_a = Mtx_MultiplyFVec4(&projection, FVec4_New(a[0], a[1], -0.1001f, 1));
        snprintf(desc, desc_size,
                 "near-plane clipped triangle: A at depth 0.1001 (near 0.1) -> screen (%.0f,%.0f) "
                 "clip w=%g, B behind the camera, C at screen (60,300)",
                 clip_x_to_px(clip_a.x / clip_a.w), clip_y_to_px(clip_a.y / clip_a.w), clip_a.w);
        break;
    }
    case K_SPHERE:
    case K_FIRE: {
        // A tiny model (radius 0.01) scaled up around the camera: like the explosion meshes.
        Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(60), C3D_AspectRatioTop, 0.1f, 100000.0f,
                      false);
        Mtx_Scale(&modelview, c->scale, c->scale, c->scale);
        sphere(0.01f, 0.002f, 0.001f, 0.0f, 0.9f, 0.4f, 0.1f, 0.5f);
        if (c->kind == K_FIRE) {
            // Hit particles around the camera, in view space (not scaled).
            const int first = vbo_count;
            quads_around_camera(200);
            for (int i = first; i < vbo_count; i++)
                for (int k = 0; k < 3; k++)
                    vbo[i].pos[k] /= c->scale; // undo the model scale for the particles
        }
        snprintf(desc, desc_size,
                 "camera-enclosing double-sided sphere, radius 0.01 x %.0f%s, %d vertices",
                 c->scale, c->kind == K_FIRE ? " + 200 particles around the camera" : "",
                 vbo_count);
        break;
    }
    case K_QUADS:
        Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(60), C3D_AspectRatioTop, 0.1f, 100000.0f,
                      false);
        quads_around_camera(300);
        snprintf(desc, desc_size, "300 small quads within 0.6 units of the camera (near 0.1)");
        break;
    }
    GSPGPU_FlushDataCache(vbo, sizeof(Vertex) * vbo_count);
}

static void apply_state(const Case* c) {
    C3D_CullFace(GPU_CULL_NONE);
    if (c->scissor)
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 40, 80, 200, 320);
    else
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    if (c->additive)
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
    else
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    if (c->depth)
        C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
    else
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (c->textured) {
        C3D_TexBind(0, &texture);
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    } else {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    }
}

// ----------------------------------------------------------------------------------------------
// GPU health and canaries

// Waits until the GPU finished the previous frame, at most GPU_TIMEOUT_MS. Returns false on a
// timeout (the GPU no longer completes work). On success a frame is open.
static bool begin_frame_with_timeout(double* waited_ms) {
    const u64 start = svcGetSystemTick();
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        const double ms = (svcGetSystemTick() - start) / (SYSCLOCK_ARM11 / 1000.0);
        if (ms > GPU_TIMEOUT_MS)
            return false;
        svcSleepThread(1000000);
    }
    if (waited_ms)
        *waited_ms = (svcGetSystemTick() - start) / (SYSCLOCK_ARM11 / 1000.0);
    return true;
}

static void fill_canaries(void) {
    for (int i = 0; i < region_count; i++) {
        Region* r = &regions[i];
        GX_MemoryFill(r->addr, r->pattern, (u32*)((u8*)r->addr + r->size),
                      GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH, NULL, 0, NULL, 0);
        gspWaitForPSC0();
    }
}

// Returns the number of changed 32-bit words over all regions and writes details to `out`.
static u32 check_canaries(const Case* c, char* out, size_t out_size) {
    u32 total = 0;
    out[0] = 0;
    for (int i = 0; i < region_count; i++) {
        Region* r = &regions[i];
        // The target's own depth buffer is written legitimately when the case uses depth.
        const bool expected = c->depth && r->addr == (u32*)target->frameBuf.depthBuf;
        GSPGPU_InvalidateDataCache(r->addr, r->size);
        u32 changed = 0, first = 0xFFFFFFFF, last = 0;
        const u32 words = r->size / 4;
        for (u32 w = 0; w < words; w++) {
            if (r->addr[w] != r->pattern) {
                if (first == 0xFFFFFFFF)
                    first = w * 4;
                last = w * 4;
                changed++;
            }
        }
        if (!expected)
            total += changed;
        const size_t len = strlen(out);
        if (changed)
            snprintf(out + len, out_size - len, "%s%s:%lu words changed (0x%08lX-0x%08lX)%s",
                     len ? "; " : "", r->name, changed, phys(r->addr) + first,
                     phys(r->addr) + last + 4, expected ? " (expected: depth writes on)" : "");
        else
            snprintf(out + len, out_size - len, "%s%s:intact", len ? "; " : "", r->name);
    }
    return total;
}

// ----------------------------------------------------------------------------------------------
// LCD screenshots, read from the LCD framebuffer registers

static void* phys_to_virt(u32 p) {
    if (p >= 0x18000000 && p < 0x18600000)
        return (void*)(0x1F000000 + (p - 0x18000000));
    const u32 base = osConvertVirtToPhys((void*)__ctru_linear_heap);
    if (p >= base && p < base + __ctru_linear_heap_size)
        return (void*)(__ctru_linear_heap + (p - base));
    return NULL;
}

static bool save_lcd(int screen, const char* path) {
    const u32 base = screen == 0 ? 0x400400 : 0x400500;
    u32 addr_a = 0, addr_b = 0, format = 0, select = 0, stride = 0;
    GSPGPU_ReadHWRegs(base + 0x68, &addr_a, 4);
    GSPGPU_ReadHWRegs(base + 0x6C, &addr_b, 4);
    GSPGPU_ReadHWRegs(base + 0x70, &format, 4);
    GSPGPU_ReadHWRegs(base + 0x78, &select, 4);
    GSPGPU_ReadHWRegs(base + 0x90, &stride, 4);
    const u32 fb_phys = (select & 1) ? addr_b : addr_a;
    const u8* fb = phys_to_virt(fb_phys);
    const int width = screen == 0 ? 400 : 320, height = 240;
    const int fmt = format & 7;
    const int bpp = fmt == 0 ? 4 : fmt == 1 ? 3 : 2;
    if (!fb || stride == 0) {
        say("  screenshot %s: LCD framebuffer 0x%08lX not readable", path, fb_phys);
        return false;
    }
    GSPGPU_InvalidateDataCache(fb, stride * width);

    const int row_size = (width * 3 + 3) & ~3;
    const int image_size = row_size * height;
    u8* bmp = malloc(54 + image_size);
    if (!bmp)
        return false;
    memset(bmp, 0, 54);
    bmp[0] = 'B', bmp[1] = 'M';
    *(u32*)(bmp + 2) = 54 + image_size;
    *(u32*)(bmp + 10) = 54;
    *(u32*)(bmp + 14) = 40;
    *(s32*)(bmp + 18) = width;
    *(s32*)(bmp + 22) = height; // bottom-up, like the LCD columns
    *(u16*)(bmp + 26) = 1;
    *(u16*)(bmp + 28) = 24;
    *(u32*)(bmp + 34) = image_size;
    for (int row = 0; row < height; row++) {
        u8* dst = bmp + 54 + row * row_size;
        for (int x = 0; x < width; x++) {
            const u8* p = fb + x * stride + row * bpp; // column x, pixel `row` from the bottom
            u8 r, g, b;
            if (fmt == 0) { // RGBA8: bytes A B G R
                b = p[1], g = p[2], r = p[3];
            } else if (fmt == 1) { // BGR8: bytes B G R
                b = p[0], g = p[1], r = p[2];
            } else {
                const u16 v = p[0] | (p[1] << 8);
                if (fmt == 2) { // RGB565
                    r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
                } else if (fmt == 3) { // RGB5A1
                    r = (v >> 11) << 3, g = ((v >> 6) & 31) << 3, b = ((v >> 1) & 31) << 3;
                } else { // RGBA4
                    r = (v >> 12) << 4, g = ((v >> 8) & 15) << 4, b = ((v >> 4) & 15) << 4;
                }
            }
            dst[x * 3 + 0] = b, dst[x * 3 + 1] = g, dst[x * 3 + 2] = r;
        }
    }
    FILE* f = fopen(path, "wb");
    bool ok = false;
    if (f) {
        ok = fwrite(bmp, 1, 54 + image_size, f) == (size_t)(54 + image_size);
        fclose(f);
    }
    free(bmp);
    return ok;
}

// ----------------------------------------------------------------------------------------------
// Progress (resume after a freeze)

typedef struct {
    int next;    // next case to run
    int running; // case that was started but not finished (-1: none)
    int quick;
} Progress;

static Progress load_progress(void) {
    Progress p = {0, -1, 0};
    FILE* f = fopen(DIR "/progress.txt", "r");
    if (f) {
        if (fscanf(f, "%d %d %d", &p.next, &p.running, &p.quick) != 3)
            p = (Progress){0, -1, 0};
        fclose(f);
    }
    return p;
}

static void save_progress(Progress p) {
    FILE* f = fopen(DIR "/progress.txt", "w");
    if (f) {
        fprintf(f, "%d %d %d\n", p.next, p.running, p.quick);
        fclose(f);
    }
}

// ----------------------------------------------------------------------------------------------

static bool wait_key(u32* pressed) {
    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();
        if (down) {
            *pressed = down;
            return true;
        }
        gspWaitForVBlank();
    }
    return false;
}

int main(int argc, char** argv) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    mkdir(DIR, 0777);

    Progress progress = load_progress();
    printf("Zeste PICA wrap calibration\n\n");
    if (progress.running >= 0) {
        printf("Case %d did not finish last time\n(console froze or was turned off).\n\n",
               progress.running);
    }
    printf("A      start / continue at case %d\n", progress.next);
    printf("X      restart from case 0 (full run)\n");
    printf("Y      restart from case 0 (quick run)\n");
    printf("START  exit\n\n");
    printf("Files go to %s\n", DIR);
    printf("During the run: START stops after\nthe current case.\n");

    u32 key = 0;
    if (!wait_key(&key) || (key & KEY_START)) {
        gfxExit();
        return 0;
    }
    if (key & (KEY_X | KEY_Y)) {
        progress = (Progress){0, -1, (key & KEY_Y) ? 1 : 0};
        append("log.txt", "\n");
    }
    build_cases(progress.quick);
    consoleClear();

    char header[96];
    snprintf(header, sizeof(header), "=== run started at case %d (%s, %d cases) ===",
             progress.next, progress.quick ? "quick" : "full", case_count);
    say("%s", header);
    if (progress.running >= 0) {
        say("case %d: FROZE - it was started but never finished (console restarted)",
            progress.running);
        append("results.csv", "%d,%s,FROZE,,,,\n", progress.running,
               progress.running < case_count ? cases[progress.running].name : "?");
        progress.running = -1;
        save_progress(progress);
    }
    {
        FILE* f = fopen(DIR "/results.csv", "r");
        if (!f)
            append("results.csv",
                   "case,name,result,gpu_ms_max,canary_changed_words,canaries,description\n");
        else
            fclose(f);
    }

    gpu_init();

    bool stop = false;
    for (int i = progress.next; i < case_count && !stop && aptMainLoop(); i++) {
        const Case* c = &cases[i];
        char desc[200];
        build_geometry(c, desc, sizeof(desc));
        consoleClear();
        printf("Case %d / %d\n%s\n\n%s\n\nSTART: stop after this case\n", i, case_count - 1,
               c->name, desc);

        // Record the case before running it: if it freezes the console, the next run knows.
        progress.running = i;
        progress.next = i + 1;
        save_progress(progress);
        say("case %d %s: %s", i, c->name, desc);

        fill_canaries();
        apply_state(c);

        double gpu_ms_max = 0;
        bool hung = false;
        int frame = 0;
        for (frame = 0; frame < FRAMES_PER_CASE; frame++) {
            hidScanInput();
            if (hidKeysDown() & KEY_START)
                stop = true;
            double waited = 0;
            if (!begin_frame_with_timeout(&waited)) {
                hung = true;
                break;
            }
            if (waited > gpu_ms_max)
                gpu_ms_max = waited;
            C3D_RenderTargetClear(target, C3D_CLEAR_COLOR, 0x102040FF, 0);
            C3D_FrameDrawOn(target);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_modelview, &modelview);
            C3D_BufInfo* buf = C3D_GetBufInfo();
            BufInfo_Init(buf);
            BufInfo_Add(buf, vbo, sizeof(Vertex), 3, 0x210);
            C3D_DrawArrays(GPU_TRIANGLES, 0, vbo_count);
            C3D_FrameEnd(0);
        }
        // Health check: the last frame must complete too.
        if (!hung) {
            double waited = 0;
            if (begin_frame_with_timeout(&waited)) {
                C3D_FrameEnd(0);
                hung = !begin_frame_with_timeout(&waited);
                if (!hung)
                    C3D_FrameEnd(0);
            } else {
                hung = true;
            }
        }
        if (hung) {
            // Do not touch VRAM or GPU registers any more: on a broken GPU that can freeze the
            // whole console.
            say("case %d %s: GPU HANG - the GPU did not finish within %d ms (frame %d). Run "
                "stopped.",
                i, c->name, GPU_TIMEOUT_MS, frame);
            append("results.csv", "%d,%s,GPU_HANG,%.1f,,,\"%s\"\n", i, c->name, gpu_ms_max, desc);
            progress.running = -1;
            save_progress(progress);
            consoleClear();
            printf("\x1b[31mCase %d broke the GPU\x1b[0m\n%s\n\n", i, c->name);
            printf("Turn the console off and on, then\nrun again: it continues with case %d.\n",
                   i + 1);
            while (aptMainLoop()) {
                hidScanInput();
                if (hidKeysDown() & KEY_START)
                    break;
                svcSleepThread(16000000);
            }
            gfxExit();
            return 0;
        }

        char canary[400];
        const u32 changed = check_canaries(c, canary, sizeof(canary));
        char top[96], bottom[96];
        snprintf(top, sizeof(top), "%s/%s_top.bmp", DIR, c->name);
        snprintf(bottom, sizeof(bottom), "%s/%s_bottom.bmp", DIR, c->name);
        const bool shot_top = save_lcd(0, top);
        const bool shot_bottom = save_lcd(1, bottom);
        say("case %d %s: %s, GPU max wait %.1f ms, canaries: %s%s", i, c->name,
            changed ? "OUT-OF-BOUNDS WRITES" : "ok", gpu_ms_max, canary,
            shot_top && shot_bottom ? "" : " (screenshot failed)");
        append("results.csv", "%d,%s,%s,%.1f,%lu,\"%s\",\"%s\"\n", i, c->name,
               changed ? "OOB_WRITES" : "OK", gpu_ms_max, changed, canary, desc);
        progress.running = -1;
        save_progress(progress);
    }

    consoleClear();
    if (progress.next >= case_count) {
        say("=== run complete ===");
        printf("All %d cases done.\nPlease send the folder\n%s\n\nSTART: exit\n", case_count, DIR);
        progress = (Progress){0, -1, progress.quick};
        save_progress(progress);
    } else {
        printf("Stopped. Next run continues at\ncase %d.\n\nSTART: exit\n", progress.next);
    }
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gspWaitForVBlank();
    }
    C3D_Fini();
    gfxExit();
    return 0;
}
