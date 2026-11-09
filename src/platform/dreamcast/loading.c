// Dreamcast loading screen. The disc driver tells it of every read (see
// trx_iso_read_observer in kos/fs_iso9660.c); the bytes read against those
// the load is expected to read make the percentage, drawn with the name of
// the file being read, at most so often that loading is not held back.

#include "loading.h"

#include <trx/config.h>
#include "dc_games.h"
#include "dc_strict.h"

#include "render/loading_font.h"
#include "render/pvr.h"
#include "vmu.h"

#include <trx/game/console/common.h>
#include <trx/version.h>

#include <arch/timer.h>
#include <kos/thread.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define M_REDRAW_INTERVAL_MS 100

extern void (*trx_iso_read_observer)(const char *name, size_t bytes);

static struct {
    bool active;
    uint64_t total;
    uint64_t done;
    char name[32];
    int32_t shown_percent;
    float shown; // what the bar shows: it follows the load, it does not jump
    uint64_t last_draw;
} m_Priv = {};

// How far the bar moves each frame, in percent (about 100% a second at 60 Hz).
#define M_BAR_STEP 1.5f

// Written on the disc by tools/dreamcast/make_disc.sh.
static const char *M_GetVersion(void)
{
    static char version[40] = "";
    if (version[0] == '\0') {
        char path[64];
        DC_Games_Path(path, sizeof(path), "VERSION.TXT");
        FILE *const fp = fopen(path, "r");
        if (fp == nullptr || fgets(version, sizeof(version), fp) == nullptr) {
            strcpy(version, "DC build ?");
        }
        if (fp != nullptr) {
            fclose(fp);
        }
        version[strcspn(version, "\r\n")] = '\0';
    }
    return version;
}

const char *DC_Loading_GetVersion(void)
{
    return M_GetVersion();
}

static int32_t M_GetPercent(void)
{
    if (m_Priv.total == 0) {
        return 0;
    }
    // Held short of the end until the load says it is done: files read
    // twice or not foreseen would otherwise run past it.
    const uint64_t percent = m_Priv.done * 100 / m_Priv.total;
    return percent > 99 ? 99 : (int32_t)percent;
}

static void M_Draw(const bool force)
{
    const uint64_t now = timer_ms_gettime64();
    if (!force && now - m_Priv.last_draw < M_REDRAW_INTERVAL_MS) {
        return;
    }
    // shortcut: the bar moves only as often as the disc is read (at most every
    // M_REDRAW_INTERVAL_MS); to move on every frame it would need a thread that
    // draws while the load computes.
    // The bar runs to the new value in a few frames, a straight line, rather
    // than jumping there (one frame at least, to show the step's name).
    const float target = (float)M_GetPercent();
    int32_t percent = (int32_t)m_Priv.shown;
    do {
        m_Priv.shown = m_Priv.shown + M_BAR_STEP < target
            ? m_Priv.shown + M_BAR_STEP
            : (target > m_Priv.shown ? target : m_Priv.shown);
        percent = (int32_t)m_Priv.shown;
        char line1[96];
        snprintf(
            line1, sizeof(line1), "LOADING  %ld%%   %s", (long)percent,
            M_GetVersion());
        // Only the bar, unless the debug mode is on.
        DC_PVR_DrawLoadingScreen(
            g_Config.rendering.dc_memory_hud ? line1 : "",
            g_Config.rendering.dc_memory_hud ? m_Priv.name : "", percent);
    } while (m_Priv.shown < target);
    m_Priv.last_draw = timer_ms_gettime64();
    m_Priv.shown_percent = percent;
    DC_VMU_ShowLoading(percent);
}

static void M_OnRead(const char *const name, const size_t bytes)
{
    if (!m_Priv.active) {
        return;
    }
    m_Priv.done += bytes;
    if (strcmp(m_Priv.name, name) != 0) {
        strncpy(m_Priv.name, name, sizeof(m_Priv.name) - 1);
        m_Priv.name[sizeof(m_Priv.name) - 1] = '\0';
        M_Draw(true);
        return;
    }
    M_Draw(false);
}

void DC_Loading_Begin(const uint64_t total_bytes, const char *const picture)
{
    // Read before the disc reads are watched: reading it would draw again.
    DC_Crumb(DC_CRUMB_LOADING, "loading");
    DC_Asm_Save(true);
    DC_Crumb_ShowPrevious();
    M_GetVersion();
    DC_LoadingFont_Load(g_TRVersion);
    DC_PVR_SetPicture(picture);
    m_Priv = (typeof(m_Priv)) {
        .active = true,
        .total = total_bytes,
    };
    trx_iso_read_observer = M_OnRead;
    M_Draw(true);
}

void DC_Loading_SetStep(const char *const step)
{
    if (!m_Priv.active) {
        return;
    }
    strncpy(m_Priv.name, step, sizeof(m_Priv.name) - 1);
    m_Priv.name[sizeof(m_Priv.name) - 1] = '\0';
    DC_Asm_Save(false);
    M_Draw(true);
}

void DC_Loading_Notice(
    const char *const title, const char *const text, const int32_t hold_ms)
{
    DC_PVR_SetPicture(nullptr);
    DC_PVR_DrawLoadingScreen(title, text, 100);
    if (hold_ms > 0) {
        thd_sleep(hold_ms);
    }
}

void DC_Loading_SaveDone(const bool saved)
{
    // In the box the notice before it was in, as the passport says it.
    const int result = DC_VMU_LastResult();
    DC_QuickSave_Say(
        !saved || result == 2 ? "Could not save to the VMU"
            : result == 3     ? "No VMU found: saved until the console is switched off"
                              : "Saved to the VMU",
        60);
}

void DC_Loading_End(void)
{
    if (!m_Priv.active) {
        return;
    }
    trx_iso_read_observer = nullptr;
    DC_Crumb(DC_CRUMB_LOADED, "loaded");
    DC_Asm_Save(false);
    m_Priv.done = m_Priv.total;
    strncpy(m_Priv.name, "", sizeof(m_Priv.name));
    // The bar runs on to the end instead of jumping there.
    for (int32_t at = (int32_t)m_Priv.shown + 10; at < 100; at += 10) {
        DC_PVR_DrawLoadingScreen("", "", at);
    }
    char line1[96];
    snprintf(line1, sizeof(line1), "LOADING  100%%   %s", M_GetVersion());
    DC_PVR_DrawLoadingScreen(
        g_Config.rendering.dc_memory_hud ? line1 : "",
        g_Config.rendering.dc_memory_hud ? m_Priv.name : "", 100);
    // Its video memory goes back to the level (the title menu reads its
    // picture again).
    DC_PVR_SetPicture(nullptr);
    // The level's font, for the next loads' text.
    DC_LoadingFont_Capture(g_TRVersion);
    m_Priv.active = false;
    // The VMU goes back to its logo; the level's screens redraw themselves.
    DC_VMU_ShowLoading(-1);
    DC_VMU_DrawLogo();
}
