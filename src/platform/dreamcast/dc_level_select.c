// Dreamcast platform layer: a level select shown before the engine starts.
//
// Drawn straight into the frame buffer in the BIOS font, before the renderer
// owns the screen, and read from the first controller. Three pages, right
// and left between them: the levels (the first entry starts the game as
// it goes in production: the intro, the title screen with its copyright and
// the main menu), the in-game cutscenes in story order, and the title's
// demos with the benchmarks. Up and down pick, A or Start goes.

#include "dc_level_select.h"

#include "dc_bench.h"

#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/video.h>
#include <kos/thread.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// Each game's levels in the order of its gameflow (data/trx/ship/games/tr2
// and tr3).
static const char *const m_LevelsTR2[] = {
    "Normal flow (logos, title, menu)",
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
static const char *const m_LevelsTR3[] = {
    "Normal flow (logos, title, menu)",
    "Lara's Home",
    "Jungle",
    "Temple Ruins",
    "The River Ganges",
    "Caves of Kaliya",
    "Coastal Village",
    "Crash Site",
    "Madubu Gorge",
    "Temple of Puna",
    "Thames Wharf",
    "Aldwych",
    "Lud's Gate",
    "City",
    "Nevada Desert",
    "High Security Compound",
    "Area 51",
    "Antarctica",
    "RX-Tech Mines",
    "Lost City of Tinnos",
    "Meteorite Cavern",
    "All Hallows",
};

// The gameflow's cutscenes, which its levels play in this order: the entry
// is the cutscene's number, named by the level that plays it.
static const char *const m_CutscenesTR2[] = {
    "After The Great Wall (cut1)",
    "After Opera House (cut2)",
    "After Diving Area (cut3)",
    "After Temple of Xian (cut4)",
};
static const char *const m_CutscenesTR3[] = {
    "After Jungle (cut6)",
    "After Temple Ruins (cut9)",
    "After Coastal Village (cut1)",
    "After Crash Site (cut4)",
    "After Thames Wharf (cut2)",
    "After Aldwych (cut5)",
    "After Lud's Gate (cut11)",
    "After Nevada Desert (cut7)",
    "After High Security Compound (cut8)",
    "After Antarctica (cut3)",
    "After City (cut12)",
};

// The title's demos in the gameflow's order, then the benchmarks: a demo run
// whole and measured (dc_bench.h), named by the demo it runs.
static const char *const m_DemosTR2[] = {
    "Demo: Venice",
    "Demo: Wreck of the Maria Doria",
    "Demo: Tibetan Foothills",
    "Benchmark: Venice demo",
};
static const char *const m_DemosTR3[] = {
    "Demo: Aldwych",
    "Demo: The River Ganges",
    "Demo: Nevada Desert",
    "Benchmark: London (Aldwych demo)",
};
// How many of the entries above are demos, and which demo each benchmark
// after them runs.
#define M_DEMO_COUNT 3
static const int32_t m_BenchDemo[] = { 0 };

#define M_LEN(a) ((int32_t)(sizeof(a) / sizeof(a[0])))

typedef enum {
    M_PAGE_LEVELS,
    M_PAGE_CUTSCENES,
    M_PAGE_DEMOS,
    M_PAGE_COUNT,
} M_PAGE_ID;

typedef struct {
    const char *title;
    const char *const *names;
    int32_t count;
    int32_t selected;
} M_PAGE;

static M_PAGE m_Pages[M_PAGE_COUNT] = {};

// Rows of entries on screen at once; the list scrolls past them.
#define M_ROWS 15
#define M_LEFT 64
#define M_TOP 72

static void M_Draw(const int32_t engine, const M_PAGE_ID page_id)
{
    const M_PAGE *const page = &m_Pages[page_id];
    vid_clear(0, 0, 0);
    char header[64];
    snprintf(
        header, sizeof(header), "TRX totoro222 - TR%d - %s (%d/%d)",
        (int)engine, page->title, (int)page_id + 1, M_PAGE_COUNT);
    bfont_draw_str(vram_s + 8 * 640 + M_LEFT, 640, true, header);
    bfont_draw_str(
        vram_s + (8 + BFONT_HEIGHT) * 640 + M_LEFT, 640, true,
        "A: start   left/right: page");
    int32_t first = page->selected - M_ROWS / 2;
    if (first > page->count - M_ROWS) {
        first = page->count - M_ROWS;
    }
    if (first < 0) {
        first = 0;
    }
    for (int32_t i = first; i < first + M_ROWS && i < page->count; i++) {
        char line[48];
        snprintf(
            line, sizeof(line), "%c %s", i == page->selected ? '>' : ' ',
            page->names[i]);
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

int32_t DC_LevelSelect_Run(const int32_t engine)
{
    const bool tr3 = engine == 3;
    m_Pages[M_PAGE_LEVELS] = (M_PAGE) {
        .title = "Levels",
        .names = tr3 ? m_LevelsTR3 : m_LevelsTR2,
        .count = tr3 ? M_LEN(m_LevelsTR3) : M_LEN(m_LevelsTR2),
    };
    m_Pages[M_PAGE_CUTSCENES] = (M_PAGE) {
        .title = "Cutscenes",
        .names = tr3 ? m_CutscenesTR3 : m_CutscenesTR2,
        .count = tr3 ? M_LEN(m_CutscenesTR3) : M_LEN(m_CutscenesTR2),
    };
    m_Pages[M_PAGE_DEMOS] = (M_PAGE) {
        .title = "Demos and benchmarks",
        .names = tr3 ? m_DemosTR3 : m_DemosTR2,
        .count = tr3 ? M_LEN(m_DemosTR3) : M_LEN(m_DemosTR2),
    };
    M_PAGE_ID page_id = M_PAGE_LEVELS;
    M_Draw(engine, page_id);
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
        M_PAGE *const page = &m_Pages[page_id];
        const int32_t step = (pressed & CONT_DPAD_DOWN) ? 1
            : (pressed & CONT_DPAD_UP)                  ? -1
                                                        : 0;
        if (step != 0) {
            page->selected = (page->selected + step + page->count) % page->count;
            M_Draw(engine, page_id);
        } else if ((pressed & CONT_DPAD_RIGHT) && page_id + 1 < M_PAGE_COUNT) {
            page_id++;
            M_Draw(engine, page_id);
        } else if ((pressed & CONT_DPAD_LEFT) && page_id > 0) {
            page_id--;
            M_Draw(engine, page_id);
        }
    }
    vid_clear(0, 0, 0);
    const int32_t selected = m_Pages[page_id].selected;
    switch (page_id) {
    case M_PAGE_CUTSCENES:
        g_DC_StartCutscene = selected;
        return -1;
    case M_PAGE_DEMOS:
        g_DC_BenchRequested = selected >= M_DEMO_COUNT;
        g_DC_StartDemo = selected >= M_DEMO_COUNT
            ? m_BenchDemo[selected - M_DEMO_COUNT]
            : selected;
        return -1;
    default:
        // Entry 0 is the title; the rest are levels 0 and on.
        return selected - 1;
    }
}
