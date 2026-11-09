// Dreamcast platform layer: the render options, kept on the memory card.
//
// The engine writes its settings file beside the game's data, which here is
// the disc: nothing it writes lasts. These few options (the ones the
// Rendering page of the menu holds) go into a small file of their own on the
// RAM disk, which the memory card keeps like a saved game (vmu.c), so that
// the console remembers them when switched off. They are loaded as the engine
// starts, and written to the card where a level, a menu or a demo is about to
// load and as the ring or the pause menu closes (which is where the options
// are changed), only if they changed: a write takes the game's time.

#include "dc_settings.h"

#include "dc_games.h"
#include "dc_mode.h"
#include "vmu.h"


#include <trx/config.h>
#include <trx/config/common.h>
#include <trx/core/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M_FILE "trdctriopt"

static char m_Path[160] = "";
// What the card holds now, as the file's text.
static char m_Stored[256] = "";
// What the card refused last: not tried again until the options change.
static char m_Refused[256] = "";

// The options, as a line of numbers: the file format, the mode, the frame rate,
// the cel shading switch, a 0 (where the adaptive switch was until format 4),
// the fps counter, the two filters, the shadow type, the bump mapping and
// room cel shading switches, the 3D pickups switch, the enemy shadows one, the
// memory readout, the load cache, the original enemy simulation one, and the
// original (linear) pose blend one, and the 16:9 screen one.
// A second line holds the game played last (0 on a disc of one game).
static void M_Format(char *const out, const size_t size, const int game)
{
    snprintf(
        out, size, "11 %d %d %d 0 %d %d %d %d %d %d %d %d %d %d %d %d %d\n%d\n",
        (int)g_Config.rendering.dc_mode, (int)g_Config.rendering.dc_fps,
        (int)g_Config.rendering.dc_cel_shading,
        (int)g_Config.ui.enable_fps_counter,
        (int)g_Config.rendering.texture_filter,
        (int)g_Config.rendering.ui_filter, (int)g_Config.visuals.shadow_type,
        (int)g_Config.rendering.dc_bump_mapping,
        (int)g_Config.rendering.dc_room_cel,
        (int)g_Config.visuals.enable_3d_pickups,
        (int)g_Config.rendering.dc_object_shadows,
        (int)g_Config.rendering.dc_memory_hud,
        (int)g_Config.rendering.dc_load_cache,
        (int)g_Config.rendering.dc_original_ai,
        (int)g_Config.rendering.dc_linear_pose,
        (int)g_DC_WidePending, game);
}

bool g_DC_WidePending = false;
static bool m_WideApplied = false;

void DC_Settings_Load(const char *const saves_dir)
{
    snprintf(m_Path, sizeof(m_Path), "%s/%s", saves_dir, M_FILE);
    if (g_DC_TestResetOptions) {
        // A test disc that starts from the defaults: the card's copy goes.
        remove(m_Path);
        DC_VMU_DeleteSave(m_Path);
        LOG_INFO("render options on the memory card deleted");
    }
    FILE *const fp = fopen(m_Path, "r");
    if (fp == nullptr) {
        // Never saved: the options the starting mode picks.
        DC_Mode_ApplyDefaults();
        return;
    }
    int version = 0;
    int v[17] = {};
    const int read = fscanf(
        fp, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &version,
        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9],
        &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16]);
    int stored_game = 0;
    if (fscanf(fp, " %d", &stored_game) != 1) {
        stored_game = 0;
    }
    fclose(fp);
    // The first file format had no bump mapping switch, and the first two no
    // room cel shading one.
    if (version == 1) {
        v[8] = 1;
    }
    if (version <= 2) {
        v[9] = 0;
    }
    // Before format 5 the pickups were always 3D.
    if (version <= 4) {
        v[10] = 1;
    }
    // Before format 6 the enemies cast no shadow volumes.
    if (version <= 5) {
        v[11] = 0;
    }
    // Before format 7 there was no memory use readout.
    if (version <= 6) {
        v[12] = 0;
    }
    // Before format 8 there was no load cache switch; it was always on.
    if (version <= 7) {
        v[13] = 1;
    }
    // Before format 9 there was only the TRX enemy simulation.
    if (version <= 8) {
        v[14] = 0;
    }
    // Before format 10 the poses were always blended through quaternions.
    if (version <= 9) {
        v[15] = 0;
    }
    // Before format 11 the screen was 4:3.
    if (version <= 10) {
        v[16] = 0;
    }
    // The adaptive rate (2) is gone: it is plain 60 now.
    if (v[1] == 2) {
        v[1] = DC_FPS_60;
    }
    // Before format 4 the 60 fps and adaptive switches were two, and the
    // Original mode came in before Experimental 60's.
    if (version >= 1 && version <= 3) {
        v[1] = v[1] == 0 ? DC_FPS_30
            : DC_FPS_60;
        if (v[0] == DC_MODE_ORIGINAL) {
            v[0] = DC_MODE_EXPERIMENTAL_60;
        }
    }
    if (version < 1 || version > 11
        || read != (version >= 11 ? 18 : version >= 10 ? 17 : version >= 9 ? 16 : version >= 8 ? 15 : version >= 7 ? 14 : version >= 6 ? 13 : version >= 5 ? 12 : version >= 3 ? 11 : 8 + version)
        || v[0] < 0
        || v[0] >= DC_MODE_NUMBER_OF || v[1] < 0 || v[1] >= DC_FPS_NUMBER_OF
        || v[5] < 0 || v[5] >= TEXTURE_FILTER_NUMBER_OF || v[6] < 0
        || v[6] >= TEXTURE_FILTER_NUMBER_OF || v[7] < 0
        || v[7] >= SHADOW_TYPE_NUMBER_OF) {
        // Whatever is wrong with them, the game goes on with the defaults.
        LOG_WARNING("the saved render options are not readable; ignored");
        DC_Mode_ApplyDefaults();
        return;
    }
    // The mode first, and its change told at once: that sets the options it
    // picks (DC_Mode_ApplyDefaults), which those the card kept then overrule.
    CONFIG_SET(g_Config.rendering.dc_mode, (DC_MODE)v[0]);
    Config_Update();
    CONFIG_SET(g_Config.rendering.dc_fps, (DC_FPS)v[1]);
    CONFIG_SET(g_Config.rendering.dc_cel_shading, (bool)(v[2] != 0));
    CONFIG_SET(g_Config.ui.enable_fps_counter, (bool)(v[4] != 0));
    CONFIG_SET(g_Config.rendering.texture_filter, (TEXTURE_FILTER)v[5]);
    CONFIG_SET(g_Config.rendering.ui_filter, (TEXTURE_FILTER)v[6]);
    CONFIG_SET(g_Config.visuals.shadow_type, (SHADOW_TYPE)v[7]);
    CONFIG_SET(g_Config.rendering.dc_bump_mapping, (bool)(v[8] != 0));
    CONFIG_SET(g_Config.rendering.dc_room_cel, (bool)(v[9] != 0));
    CONFIG_SET(g_Config.visuals.enable_3d_pickups, (bool)(v[10] != 0));
    // (Saved as on or off once: 1 is the volume.)
    CONFIG_SET(
        g_Config.rendering.dc_object_shadows,
        (DC_OBJECT_SHADOWS)(v[11] >= 0 && v[11] < DC_OBJECT_SHADOWS_NUMBER_OF
                                ? v[11]
                                : 0));
    CONFIG_SET(g_Config.rendering.dc_memory_hud, (bool)(v[12] != 0));
    CONFIG_SET(g_Config.rendering.dc_load_cache, (bool)(v[13] != 0));
    CONFIG_SET(g_Config.rendering.dc_original_ai, (bool)(v[14] != 0));
    CONFIG_SET(g_Config.rendering.dc_linear_pose, (bool)(v[15] != 0));
    // The screen shape is only applied at the next level load: changing it live distorts.
    g_DC_WidePending = v[16] != 0;
    if (!m_WideApplied) {
        m_WideApplied = true;
        CONFIG_SET(
            g_Config.rendering.aspect_mode,
            g_DC_WidePending ? ASPECT_MODE_16_9 : ASPECT_MODE_4_3);
    }
    if (g_DC_TestOriginalAI >= 0) {
        CONFIG_SET(
            g_Config.rendering.dc_original_ai, (bool)(g_DC_TestOriginalAI != 0));
    }
    // A test disc's shadows win over what the card holds.
    if (g_DC_TestShadow >= 0 && g_DC_TestShadow < SHADOW_TYPE_NUMBER_OF) {
        CONFIG_SET(g_Config.visuals.shadow_type, (SHADOW_TYPE)g_DC_TestShadow);
    }
    if (g_DC_TestObjectShadows >= 0) {
        CONFIG_SET(
            g_Config.rendering.dc_object_shadows,
            (DC_OBJECT_SHADOWS)(g_DC_TestObjectShadows
                                        < DC_OBJECT_SHADOWS_NUMBER_OF
                                    ? g_DC_TestObjectShadows
                                    : 0));
    }
    if (g_DC_TestFps >= 0) {
        CONFIG_SET(
            g_Config.rendering.dc_fps,
            g_DC_TestFps >= 60 ? DC_FPS_60 : DC_FPS_30);
    }
    if (g_DC_TestWide >= 0) {
        CONFIG_SET(
            g_Config.rendering.aspect_mode,
            g_DC_TestWide != 0 ? ASPECT_MODE_16_9 : ASPECT_MODE_4_3);
    }
    // As the card holds them, game included: a different game gets written.
    M_Format(m_Stored, sizeof(m_Stored), stored_game);
    LOG_INFO("render options loaded from the memory card");
}

void DC_Settings_Save(void)
{
    // A test disc that forces the mode or the shadows is not the player
    // choosing: those would stay on the card.
    if (m_Path[0] == '\0' || g_DC_TestMode >= 0 || g_DC_TestShadow >= 0
        || g_DC_TestCel >= 0 || g_DC_TestObjectShadows >= 0 || g_DC_TestOriginalAI >= 0 || g_DC_TestFps >= 0 || g_DC_TestBump >= 0
        || g_DC_TestWide >= 0) {
        return;
    }
    char now[256];
    M_Format(now, sizeof(now), DC_Games_CurrentId());
    if (strcmp(now, m_Stored) == 0 || strcmp(now, m_Refused) == 0) {
        return;
    }
    FILE *const fp = fopen(m_Path, "w");
    if (fp == nullptr) {
        return;
    }
    const bool ok = fputs(now, fp) >= 0;
    fclose(fp);
    if (ok && DC_VMU_StoreSmall(m_Path, "TRX options")) {
        snprintf(m_Stored, sizeof(m_Stored), "%s", now);
        LOG_INFO("render options written to the memory card");
    } else {
        // Most likely the card is full: they last until switched off.
        snprintf(m_Refused, sizeof(m_Refused), "%s", now);
        LOG_WARNING("render options could not be written to the memory card");
    }
}
