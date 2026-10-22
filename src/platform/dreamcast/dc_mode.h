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

const DC_MODE_PARAMS *DC_Mode_Get(void);

// The rate the game runs at: the mode's, except that a 60 fps mode falls back
// to 30 while the scene is too heavy to draw even that.
int32_t DC_Mode_GetFPS(void);

// Tells how long a frame took to run and draw, excluding its wait. Returns
// true when that moved the game between 30 and 60.
bool DC_Mode_ReportFrame(double seconds);

// How many lights cast Lara's shadow: the mode's, and a single one in TR3.
int32_t DC_Mode_GetShadowLights(void);
