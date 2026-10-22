// Dreamcast output: the part of trx/game/output/common.c that brings the
// output up and down and frames each scene, with the PowerVR in place of
// OpenGL. Which sources make up the scene, and what they hear about level
// changes, is the same as on the desktop.

#include "pvr.h"
#include "shadow.h"

#include <platform/dreamcast/dc_bench.h>
#include <platform/dreamcast/dc_mode.h>
#include <platform/dreamcast/dc_probe.h> // TEMP
#include <platform/dreamcast/dc_watchdog.h>

#include <platform/dreamcast/renderer.h>
#include <trx/config.h>
#include <trx/core/log.h>
#include <trx/core/memory.h>
#include <trx/core/subsystem.h>
#include <trx/game/clock/timer.h>
#include <trx/game/level.h>
#include <trx/game/output/binocular_mask.h>
#include <trx/game/output/common.h>
#include <trx/game/output/func.h>
#include <trx/game/output/lights.h>
#include <trx/game/output/lights/priv.h>
#include <trx/game/output/mesh_batcher/batcher.h>
#include <trx/game/output/overlay.h>
#include <trx/game/output/scene_compositor.h>
#include <trx/game/output/sky.h>
#include <trx/game/output/sources/lightnings.h>
#include <trx/game/output/sources/misc.h>
#include <trx/game/output/sources/objects.h>
#include <trx/game/output/sources/overlay.h>
#include <trx/game/output/sources/poly_fx.h>
#include <trx/game/output/sources/rooms.h>
#include <trx/game/output/sources/rooms_debug.h>
#include <trx/game/output/sources/shadows.h>
#include <trx/game/output/sources/sky.h>
#include <trx/game/output/sources/sprites.h>
#include <trx/game/output/sources/ui.h>
#include <trx/game/output/state.h>
#include <trx/game/output/textures.h>
#include <trx/game/shell.h>

#include <arch/timer.h>
#include <kos/thread.h>

#include <stdio.h>
#include <string.h>

static MESH_BATCHER *m_Batcher = nullptr;
static OUTPUT_UNIFORMS *m_Uniforms = nullptr;

static int32_t m_DrawnFrames = 0;
static int32_t m_FPS = 0;
static CLOCK_TIMER m_FPSTimer = { .type = CLOCK_TIMER_REAL };

uint32_t g_DC_FrameCount = 0;

// Object meshes not drawn for this many frames are let go (they are built
// again when next drawn), checked every so many frames.
#define M_MESH_MAX_AGE 300
#define M_MESH_EVICT_INTERVAL 30
// And past this many vertices (~0.8 MB with what the batcher keeps per
// vertex), the ones drawn longest ago go: TR3's jungles keep twice TR2's.
#define M_MESH_MAX_VERTICES 24000

int32_t LUA_DC_CollectGarbage(void);

// An allocation is about to fail: the object meshes not drawn this frame and
// the Lua state's garbage go, and the allocation is tried again.
static bool M_HandleLowMemory(void)
{
    OutputSource_Objects_EvictUnused(1);
    LUA_DC_CollectGarbage();
    return true;
}

static void M_Shutdown(void)
{
    SceneCompositor_Shutdown();
    OutputSource_Rooms_Shutdown();
    OutputSource_RoomsDebug_Shutdown();
    OutputSource_Objects_Shutdown();
    OutputSource_Sprites_Shutdown();
    OutputSource_Lightnings_Shutdown();
    OutputSource_Sky_Shutdown();
    OutputSource_PolyFX_Shutdown();
    OutputSource_Shadows_Shutdown();
    OutputSource_Misc_Shutdown();
    OutputSource_Overlay_Shutdown();
    OutputSource_UI_Shutdown();

    if (m_Uniforms != nullptr) {
        Output_Uniforms_Free(m_Uniforms);
        m_Uniforms = nullptr;
    }
    if (m_Batcher != nullptr) {
        MeshBatcher_Destroy(m_Batcher);
        m_Batcher = nullptr;
    }

    Output_Textures_Shutdown();
    Output_Lights_Shutdown();
    DC_PVR_Shutdown();
}

RESULT Output_Init(void)
{
    DC_PVR_Init();
    SceneCompositor_Init();
    Output_Textures_Init();

    m_Uniforms = Output_Uniforms_Create();
    m_Batcher = MeshBatcher_Create();
    OutputSource_Sky_Init();
    SceneCompositor_AddSource(MeshBatcher_AsSource(m_Batcher));
    OutputSource_Rooms_Init(m_Batcher);
    OutputSource_RoomsDebug_Init();
    OutputSource_Objects_Init(m_Batcher);
    OutputSource_Sprites_Init(m_Batcher);
    OutputSource_Lightnings_Init();
    OutputSource_PolyFX_Init();
    OutputSource_Shadows_Init(m_Batcher);
    OutputSource_Misc_Init();
    OutputSource_Overlay_Init();

    Output_Lights_Init();
    OutputSource_UI_Init();

    Output_ApplyRenderSettings();
    Memory_SetLowMemoryHandler(M_HandleLowMemory);
    return OK;
}

bool Output_IsHeadless(void)
{
    return Shell_GetArgs()->headless || Shell_IsDrawingSkipped();
}

const OUTPUT_UNIFORMS *Output_GetUniforms(void)
{
    return m_Uniforms;
}

OUTPUT_MESH_SHADER *Output_GetMeshShader(void)
{
    return nullptr;
}

OUTPUT_UI_SHADER *Output_GetUIShader(void)
{
    return nullptr;
}

#if defined(TRX_DC_WATCHDOG)
static void M_StatsBeginScene(void);
static uint32_t g_DC_ShadowMapUs = 0;
static uint32_t g_DC_SubmitUs = 0;
extern uint32_t g_DC_PassUs;
extern uint32_t g_DC_BatcherFrameInstances;
extern uint32_t g_DC_BatcherFrameVertices;
extern uint32_t g_DC_BatcherXformUs;
#endif

void Output_BeginScene(void)
{
#if defined(TRX_DC_WATCHDOG)
    M_StatsBeginScene();
#endif
    Output_ApplyFOV();
    const RGBA_F background = Output_GetBackgroundColor();
    DC_PVR_SetBackgroundColor(background.r, background.g, background.b);
#if defined(TRX_DC_WATCHDOG)
    const uint64_t map_start = timer_us_gettime64();
    DC_Shadow_RenderMaps();
    g_DC_ShadowMapUs = (uint32_t)(timer_us_gettime64() - map_start);
#else
    DC_Shadow_RenderMaps();
#endif
    DC_PVR_BeginFrame();
    SceneCompositor_BeginScene();
    Output_Lights_BeginScene();
}

#if defined(TRX_DC_WATCHDOG)

extern _Thread_local uint64_t g_DC_SleepUs;

// Where each frame's time goes, as average and peak over a window of frames:
// logic (from the last frame's end to this one's start, less sleeping), the
// CPU drawing it, the soft shadows' map, the wait for the PowerVR, and the
// PowerVR's own render.
#define M_STATS_WINDOW 120
enum { M_LOGIC, M_DRAW, M_PASS, M_SUBMIT, M_MAP, M_MAP_WAIT, M_WAIT, M_RENDER,
       M_POLYS, M_INSTANCES, M_VERTICES, M_OP, M_TR, M_PT, M_MOD, M_XFORM,
       M_STAT_COUNT };
static const char *const m_StatNames[M_STAT_COUNT] = {
    "logic", "stage", "pass", "submit", "map", "mwait", "wait", "pvr", "polys",
    "inst", "verts",
    "OP", "TR", "PT", "MOD", "xform",
};
static struct {
    uint64_t begin_us;
    uint64_t end_us;
    uint64_t sleep_us;
    int32_t frames;
    uint64_t sum[M_STAT_COUNT];
    uint32_t max[M_STAT_COUNT];
    // TEMP: see dc_probe.h
    uint64_t probe_sum[DC_PROBE_COUNT];
    uint32_t probe_max[DC_PROBE_COUNT];
} m_Stats = {};

// TEMP: see dc_probe.h
uint32_t g_DC_ProbeUs[DC_PROBE_COUNT] = {};
static const char *const m_ProbeNames[DC_PROBE_COUNT] = {
    "ev",  "lua",  "rem",  "inp",  "items", "fx",   "lara", "post",
    "end", "ctrl", "mctl", "flip", "log",   "tcyc", "tuvw", "tcomp",
    "vroom", "vobj", "vgen", "soft", "emit", "cel", "bump", "envp",
    "sscene", "spre", "sbounds", "ssky", "sroom", "sstat", "sitem", "slara",
    "sfx", "spaint", "sgflush", "sui", "soflush", "nrooms",
    "llight", "lshadow", "lmesh", "lwater", "ljoint", "lhair", "lequip", "troom", "tplain", "tgen", "gown", "genv", "gfog", "gbill", "goth", "equad", "etri", "eclip", "eout", "eback", "msil", "mend",
};
uint32_t g_DC_LuaEvUs[DC_PROBE_LUA_EVENTS] = {};
uint32_t g_DC_LuaEvN[DC_PROBE_LUA_EVENTS] = {};

static void M_StatsBeginScene(void)
{
    m_Stats.begin_us = timer_us_gettime64();
}

// The benchmark (dc_bench.h): every frame of a demo, from its first drawn
// to its last.
static struct {
    bool active;
    uint64_t last_us;
    int32_t frames;
    uint64_t time_us;
    uint32_t worst_us;
    int32_t slow;
    uint64_t sum[M_STAT_COUNT];
} m_Bench = {};

static void M_BenchFrame(const uint64_t now, const uint32_t value[])
{
    if (!m_Bench.active) {
        return;
    }
    // A gap of a second or more is a load, not a frame.
    if (m_Bench.last_us != 0 && now - m_Bench.last_us < 1000000) {
        const uint32_t dt = (uint32_t)(now - m_Bench.last_us);
        m_Bench.frames++;
        m_Bench.time_us += dt;
        if (dt > m_Bench.worst_us) {
            m_Bench.worst_us = dt;
        }
        // Slower than 30 fps, with a little room for the timer.
        if (dt > 35000) {
            m_Bench.slow++;
        }
        for (int32_t i = 0; i < M_STAT_COUNT; i++) {
            m_Bench.sum[i] += value[i];
        }
    }
    m_Bench.last_us = now;
}

void DC_Bench_Begin(void)
{
    m_Bench = (typeof(m_Bench)) { .active = true };
}

void DC_Bench_End(void)
{
    m_Bench.active = false;
    const int32_t n = m_Bench.frames > 0 ? m_Bench.frames : 1;
    const uint32_t fps10 = m_Bench.time_us > 0
        ? (uint32_t)((uint64_t)m_Bench.frames * 10000000 / m_Bench.time_us)
        : 0;
    const uint32_t min10 =
        m_Bench.worst_us > 0 ? 10000000 / m_Bench.worst_us : 0;
    // Averages in tenths of a millisecond.
#define M_AVG(i) ((unsigned)(m_Bench.sum[i] / n / 100))
    char line1[96];
    char line2[96];
    snprintf(
        line1, sizeof(line1), "BENCH %d frames  avg %u.%u fps  min %u.%u  slow %d%%",
        m_Bench.frames, fps10 / 10, fps10 % 10, min10 / 10, min10 % 10,
        m_Bench.slow * 100 / n);
    snprintf(
        line2, sizeof(line2),
        "logic %u.%u stage %u.%u pass %u.%u map %u.%u pvr %u.%u ms",
        M_AVG(M_LOGIC) / 10, M_AVG(M_LOGIC) % 10, M_AVG(M_DRAW) / 10,
        M_AVG(M_DRAW) % 10, M_AVG(M_PASS) / 10, M_AVG(M_PASS) % 10,
        M_AVG(M_MAP) / 10, M_AVG(M_MAP) % 10, M_AVG(M_RENDER) / 10,
        M_AVG(M_RENDER) % 10);
#undef M_AVG
    char log[200];
    snprintf(log, sizeof(log), "[bench] %s | %s\n", line1, line2);
    DC_Watchdog_Print(log);
    // Shown until read: ten seconds.
    for (int32_t i = 0; i < 100; i++) {
        DC_PVR_DrawLoadingScreen(line1, line2, 100);
        thd_sleep(100);
    }
}

static void M_StatsEndScene(void)
{
    const uint64_t now = timer_us_gettime64();
    DC_PVR_FRAME_STATS pvr;
    DC_PVR_GetFrameStats(&pvr);
    const uint64_t slept = g_DC_SleepUs - m_Stats.sleep_us;
    m_Stats.sleep_us = g_DC_SleepUs;
    uint32_t value[M_STAT_COUNT] = {
        [M_LOGIC] = 0,
        [M_DRAW] = (uint32_t)(now - m_Stats.begin_us) - pvr.wait_us
            - g_DC_ShadowMapUs - g_DC_PassUs - g_DC_SubmitUs,
        [M_PASS] = g_DC_PassUs,
        [M_SUBMIT] = g_DC_SubmitUs,
        [M_MAP] = g_DC_ShadowMapUs - pvr.map_wait_us,
        [M_MAP_WAIT] = pvr.map_wait_us,
        [M_WAIT] = pvr.wait_us,
        [M_RENDER] = pvr.render_us,
        [M_POLYS] = (uint32_t)pvr.polygons,
        [M_INSTANCES] = g_DC_BatcherFrameInstances,
        [M_VERTICES] = g_DC_BatcherFrameVertices,
        [M_XFORM] = g_DC_BatcherXformUs,
        [M_OP] = pvr.list_bytes[0],
        [M_TR] = pvr.list_bytes[1],
        [M_PT] = pvr.list_bytes[2],
        [M_MOD] = pvr.list_bytes[3],
    };
    if (m_Stats.end_us != 0) {
        const uint64_t between = m_Stats.begin_us - m_Stats.end_us;
        value[M_LOGIC] = between > slept ? (uint32_t)(between - slept) : 0;
    }
    m_Stats.end_us = now;
    M_BenchFrame(now, value);
    g_DC_PassUs = 0;
    g_DC_BatcherFrameInstances = 0;
    g_DC_BatcherFrameVertices = 0;
    g_DC_BatcherXformUs = 0;
    for (int32_t i = 0; i < M_STAT_COUNT; i++) {
        m_Stats.sum[i] += value[i];
        if (value[i] > m_Stats.max[i]) {
            m_Stats.max[i] = value[i];
        }
    }
    // TEMP: see dc_probe.h
    for (int32_t i = 0; i < DC_PROBE_COUNT; i++) {
        m_Stats.probe_sum[i] += g_DC_ProbeUs[i];
        if (g_DC_ProbeUs[i] > m_Stats.probe_max[i]) {
            m_Stats.probe_max[i] = g_DC_ProbeUs[i];
        }
        g_DC_ProbeUs[i] = 0;
    }
    if (++m_Stats.frames < M_STATS_WINDOW) {
        return;
    }
    // Times in tenths of a millisecond, list sizes in KiB.
    char line[320];
    int32_t len = snprintf(line, sizeof(line), "[frame] %d fps", (int)m_FPS);
    for (int32_t i = 0; i < M_STAT_COUNT; i++) {
        const uint32_t div = i < M_POLYS || i == M_XFORM ? 100
            : i <= M_VERTICES                       ? 1
                                                    : 1024;
        len += snprintf(
            line + len, sizeof(line) - len, " %s=%lu/%lu", m_StatNames[i],
            (unsigned long)(m_Stats.sum[i] / m_Stats.frames / div),
            (unsigned long)(m_Stats.max[i] / div));
    }
    // TEMP: see dc_probe.h. Its own line, in microseconds, timed itself.
    char probes[1024];
    len = snprintf(probes, sizeof(probes), "[probe]");
    for (int32_t i = 0; i < DC_PROBE_COUNT; i++) {
        len += snprintf(
            probes + len, sizeof(probes) - len, " %s=%lu/%lu", m_ProbeNames[i],
            (unsigned long)(m_Stats.probe_sum[i] / m_Stats.frames),
            (unsigned long)m_Stats.probe_max[i]);
    }
    // Lua events of the window: "e<type>=<us per frame>x<fires per frame>".
    char events[320];
    len = snprintf(events, sizeof(events), "[luaev]");
    for (int32_t i = 0; i < DC_PROBE_LUA_EVENTS && len < 300; i++) {
        if (g_DC_LuaEvN[i] != 0) {
            len += snprintf(
                events + len, sizeof(events) - len, " e%d=%lux%lu", (int)i,
                (unsigned long)(g_DC_LuaEvUs[i] / m_Stats.frames),
                (unsigned long)(g_DC_LuaEvN[i] / m_Stats.frames));
        }
    }
    memset(g_DC_LuaEvUs, 0, sizeof(g_DC_LuaEvUs));
    memset(g_DC_LuaEvN, 0, sizeof(g_DC_LuaEvN));
    // Through the log thread: written straight to the serial port, the three
    // lines held the game up for a frame every window.
    DC_PROBE(
        DC_PROBE_LOG, DC_Watchdog_Print(line); DC_Watchdog_Print("\n");
        DC_Watchdog_Print(probes); DC_Watchdog_Print("\n");
        DC_Watchdog_Print(events); DC_Watchdog_Print("\n"));
    memset(m_Stats.sum, 0, sizeof(m_Stats.sum));
    memset(m_Stats.max, 0, sizeof(m_Stats.max));
    memset(m_Stats.probe_sum, 0, sizeof(m_Stats.probe_sum));
    memset(m_Stats.probe_max, 0, sizeof(m_Stats.probe_max));
    m_Stats.frames = 0;
}
#else
// shortcut: without the watchdog's frame stats there is nothing to measure;
// the demo just runs.
void DC_Bench_Begin(void)
{
}

void DC_Bench_End(void)
{
}
#endif

int32_t g_DC_StartDemo = -1;
bool g_DC_BenchRequested = false;

void Output_EndScene(void)
{
    SceneCompositor_EndScene();
#if defined(TRX_DC_WATCHDOG)
    const uint64_t submit_start = timer_us_gettime64();
    DC_PVR_EndFrame();
    g_DC_SubmitUs = (uint32_t)(timer_us_gettime64() - submit_start);
    M_StatsEndScene();
#else
    DC_PVR_EndFrame();
#endif
    m_DrawnFrames++;
    g_DC_FrameCount++;
    if (g_DC_FrameCount % M_MESH_EVICT_INTERVAL == 0) {
        OutputSource_Objects_EvictUnused(M_MESH_MAX_AGE);
        OutputSource_Objects_EvictToBudget(M_MESH_MAX_VERTICES);
    }
    if (ClockTimer_CheckElapsedAndTake(&m_FPSTimer, 1.0)) {
        m_FPS = m_DrawnFrames;
        m_DrawnFrames = 0;
    }
}

int32_t Output_GetMeasuredFPS(void)
{
    return m_FPS;
}

void Output_Flush(void)
{
    SceneCompositor_Flush();
}

void Output_FlipScreen(void)
{
    // The PowerVR flips on its own once the scene is rendered.
}

void Output_SwitchViewport(const VIEWPORT_SPACE space)
{
    // One screen, one viewport; a switch starts a fresh depth range.
    DC_PVR_NextDepthLayer();
}

void Output_SetSupersamplingEnabled(const bool enabled)
{
    Viewport_SetSupersamplingEnabled(enabled);
}

void Output_ApplyRenderSettings(void)
{
    Output_Textures_ApplyRenderSettings();
    Output_ApplyLevelSettings();
    DC_PVR_SetFilter(g_Config.rendering.texture_filter == TEXTURE_FILTER_BILINEAR);
}

void Output_ApplyLevelSettings(void)
{
    Output_SetWaterColor(Level_GetWaterColor());
    Output_SetBackgroundColor(Level_GetBackgroundColor());
    // Fixed on the Dreamcast, whatever the level and the player ask: black
    // fog from 12 blocks, nothing drawn past 20.
    Output_SetFogColor((RGBA_8888) { .r = 0, .g = 0, .b = 0, .a = 255 });
    Output_SetFogStart(12 * WALL_L);
    Output_SetFogEnd(20 * WALL_L);
}

void Output_DispatchLevelLoad(void)
{
    Output_Textures_ObserveLevelLoad();
    Output_Lights_ObserveLevelLoad();
    Output_Sky_ObserveLevelLoad();
    Output_BinocularMask_ObserveLevelLoad();

    OutputSource_Objects_ObserveLevelLoad();
    OutputSource_Rooms_ObserveLevelLoad();
    OutputSource_RoomsDebug_ObserveLevelLoad();
    OutputSource_Sprites_ObserveLevelLoad();

    MeshBatcher_Seal(m_Batcher);

    Output_ApplyLevelSettings();
}

void Output_DispatchLevelUnload(void)
{
    // Video memory holds one level's pages; the next level needs the room
    // before it starts loading.
    DC_PVR_FreeTexturePages();
    Output_Sky_ObserveLevelUnload();
    Output_BinocularMask_ObserveLevelUnload();
    OutputSource_Objects_ObserveLevelUnload();
    OutputSource_Rooms_ObserveLevelUnload();
    OutputSource_RoomsDebug_ObserveLevelUnload();
    OutputSource_Sprites_ObserveLevelUnload();
    // Last, once no mesh is left to read them.
    Output_Textures_ObserveLevelUnload();
}

void Output_RefreshObjectMeshes(void)
{
    if (!OutputSource_Objects_HasMeshes()) {
        return;
    }
    OutputSource_Objects_ObserveLevelLoad();
    MeshBatcher_Seal(m_Batcher);
}

void Output_DispatchRoomFlip(const ROOM *const room)
{
    OutputSource_Rooms_ObserveRoomFlip(room);
    OutputSource_RoomsDebug_ObserveRoomFlip(room);
}

void Output_DispatchObjectMeshSwap(
    const int32_t mesh_idx_1, const int32_t mesh_idx_2)
{
    OutputSource_Objects_ObserveObjectMeshSwap(mesh_idx_1, mesh_idx_2);
}

void Output_DispatchObjectMeshUpdate(const int32_t mesh_idx)
{
    OutputSource_Objects_ObserveObjectMeshUpdate(mesh_idx);
}

void Output_DispatchObjectMeshGeometry(
    const int32_t mesh_idx, const XYZ_F *const positions,
    const XYZ_F *const normals, const float *const tint_factors)
{
    OutputSource_Objects_ObserveObjectMeshGeometry(
        mesh_idx, positions, normals, tint_factors);
}

bool DC_Renderer_ReserveTexturePages(const int32_t count)
{
    return DC_PVR_ReserveTexturePages(count);
}

void DC_Renderer_StoreTexturePage(
    const int32_t page, const uint16_t *const texels, const uint8_t format)
{
    DC_PVR_StoreTexturePage(page, texels, format);
}

uint16_t DC_Renderer_GetTexel(const int32_t page, const int32_t x, const int32_t y)
{
    return DC_PVR_GetTexel(page, x, y);
}

bool DC_Renderer_HasTransparency(
    const int32_t page, const int32_t x0, const int32_t y0, const int32_t x1,
    const int32_t y1)
{
    return DC_PVR_HasTransparency(page, x0, y0, x1, y1);
}

void DC_Renderer_CommitTexturePages(void)
{
    DC_PVR_CommitTexturePages();
}

void DC_Renderer_FreeTexturePages(void)
{
    DC_PVR_FreeTexturePages();
}

int32_t DC_Renderer_GetTexturePageCount(void)
{
    return DC_PVR_GetTexturePageCount();
}

bool DC_Renderer_IsLowRes(void)
{
    return DC_PVR_IsLowRes();
}

REGISTER_SUBSYSTEM(.shutdown = M_Shutdown)
