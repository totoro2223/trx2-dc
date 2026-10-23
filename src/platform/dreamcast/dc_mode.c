// Dreamcast platform layer: the graphics / performance modes (see dc_mode.h).

#include "dc_mode.h"

#include <trx/config.h>
#include <trx/version.h>

static const DC_MODE_PARAMS m_Modes[DC_MODE_NUMBER_OF] = {
    // The fog is the same in every mode (Output_ApplyLevelSettings). Past 16
    // blocks the Great Wall's views once handed the PowerVR more polygons
    // than it kept: Lara, drawn last, lost her head and guns there.
    [DC_MODE_GRAPHICS] = {
        .shadow_lights = 2,
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
        .shadow_lights = 2,
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

int32_t g_DC_TestMode = -1;

const DC_MODE_PARAMS *DC_Mode_Get(void)
{
    const DC_MODE mode = g_Config.rendering.dc_mode;
    return &m_Modes[mode >= 0 && mode < DC_MODE_NUMBER_OF ? mode : 0];
}

// Below about 30 frames a second there is no frame between two ticks to
// interpolate, and the interpolation only adds to the cost of each one.
#define M_SLOW_SECONDS 0.033
#define M_FAST_SECONDS 0.020
#define M_HOLD_FRAMES 240

static bool m_Slow = false;
static double m_Average = 0.0;
static int32_t m_Hold = 0;

int32_t DC_Mode_GetFPS(void)
{
    const int32_t fps = DC_Mode_Get()->fps;
    return fps == 60 && m_Slow ? 30 : fps;
}

bool DC_Mode_ReportFrame(const double seconds)
{
    if (DC_Mode_Get()->fps != 60 || !g_Config.rendering.dc_adaptive_fps) {
        m_Slow = false;
        return false;
    }
    m_Average += (seconds - m_Average) * 0.1;
    if (m_Hold > 0) {
        m_Hold--;
        return false;
    }
    const bool slow = m_Slow ? m_Average > M_FAST_SECONDS
                             : m_Average > M_SLOW_SECONDS;
    if (slow == m_Slow) {
        return false;
    }
    m_Slow = slow;
    m_Hold = M_HOLD_FRAMES;
    return true;
}

int32_t DC_Mode_GetShadowLights(void)
{
    // TR3's levels are busier and its look has one shadow under Lara.
    return g_TRVersion >= 3 ? 1 : DC_Mode_Get()->shadow_lights;
}
