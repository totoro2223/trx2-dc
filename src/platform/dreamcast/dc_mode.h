#pragma once

// What the renderer spends a frame on, by rendering.dc_mode: what each mode
// allows at most.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    // Lara's shadows: how many lights cast them, whether soft shadows are
    // drawn as shadow volumes instead, and whether Lara shadows herself.
    int32_t shadow_lights;
    bool volume_shadows;
    bool self_shadow;
    // Whether the cel shading may be drawn (rendering.dc_cel_shading turns
    // it off).
    bool cel_shading;
    // Whether the room faces near the camera take relief (bump mapping).
    bool bump_mapping;
    // Frames drawn a second; TRX interpolates the world above 30.
    int32_t fps;
    // 320x240 instead of 640x480, from the next level load on.
    bool low_res;
    // The world's textures filtered bilinearly (rendering.texture_filter
    // decides the rest).
    bool bilinear;
} DC_MODE_PARAMS;

// The mode a test disc starts in whatever the settings say (MODE.TXT), or -1.
extern int32_t g_DC_TestMode;
// A test disc's shadow type, whatever the mode picks (-1 for none).
extern int32_t g_DC_TestShadow;
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
// A test disc hands Lara this weapon (a LARA_GUN_TYPE) a moment into the level.
extern int32_t g_DC_TestGive;
// A test disc leaves the game this many frames into the level (the console
// then starts the disc over).
extern int32_t g_DC_TestExit;
// A test disc saves the game to the next slot every this many frames, and
// loads the first one back after each fifth (0: never).
extern int32_t g_DC_TestSave;
// How many games have run since the console started.
extern int32_t g_DC_Run;

const DC_MODE_PARAMS *DC_Mode_Get(void);

// The rate the game runs at: the mode's, except that a 60 fps mode falls back
// to 30 while the scene is too heavy to draw even that.
int32_t DC_Mode_GetFPS(void);

// Tells how long a frame took to run and draw, excluding its wait. Returns
// true when that moved the game between 30 and 60.
bool DC_Mode_ReportFrame(double seconds);

// How many lights cast Lara's shadow: the mode's, and a single one in TR3.
int32_t DC_Mode_GetShadowLights(void);
