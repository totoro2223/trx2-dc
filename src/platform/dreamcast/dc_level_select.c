// Dreamcast platform layer: a level select shown before the engine starts.
//
// Drawn straight into the frame buffer in the BIOS font, before the renderer
// owns the screen, and read from the first controller. Three pages, right
// and left between them: the levels (the first entry starts the game as
// it goes in production: the intro, the title screen with its copyright and
// the main menu), the in-game cutscenes in story order, and the title's
// demos with the benchmarks. Up and down pick, A or Start goes.

#include "dc_level_select.h"

#include "bfont_small.h"
#include "dc_bench.h"
#include "dc_games.h"
#include "dc_strict.h"

#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/video.h>
#include <kos/thread.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Each game's levels in the order of its gameflow (data/trx/ship/games/tr2
// tr1, tr2 and tr3).
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
static const char *const m_LevelsTR1[] = {
    "Title screen",
    "Lara's Home",
    "Caves",
    "City of Vilcabamba",
    "Lost Valley",
    "Tomb of Qualopec",
    "St. Francis' Folly",
    "Colosseum",
    "Palace Midas",
    "The Cistern",
    "Tomb of Tihocan",
    "City of Khamoon",
    "Obelisk of Khamoon",
    "Sanctuary of the Scion",
    "Natla's Mines",
    "Atlantis",
    "The Great Pyramid",
};
// The expansions' levels, in their gameflows' order (data/trx/ship/games/
// tr1-ub, tr2-gm and tr3-la).
static const char *const m_LevelsUB[] = {
    "Title screen",
    "Return to Egypt",
    "Temple of the Cat",
    "Atlantean Stronghold",
    "The Hive",
};
static const char *const m_LevelsGM[] = {
    "Title screen",
    "The Cold War",
    "Fool's Gold",
    "Furnace of the Gods",
    "Kingdom",
    "Nightmare in Vegas",
};
static const char *const m_LevelsLA[] = {
    "Title screen",
    "Highland Fling",
    "Willard's Lair",
    "Shakespeare Cliff",
    "Sleeping with the Fishes",
    "It's a Madhouse!",
    "Reunion",
};
static const char *const m_NoEntries[] = { "(none)" };

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
static const char *const m_CutscenesTR1[] = {
    "After Tomb of Qualopec (cut1)",
    "After Tomb of Tihocan (cut2)",
    "After Natla's Mines (cut3)",
    "The Great Pyramid (cut4)",
};
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
static const char *const m_DemosTR1[] = {
    "Demo: City of Vilcabamba",
    "Demo: Lost Valley",
    "Benchmark: Vilcabamba demo",
};
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
#define M_DEMO_COUNT_TR23 3
#define M_DEMO_COUNT_TR1 2
static const int32_t m_BenchDemo[] = { 0 };

#define M_LEN(a) ((int32_t)(sizeof(a) / sizeof(a[0])))

typedef enum {
    M_PAGE_LEVELS,
    M_PAGE_CUTSCENES,
    M_PAGE_DEMOS,
    // The expansion's levels, on a disc that has it (not inside it).
    M_PAGE_EXPANSION,
    M_PAGE_MAX,
} M_PAGE_ID;

typedef struct {
    const char *title;
    const char *const *names;
    int32_t count;
    int32_t selected;
} M_PAGE;

static bool m_LTrigHeld = false;
static bool m_RTrigHeld = false;
static M_PAGE m_Pages[M_PAGE_MAX] = {};
static int32_t m_PageCount = M_PAGE_EXPANSION;

// Rows of entries on screen at once at most; the list scrolls past them.
#define M_ROWS 15

// The screen as it is now: at 320x240 (a game of the disc ran there before)
// the text is the BIOS font at half size, and the layout is as 640x480's
// halved. The font never has to be scaled by the screen.
static bool M_IsLowRes(void)
{
    return vid_mode != nullptr && vid_mode->width < 640;
}

static int32_t M_Width(void)
{
    return vid_mode != nullptr ? vid_mode->width : 640;
}

static int32_t M_Height(void)
{
    return vid_mode != nullptr ? vid_mode->height : 480;
}

static int32_t M_LineHeight(void)
{
    return M_IsLowRes() ? DC_BFONT_SMALL_HEIGHT : BFONT_HEIGHT;
}

static int32_t M_CharWidth(void)
{
    return M_IsLowRes() ? DC_BFONT_SMALL_WIDTH : BFONT_THIN_WIDTH;
}

static void M_Text(const int32_t x, const int32_t y, const char *const text)
{
    const int32_t width = M_Width();
    if (M_IsLowRes()) {
        DC_BFontSmall_DrawStr(
            vram_s + y * width + x, (uint32_t)width, 0xFFFF, 0x0000, true,
            text);
    } else {
        bfont_draw_str(vram_s + y * width + x, (uint32_t)width, true, text);
    }
}

// A box in the style of the game's own notices: a dark panel with a green
// frame, text inside, its right edge or bottom edge at the given spot.
static void M_DrawBox(const int32_t right, const int32_t y, const char *const text)
{
    const int32_t pad = M_IsLowRes() ? 4 : 8;
    const int32_t w = (int32_t)strlen(text) * M_CharWidth() + 2 * pad;
    const int32_t h = M_LineHeight() + pad;
    const int32_t x = right - w;
    for (int32_t j = 0; j < h; j++) {
        for (int32_t i = 0; i < w; i++) {
            const bool edge = i < 2 || j < 2 || i >= w - 2 || j >= h - 2;
            vram_s[(y + j) * M_Width() + x + i] = edge ? 0x07E0 : 0x0120;
        }
    }
    M_Text(x + pad, y + pad / 2, text);
}

static void M_DrawGames(void)
{
    const int32_t margin = M_IsLowRes() ? 8 : 16;
    char text[40];
    if (DC_Games_PrevId() != 0) {
        snprintf(text, sizeof(text), "Press L to go to TR%d", DC_Games_PrevId());
        M_DrawBox(M_Width() - margin, margin / 2, text);
    }
    if (DC_Games_NextId() != 0) {
        snprintf(text, sizeof(text), "Press R to go to TR%d", DC_Games_NextId());
        M_DrawBox(
            M_Width() - margin,
            M_Height() - margin - M_LineHeight() - margin / 2, text);
    }
}

static void M_Draw(const int32_t engine, const M_PAGE_ID page_id)
{
    const M_PAGE *const page = &m_Pages[page_id];
    vid_clear(0, 0, 0);
    const int32_t left = M_IsLowRes() ? 32 : 64;
    const int32_t line_height = M_LineHeight();
    const int32_t top = 8 + 2 * line_height + (M_IsLowRes() ? 4 : 16);
    char header[64];
    snprintf(
        header, sizeof(header), "TRX totoro222 - TR%d - %s (%d/%d)",
        (int)engine, page->title, (int)page_id + 1, (int)m_PageCount);
    M_Text(left, 8, header);
    M_Text(left, 8 + line_height, "A: start   left/right: page");
    int32_t rows = (M_Height() - top - 8) / line_height;
    if (rows > M_ROWS) {
        rows = M_ROWS;
    }
    int32_t first = page->selected - rows / 2;
    if (first > page->count - rows) {
        first = page->count - rows;
    }
    if (first < 0) {
        first = 0;
    }
    for (int32_t i = first; i < first + rows && i < page->count; i++) {
        char line[48];
        snprintf(
            line, sizeof(line), "%c %s", i == page->selected ? '>' : ' ',
            page->names[i]);
        M_Text(left, top + (i - first) * line_height, line);
    }
    M_DrawGames();
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

int32_t DC_LevelSelect_Run(const int32_t engine, const char *const mod)
{
    DC_Crumb(DC_CRUMB_SELECTOR, "level list");
    const bool expansion = mod != nullptr && mod[0] != '\0';
    const bool tr3 = engine == 3;
    const bool tr1 = engine == 1;
    const int32_t demo_count = tr1 ? M_DEMO_COUNT_TR1 : M_DEMO_COUNT_TR23;
    m_Pages[M_PAGE_LEVELS] = (M_PAGE) {
        .title = "Levels",
        .names = tr1 ? m_LevelsTR1 : tr3 ? m_LevelsTR3 : m_LevelsTR2,
        .count = tr1 ? M_LEN(m_LevelsTR1)
            : tr3    ? M_LEN(m_LevelsTR3)
                     : M_LEN(m_LevelsTR2),
    };
    if (expansion) {
        // An expansion's levels, and no cutscenes or demos of its own.
        m_Pages[M_PAGE_LEVELS].names =
            tr1 ? m_LevelsUB : tr3 ? m_LevelsLA : m_LevelsGM;
        m_Pages[M_PAGE_LEVELS].count = tr1 ? M_LEN(m_LevelsUB)
            : tr3                          ? M_LEN(m_LevelsLA)
                                           : M_LEN(m_LevelsGM);
    }
    m_Pages[M_PAGE_CUTSCENES] = (M_PAGE) {
        .title = "Cutscenes",
        .names = tr1 ? m_CutscenesTR1 : tr3 ? m_CutscenesTR3 : m_CutscenesTR2,
        .count = tr1 ? M_LEN(m_CutscenesTR1)
            : tr3    ? M_LEN(m_CutscenesTR3)
                     : M_LEN(m_CutscenesTR2),
    };
    m_Pages[M_PAGE_DEMOS] = (M_PAGE) {
        .title = "Demos and benchmarks",
        .names = tr1 ? m_DemosTR1 : tr3 ? m_DemosTR3 : m_DemosTR2,
        .count = tr1 ? M_LEN(m_DemosTR1)
            : tr3    ? M_LEN(m_DemosTR3)
                     : M_LEN(m_DemosTR2),
    };
    if (expansion) {
        m_Pages[M_PAGE_CUTSCENES].names = m_NoEntries;
        m_Pages[M_PAGE_CUTSCENES].count = 1;
        m_Pages[M_PAGE_DEMOS].names = m_NoEntries;
        m_Pages[M_PAGE_DEMOS].count = 1;
    }
    // The expansion's levels, where this game's folder holds it.
    const bool has_expansion = !expansion && DC_Games_ExpansionAvailable();
    m_PageCount = has_expansion ? M_PAGE_MAX : M_PAGE_EXPANSION;
    if (has_expansion) {
        m_Pages[M_PAGE_EXPANSION] = (M_PAGE) {
            .title = "Expansion levels",
            .names = tr1 ? m_LevelsUB : tr3 ? m_LevelsLA : m_LevelsGM,
            .count = tr1 ? M_LEN(m_LevelsUB)
                : tr3    ? M_LEN(m_LevelsLA)
                         : M_LEN(m_LevelsGM),
        };
    }
    M_PAGE_ID page_id = M_PAGE_LEVELS;
    M_Draw(engine, page_id);
    // A test disc picks an entry by itself (make_disc_all.sh's SELPICK.TXT:
    // the page and the entry).
    bool picked = false;
    {
        char path[96];
        DC_Games_Path(path, sizeof(path), "SELPICK.TXT");
        FILE *const pick = fopen(path, "r");
        if (pick != nullptr) {
            int page = 0;
            int entry = 0;
            if (fscanf(pick, "%d %d", &page, &entry) == 2 && page >= 0
                && page < m_PageCount && entry >= 0
                && entry < m_Pages[page].count) {
                page_id = (M_PAGE_ID)page;
                m_Pages[page].selected = entry;
                picked = true;
            }
            fclose(pick);
        }
    }
    // Buttons count from their press, so one held from the boot does nothing.
    uint32_t held = M_Buttons();
    while (true) {
        thd_sleep(16);
        // Waiting for the player is not a hang.
        DC_Crumb_Beat();
        if (picked) {
            break;
        }
        const uint32_t buttons = M_Buttons();
        const uint32_t pressed = buttons & ~held;
        held = buttons;
        if (pressed & (CONT_A | CONT_START)) {
            break;
        }
        // The triggers go to the game before and after on a disc of several
        // (the console starts over in it).
        maple_device_t *const pad = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        const cont_state_t *const pad_state =
            pad != nullptr ? maple_dev_status(pad) : nullptr;
        if (pad_state != nullptr && (pressed & CONT_START) == 0) {
            const bool left = pad_state->ltrig > 128 && m_LTrigHeld == false;
            const bool right = pad_state->rtrig > 128 && m_RTrigHeld == false;
            m_LTrigHeld = pad_state->ltrig > 128;
            m_RTrigHeld = pad_state->rtrig > 128;
            if ((left && DC_Games_Choose(-1, false))
                || (right && DC_Games_Choose(1, false))) {
                vid_clear(0, 0, 0);
                return DC_LEVEL_SELECT_RESTART;
            }
        }
        M_PAGE *const page = &m_Pages[page_id];
        const int32_t step = (pressed & CONT_DPAD_DOWN) ? 1
            : (pressed & CONT_DPAD_UP)                  ? -1
                                                        : 0;
        if (step != 0) {
            page->selected = (page->selected + step + page->count) % page->count;
            M_Draw(engine, page_id);
        } else if ((pressed & CONT_DPAD_RIGHT) && page_id + 1 < m_PageCount) {
            page_id++;
            M_Draw(engine, page_id);
        } else if ((pressed & CONT_DPAD_LEFT) && page_id > 0) {
            page_id--;
            M_Draw(engine, page_id);
        }
    }
    vid_clear(0, 0, 0);
    const int32_t selected = m_Pages[page_id].selected;
    if (expansion && page_id != M_PAGE_LEVELS) {
        return -1;
    }
    switch (page_id) {
    case M_PAGE_EXPANSION:
        // Starts the disc over inside the expansion, in the level.
        DC_Games_ChooseExpansion(selected > 0 ? selected : -1);
        return DC_LEVEL_SELECT_RESTART;
    case M_PAGE_CUTSCENES:
        g_DC_StartCutscene = selected;
        return -1;
    case M_PAGE_DEMOS:
        g_DC_BenchRequested = selected >= demo_count;
        g_DC_StartDemo = selected >= demo_count
            ? m_BenchDemo[selected - demo_count]
            : selected;
        return -1;
    default:
        // An expansion has no gym: its levels are numbered from 1, as the
        // entries are (0 is the title).
        if (expansion) {
            return selected > 0 ? selected : -1;
        }
        // Entry 0 is the title; the rest are levels 0 and on.
        return selected - 1;
    }
}
