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
//   Y      : unflushed transfer source           (CPU data cache detection)
//   L      : unlocked counter from 2 cores        (race exposure: lost updates)
//   R      : oversized near-plane triangle + NaN vertex (strict PICA)
//   DOWN   : low-priority thread starved by a busy main thread (starvation report)
//   SELECT : SD card timings and the remove_all pattern (SD latency model)
//   UP     : texture copy read too early after a citro3d frame (asynchronous GX transfers)
//   START  : exit
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
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

// Explicit heap sizes, like OpenMW: libctru's default (everything but the linear heap) exceeds
// the 128 MB heap area in the 178 MB mode and the kernel refuses it. 64 + 24 MB fits both the
// 96 MB mode used for the .3dsx and the 178 MB mode of the .cia/.cxi.
u32 __ctru_heap_size = 64 * 1024 * 1024;
u32 __ctru_linear_heap_size = 24 * 1024 * 1024;

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
// Zeste draw tags: label the GPU work that follows in the command list. The register write uses an
// empty byte mask, so it changes nothing on hardware; the debug string gives the id a name (once
// per id is enough). Zeste shows the name in its reports and command-list dumps.
#define ZESTE_TAG_REG 0x130 // GPUREG_FRAGOP_SHADOW
static void zeste_tag(u32 id, const char* name) {
    static bool named[32];
    if (id < 32 && !named[id]) {
        char text[96];
        int n = snprintf(text, sizeof(text), "ZESTE:tag %lu %s", id, name);
        svcOutputDebugString(text, n);
        named[id] = true;
    }
    GPUCMD_AddMaskedWrite(ZESTE_TAG_REG, 0x0, id);
}

static void draw_vertices_tagged(const vertex* src, int count, bool flush, bool perspective,
                                 u32 tag_id, const char* tag_name);

static void draw_vertices(const vertex* src, int count, bool flush, bool perspective) {
    draw_vertices_tagged(src, count, flush, perspective, 0, NULL);
}

static void draw_vertices_tagged(const vertex* src, int count, bool flush, bool perspective,
                                 u32 tag_id, const char* tag_name) {
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
    if (tag_name)
        zeste_tag(tag_id, tag_name);
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
    // citro3d flushes the whole linear heap in C3D_FrameEnd, so buffers used by draws inside a
    // frame are always flushed. A transfer started outside a frame is not: fill a buffer with
    // the CPU and let the transfer engine read it without GSPGPU_FlushDataCache.
    say("[Y] CPU-filled buffer copied by the transfer engine without a flush");
    const u32 size = 64 * 64 * 4;
    u32* src = linearAlloc(size);
    u32* dst = linearAlloc(size);
    for (u32 i = 0; i < size / 4; i++)
        src[i] = 0xFF00FF00u ^ i;
    GX_TextureCopy(src, GX_BUFFER_DIM(0, 0), dst, GX_BUFFER_DIM(0, 0), size, GX_TRANSFER_RAW_COPY(1));
    gspWaitForPPF();
    GSPGPU_InvalidateDataCache(dst, size);
    say("[Y] done: dst[1]=0x%08lX (expect a cache report)", dst[1]);
    linearFree(src);
    linearFree(dst);
}

#define RACE_ITERS 1000000
static volatile u32 race_counter;
static volatile bool race_go;
static void race_thread(void* arg) {
    while (!race_go)
        svcSleepThread(1000);
    for (u32 i = 0; i < RACE_ITERS; i++)
        race_counter = race_counter + 1; // unlocked read-modify-write
}

static void test_race(void) {
    // Two threads on different cores increment a shared counter without a lock. Every lost update
    // is a race that happened. On hardware both cores run truly in parallel.
    say("[L] 2 threads (core 0 and core 1) x %d unlocked increments", RACE_ITERS);
    race_counter = 0;
    race_go = false;
    Thread a = threadCreate(race_thread, NULL, 0x2000, 0x31, 0, false);
    Thread b = threadCreate(race_thread, NULL, 0x2000, 0x31, 1, false);
    race_go = true;
    if (a) {
        threadJoin(a, U64_MAX);
        threadFree(a);
    }
    if (b) {
        threadJoin(b, U64_MAX);
        threadFree(b);
    }
    say("[L] counter=%lu lost updates=%lu", race_counter, 2 * RACE_ITERS - race_counter);
}

static volatile u32 low_prio_runs;
static void low_prio_thread(void* arg) {
    for (int i = 0; i < 100; i++) {
        low_prio_runs++;
        svcSleepThread(1000000);
    }
}

static void test_starvation(void) {
    // A low-priority thread on core 0 while the main thread computes for 2 s without sleeping:
    // on hardware it never runs during that time.
    say("[DOWN] main thread busy for 2 s, low-priority thread on core 0");
    low_prio_runs = 0;
    Thread t = threadCreate(low_prio_thread, NULL, 0x1000, 0x3F, 0, false);
    svcSleepThread(1000000); // let it start once
    u64 end = svcGetSystemTick() + 2ULL * SYSCLOCK_ARM11;
    while (svcGetSystemTick() < end) {
    }
    say("[DOWN] low-priority thread ran %lu times during the busy loop", low_prio_runs);
    if (t) {
        threadJoin(t, U64_MAX);
        threadFree(t);
    }
}

static double ms_since(u64 start) {
    return (double)(svcGetSystemTick() - start) / (SYSCLOCK_ARM11 / 1000.0);
}

static void test_sd(void) {
    say("[SELECT] SD card timings");
    mkdir("sdmc:/zeste-test", 0777);
    u64 t = svcGetSystemTick();
    FILE* f = fopen("sdmc:/zeste-test/data.bin", "wb");
    say("[SELECT] create file: %.1f ms", ms_since(t));
    static u8 buf[64 * 1024];
    memset(buf, 0x5A, sizeof(buf));
    t = svcGetSystemTick();
    for (int i = 0; i < 16; i++)
        fwrite(buf, 1, sizeof(buf), f);
    fclose(f);
    say("[SELECT] write 1 MB: %.1f ms", ms_since(t));
    f = fopen("sdmc:/zeste-test/data.bin", "rb");
    setvbuf(f, NULL, _IONBF, 0);
    t = svcGetSystemTick();
    for (int i = 0; i < 64; i++)
        fread(buf, 1, 1024, f);
    say("[SELECT] 64 reads of 1 KB: %.1f ms", ms_since(t));
    t = svcGetSystemTick();
    fread(buf, 1, sizeof(buf), f);
    say("[SELECT] 1 read of 64 KB: %.1f ms", ms_since(t));
    fclose(f);
    t = svcGetSystemTick();
    mkdir("sdmc:/zeste-test/dir", 0777);
    say("[SELECT] mkdir: %.1f ms", ms_since(t));
    for (int i = 0; i < 3; i++) {
        char name[64];
        snprintf(name, sizeof(name), "sdmc:/zeste-test/dir/f%d", i);
        FILE* e = fopen(name, "wb");
        if (e)
            fclose(e);
    }
    // remove_all pattern: delete entries while the directory is still being iterated.
    DIR* d = opendir("sdmc:/zeste-test/dir");
    struct dirent* ent;
    int removed = 0;
    while (d && (ent = readdir(d)) != NULL && removed < 10) {
        char name[300];
        snprintf(name, sizeof(name), "sdmc:/zeste-test/dir/%s", ent->d_name);
        unlink(name);
        removed++;
    }
    if (d)
        closedir(d);
    rmdir("sdmc:/zeste-test/dir");
    unlink("sdmc:/zeste-test/data.bin");
    rmdir("sdmc:/zeste-test");
    say("[SELECT] deleted %d entries while iterating (remove_all pattern)", removed);
}

// UP: the pattern the hardware calibration (tools/zeste/calib v2) hit. libctru's GSP events are
// sticky: citro3d's frame already raised PPF with its display transfer and nobody cleared it, so
// gspWaitForPPF() (nextEvent = false) returns at once instead of waiting for the texture copy,
// and the CPU reads the destination while the copy is still running. C3D_SyncTextureCopy outside
// a frame ends with the same gspWaitForPPF(). Azahar finishes transfers at the register write,
// so it always reads complete data; Zeste with async_gx reads the old contents and reports it.
#define READBACK_MARKER 0xDEADBEEFu
#define READBACK_SOURCE 0x5A5A5A5Au

static u32 count_words(const u32* p, u32 words, u32 value) {
    u32 n = 0;
    for (u32 i = 0; i < words; i++)
        n += p[i] == value;
    return n;
}

static void prepare_readback(u32* readback, u32 size) {
    for (u32 i = 0; i < size / 4; i++)
        readback[i] = READBACK_MARKER;
    GSPGPU_FlushDataCache(readback, size);
    // A citro3d frame: its display transfer raises PPF, which stays set.
    draw_vertices(small_tri, 3, true, false);
    gspWaitForVBlank();
    gspWaitForVBlank();
    GSPGPU_InvalidateDataCache(readback, size);
}

static void report_readback(const char* how, u32* readback, u32 size) {
    GSPGPU_InvalidateDataCache(readback, size);
    const u32 words = size / 4;
    const u32 early = count_words(readback, words, READBACK_MARKER);
    svcSleepThread(10000000); // long after any transfer
    GSPGPU_InvalidateDataCache(readback, size);
    const u32 later = count_words(readback, words, READBACK_MARKER);
    const u32 copied = count_words(readback, words, READBACK_SOURCE);
    say("[UP] %s: %lu of %lu words not copied yet when the wait returned; 10 ms later %lu "
        "(%lu copied)",
        how, early, words, later, copied);
}

static void test_early_readback(void) {
    say("[UP] texture copy VRAM -> linear read right after the wait (Azahar: 0 words missing)");
    const u32 size = 256 * 1024;
    u32* src = vramAlloc(size);
    u32* readback = linearAlloc(size);
    if (!src || !readback) {
        say("[UP] allocation failed");
        return;
    }
    C3D_SyncMemoryFill(src, READBACK_SOURCE, src + size / 4, GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH,
                       NULL, 0, NULL, 0);
    svcSleepThread(10000000); // the fill's own wait has the same problem: let it finish

    // Calibration v2: GX_TextureCopy, then gspWaitForPPF.
    prepare_readback(readback, size);
    GX_TextureCopy(src, GX_BUFFER_DIM(size >> 4, 0), readback, GX_BUFFER_DIM(size >> 4, 0), size,
                   GX_TRANSFER_RAW_COPY(1));
    gspWaitForPPF();
    report_readback("GX_TextureCopy + gspWaitForPPF", readback, size);

    // Calibration v3: C3D_SyncTextureCopy outside a frame.
    prepare_readback(readback, size);
    C3D_SyncTextureCopy(src, GX_BUFFER_DIM(size >> 4, 0), readback, GX_BUFFER_DIM(size >> 4, 0),
                        size, GX_TRANSFER_RAW_COPY(1));
    report_readback("C3D_SyncTextureCopy", readback, size);

    linearFree(readback);
    vramFree(src);
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
    draw_vertices_tagged(tri, 6, true, true, 1, "near-plane test triangle");
    say("[R] done (expect strict PICA reports)");
}

int main(int argc, char** argv) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    gpu_init();

    say("Zeste test ROM");
    info_tests();
    say("A texbind(NULL)  B stack overflow  X null write+handler");
    say("Y unflushed VBO  L race  R near plane  UP early readback");
    say("DOWN starvation  SELECT SD  START exit");

    int frames = 0;
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
        if (kDown & KEY_DOWN)
            test_starvation();
        if (kDown & KEY_SELECT)
            test_sd();
        if (kDown & KEY_UP)
            test_early_readback();
        if (!kDown)
            draw_vertices(small_tri, 3, true, false);
        if (++frames == 60)
            say("[info] C3D_GetDrawingTime after 60 frames: %.3f ms (Azahar ~0.25 constant)",
                C3D_GetDrawingTime());
    }

    shaderProgramFree(&program);
    DVLB_Free(vshader_dvlb);
    C3D_Fini();
    gfxExit();
    return 0;
}
