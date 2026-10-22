#pragma once

// TEMP (TAREAS.md §1): timers that split the "logic" time of a frame. The
// sums go out with the [frame] line (render/output_common.c). Remove when the
// measuring is done.

#include <arch/timer.h>
#include <stdint.h>

enum {
    DC_PROBE_EVENTS, // Shell_ProcessEvents
    DC_PROBE_LUA, // every Lua event fired (M_FireEvent), nested in the rest
    DC_PROBE_REMEMBER, // Interpolation_Remember
    DC_PROBE_INPUT, // Input_Update .. Camera_Binoculars_Control
    DC_PROBE_ITEMS, // Item_Control
    DC_PROBE_FX, // Effect_Control + Sparks_Control
    DC_PROBE_LARA, // Lara_Control + Lara_Hair_Control
    DC_PROBE_POST, // Game_TickPostControl
    DC_PROBE_END, // Game_TickEndFrame
    DC_PROBE_CONTROL, // Game_Control, whole
    DC_PROBE_MCONTROL, // executor M_Control, whole
    DC_PROBE_FLIP, // Output_FlipScreen
    DC_PROBE_LOG, // the [frame] line itself (serial)
    DC_PROBE_TEX_CYCLE, // Output_CycleAnimatedTextures
    DC_PROBE_TEX_UVW, // Output_Textures_CycleAnimations
    DC_PROBE_TEX_COMP, // SceneCompositor_AnimateTextures
    // Batcher: vertices per path of M_TransformInstance (counts, not time)...
    DC_PROBE_XF_LIT,
    DC_PROBE_XF_PLAIN,
    DC_PROBE_XF_GENERIC,
    // ...and time in the extra passes of M_DrawInstance.
    DC_PROBE_SOFT, // M_EmitSoftShadows / M_EmitSelfShadow
    DC_PROBE_EMIT, // the opaque + punch-through M_EmitIndices
    DC_PROBE_CEL, // the cel shading pass
    DC_PROBE_BUMP, // M_EmitBump
    DC_PROBE_ENV, // the reflections pass
    // The "stage" part of a frame (everything drawn outside the passes).
    DC_PROBE_S_SCENE, // Output_BeginScene + viewport switches + EndScene
    DC_PROBE_S_PRE, // Interpolate, CutSeq_PreDraw, Camera_Apply, fog
    DC_PROBE_S_BOUNDS, // M_GetBounds: portal traversal
    DC_PROBE_S_SKY, // M_DrawSkybox
    DC_PROBE_S_ROOMGEO, // Output_DrawRoom
    DC_PROBE_S_STATICS, // static meshes of the rooms
    DC_PROBE_S_ITEMS, // items of the rooms
    DC_PROBE_S_LARA, // Lara_Draw
    DC_PROBE_S_FX, // effects, FX, sparks, ropes, lens flares
    DC_PROBE_S_PAINT, // LUA_Scene_Paint
    DC_PROBE_S_GFLUSH, // SceneCompositor_Flush in Game_Draw (has passes)
    DC_PROBE_S_UI, // UI_BeginScene .. UI_Draw
    DC_PROBE_S_OFLUSH, // Output_Flush (has passes)
    DC_PROBE_S_ROOMS, // rooms drawn (count, not time)
    // Inside Lara_Draw.
    DC_PROBE_L_LIGHT, // M_CalculateLight
    DC_PROBE_L_SHADOW, // OutputSource_Shadows_Draw
    DC_PROBE_L_MESH, // Output_DrawObjectMesh(_I) of her meshes
    DC_PROBE_L_WATER, // water observe/push and the matrix copies before it
    DC_PROBE_L_JOINTS, // Lara_Joints_StashMatrix + Lara_Joints_Draw
    DC_PROBE_L_HAIR, // Lara_Hair_Draw
    DC_PROBE_L_EQUIP, // M_DrawEquipment
    // Time in each M_Xform* path.
    DC_PROBE_XT_ROOM,
    DC_PROBE_XT_PLAIN,
    DC_PROBE_XT_GENERIC,
    // Vertices of the generic path by why (counts): own light, reflections,
    // coloured fog, billboards, the rest.
    DC_PROBE_XG_OWN,
    DC_PROBE_XG_ENV,
    DC_PROBE_XG_FOG,
    DC_PROBE_XG_BILL,
    DC_PROBE_XG_OTHER,
    // Faces through M_EmitIndices (counts): quads out whole, triangles out,
    // clipped at the near plane, all outside one side, facing away.
    DC_PROBE_E_QUAD,
    DC_PROBE_E_TRI,
    DC_PROBE_E_CLIP,
    DC_PROBE_E_OUT,
    DC_PROBE_E_BACK,
    // Inside DC_Shadow_RenderMaps: the silhouettes, and the frame ended.
    DC_PROBE_M_SIL,
    DC_PROBE_M_END,
    DC_PROBE_COUNT,
};

extern uint32_t g_DC_ProbeUs[DC_PROBE_COUNT];

// Per Lua event type: time and number of fires (index = LUA_EVENT_TYPE).
#define DC_PROBE_LUA_EVENTS 96
extern uint32_t g_DC_LuaEvUs[DC_PROBE_LUA_EVENTS];
extern uint32_t g_DC_LuaEvN[DC_PROBE_LUA_EVENTS];

#define DC_PROBE(i, stmt)                                                      \
    do {                                                                       \
        const uint64_t _probe_t0 = timer_us_gettime64();                       \
        stmt;                                                                  \
        g_DC_ProbeUs[i] += (uint32_t)(timer_us_gettime64() - _probe_t0);       \
    } while (0)

// For spans with commas in them, which DC_PROBE cannot take.
#define DC_PROBE_START(t) const uint64_t t = timer_us_gettime64()
#define DC_PROBE_STOP(i, t)                                                    \
    (g_DC_ProbeUs[i] += (uint32_t)(timer_us_gettime64() - (t)))
