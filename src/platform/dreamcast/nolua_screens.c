// The screens the engine offers besides the ring's: pause, statistics,
// pictures and the level view (screens/*.lua), without Lua.
//
// shortcut: the globe, examine and combine are plain lists and boxes. The
// statistics list the same rows in one box, without the per-game looks or the
// assault course page.
#include "nolua.h"

#include <trx/config.h>
#include <trx/game/const.h>
#include <trx/game/game.h>
#include <trx/game/game_flow.h>
#include <trx/game/game_strings/entries.h>
#include <trx/game/input.h>
#include <trx/game/inventory.h>
#include <trx/game/objects/names.h>
#include <trx/game/lua/events/ui.h>
#include <trx/game/paths.h>
#include <trx/game/savegame/resume.h>
#include <trx/game/stats.h>
#include <trx/game/ui/draw.h>

#include <stdio.h>
#include <string.h>

#define M_FADE_TICKS 12

static const char *M_GS(const char *const key, const char *const fallback)
{
    const char *const text = GameString_Get(key);
    return text != nullptr && strcmp(text, key) != 0 ? text : fallback;
}

// --- Pause ---

typedef enum {
    M_PAUSE_IN,
    M_PAUSE_WAIT,
    M_PAUSE_ASK,
    M_PAUSE_SURE,
    M_PAUSE_OUT,
} M_PAUSE_STATE;

static struct {
    bool open;
    M_PAUSE_STATE state;
    int32_t ticks;
    UI_TAKEOVER_CHOICE leave;
    DC_REQ req;
} m_Pause;

static void M_PauseAsk(const M_PAUSE_STATE state)
{
    const bool sure = state == M_PAUSE_SURE;
    m_Pause.state = state;
    DC_Req_Init(
        &m_Pause.req,
        sure ? M_GS("general/pause/are_you_sure", "Are you sure?")
             : M_GS("general/pause/exit_to_title", "Exit to title?"),
        0.0f, 1.0f);
    DC_Req_Add(
        &m_Pause.req,
        sure ? M_GS("general/pause/yes", "Yes")
             : M_GS("general/pause/continue", "Continue"));
    DC_Req_Add(
        &m_Pause.req,
        sure ? M_GS("general/pause/no", "No")
             : M_GS("general/pause/quit", "Quit"));
}

static void M_PauseLeave(const UI_TAKEOVER_CHOICE choice)
{
    DC_NoLua_SetCaption(nullptr, true, false);
    m_Pause.leave = choice;
    m_Pause.state = M_PAUSE_OUT;
}

static void M_PauseControl(void)
{
    switch (m_Pause.state) {
    case M_PAUSE_IN:
        if (++m_Pause.ticks >= M_FADE_TICKS) {
            m_Pause.state = M_PAUSE_WAIT;
            DC_NoLua_SetCaption(
                M_GS("general/pause/paused", "Paused"), true, false);
        }
        break;
    case M_PAUSE_WAIT:
        if (g_InputDB.pause || (!Game_IsPlayable() && g_InputDB.option)) {
            M_PauseLeave(UI_TAKEOVER_CHOICE_RESUME);
        } else if (g_InputDB.option) {
            M_PauseAsk(M_PAUSE_ASK);
        }
        break;
    case M_PAUSE_ASK: {
        const int32_t picked = DC_Req_Control(&m_Pause.req);
        if (picked == DC_REQ_CANCELLED) {
            m_Pause.state = M_PAUSE_WAIT;
        } else if (picked == 0) {
            M_PauseLeave(UI_TAKEOVER_CHOICE_RESUME);
        } else if (picked == 1) {
            M_PauseAsk(M_PAUSE_SURE);
        }
        break;
    }
    case M_PAUSE_SURE: {
        const int32_t picked = DC_Req_Control(&m_Pause.req);
        if (picked == 0) {
            M_PauseLeave(UI_TAKEOVER_CHOICE_EXIT_TO_TITLE);
        } else if (picked == 1) {
            M_PauseLeave(UI_TAKEOVER_CHOICE_RESUME);
        } else if (picked == DC_REQ_CANCELLED) {
            M_PauseAsk(M_PAUSE_ASK);
        }
        break;
    }
    case M_PAUSE_OUT:
        if (--m_Pause.ticks <= 0) {
            DC_Screens_Close(UI_TAKEOVER_PAUSE, m_Pause.leave);
        }
        break;
    }
}

static bool M_PauseOpen(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    m_Pause = (typeof(m_Pause)) { .open = true };
    return true;
}

static void M_PauseRelease(const UI_TAKEOVER screen)
{
    m_Pause.open = false;
    DC_NoLua_SetCaption(nullptr, true, false);
}

static void M_PausePaint(void)
{
    const float progress = (float)m_Pause.ticks / M_FADE_TICKS;
    UI_ScheduleDrawBackdrop(g_Config.ui.pause_background_style, progress, nullptr);
    if (m_Pause.state == M_PAUSE_ASK || m_Pause.state == M_PAUSE_SURE) {
        DC_Req_Paint(&m_Pause.req);
    }
}

// --- Statistics ---

static struct {
    bool open;
    const GF_LEVEL *level;
    bool is_final;
    // Opened from the ring by the compass or the stopwatch, rather than at the
    // end of a level; a compass that shows nothing is put away on a key.
    bool from_ring;
    bool hidden;
    DC_REQ req;
} m_Stats;

static void M_StatsRow(const char *const name, const char *const value)
{
    DC_REQ_ROW *const row = DC_Req_Add(&m_Stats.req, name);
    if (row != nullptr) {
        snprintf(row->right, sizeof(row->right), "%s", value);
    }
}

static void M_StatsCount(
    const char *const name, const uint32_t count, const uint32_t max)
{
    char value[16];
    snprintf(value, sizeof(value), "%u/%u", (unsigned)count, (unsigned)max);
    M_StatsRow(name, value);
}

static void M_StatsFill(const STATS_COMMON *const s, const LEVEL_MAX_STATS *max)
{
    char value[16];
    const uint32_t secs = s->timer / LOGIC_FPS;
    snprintf(
        value, sizeof(value), "%02u:%02u:%02u", (unsigned)(secs / 3600),
        (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
    M_StatsRow(M_GS("general/stats/time_taken", "Time Taken"), value);
    M_StatsCount(
        M_GS("general/stats/secrets", "Secrets"), s->counts[STATS_CAT_SECRETS],
        max->maxes[STATS_CAT_SECRETS]);
    M_StatsCount(
        M_GS("general/stats/pickups", "Pickups"), s->counts[STATS_CAT_PICKUPS],
        max->maxes[STATS_CAT_PICKUPS]);
    M_StatsCount(
        M_GS("general/stats/kills", "Kills"), s->counts[STATS_CAT_KILLS],
        max->maxes[STATS_CAT_KILLS]);
    snprintf(
        value, sizeof(value), "%u/%u", (unsigned)s->ammo_hits,
        (unsigned)s->ammo_used);
    M_StatsRow(M_GS("general/stats/ammo", "Ammo Hits/Used"), value);
    snprintf(value, sizeof(value), "%.1f", s->medipacks_used);
    M_StatsRow(M_GS("general/stats/medipacks_used", "Health Packs Used"), value);
    // 445 units to the metre.
    snprintf(value, sizeof(value), "%um", (unsigned)(s->distance_travelled / 445));
    M_StatsRow(M_GS("general/stats/distance_travelled", "Distance Travelled"), value);
}

static bool M_StatsOpen(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    m_Stats = (typeof(m_Stats)) { .open = true };
    DC_Req_Init(
        &m_Stats.req,
        args->is_final ? M_GS("general/stats/final_statistics", "Final Statistics")
                       : args->level != nullptr ? args->level->title
                                                : "",
        290.0f, 1.0f);
    if (args->is_final) {
        const FINAL_STATS final = Stats_ComputeFinalStats(true);
        M_StatsFill(&final.stats, &final.max_stats);
    } else {
        const LEVEL_STATS *const stats = Stats_GetLevelStats(args->level);
        if (stats == nullptr) {
            return false;
        }
        LEVEL_MAX_STATS max = {};
        for (int32_t i = 0; i < STATS_CAT_NUMBER_OF; i++) {
            STATS_CATEGORY cat;
            if (Stats_GetCategory(args->level, (STATS_CATEGORY_ID)i, &cat)) {
                max.maxes[i] = cat.max;
            }
        }
        M_StatsFill((const STATS_COMMON *)stats, &max);
    }
    m_Stats.req.selected = -1;
    return true;
}

static void M_StatsRelease(const UI_TAKEOVER screen)
{
    m_Stats.open = false;
}

bool DC_NoLua_ReleaseStatsEntry(void)
{
    if (!m_Stats.open || !m_Stats.from_ring) {
        return false;
    }
    m_Stats.open = false;
    return true;
}

bool DC_NoLua_OpenStatsEntry(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    const GF_LEVEL *const level = GF_GetCurrentLevel();
    if (level == nullptr) {
        return false;
    }
    UI_TAKEOVER_ARGS own = *args;
    own.level = level;
    own.is_final = false;
    if (!M_StatsOpen(screen, &own)) {
        return false;
    }
    m_Stats.from_ring = true;
    m_Stats.hidden = args->object_id == O_COMPASS_OPTION
        && !g_Config.gameplay.enable_compass_stats;
    return true;
}

// --- Pictures and the level view ---

static struct {
    bool open;
    UI_TAKEOVER screen;
    const char *path;
    bool is_loading;
    bool can_cross_fade_in;
    int32_t fade_in;
    int32_t display;
    int32_t fade_out;
    int32_t ticks;
    bool leaving;
    bool drawn;
} m_Pic;

static bool M_PicOpen(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    m_Pic = (typeof(m_Pic)) {
        .open = true,
        .screen = screen,
        .path = args->background != nullptr
            ? GamePath_PeekResolve(
                GAME_DYNAMIC_PATH_IMAGE_FILE, args->background)
            : nullptr,
        .is_loading = args->is_loading,
        .fade_in = args->fade_in_ticks,
        .display = args->display_ticks,
        .fade_out = args->fade_out_ticks,
    };
    return true;
}

static void M_PicRelease(const UI_TAKEOVER screen)
{
    m_Pic.open = false;
}

static void M_PicEnd(void)
{
    if (m_Pic.screen == UI_TAKEOVER_PICTURE && m_Pic.is_loading) {
        DC_NoLua_LevelHandOver(m_Pic.fade_out);
    }
    DC_Screens_Close(m_Pic.screen, UI_TAKEOVER_CHOICE_CONFIRM);
}

static void M_PicControl(void)
{
    m_Pic.ticks++;
    if (!m_Pic.leaving) {
        if (m_Pic.ticks >= m_Pic.fade_in + m_Pic.display
            || g_InputDB.menu_skip || g_InputDB.menu_confirm) {
            m_Pic.leaving = true;
            m_Pic.ticks = 0;
        }
    } else if (m_Pic.is_loading ? m_Pic.drawn : m_Pic.ticks >= m_Pic.fade_out) {
        M_PicEnd();
    }
}

static void M_PicPaint(void)
{
    if (m_Pic.path != nullptr) {
        UI_ScheduleDrawBackground(m_Pic.path, false, 0.0f);
    }
    // A loading picture hands over to the level once it has been drawn.
    m_Pic.drawn = true;
    float dark = 0.0f;
    if (m_Pic.leaving && !m_Pic.is_loading) {
        dark = m_Pic.fade_out > 0 ? (float)m_Pic.ticks / m_Pic.fade_out : 1.0f;
    } else if (!m_Pic.leaving && m_Pic.fade_in > 0) {
        dark = 1.0f - (float)m_Pic.ticks / m_Pic.fade_in;
    }
    if (dark > 0.0f) {
        UI_ScheduleDrawDarken(dark > 1.0f ? 1.0f : dark);
    }
}

// --- Globe ---

// shortcut: a list of the areas that can be started next, not the spinning
// globe; upgrade when the globe model is drawn without Lua.
static struct {
    bool open;
    const char *path;
    int32_t level[MAX_GLOBE_ZONES];
    DC_REQ req;
} m_Globe;

static bool M_GlobeDone(const int32_t ordinal)
{
    const GF_LEVEL *const level = GF_GetLevel(GFLT_MAIN, ordinal);
    const RESUME_INFO *const resume =
        level != nullptr ? SG_Resume_GetEntry(level) : nullptr;
    return resume != nullptr && resume->level_completed;
}

static bool M_GlobeOpen(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    m_Globe = (typeof(m_Globe)) {
        .open = true,
        .path = args->background != nullptr
            ? GamePath_PeekResolve(
                GAME_DYNAMIC_PATH_IMAGE_FILE, args->background)
            : nullptr,
    };
    DC_Req_Init(&m_Globe.req, M_GS("general/globe_select/title", "Next Adventure"), 200.0f, 0.5f);
    const int32_t count = MIN(GF_GetGlobeEntryCount(), MAX_GLOBE_ZONES);
    int32_t rows = 0;
    for (int32_t i = 0; i < count; i++) {
        const GF_GLOBE_ENTRY *const e = GF_GetGlobeEntry(i);
        bool ready = e->start_level_ordinal > 0
            && GF_GetLevel(GFLT_MAIN, e->start_level_ordinal) != nullptr
            && !M_GlobeDone(e->completion_level_ordinal);
        for (int32_t bit = 0; bit < count; bit++) {
            if ((e->prereq_mask & (1u << bit)) != 0
                && !M_GlobeDone(
                    GF_GetGlobeEntry(bit)->completion_level_ordinal)) {
                ready = false;
            }
        }
        if (ready) {
            char key[40];
            snprintf(key, sizeof(key), "general/globe_select/area_%d", i + 1);
            char fallback[16];
            snprintf(fallback, sizeof(fallback), "Area %d", i + 1);
            DC_Req_Add(&m_Globe.req, M_GS(key, fallback));
            m_Globe.level[rows++] = e->start_level_ordinal;
        }
    }
    if (rows == 0) {
        return false;
    }
    return true;
}

static void M_GlobeRelease(const UI_TAKEOVER screen)
{
    m_Globe.open = false;
}

static void M_GlobeControl(void)
{
    const int32_t picked = DC_Req_Control(&m_Globe.req);
    if (picked >= 0) {
        GF_OverrideCommand(
            (GF_COMMAND) {
                .action = GF_START_GAME,
                .param = m_Globe.level[picked],
            },
            true);
        DC_Screens_Close(UI_TAKEOVER_GLOBE_SELECT, UI_TAKEOVER_CHOICE_CONFIRM);
    }
}

// --- Examine and combine ---

// shortcut: the item's description in a box, not the turning model; and the
// combine screen joins the item with its one partner at once, without the
// animation.
static struct {
    bool open;
    bool combine;
    DC_REQ req;
} m_Item;

static bool M_ItemOpen(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    m_Item = (typeof(m_Item)) {
        .open = true,
        .combine = screen == UI_TAKEOVER_COMBINE,
    };
    if (m_Item.combine) {
        OBJECT_ID partner;
        if (Inv_GetCombinePartners(args->object_id, &partner, 1) < 1
            || Inv_Combine(args->object_id, partner) == NO_OBJECT) {
            m_Item.open = false;
            return false;
        }
        return true;
    }
    const char *const text = Object_GetDescription(args->object_id);
    if (text == nullptr) {
        m_Item.open = false;
        return false;
    }
    DC_Req_Init(&m_Item.req, "", 300.0f, 0.5f);
    DC_Req_Add(&m_Item.req, text);
    m_Item.req.selected = -1;
    return true;
}

static void M_ItemRelease(const UI_TAKEOVER screen)
{
    m_Item.open = false;
}

static void M_ItemControl(void)
{
    if (m_Item.combine || g_InputDB.menu_confirm || g_InputDB.menu_back
        || g_InputDB.menu_show_info) {
        DC_Screens_Close(
            m_Item.combine ? UI_TAKEOVER_COMBINE : UI_TAKEOVER_EXAMINE,
            UI_TAKEOVER_CHOICE_CONFIRM);
    }
}

// --- Events ---

static bool M_OnTick(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (m_Pause.open) {
        M_PauseControl();
    }
    if (m_Stats.open
        && (g_InputDB.menu_confirm || g_InputDB.menu_back
            || g_InputDB.menu_skip)) {
        const bool from_ring = m_Stats.from_ring;
        DC_Screens_Close(
            from_ring ? UI_TAKEOVER_RING_ENTRY : UI_TAKEOVER_STATS,
            g_InputDB.menu_back ? UI_TAKEOVER_CHOICE_CANCEL
                                : UI_TAKEOVER_CHOICE_CONFIRM);
    }
    if (m_Pic.open) {
        M_PicControl();
    }
    if (m_Globe.open) {
        M_GlobeControl();
    }
    if (m_Item.open) {
        M_ItemControl();
    }
    return false;
}

static bool M_OnPaint(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (m_Pause.open) {
        M_PausePaint();
    }
    if (m_Stats.open && !m_Stats.hidden) {
        DC_Req_Paint(&m_Stats.req);
    }
    if (m_Pic.open) {
        M_PicPaint();
    }
    if (m_Globe.open) {
        if (m_Globe.path != nullptr) {
            UI_ScheduleDrawBackground(m_Globe.path, false, 0.0f);
        }
        DC_Req_Paint(&m_Globe.req);
    }
    if (m_Item.open && !m_Item.combine) {
        DC_Req_Paint(&m_Item.req);
    }
    return false;
}

void DC_NoLua_SetupScreens(void)
{
    DC_Screens_Define(UI_TAKEOVER_PAUSE, M_PauseOpen, M_PauseRelease);
    DC_Screens_Define(UI_TAKEOVER_STATS, M_StatsOpen, M_StatsRelease);
    DC_Screens_Define(UI_TAKEOVER_PICTURE, M_PicOpen, M_PicRelease);
    DC_Screens_Define(UI_TAKEOVER_LEVEL_VIEW, M_PicOpen, M_PicRelease);
    DC_Screens_Define(UI_TAKEOVER_GLOBE_SELECT, M_GlobeOpen, M_GlobeRelease);
    DC_Screens_Define(UI_TAKEOVER_EXAMINE, M_ItemOpen, M_ItemRelease);
    DC_Screens_Define(UI_TAKEOVER_COMBINE, M_ItemOpen, M_ItemRelease);
    DC_Events_Listen(LUA_EVENT_TICK, M_OnTick);
    DC_Events_Listen(LUA_EVENT_UI_PAINT, M_OnPaint);
}
