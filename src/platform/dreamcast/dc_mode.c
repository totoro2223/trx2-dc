// Dreamcast platform layer: the graphics / performance modes (see dc_mode.h).

#include "dc_mode.h"

#include <trx/config.h>

static const DC_MODE_PARAMS m_Modes[DC_MODE_NUMBER_OF] = {
    // The fog is the same in every mode (Output_ApplyLevelSettings). Past 16
    // blocks the Great Wall's views once handed the PowerVR more polygons
    // than it kept: Lara, drawn last, lost her head and guns there.
    [DC_MODE_GRAPHICS] = {
        .shadow_lights = 3,
        .self_shadow = true,
        .cel_shading = true,
        .bump_mapping = true,
        .fps = 30,
        .low_res = false,
        .bilinear = true,
    },
    // The shadows cut back until the busiest places of TR2's
    // first level hold 30 frames: volumes cost a fraction of the soft
    // shadows.
    [DC_MODE_PERFORMANCE] = {
        .shadow_lights = 3,
        .volume_shadows = true,
        .self_shadow = false,
        .cel_shading = true,
        .fps = 30,
        .low_res = false,
        .bilinear = true,
    },
    // shortcut: the SH-4 needs more than 16 ms a frame for TR2's levels even
    // so, and the fewer pixels only spare the PowerVR; the frame rate shakes
    // where it falls short.
    [DC_MODE_EXPERIMENTAL_60] = {
        .shadow_lights = 1,
        .volume_shadows = true,
        .self_shadow = false,
        .fps = 60,
        .low_res = true,
        .bilinear = false,
    },
};

const DC_MODE_PARAMS *DC_Mode_Get(void)
{
    const DC_MODE mode = g_Config.rendering.dc_mode;
    return &m_Modes[mode >= 0 && mode < DC_MODE_NUMBER_OF ? mode : 0];
}
