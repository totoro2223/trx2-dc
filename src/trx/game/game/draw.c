#include <trx/config.h>
#include <trx/game/camera.h>
#include <trx/game/cutseq.h>
#include <trx/game/interpolation.h>
#include <trx/game/lua/events/scene.h>
#include <trx/game/output.h>
#include <trx/game/ui/draw.h>

// TEMP: see platform/dreamcast/dc_probe.h
#if defined(__DREAMCAST__)
    #include <platform/dreamcast/dc_probe.h>
#else
    #define DC_PROBE(i, stmt) stmt
    #define DC_PROBE_START(t)
    #define DC_PROBE_STOP(i, t)
#endif

void Game_Draw(const bool draw_overlay)
{
    DC_PROBE_START(probe_pre);
    Interpolation_Interpolate();
    CutSeq_PreDraw();
    Camera_Apply();
    Output_FlushPendingFog();
    DC_PROBE_STOP(DC_PROBE_S_PRE, probe_pre);
    Room_DrawAllRooms(g_Camera.interp.room_num, g_Camera.target.room_num);
    DC_PROBE(DC_PROBE_S_PAINT, LUA_Scene_Paint());
    if (draw_overlay && !CutSeq_IsActive()) {
        UI_DrawBinocularMask();
    }
    CutSeq_DrawOverlay();
    Output_Overlay_DrawLetterbox();
    DC_PROBE(DC_PROBE_S_GFLUSH, SceneCompositor_Flush());
    if (g_Config.visuals.enable_reflections) {
        Output_Textures_UpdateEnvironmentMap();
    }
}
