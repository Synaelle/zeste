// Zeste hardware-accuracy test ROM.
//
// Each test triggers one behaviour that differs between Azahar and a real New 3DS. Run it in
// Zeste with the matching option enabled and check the log / inaccuracy report.
//
// Non-fatal tests run automatically at start (results printed on the bottom screen and to the
// debug output via svcOutputDebugString). Fatal tests run on a button press:
//   A      : C3D_TexBind(1, NULL) + draw         (null texture -> data abort on hardware)
//   B      : recursive stack overflow            (fault below the main stack)
//   X      : write to NULL with threadOnException installed (handler must run)
//   Y      : unflushed vertex buffer             (CPU data cache detection)
//   L      : unlocked linearAlloc from 4 threads (race exposure)
//   R      : oversized near-plane triangle + NaN vertex (strict PICA)
//   START  : exit
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "vshader_shbin.h"

#define CLEAR_COLOR 0x203040FF
#define DISPLAY_TRANSFER_FLAGS                                                                     \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |               \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct {
    float x, y, z;
    float r, g, b, a;
} vertex;

static DVLB_s* vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection;
static C3D_Mtx projection;
static C3D_RenderTarget* target;
static C3D_AttrInfo* attr_info;

static void say(const char* fmt, ...) {
    char buf[256];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    printf("%s\n", buf);
    svcOutputDebugString(buf, strlen(buf));
}

// ---------------------------------------------------------------------------------------------
// Info tests (memory region size, processor ID, linear heap physical address, CPU speed)

static u32 processor_id_in_thread;
static void cpuid_thread(void* arg) {
    processor_id_in_thread = (u32)svcGetProcessorID();
}

static u64 busy_loop_ticks(void) {
    u64 start = svcGetSystemTick();
    volatile u32 acc = 0;
    for (u32 i = 0; i < 20000000; i++)
        acc += i * 3;
    return svcGetSystemTick() - start;
}

static void info_tests(void) {
    say("[info] APPLICATION region = %lu KB", osGetMemRegionSize(MEMREGION_APPLICATION) / 1024);
    say("[info] svcGetProcessorID (main) = %d", svcGetProcessorID());
    Thread t = threadCreate(cpuid_thread, NULL, 0x1000, 0x30, 1, false);
    if (t) {
        threadJoin(t, U64_MAX);
        threadFree(t);
        say("[info] svcGetProcessorID (core 1 thread) = %lu", processor_id_in_thread);
    }
    void* lin = linearAlloc(0x1000);
    say("[info] linearAlloc vaddr=%p paddr=0x%08lX", lin, osConvertVirtToPhys(lin));
    linearFree(lin);

    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    if (n3ds) {
        osSetSpeedupEnable(false);
        u64 slow = busy_loop_ticks();
        osSetSpeedupEnable(true);
        u64 fast = busy_loop_ticks();
        say("[info] busy loop: speedup off %llu ticks, on %llu ticks (ratio %.2f, hw ~3)", slow,
            fast, fast ? (double)slow / (double)fast : 0.0);
    }
}

// ---------------------------------------------------------------------------------------------
// GPU helpers

static void gpu_init(void) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    vshader_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);
    C3D_BindProgram(&program);
    uLoc_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");

    attr_info = C3D_GetAttrInfo();
    AttrInfo_Init(attr_info);
    AttrInfo_AddLoader(attr_info, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attr_info, 1, GPU_FLOAT, 4);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

// Draws `count` vertices from a linear buffer. If `flush` is false the buffer is NOT flushed
// from the CPU data cache before the GPU reads it.
static void draw_vertices(const vertex* src, int count, bool flush, bool perspective) {
    vertex* vbo = linearAlloc(sizeof(vertex) * count);
    memcpy(vbo, src, sizeof(vertex) * count);
    if (flush)
        GSPGPU_FlushDataCache(vbo, sizeof(vertex) * count);

    if (perspective)
        Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(80.0f), C3D_AspectRatioTop, 0.01f,
                      1000.0f, false);
    else
        Mtx_OrthoTilt(&projection, 0.0, 400.0, 0.0, 240.0, 0.0, 1.0, true);

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C3D_FrameDrawOn(target);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, vbo, sizeof(vertex), 2, 0x10);
    C3D_DrawArrays(GPU_TRIANGLES, 0, count);
    C3D_FrameEnd(0);
    // Keep the buffer: the GPU may still read it during the next frame.
}

static const vertex small_tri[] = {
    {200.0f, 200.0f, 0.5f, 1, 0, 0, 1},
    {100.0f, 40.0f, 0.5f, 0, 1, 0, 1},
    {300.0f, 40.0f, 0.5f, 0, 0, 1, 1},
};

// ---------------------------------------------------------------------------------------------
// Fatal / interactive tests

static void test_null_texbind(void) {
    say("[A] C3D_TexBind(1, NULL) then draw");
    C3D_TexBind(1, NULL);
    draw_vertices(small_tri, 3, true, false);
    say("[A] survived (Azahar behaviour; hardware data-aborts)");
}

static volatile u32 sink;
static u32 recurse(u32 depth) {
    volatile u8 big[4096];
    big[0] = (u8)depth;
    big[4095] = (u8)depth;
    sink += big[0];
    return recurse(depth + 1) + big[4095];
}

static void test_stack_overflow(void) {
    say("[B] recursing until the main stack overflows");
    sink = recurse(0);
    say("[B] survived (should never print)");
}

static u8 handler_stack[0x1000] __attribute__((aligned(8)));
static void exception_handler(ERRF_ExceptionInfo* excep, CpuRegisters* regs) {
    say("[X] exception handler ran: type=%d far=0x%08lX fsr=0x%08lX pc=0x%08lX", excep->type,
        excep->far, excep->fsr, regs->pc);
    say("[X] PASS");
    for (;;)
        svcSleepThread(1000000000LL);
}

static void test_null_write_with_handler(void) {
    say("[X] installing threadOnException, writing to 0x0");
    threadOnException(exception_handler, handler_stack + sizeof(handler_stack),
                      WRITE_DATA_TO_HANDLER_STACK);
    *(volatile u32*)0 = 0xDEADBEEF;
    say("[X] FAIL: write to NULL did not fault (Azahar behaviour)");
}

static void test_unflushed(void) {
    say("[Y] drawing from a vertex buffer that was never flushed");
    draw_vertices(small_tri, 3, false, false);
    say("[Y] done (expect a cache report)");
}

#define RACE_THREADS 4
#define RACE_ITERS 2000
static volatile bool race_go;
static void race_thread(void* arg) {
    while (!race_go)
        svcSleepThread(1000);
    for (int i = 0; i < RACE_ITERS; i++) {
        void* p = linearAlloc(64 + (i & 63));
        if (p)
            linearFree(p);
    }
}

static void test_race(void) {
    say("[L] %d threads x %d unlocked linearAlloc/linearFree", RACE_THREADS, RACE_ITERS);
    Thread threads[RACE_THREADS];
    race_go = false;
    for (int i = 0; i < RACE_THREADS; i++)
        threads[i] = threadCreate(race_thread, NULL, 0x2000, 0x30, i & 1 ? 1 : 0, false);
    race_go = true;
    for (int i = 0; i < RACE_THREADS; i++) {
        if (threads[i]) {
            threadJoin(threads[i], U64_MAX);
            threadFree(threads[i]);
        }
    }
    say("[L] free linear space now %lu KB", linearSpaceFree() / 1024);
}

static void test_near_plane(void) {
    say("[R] near-plane triangle with huge screen extent + NaN vertex");
    // One vertex just in front of the near plane, two far behind the camera.
    const vertex tri[] = {
        {0.0f, 0.0f, -0.0101f, 1, 1, 1, 1},
        {-5000.0f, -1.0f, 50.0f, 1, 0, 0, 1},
        {5000.0f, -1.0f, 50.0f, 0, 1, 0, 1},
        {NAN, 0.0f, -1.0f, 1, 1, 1, 1},
        {1.0f, 0.0f, -1.0f, 1, 1, 1, 1},
        {0.0f, 1.0f, -1.0f, 1, 1, 1, 1},
    };
    draw_vertices(tri, 6, true, true);
    say("[R] done (expect strict PICA reports)");
}

int main(int argc, char** argv) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    gpu_init();

    say("Zeste test ROM");
    info_tests();
    say("A texbind(NULL)  B stack overflow  X null write+handler");
    say("Y unflushed VBO  L race  R near plane  START exit");

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START)
            break;
        if (kDown & KEY_A)
            test_null_texbind();
        if (kDown & KEY_B)
            test_stack_overflow();
        if (kDown & KEY_X)
            test_null_write_with_handler();
        if (kDown & KEY_Y)
            test_unflushed();
        if (kDown & KEY_L)
            test_race();
        if (kDown & KEY_R)
            test_near_plane();
        if (!kDown)
            draw_vertices(small_tri, 3, true, false);
    }

    shaderProgramFree(&program);
    DVLB_Free(vshader_dvlb);
    C3D_Fini();
    gfxExit();
    return 0;
}
