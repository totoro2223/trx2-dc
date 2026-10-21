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
