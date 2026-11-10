#pragma once

// What the renderer spends a frame on, by rendering.dc_mode: what each mode
// fixes, and the options it sets as it is chosen, which the player may then
// change (DC_Mode_ApplyDefaults).

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    // Lara's shadows: how many lights cast them, and whether Lara shadows
    // herself.
    int32_t shadow_lights;
    bool self_shadow;
    // 320x240 instead of 640x480, from the next level load on.
    bool low_res;

    // Set as the mode is chosen: the shadow type (the game's own, the shadow
    // volumes, or the soft shadows), the frame rate (a DC_FPS), the cel
    // shading, the relief, and the bilinear texture filter.
    bool game_shadow;
    bool volume_shadows;
    int32_t fps;
    bool cel_shading;
    bool bump_mapping;
    bool room_cel;
    bool bilinear;
    // The native renderer (PLAN_60_FIJO.md): rooms from the OCRAM batches,
    // the logic on its own thread, the polygon cap.
    bool native;
} DC_MODE_PARAMS;

// The mode a test disc starts in whatever the settings say (MODE.TXT), or -1.
extern int32_t g_DC_TestMode;
// A test disc's shadow type, whatever the mode picks (-1 for none).
extern int32_t g_DC_TestShadow;
// A test disc that fixes the cel shading and the room cel shading (0 or 1;
// -1 leaves them to the mode).
// A test disc that caps the snowflakes/raindrops (make_disc.sh TRX_DC_SNOW, -1: no cap).
extern int32_t g_DC_TestSnow;
// Test discs (make_disc.sh TRX_DC_ASMOFF): the assembly routines to leave out,
// a bit each, so that one at fault can be told apart: 1 portal windows, 2
// coloured fog, 4 sight rays, 8 weather update, 16 weather draw, 32 effects.
extern uint32_t g_DC_AsmOff;
extern uint32_t g_DC_AsmOff2;
// A test disc's fog start and end in blocks (make_disc.sh TRX_DC_FOG), -1: the fixed ones.
extern int32_t g_DC_TestFogStart;
extern int32_t g_DC_TestFogEnd;
extern int32_t g_DC_TestCel;
// A test disc that fixes the frame rate (TRX_DC_FPS: 30 or 60).
extern int32_t g_DC_TestFps;
// 16:9 anamorphic forced on (1) or off (0) by a test disc; -1 leaves it.
extern int32_t g_DC_TestWide;
// A test disc that fixes the enemy simulation (TRX_DC_AI: 0 TRX, 1 original).
extern int32_t g_DC_TestOriginalAI;
extern int32_t g_DC_TestRoomCel;
extern int32_t g_DC_TestBump;
// A test disc with the object shadows on (make_disc.sh's TRX_DC_OBJECT_SHADOWS,
// 1), which the menu does not offer: its row cost the menu memory it lacks.
extern int32_t g_DC_TestObjectShadows;
// A test disc that deletes the render options from the memory card as it
// starts, and begins from the defaults (make_disc.sh's TRX_DC_RESETOPT,
// RESETOPT.TXT).
extern int32_t g_DC_TestResetOptions;
// A test disc that changes the mode while it runs, as the menu does: the mode
// and the frame to do it on (make_disc.sh's TRX_DC_SWITCH, SWITCH.TXT).
extern int32_t g_DC_TestSwitchMode;
extern int32_t g_DC_TestSwitchFrame;
// A test disc that goes through levels one after another: the last level and
// the frames to stay in each (make_disc.sh's TRX_DC_LEVELS, LEVELS.TXT).
extern int32_t g_DC_TestLevelStart;
extern int32_t g_DC_TestLevelLast;
extern int32_t g_DC_TestLevelFrames;
// A test disc fires the pistols' smoke every this many frames (0: never).
extern int32_t g_DC_TestFire;
// A test disc leaves the probe timers off the screen (they cost UI time).
extern int32_t g_DC_TestNoHud;
// A test disc shows the frame rate counter (TRX_DC_SHOWFPS).
extern int32_t g_DC_TestShowFps;
// A test disc where L cycles the on-screen readouts and R the graphic modes
// (TRX_DC_DEBUGKEYS); the game demos always do.
extern int32_t g_DC_TestDebugKeys;
// A test disc hands Lara this weapon (a LARA_GUN_TYPE) a moment into the level.
extern int32_t g_DC_TestGive;
// A test disc leaves the game this many frames into the level (the console
// then starts the disc over).
extern int32_t g_DC_TestExit;
// A test disc completes the level this many frames into it (0: never).
extern int32_t g_DC_TestComplete;
// A test disc saves the game to the next slot every this many frames, and
// loads the first one back after each fifth (0: never).
extern int32_t g_DC_TestSave;
// How many games have run since the console started.
extern int32_t g_DC_Run;

// Percent of the last second the SH-4 spent working, not waiting (sleeping or
// for the PowerVR). Kept up to date by the frame stats.
extern int32_t g_DC_CpuPercent;

const DC_MODE_PARAMS *DC_Mode_Get(void);

// Sets the options the mode picks (the frame rate, cel shading, relief, texture
// filter and shadow), as it is chosen.
void DC_Mode_ApplyDefaults(void);

// The rate the game runs at: rendering.dc_fps's, except that adaptive falls
// back to 30 while the scene is too heavy to draw even that.
int32_t DC_Mode_GetFPS(void);

// Tells how long a frame took to run and draw, excluding its wait. Returns
// true when that moved the game between 30 and 60.
bool DC_Mode_ReportFrame(double seconds);

// How many lights cast Lara's shadow: the mode's, and a single one in TR3.
int32_t DC_Mode_GetShadowLights(void);

// The native rooms (PLAN_60_FIJO.md): the chunks drawn batch by batch from
// a buffer in the half of the data cache kept as RAM. Set by the mode each
// frame (batcher.c).
extern bool g_DC_NativeRooms;
void DC_Batcher_SetNative(bool on);
