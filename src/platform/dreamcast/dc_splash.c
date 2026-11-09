// Dreamcast platform layer: the credits screen shown once, when the disc
// starts, before the level list. Drawn into the frame buffer like the list
// is, in the game's own font (loading_font.c) where the disc carries it and
// in the BIOS font otherwise. Switching games does not show it again.

#include "dc_splash.h"

#include "bfont_small.h"
#include "dc_games.h"
#include "dc_strict.h"
#include "render/loading_font.h"

#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/video.h>
#include <kos/thread.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define M_SHOW_MS 8000
#define M_ROW_HEIGHT 48 // room for a line at twice the font's size

// An empty string is a gap. The first line is the title.
static const char *m_Lines[] = {
    "TOMB RAIDER ORIGINAL TRILOGY",
    "",
    "Based on TRX by LostArtefacts",
    "and contributors",
    "Dreamcast port using KallistiOS",
    "",
    "Tomb Raider (c) 1996-1998",
    "Core Design Ltd.",
    "Published by Eidos Interactive",
    "Trademarks belong to their owners",
    "Not affiliated with them",
    "",
    "Free software under the GNU GPL v3",
    "You may redistribute and modify it",
    "Distributed WITHOUT ANY WARRANTY",
};

static uint32_t M_Buttons(void)
{
    maple_device_t *const pad = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    const cont_state_t *const state =
        pad != nullptr ? maple_dev_status(pad) : nullptr;
    return state != nullptr ? state->buttons : 0;
}

// One line, centred at y: the game's font through a scratch row (ARGB4444 to
// the frame buffer's RGB565), or the BIOS's if the disc has no font.
static void M_DrawLine(
    const int game, uint16_t *const row, const int32_t y, const char *const text,
    const int32_t wanted_scale)
{
    const int32_t width = vid_mode->width;
    const bool low = width < 640;
    const int32_t scale =
        DC_LoadingFont_Scale(game, width - 16, wanted_scale, text, text);
    memset(row, 0, (size_t)width * M_ROW_HEIGHT * sizeof(uint16_t));
    const int32_t used = DC_LoadingFont_Draw(
        game, row, width, width, M_ROW_HEIGHT, text, scale);
    if (used < 0) {
        const int32_t chars = (int32_t)strlen(text);
        const int32_t x =
            (width - chars * (low ? DC_BFONT_SMALL_WIDTH : BFONT_THIN_WIDTH))
            / 2;
        uint16_t *const at = vram_s + y * width + (x > 0 ? x : 0);
        if (low) {
            DC_BFontSmall_DrawStr(at, (uint32_t)width, 0xFFFF, 0x0000, true, text);
        } else {
            bfont_draw_str(at, (uint32_t)width, true, text);
        }
        return;
    }
    const int32_t x0 = (width - used) / 2;
    for (int32_t j = 0; j < M_ROW_HEIGHT && y + j < (int32_t)vid_mode->height;
         j++) {
        for (int32_t i = 0; i < used; i++) {
            const uint16_t t = row[j * width + i];
            if ((t >> 12) < 8) {
                continue;
            }
            const uint32_t r = ((t >> 8) & 15) * 17;
            const uint32_t g = ((t >> 4) & 15) * 17;
            const uint32_t b = (t & 15) * 17;
            vram_s[(y + j) * width + x0 + i] =
                (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

void DC_Splash_Show(const int32_t engine)
{
    static bool shown = false;
    if (shown) {
        return;
    }
    shown = true;
    if (vid_mode == nullptr) {
        return;
    }
    const bool low = vid_mode->width < 640;
    uint16_t *const row =
        malloc((size_t)vid_mode->width * M_ROW_HEIGHT * sizeof(uint16_t));
    if (row == nullptr) {
        return;
    }
    DC_LoadingFont_Load(engine);
    vid_clear(0, 0, 0);

    const int32_t pitch = low ? 13 : 22;
    const int32_t count = (int32_t)(sizeof(m_Lines) / sizeof(m_Lines[0]));
    // The title is twice the size and takes two rows; a gap is half a row.
    int32_t total = 0;
    for (int32_t i = 0; i < count; i++) {
        total += i == 0 && !low ? pitch * 2
            : m_Lines[i][0] == '\0' ? pitch / 2
                                    : pitch;
    }
    int32_t y = (vid_mode->height - total) / 2;
    for (int32_t i = 0; i < count; i++) {
        if (m_Lines[i][0] != '\0') {
            M_DrawLine(engine, row, y, m_Lines[i], i == 0 && !low ? 2 : 1);
        }
        y += i == 0 && !low ? pitch * 2
            : m_Lines[i][0] == '\0' ? pitch / 2
                                    : pitch;
    }
    free(row);

    // Any of A or Start leaves it; so does the time. A button held from the
    // boot does nothing.
    uint32_t held = M_Buttons();
    for (int32_t waited = 0; waited < M_SHOW_MS; waited += 16) {
        thd_sleep(16);
        DC_Crumb_Beat();
        const uint32_t buttons = M_Buttons();
        if ((buttons & ~held) & (CONT_A | CONT_START)) {
            break;
        }
        held = buttons;
    }
    vid_clear(0, 0, 0);
}
