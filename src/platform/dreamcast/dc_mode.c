// Dreamcast platform layer: the graphics / performance modes (see dc_mode.h).

#include "dc_mode.h"

#include <trx/config.h>
#include <trx/config/common.h>
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
        .fps = DC_FPS_30,
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
        .fps = DC_FPS_30,
        .low_res = false,
        .bilinear = true,
    },
    // The game as the first discs drew it: its own shadow, unfiltered.
    [DC_MODE_ORIGINAL] = {
        .shadow_lights = 1,
        .game_shadow = true,
        .self_shadow = false,
        .fps = DC_FPS_30,
        .low_res = false,
        .bilinear = false,
    },
    // shortcut: the SH-4 needs more than 16 ms a frame for TR2's levels even
    // so, and the fewer pixels only spare the PowerVR; the frame rate shakes
    // where it falls short.
    [DC_MODE_EXPERIMENTAL_60] = {
        .shadow_lights = 1,
        .volume_shadows = true,
        .self_shadow = false,
        .fps = DC_FPS_60_ADAPTIVE,
        .low_res = true,
        .bilinear = false,
    },
};

int32_t g_DC_TestMode = -1;
int32_t g_DC_TestShadow = -1;
int32_t g_DC_TestSwitchMode = -1;
int32_t g_DC_TestSwitchFrame = 0;
int32_t g_DC_TestLevelStart = 0;
int32_t g_DC_TestLevelLast = -1;
int32_t g_DC_TestLevelFrames = 0;
int32_t g_DC_TestFire = 0;
int32_t g_DC_TestGive = 0;
int32_t g_DC_TestExit = 0;
int32_t g_DC_TestSave = 0;
int32_t g_DC_Run = 0;

const DC_MODE_PARAMS *DC_Mode_Get(void)
{
    const DC_MODE mode = g_Config.rendering.dc_mode;
    return &m_Modes[mode >= 0 && mode < DC_MODE_NUMBER_OF ? mode : 0];
}

void DC_Mode_ApplyDefaults(void)
{
    const DC_MODE_PARAMS *const mode = DC_Mode_Get();
    CONFIG_SET(g_Config.rendering.dc_fps, (DC_FPS)mode->fps);
    CONFIG_SET(g_Config.rendering.dc_cel_shading, mode->cel_shading);
    CONFIG_SET(g_Config.rendering.dc_bump_mapping, mode->bump_mapping);
    CONFIG_SET(
        g_Config.rendering.texture_filter,
        mode->bilinear ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
    // The game's own shadow: TR1 and TR2's octagon, TR3's sprite.
    SHADOW_TYPE shadow = mode->volume_shadows ? SHADOW_TYPE_VOLUME
        : !mode->game_shadow                  ? SHADOW_TYPE_SOFT_HIGH
        : g_TRVersion >= 3                    ? SHADOW_TYPE_SPRITE
                                              : SHADOW_TYPE_OCTAGON;
    if (g_DC_TestShadow >= 0 && g_DC_TestShadow < SHADOW_TYPE_NUMBER_OF) {
        shadow = (SHADOW_TYPE)g_DC_TestShadow;
    }
    CONFIG_SET(g_Config.visuals.shadow_type, shadow);
}

// Below about 30 frames a second there is no frame between two ticks to
// interpolate, and the interpolation only adds to the cost of each one.
#define M_SLOW_SECONDS 0.033
#define M_FAST_SECONDS 0.020
#define M_HOLD_FRAMES 240

static bool m_Slow = false;
static double m_Average = 0.0;
static int32_t m_Hold = 0;

// The rate asked for, whatever the mode.
static int32_t M_BaseFPS(void)
{
    return g_Config.rendering.dc_fps == DC_FPS_30 ? 30 : 60;
}

int32_t DC_Mode_GetFPS(void)
{
    return M_BaseFPS() == 60 && m_Slow ? 30 : M_BaseFPS();
}

bool DC_Mode_ReportFrame(const double seconds)
{
    if (g_Config.rendering.dc_fps != DC_FPS_60_ADAPTIVE) {
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
