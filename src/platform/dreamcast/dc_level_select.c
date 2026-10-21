// Dreamcast platform layer: a level select shown before the engine starts.
//
// Drawn straight into the frame buffer in the BIOS font, before the renderer
// owns the screen, and read from the first controller: up and down pick, A or
// Start goes. The first entry starts the game as usual, at the title.

#include "dc_level_select.h"

#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/video.h>
#include <kos/thread.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// TR2's levels in the order of its gameflow (data/trx/ship/games/tr2).
static const char *const m_Names[] = {
    "Title screen",
    "Lara's Home",
    "The Great Wall",
    "Venice",
    "Bartoli's Hideout",
    "Opera House",
    "Offshore Rig",
    "Diving Area",
    "40 Fathoms",
    "Wreck of the Maria Doria",
    "Living Quarters",
    "The Deck",
    "Tibetan Foothills",
    "Barkhang Monastery",
    "Catacombs of the Talion",
    "Ice Palace",
    "Temple of Xian",
    "Floating Islands",
    "The Dragon's Lair",
    "Home Sweet Home",
};
#define M_COUNT ((int32_t)(sizeof(m_Names) / sizeof(m_Names[0])))
// Rows of entries on screen at once; the list scrolls past them.
#define M_ROWS 16
#define M_LEFT 64
#define M_TOP 48

static void M_Draw(const int32_t selected)
{
    vid_clear(0, 0, 0);
    bfont_draw_str(
        vram_s + 8 * 640 + M_LEFT, 640, true,
        "TRX totoro222 - select a level (A)");
    int32_t first = selected - M_ROWS / 2;
    if (first > M_COUNT - M_ROWS) {
        first = M_COUNT - M_ROWS;
    }
    if (first < 0) {
        first = 0;
    }
    for (int32_t i = first; i < first + M_ROWS && i < M_COUNT; i++) {
        char line[48];
        snprintf(
            line, sizeof(line), "%c %s", i == selected ? '>' : ' ', m_Names[i]);
        const int32_t y = M_TOP + (i - first) * BFONT_HEIGHT;
        bfont_draw_str(vram_s + y * 640 + M_LEFT, 640, true, line);
    }
}

static uint32_t M_Buttons(void)
{
    maple_device_t *const pad = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (pad == nullptr) {
        return 0;
    }
    const cont_state_t *const state = maple_dev_status(pad);
    return state != nullptr ? state->buttons : 0;
}

int32_t DC_LevelSelect_Run(void)
{
    int32_t selected = 0;
    M_Draw(selected);
    // Buttons count from their press, so one held from the boot does nothing.
    uint32_t held = M_Buttons();
    while (true) {
        thd_sleep(16);
        const uint32_t buttons = M_Buttons();
        const uint32_t pressed = buttons & ~held;
        held = buttons;
        if (pressed & (CONT_A | CONT_START)) {
            break;
        }
        const int32_t step = (pressed & CONT_DPAD_DOWN) ? 1
            : (pressed & CONT_DPAD_UP)                  ? -1
                                                        : 0;
        if (step != 0) {
            selected = (selected + step + M_COUNT) % M_COUNT;
            M_Draw(selected);
        }
    }
    vid_clear(0, 0, 0);
    // Entry 0 is the title; the rest are levels 0 and on.
    return selected - 1;
}
