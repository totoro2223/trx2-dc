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

// The options, as a line of numbers: the mode, the 60 fps and cel shading and
// adaptive switches, the fps counter, the two filters and the shadow type.
static void M_Format(char *const out, const size_t size)
{
    snprintf(
        out, size, "1 %d %d %d %d %d %d %d %d\n",
        (int)g_Config.rendering.dc_mode, (int)g_Config.rendering.dc_60fps,
        (int)g_Config.rendering.dc_cel_shading,
        (int)g_Config.rendering.dc_adaptive_fps,
        (int)g_Config.ui.enable_fps_counter,
        (int)g_Config.rendering.texture_filter,
        (int)g_Config.rendering.ui_filter, (int)g_Config.visuals.shadow_type);
}

void DC_Settings_Load(const char *const saves_dir)
{
    snprintf(m_Path, sizeof(m_Path), "%s/%s", saves_dir, M_FILE);
    FILE *const fp = fopen(m_Path, "r");
    if (fp == nullptr) {
        return; // never saved: the options stay as they start
    }
    int version = 0;
    int v[8] = {};
    const int read = fscanf(
        fp, "%d %d %d %d %d %d %d %d %d", &version, &v[0], &v[1], &v[2], &v[3],
        &v[4], &v[5], &v[6], &v[7]);
    fclose(fp);
    if (read != 9 || version != 1 || v[0] < 0 || v[0] >= DC_MODE_NUMBER_OF
        || v[5] < 0 || v[5] >= TEXTURE_FILTER_NUMBER_OF || v[6] < 0
        || v[6] >= TEXTURE_FILTER_NUMBER_OF || v[7] < 0
        || v[7] >= SHADOW_TYPE_NUMBER_OF) {
        LOG_WARNING("the saved render options are not readable; ignored");
        return;
    }
    CONFIG_SET(g_Config.rendering.dc_mode, (DC_MODE)v[0]);
    CONFIG_SET(g_Config.rendering.dc_60fps, v[1] != 0);
    CONFIG_SET(g_Config.rendering.dc_cel_shading, v[2] != 0);
    CONFIG_SET(g_Config.rendering.dc_adaptive_fps, v[3] != 0);
    CONFIG_SET(g_Config.ui.enable_fps_counter, v[4] != 0);
    CONFIG_SET(g_Config.rendering.texture_filter, (TEXTURE_FILTER)v[5]);
    CONFIG_SET(g_Config.rendering.ui_filter, (TEXTURE_FILTER)v[6]);
    CONFIG_SET(g_Config.visuals.shadow_type, (SHADOW_TYPE)v[7]);
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
