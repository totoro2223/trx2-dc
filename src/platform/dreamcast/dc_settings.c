// Dreamcast platform layer: the render options, kept on the memory card.
//
// The engine writes its settings file beside the game's data, which here is
// the disc: nothing it writes lasts. These few options (the ones the
// Rendering page of the menu holds) go into a small file of their own on the
// RAM disk, which the memory card keeps like a saved game (vmu.c), so that
// the console remembers them when switched off. They are loaded as the engine
// starts, where the main menu begins, and written to the card as the main
// menu ends, once, and only if they changed: a write takes the game's time.

#include "dc_settings.h"

#include "dc_mode.h"
#include "vmu.h"

#include <trx/config.h>
#include <trx/config/common.h>
#include <trx/core/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M_FILE "dcopts.txt"

static char m_Path[160] = "";
// What the card holds now, as the file's text.
static char m_Stored[256] = "";

// The options, as a line of numbers: the file format, the mode, the frame rate,
// the cel shading switch, a 0 (where the adaptive switch was until format 4),
// the fps counter, the two filters, the shadow type, and the bump mapping and
// room cel shading switches.
static void M_Format(char *const out, const size_t size)
{
    snprintf(
        out, size, "4 %d %d %d 0 %d %d %d %d %d %d\n",
        (int)g_Config.rendering.dc_mode, (int)g_Config.rendering.dc_fps,
        (int)g_Config.rendering.dc_cel_shading,
        (int)g_Config.ui.enable_fps_counter,
        (int)g_Config.rendering.texture_filter,
        (int)g_Config.rendering.ui_filter, (int)g_Config.visuals.shadow_type,
        (int)g_Config.rendering.dc_bump_mapping,
        (int)g_Config.rendering.dc_room_cel);
}

void DC_Settings_Load(const char *const saves_dir)
{
    snprintf(m_Path, sizeof(m_Path), "%s/%s", saves_dir, M_FILE);
    FILE *const fp = fopen(m_Path, "r");
    if (fp == nullptr) {
        // Never saved: the options the starting mode picks.
        DC_Mode_ApplyDefaults();
        return;
    }
    int version = 0;
    int v[10] = {};
    const int read = fscanf(
        fp, "%d %d %d %d %d %d %d %d %d %d %d", &version, &v[0], &v[1], &v[2],
        &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    fclose(fp);
    // The first file format had no bump mapping switch, and the first two no
    // room cel shading one.
    if (version == 1) {
        v[8] = 1;
    }
    if (version <= 2) {
        v[9] = 0;
    }
    // Before format 4 the 60 fps and adaptive switches were two, and the
    // Original mode came in before Experimental 60's.
    if (version >= 1 && version <= 3) {
        v[1] = v[1] == 0 ? DC_FPS_30
            : v[3] != 0  ? DC_FPS_60_ADAPTIVE
                         : DC_FPS_60;
        if (v[0] == DC_MODE_ORIGINAL) {
            v[0] = DC_MODE_EXPERIMENTAL_60;
        }
    }
    if (version < 1 || version > 4
        || read != (version >= 3 ? 11 : 8 + version) || v[0] < 0
        || v[0] >= DC_MODE_NUMBER_OF || v[1] < 0 || v[1] >= DC_FPS_NUMBER_OF
        || v[5] < 0 || v[5] >= TEXTURE_FILTER_NUMBER_OF || v[6] < 0
        || v[6] >= TEXTURE_FILTER_NUMBER_OF || v[7] < 0
        || v[7] >= SHADOW_TYPE_NUMBER_OF) {
        LOG_WARNING("the saved render options are not readable; ignored");
        return;
    }
    // The mode first, and its change told at once: that sets the options it
    // picks (DC_Mode_ApplyDefaults), which those the card kept then overrule.
    CONFIG_SET(g_Config.rendering.dc_mode, (DC_MODE)v[0]);
    Config_Update();
    CONFIG_SET(g_Config.rendering.dc_fps, (DC_FPS)v[1]);
    CONFIG_SET(g_Config.rendering.dc_cel_shading, v[2] != 0);
    CONFIG_SET(g_Config.ui.enable_fps_counter, v[4] != 0);
    CONFIG_SET(g_Config.rendering.texture_filter, (TEXTURE_FILTER)v[5]);
    CONFIG_SET(g_Config.rendering.ui_filter, (TEXTURE_FILTER)v[6]);
    CONFIG_SET(g_Config.visuals.shadow_type, (SHADOW_TYPE)v[7]);
    CONFIG_SET(g_Config.rendering.dc_bump_mapping, v[8] != 0);
    CONFIG_SET(g_Config.rendering.dc_room_cel, v[9] != 0);
    M_Format(m_Stored, sizeof(m_Stored));
    LOG_INFO("render options loaded from the memory card");
}

void DC_Settings_Save(void)
{
    if (m_Path[0] == '\0') {
        return;
    }
    char now[256];
    M_Format(now, sizeof(now));
    if (strcmp(now, m_Stored) == 0) {
        return;
    }
    FILE *const fp = fopen(m_Path, "w");
    if (fp == nullptr) {
        return;
    }
    const bool ok = fputs(now, fp) >= 0;
    fclose(fp);
    if (ok && DC_VMU_StoreSave(m_Path, "TRX options")) {
        snprintf(m_Stored, sizeof(m_Stored), "%s", now);
        LOG_INFO("render options written to the memory card");
    }
}
