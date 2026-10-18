// Dreamcast output: the part of trx/game/output/common.c that brings the
// output up and down and frames each scene, with the PowerVR in place of
// OpenGL. Which sources make up the scene, and what they hear about level
// changes, is the same as on the desktop.

#include "pvr.h"

#include <platform/dreamcast/renderer.h>
#include <trx/config.h>
#include <trx/core/log.h>
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

static MESH_BATCHER *m_Batcher = nullptr;
static OUTPUT_UNIFORMS *m_Uniforms = nullptr;

static int32_t m_DrawnFrames = 0;
static int32_t m_FPS = 0;
static CLOCK_TIMER m_FPSTimer = { .type = CLOCK_TIMER_REAL };

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

void Output_BeginScene(void)
{
    Output_ApplyFOV();
    const RGBA_F background = Output_GetBackgroundColor();
    DC_PVR_SetBackgroundColor(background.r, background.g, background.b);
    DC_PVR_BeginFrame();
    SceneCompositor_BeginScene();
    Output_Lights_BeginScene();
}

#if defined(TRX_DC_WATCHDOG)
// The frame rate and polygon count go to the log every few seconds.
static void M_LogFrameStats(void)
{
    static int32_t frame = 0;
    if (++frame % 120 == 0) {
        LOG_DEBUG(
            "frame %d: %d polygons, %d fps", frame,
            DC_PVR_GetSubmittedCount(), m_FPS);
    }
}
#endif

void Output_EndScene(void)
{
    SceneCompositor_EndScene();
#if defined(TRX_DC_WATCHDOG)
    M_LogFrameStats();
#endif
    DC_PVR_EndFrame();
    m_DrawnFrames++;
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
    Output_SetFogColor(Level_GetFogColor());
    Output_SetBackgroundColor(Level_GetBackgroundColor());
    Output_SetFogStart(Level_GetFogStart() * WALL_L);
    Output_SetFogEnd(Level_GetFogEnd() * WALL_L);
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

uint16_t *DC_Renderer_ReserveTexturePages(const int32_t count)
{
    return DC_PVR_ReserveTexturePages(count);
}

void DC_Renderer_WriteTexels(
    uint16_t *const dst, const uint16_t *const src, const size_t count)
{
    DC_PVR_WriteTexels(dst, src, count);
}

void DC_Renderer_CommitTexturePages(
    const int32_t count, const uint8_t *const formats)
{
    DC_PVR_CommitTexturePages(count, formats);
}

void DC_Renderer_FreeTexturePages(void)
{
    DC_PVR_FreeTexturePages();
}

int32_t DC_Renderer_GetTexturePageCount(void)
{
    return DC_PVR_GetTexturePageCount();
}

REGISTER_SUBSYSTEM(.shutdown = M_Shutdown)
