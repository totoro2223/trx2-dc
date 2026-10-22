// Dreamcast loading screen. The disc driver tells it of every read (see
// trx_iso_read_observer in kos/fs_iso9660.c); the bytes read against those
// the load is expected to read make the percentage, drawn with the name of
// the file being read, at most so often that loading is not held back.

#include "loading.h"

#include "render/pvr.h"

#include <arch/timer.h>

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
    uint64_t last_draw;
} m_Priv = {};

// Written on the disc by tools/dreamcast/make_disc.sh.
static const char *M_GetVersion(void)
{
    static char version[40] = "";
    if (version[0] == '\0') {
        FILE *const fp = fopen("/cd/VERSION.TXT", "r");
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
    const int32_t percent = M_GetPercent();
    m_Priv.last_draw = now;
    m_Priv.shown_percent = percent;
    char line1[96];
    snprintf(
        line1, sizeof(line1), "LOADING  %ld%%   %s", (long)percent,
        M_GetVersion());
    DC_PVR_DrawLoadingScreen(line1, m_Priv.name, percent);
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
    M_GetVersion();
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
    M_Draw(true);
}

void DC_Loading_End(void)
{
    if (!m_Priv.active) {
        return;
    }
    trx_iso_read_observer = nullptr;
    m_Priv.done = m_Priv.total;
    strncpy(m_Priv.name, "", sizeof(m_Priv.name));
    char line1[96];
    snprintf(line1, sizeof(line1), "LOADING  100%%   %s", M_GetVersion());
    DC_PVR_DrawLoadingScreen(line1, m_Priv.name, 100);
    // Its video memory goes back to the level (the title menu reads its
    // picture again).
    DC_PVR_SetPicture(nullptr);
    m_Priv.active = false;
}
