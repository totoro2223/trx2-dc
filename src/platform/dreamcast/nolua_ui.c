// The interface around the overlay without Lua: what fade.lua,
// level_start.lua, captions.lua, hud/overlay/pickups.lua and hud/assault.lua
// did. Everything is polled from the engine as a frame is built; nothing is
// allocated.
#include "nolua.h"

#include <trx/config.h>
#include <trx/core/log.h>
#include <trx/core/math/const.h>
#include <trx/game/const.h>
#include <trx/game/game/state.h>
#include <trx/game/game_flow/common.h>
#include <trx/game/game_strings/entries.h>
#include <trx/game/gym.h>
#include <trx/game/interpolation.h>
#include <trx/game/inventory.h>
#include <trx/game/lua/events/ui.h>
#include <trx/game/objects/common.h>
#include <trx/game/output/const.h>
#include <trx/game/output/textures.h>
#include <trx/game/savegame/resume.h>
#include <trx/game/ui/common.h>
#include <trx/game/ui/draw.h>
#include <trx/game/ui/regions.h>
#include <trx/game/ui/scaler.h>
#include <trx/game/ui/text.h>
#include <trx/game/ui/tween.h>
#include <trx/version.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

// --- Shared drawing ---

static void M_DrawSprite(
    const int32_t sprite_idx, const float x, const float y, const float scale,
    const RGBA_F top, const RGBA_F bottom)
{
    const RGBA_F colors[4] = { top, top, bottom, bottom };
    const int32_t s = lroundf(UI_ScaleX(scale) * PHD_ONE);
    UI_ScheduleDrawScreenSprite(
        lroundf(UI_ScaleX(x)), lroundf(UI_ScaleY(y)), 0, s, s, sprite_idx,
        colors);
}

static int32_t M_SpriteCount(const OBJECT_ID object_id)
{
    const OBJECT *const object = Object_Get(object_id);
    return object->loaded && object->mesh_count < 0 ? -object->mesh_count : 0;
}

static void M_MeasureText(
    const char *const text, const float scale, float *const w, float *const h)
{
    UI_Text_Measure(text, w, h, (UI_TEXT_SETTINGS) { .scale = scale });
}

static void M_DrawText(
    const char *const text, const float x, const float y, const float scale)
{
    UI_Text_Draw(text, x, y, (UI_TEXT_SETTINGS) { .scale = scale });
}

static bool M_IsLevelType(const GF_LEVEL_TYPE type)
{
    const GF_LEVEL *const level = GF_GetCurrentLevel();
    return level != nullptr && level->type == type;
}

// --- Fade layers (trx.ui.layers.fade, fade.lua) ---

#define M_MAX_FADES 8

// The events that close a fade that outlives its screen.
#define M_DROP_TITLE 0x01
#define M_DROP_UNLOAD 0x02
#define M_DROP_CUTSCENE 0x04
#define M_DROP_DEMO 0x08

// A layer over the interface, found by an id that is never reused, so that a
// slot given to another layer does not answer for the old one.
typedef struct {
    int32_t id;
    UI_TWEEN opacity;
    // A snapshot layer holds the frame; the Dreamcast draws it as nothing
    // (render/stubs.c), so it only stands for a fade that is still going.
    bool black;
    bool holds;
    bool over_console;
    bool moving;
    bool close_at_rest;
    uint8_t drop_on;
    void (*on_done)(int32_t id);
} M_FADE;

static M_FADE m_Fades[M_MAX_FADES];
static int32_t m_NextFadeID = 1;

static bool M_FadesEnabled(void)
{
    return g_Config.visuals.enable_fade_effects;
}

static M_FADE *M_FindFade(const int32_t id)
{
    for (int32_t i = 0; id != 0 && i < M_MAX_FADES; i++) {
        if (m_Fades[i].id == id) {
            return &m_Fades[i];
        }
    }
    return nullptr;
}

// Tells the engine whether an open layer makes it wait to start its next
// screen, or to exit.
static void M_UpdateHold(void)
{
    bool held = false;
    for (int32_t i = 0; i < M_MAX_FADES; i++) {
        held |= m_Fades[i].id != 0 && m_Fades[i].holds;
    }
    LUA_UI_SetHoldingScreen(held);
}

static int32_t M_OpenFade(
    const float opacity, const bool black, const bool holds,
    const uint8_t drop_on)
{
    for (int32_t i = 0; i < M_MAX_FADES; i++) {
        M_FADE *const fade = &m_Fades[i];
        if (fade->id != 0) {
            continue;
        }
        *fade = (M_FADE) {
            .id = m_NextFadeID++,
            .black = black,
            .holds = holds,
            .drop_on = drop_on,
        };
        UI_Tween_Set(&fade->opacity, opacity);
        if (holds) {
            M_UpdateHold();
        }
        return fade->id;
    }
    LOG_WARNING("nolua: too many fade layers");
    return 0;
}

static void M_CloseFade(const int32_t id)
{
    M_FADE *const fade = M_FindFade(id);
    if (fade == nullptr) {
        return;
    }
    const bool held = fade->holds;
    fade->id = 0;
    if (held) {
        M_UpdateHold();
    }
}

static bool M_IsFadeOpen(const int32_t id)
{
    return M_FindFade(id) != nullptr;
}

// Moves a layer's opacity, then runs `on_done` on the tick the move ends. A
// later move cuts it short. A move to where the layer already is waits out
// the ticks, which is how a fade rests black for a hold.
static void M_FadeTo(
    const int32_t id, const float target, const int32_t ticks,
    const bool close, void (*const on_done)(int32_t id))
{
    M_FADE *const fade = M_FindFade(id);
    if (fade == nullptr) {
        return;
    }
    UI_Tween_To(&fade->opacity, target, ticks, 0, UI_EASE_LINEAR);
    fade->moving = true;
    fade->close_at_rest = close;
    fade->on_done = on_done;
}

static void M_DropFades(const uint8_t event)
{
    for (int32_t i = 0; i < M_MAX_FADES; i++) {
        if (m_Fades[i].drop_on & event) {
            M_CloseFade(m_Fades[i].id);
        }
    }
}

// Reports each move that ended since the last tick, once. The ended moves are
// gathered first, so a layer that a callback opens waits for the next tick.
static void M_TickFades(void)
{
    int32_t ended[M_MAX_FADES];
    int32_t count = 0;
    for (int32_t i = 0; i < M_MAX_FADES; i++) {
        const M_FADE *const fade = &m_Fades[i];
        if (fade->id != 0 && fade->moving
            && !UI_Tween_IsMoving(&fade->opacity)) {
            ended[count++] = fade->id;
        }
    }
    for (int32_t i = 0; i < count; i++) {
        M_FADE *const fade = M_FindFade(ended[i]);
        if (fade == nullptr || !fade->moving) {
            continue;
        }
        void (*const on_done)(int32_t) = fade->on_done;
        fade->moving = false;
        fade->on_done = nullptr;
        if (fade->close_at_rest) {
            M_CloseFade(ended[i]);
        }
        if (on_done != nullptr) {
            on_done(ended[i]);
        }
    }
}

static void M_PaintFades(const bool over_console)
{
    for (int32_t i = 0; i < M_MAX_FADES; i++) {
        const M_FADE *const fade = &m_Fades[i];
        if (fade->id != 0 && fade->black
            && fade->over_console == over_console) {
            UI_ScheduleDrawScreenBlack(UI_Tween_Get(&fade->opacity));
        }
    }
}

// fade.across: a black that outlives the screen that starts it, moved over
// `ticks` while fades run and at once where they do not, closing out of
// sight.
static int32_t M_Across(
    const float from, const float to, const int32_t ticks,
    const uint8_t drop_on)
{
    const int32_t id = M_OpenFade(from, true, false, drop_on);
    M_FadeTo(id, to, M_FadesEnabled() ? ticks : 0, to == 0.0f, nullptr);
    return id;
}

// --- Changing screens and exiting (fade.lua) ---

// How the screen that left asked the next one to come in; ticks < 0 for a
// cut.
static struct {
    int32_t ticks;
    int32_t hold;
} m_Handed = { .ticks = -1 };

static int32_t m_BlackHold = 0;

static void M_RestThenClose(const int32_t id)
{
    M_FadeTo(id, 1.0f, m_BlackHold, true, nullptr);
}

// Fades the frame on show to black, rests there, and lets the next screen
// start.
// ponytail: the frame itself is not kept under the black (the Dreamcast draws
// snapshots as nothing), and one hold length is shared by every black at
// once; keep it per layer if two screens ever hand over together.
static void M_FadeToBlack(const int32_t ticks, const int32_t hold)
{
    m_BlackHold = hold;
    const int32_t id = M_OpenFade(0.0f, true, true, 0);
    M_FadeTo(id, 1.0f, ticks, false, M_RestThenClose);
}

int32_t DC_NoLua_FadeOpen(const float opacity, const bool holds)
{
    return M_OpenFade(
        opacity, true, holds, M_DROP_TITLE | M_DROP_UNLOAD | M_DROP_DEMO);
}

void DC_NoLua_FadeMove(
    const int32_t id, const float target, const int32_t ticks,
    const bool close, void (*const on_done)(int32_t id))
{
    M_FadeTo(id, target, ticks, close, on_done);
}

void DC_NoLua_FadeClose(const int32_t id)
{
    M_CloseFade(id);
}

bool DC_NoLua_FadesEnabled(void)
{
    return M_FadesEnabled();
}

void DC_NoLua_FadeHandOver(const int32_t ticks, const int32_t hold)
{
    m_Handed.ticks = ticks;
    m_Handed.hold = hold;
}

void DC_NoLua_FadeLeaveToBlack(const int32_t ticks, const int32_t hold)
{
    M_FadeToBlack(ticks, hold);
}

static bool M_OnScreenChange(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (arg_count < 3) {
        return false;
    }
    int32_t ticks = m_Handed.ticks;
    int32_t hold = m_Handed.hold;
    m_Handed.ticks = -1;
    if (ticks < 0 && args[1].value.i32 > 0) {
        ticks = args[1].value.i32;
        hold = args[2].value.i32;
    }
    if (ticks >= 0 && !args[0].value.b) {
        M_FadeToBlack(ticks, hold);
    }
    return false;
}

// The game fades to black over the console as it exits, over a third of a
// second, and holds the black for a tenth before it lets the game go.
#define M_EXIT_TICKS ((LOGIC_FPS + 1) / 3)
#define M_EXIT_HOLD ((LOGIC_FPS + 5) / 10)

static void M_ExitReleased(const int32_t id)
{
    M_FADE *const fade = M_FindFade(id);
    if (fade != nullptr) {
        fade->holds = false;
        M_UpdateHold();
    }
}

static void M_ExitRest(const int32_t id)
{
    M_FadeTo(id, 1.0f, M_EXIT_HOLD, false, M_ExitReleased);
}

static bool M_OnExit(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (!M_FadesEnabled() || !g_Config.visuals.enable_exit_fade_effects) {
        return false;
    }
    const int32_t id = M_OpenFade(0.0f, true, true, 0);
    M_FADE *const fade = M_FindFade(id);
    if (fade != nullptr) {
        fade->over_console = true;
    }
    M_FadeTo(id, 1.0f, M_EXIT_TICKS, false, M_ExitRest);
    return false;
}

// --- How a level comes in (level_start.lua) ---

// How long a level takes to fade in from black.
#define M_LEVEL_FADE_IN_TICKS 16

// What is on screen as a level comes in: the fade a loading picture handed
// over and how long it takes to go, the fade that is going, and the wait for
// the level to run for the first time.
static struct {
    int32_t held;
    int32_t held_ticks;
    int32_t going;
    bool waiting;
} m_Level;

static void M_DropLevel(void)
{
    m_Level = (typeof(m_Level)) {};
}

// ponytail: visuals.level_start_transition was a Lua setting; this is its
// default for TR1-3, cross-fade, or disabled with the fade effects off. The
// TR4 "through black" way is not ported.
void DC_NoLua_LevelHandOver(const int32_t fade_out_ticks)
{
    M_CloseFade(m_Level.held);
    M_DropLevel();
    if (M_FadesEnabled()) {
        m_Level.held = M_OpenFade(
            1.0f, false, false,
            M_DROP_TITLE | M_DROP_UNLOAD | M_DROP_CUTSCENE);
        m_Level.held_ticks = fade_out_ticks;
    }
}

static void M_LevelGameStart(void)
{
    if (M_IsLevelType(GFL_CUTSCENE)) {
        M_DropLevel();
        return;
    }
    const int32_t held = m_Level.held;
    m_Level.held = 0;
    if (M_IsFadeOpen(held)) {
        M_FadeTo(
            held, 0.0f, M_FadesEnabled() ? m_Level.held_ticks : 0, true,
            nullptr);
        m_Level.going = held;
    }
    m_Level.waiting = true;
}

// Fades the level in from black as it first runs, unless the fade from its
// loading picture is still on screen.
static bool M_OnBeforeControl(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (!m_Level.waiting) {
        return false;
    }
    m_Level.waiting = false;
    const int32_t going = m_Level.going;
    m_Level.going = 0;
    if (M_IsFadeOpen(going) || !M_FadesEnabled()) {
        return false;
    }
    M_Across(
        1.0f, 0.0f, M_LEVEL_FADE_IN_TICKS, M_DROP_TITLE | M_DROP_UNLOAD);
    return false;
}

// --- Captions and arrows (captions.lua, trx.overlay) ---

#define M_BLINK_PERIOD 10
#define M_VERSION_SCALE 0.5f
#define M_ARROW_SPACING 15.0f
#define M_BAR_HEIGHT 18.0f

static struct {
    char text[128];
    bool shown;
    bool flashing;
    bool hidden;
    int32_t blink;
    bool demo;
    uint8_t arrows;
    bool version;
    int32_t caption_slot;
    int32_t version_slot;
    int32_t arrow_slots[DC_ARROW_CAPTION_LEFT];
} m_Captions;

static const UI_REGION m_ArrowRegions[DC_ARROW_CAPTION_LEFT] = {
    [DC_ARROW_TOP_LEFT] = UI_REGION_TOP_LEFT,
    [DC_ARROW_TOP_RIGHT] = UI_REGION_TOP_RIGHT,
    [DC_ARROW_BOTTOM_LEFT] = UI_REGION_BOTTOM_LEFT,
    [DC_ARROW_BOTTOM_RIGHT] = UI_REGION_BOTTOM_RIGHT,
};

static const char *const m_ArrowTexts[DC_ARROW_CAPTION_LEFT] = {
    "\\{arrow up}",
    "\\{arrow up}",
    "\\{arrow down}",
    "\\{arrow down}",
};

#define M_LEFT_TEXT "\\{button left}"
#define M_RIGHT_TEXT "\\{button right}"

void DC_NoLua_SetCaption(
    const char *const text, const bool plain, const bool flashing)
{
    m_Captions.shown = text != nullptr;
    m_Captions.flashing = text != nullptr && flashing;
    if (text == nullptr) {
        return;
    }
    if (plain) {
        snprintf(m_Captions.text, sizeof(m_Captions.text), "%s", text);
    } else {
        snprintf(
            m_Captions.text, sizeof(m_Captions.text),
            GS("general/inventory_ring/object_name_fmt"), text);
    }
}

void DC_NoLua_ShowArrow(const DC_ARROW arrow, const bool shown)
{
    if (shown) {
        m_Captions.arrows |= 1 << arrow;
    } else {
        m_Captions.arrows &= ~(1 << arrow);
    }
}

// ponytail: ui.show_title_version was a Lua setting, on by default; it is
// always on here.
void DC_NoLua_ShowVersion(const bool shown)
{
    m_Captions.version = shown;
}

static bool M_HasArrow(const DC_ARROW arrow)
{
    return (m_Captions.arrows & (1 << arrow)) != 0;
}

static void M_TickCaptions(void)
{
    // A demo names itself, flashing while it runs and still as it fades out.
    if (m_Captions.demo) {
        DC_NoLua_SetCaption(
            GS("general/misc/demo_mode"), true, Game_IsPlaying());
    }
    if (!m_Captions.flashing) {
        m_Captions.hidden = false;
        m_Captions.blink = 0;
    } else if (++m_Captions.blink >= M_BLINK_PERIOD) {
        m_Captions.blink = 0;
        m_Captions.hidden = !m_Captions.hidden;
    }
}

// The caption with the arrows that say it steps left and right, each arrow
// keeping its room while it is off.
static void M_MeasureCaption(float *const w, float *const h)
{
    float lw;
    float lh;
    float cw;
    float ch;
    float rw;
    float rh;
    M_MeasureText(M_LEFT_TEXT, 1.0f, &lw, &lh);
    M_MeasureText(m_Captions.text, 1.0f, &cw, &ch);
    M_MeasureText(M_RIGHT_TEXT, 1.0f, &rw, &rh);
    const float gap = M_ARROW_SPACING * UI_Scaler_GetTextScale();
    *w = lw + gap + cw + gap + rw;
    *h = fmaxf(fmaxf(lh, ch), rh);
}

static void M_PaintCaption(const float x, const float y, const float h)
{
    const char *const parts[3] = {
        M_HasArrow(DC_ARROW_CAPTION_LEFT) ? M_LEFT_TEXT : nullptr,
        m_Captions.text,
        M_HasArrow(DC_ARROW_CAPTION_RIGHT) ? M_RIGHT_TEXT : nullptr,
    };
    const char *const sizes[3] = { M_LEFT_TEXT, m_Captions.text, M_RIGHT_TEXT };
    const float gap = M_ARROW_SPACING * UI_Scaler_GetTextScale();
    float at = x;
    for (int32_t i = 0; i < 3; i++) {
        float pw;
        float ph;
        M_MeasureText(sizes[i], 1.0f, &pw, &ph);
        if (parts[i] != nullptr) {
            M_DrawText(parts[i], at, y + (h - ph) * 0.5f, 1.0f);
        }
        at += pw + gap;
    }
}

// A corner arrow stands as tall as a bar, so that a corner keeps the room it
// has while it shows one.
static void M_MeasureCornerArrow(
    const DC_ARROW arrow, float *const w, float *const h)
{
    float th;
    M_MeasureText(m_ArrowTexts[arrow], 1.0f, w, &th);
    *h = fmaxf(
        th,
        M_BAR_HEIGHT * (float)UI_Scaler_GetScale(UI_SCALER_TARGET_BAR));
}

// ponytail: an arrow in a corner shows whatever else stands there (Lua showed
// it only alone); the regions would need to say what a corner holds.
static void M_ReserveCaptions(void)
{
    float w;
    float h;
    m_Captions.caption_slot = -1;
    if (m_Captions.shown) {
        M_MeasureCaption(&w, &h);
        m_Captions.caption_slot =
            UI_Region_Reserve(UI_REGION_BOTTOM_CENTER, w, h);
    }
    m_Captions.version_slot = -1;
    if (m_Captions.version) {
        M_MeasureText(g_TRXVersion, M_VERSION_SCALE, &w, &h);
        m_Captions.version_slot =
            UI_Region_Reserve(UI_REGION_BOTTOM_RIGHT, w, h);
    }
    for (int32_t i = 0; i < DC_ARROW_CAPTION_LEFT; i++) {
        m_Captions.arrow_slots[i] = -1;
        if (M_HasArrow(i)) {
            M_MeasureCornerArrow(i, &w, &h);
            m_Captions.arrow_slots[i] =
                UI_Region_Reserve(m_ArrowRegions[i], w, h);
        }
    }
}

static void M_PaintCaptions(void)
{
    float x;
    float y;
    float w;
    float h;
    if (!m_Captions.hidden
        && UI_Region_GetSlotBox(m_Captions.caption_slot, &x, &y, &w, &h)) {
        M_PaintCaption(x, y, h);
    }
    if (UI_Region_GetSlotBox(m_Captions.version_slot, &x, &y, &w, &h)) {
        M_DrawText(g_TRXVersion, x, y, M_VERSION_SCALE);
    }
    for (int32_t i = 0; i < DC_ARROW_CAPTION_LEFT; i++) {
        if (UI_Region_GetSlotBox(m_Captions.arrow_slots[i], &x, &y, &w, &h)) {
            float tw;
            float th;
            M_MeasureText(m_ArrowTexts[i], 1.0f, &tw, &th);
            M_DrawText(m_ArrowTexts[i], x, y + (h - th) * 0.5f, 1.0f);
        }
    }
}

static void M_DemoGameStart(void)
{
    if (M_IsLevelType(GFL_DEMO)) {
        m_Captions.demo = true;
    } else if (m_Captions.demo) {
        // Stands for the level unload this build does not fire.
        m_Captions.demo = false;
        DC_NoLua_SetCaption(nullptr, false, false);
    }
}

// --- Pickups (hud/overlay/pickups.lua) ---

#define M_PICKUP_ROWS 3
#define M_PICKUP_COLUMNS 4
#define M_MAX_PICKUPS (M_PICKUP_ROWS * M_PICKUP_COLUMNS)
#define M_EASE_IN_TICKS (LOGIC_FPS / 2)
#define M_DISPLAY_TICKS (LOGIC_FPS * 2)
#define M_EASE_OUT_TICKS LOGIC_FPS
#define M_SPIN_PER_TICK (4 * DEG_1)

typedef enum {
    M_PICKUP_EASE_IN,
    M_PICKUP_DISPLAY,
    M_PICKUP_EASE_OUT,
} M_PICKUP_PHASE;

typedef struct {
    OBJECT_ID object;
    // The model drawn, or NO_OBJECT for the object's sprite.
    OBJECT_ID icon;
    int32_t column;
    int32_t row;
    M_PICKUP_PHASE phase;
    int32_t elapsed;
    int32_t total;
} M_PICKUP;

static M_PICKUP m_Pickups[M_MAX_PICKUPS];
static int32_t m_PickupCount = 0;

// How many of an entry's own ticks have gone by at the frame being drawn. An
// entry counts only the ticks the game is played in.
static float M_TicksDrawn(const int32_t count)
{
    return count - 1 + (float)Interpolation_GetUIRate();
}

// How far an entry has slid in, from 0 off the screen to 1 in its cell.
// Sprites do not slide, as in the original.
static float M_Slid(const M_PICKUP *const entry)
{
    if (entry->icon == NO_OBJECT || entry->phase == M_PICKUP_DISPLAY) {
        return 1.0f;
    }
    const int32_t ticks = entry->phase == M_PICKUP_EASE_IN ? M_EASE_IN_TICKS
                                                           : M_EASE_OUT_TICKS;
    const float ratio =
        fminf(fmaxf(M_TicksDrawn(entry->elapsed) / ticks, 0.0f), 1.0f);
    const float eased = UI_Ease_Apply(UI_EASE_QUAD_IN_OUT, ratio);
    return entry->phase == M_PICKUP_EASE_IN ? eased : 1.0f - eased;
}

static void M_PickupBox(
    const M_PICKUP *const entry, float *const x, float *const y,
    float *const w, float *const h)
{
    const float canvas_w = UI_GetCanvasWidth();
    const float canvas_h = UI_GetCanvasHeight();
    const float bottom = UI_GetWorkingCanvasBottom();
    const float height = canvas_h * g_Config.ui.pickup_scale / 6.0f;
    const float width = height * 5.0f / 4.0f;
    const float margin_y = canvas_h / 16.0f;
    const float margin_x = margin_y * 4.0f / 3.0f;
    const float gap_x = width / 8.0f;
    const float gap_y = height / 8.0f;

    const float from_x = canvas_w + margin_x + width;
    const float from_y = bottom - margin_y - height / 2.0f;
    const float to_x =
        canvas_w - margin_x - width / 2.0f - (width + gap_x) * entry->column;
    const float to_y = from_y - (height + gap_y) * entry->row;

    const float ratio = M_Slid(entry);
    *x = from_x + (to_x - from_x) * ratio - width / 2.0f;
    *y = from_y + (to_y - from_y) * ratio - height / 2.0f;
    *w = width;
    *h = height;
}

// The model the ring uses, or NO_OBJECT where the object has none.
static OBJECT_ID M_ModelOf(const OBJECT_ID object_id)
{
    const OBJECT_ID icon = Inv_GetItemOption(object_id);
    if (icon == NO_OBJECT || !Object_Get(icon)->loaded) {
        return NO_OBJECT;
    }
    const ANIM *const anim = Object_GetAnim(Object_Get(icon), 0);
    return anim != nullptr && anim->frame_ptr != nullptr ? icon : NO_OBJECT;
}

// A free cell. Models give theirs up as they start to leave; sprites hold
// theirs until they go.
static bool M_FreeCell(int32_t *const column, int32_t *const row)
{
    for (int32_t index = 0; index < M_MAX_PICKUPS; index++) {
        bool taken = false;
        for (int32_t i = 0; i < m_PickupCount && !taken; i++) {
            const M_PICKUP *const entry = &m_Pickups[i];
            const bool leaving = entry->icon != NO_OBJECT
                && entry->phase == M_PICKUP_EASE_OUT;
            taken = entry->column == index % M_PICKUP_COLUMNS
                && entry->row == index / M_PICKUP_COLUMNS && !leaving;
        }
        if (!taken) {
            *column = index % M_PICKUP_COLUMNS;
            *row = index / M_PICKUP_COLUMNS;
            return true;
        }
    }
    return false;
}

static bool M_OnShowPickup(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    int32_t column;
    int32_t row;
    if (arg_count < 1 || m_PickupCount >= M_MAX_PICKUPS
        || !M_FreeCell(&column, &row)) {
        return false;
    }
    const OBJECT_ID object_id = args[0].value.i32;
    // An object without a sprite shows its model, even with 3D pickups off.
    const bool sprite = M_SpriteCount(object_id) > 0;
    OBJECT_ID icon = M_ModelOf(object_id);
    if (sprite && !g_Config.visuals.enable_3d_pickups) {
        icon = NO_OBJECT;
    }
    if (icon == NO_OBJECT && !sprite) {
        return false;
    }
    // ponytail: a model turns from 0 with every mesh drawn; the ring's pose
    // for it (ring/entries.lua) comes with the ring port.
    m_Pickups[m_PickupCount++] = (M_PICKUP) {
        .object = object_id,
        .icon = icon,
        .column = column,
        .row = row,
        .phase = M_PICKUP_EASE_IN,
    };
    return false;
}

// Advances an entry and answers whether it stays on screen.
static bool M_AdvancePickup(M_PICKUP *const entry)
{
    entry->elapsed++;
    entry->total++;
    if (entry->phase == M_PICKUP_EASE_IN) {
        if (entry->elapsed >= M_EASE_IN_TICKS) {
            entry->elapsed = 0;
            entry->phase = M_PICKUP_DISPLAY;
        }
    } else if (entry->phase == M_PICKUP_DISPLAY) {
        if (entry->elapsed >= M_DISPLAY_TICKS) {
            entry->elapsed = 0;
            entry->phase = M_PICKUP_EASE_OUT;
        }
    } else if (entry->elapsed >= M_EASE_OUT_TICKS) {
        return false;
    }
    return true;
}

static void M_TickPickups(void)
{
    if (m_PickupCount == 0 || !Game_IsPlaying()) {
        return;
    }
    int32_t kept = 0;
    for (int32_t i = 0; i < m_PickupCount; i++) {
        if (M_AdvancePickup(&m_Pickups[i])) {
            m_Pickups[kept++] = m_Pickups[i];
        }
    }
    m_PickupCount = kept;
}

static void M_PaintPickup(const M_PICKUP *const entry)
{
    float x;
    float y;
    float w;
    float h;
    M_PickupBox(entry, &x, &y, &w, &h);
    if (entry->icon != NO_OBJECT) {
        const float spin = M_SPIN_PER_TICK * M_TicksDrawn(entry->total);
        const XYZ_16 rot = { .y = (int16_t)((int32_t)floorf(spin) % DEG_360) };
        const int32_t sx = lroundf(UI_ScaleX(x));
        const int32_t sy = lroundf(UI_ScaleY(y));
        UI_ScheduleDrawModel(
            entry->icon, UINT32_MAX, sx, sy, lroundf(UI_ScaleX(x + w)) - sx,
            lroundf(UI_ScaleY(y + h)) - sy, rot, true, 0, nullptr);
        return;
    }
    const int32_t sprite_idx = Object_Get(entry->object)->mesh_idx;
    const SPRITE_TEXTURE *const sprite = Output_GetSpriteTexture(sprite_idx);
    if (sprite == nullptr) {
        return;
    }
    const float sw = fabsf((float)sprite->x1 - sprite->x0);
    const float sh = fabsf((float)sprite->y1 - sprite->y0);
    if (sw <= 0.0f || sh <= 0.0f) {
        return;
    }
    const float scale = fminf(h / sh, w / sw);
    const RGBA_F white = { 1.0f, 1.0f, 1.0f, 1.0f };
    M_DrawSprite(
        sprite_idx, x + (w - sw * scale) / 2.0f - sprite->x0 * scale,
        y + (h - sh * scale) / 2.0f - sprite->y0 * scale, scale, white, white);
}

// ponytail: ui.show_pickups_overlay was a Lua setting, on by default; it is
// always on here.
static void M_PaintPickups(void)
{
    if (!Game_IsPlaying()) {
        return;
    }
    for (int32_t i = 0; i < m_PickupCount; i++) {
        M_PaintPickup(&m_Pickups[i]);
    }
}

// --- The assault course readouts (hud/assault.lua) ---

#define M_DIGIT_ADVANCE 20
#define M_PENALTY_GAP 40.0f
#define M_PENALTY_SPACING 3.0f
#define M_LAP_SPACING 20.0f

typedef struct {
    int8_t sprite; // -1 for none
    int8_t offset;
    int8_t advance;
    int8_t drop;
    int8_t nudge;
    bool mark;
} M_GLYPH;

typedef struct {
    RGBA_F top;
    RGBA_F bottom;
} M_SHADE;

#define M_RGB(c)                                                               \
    { ((c) >> 16 & 0xFF) / 255.0f, ((c) >> 8 & 0xFF) / 255.0f,               \
      ((c) & 0xFF) / 255.0f, 1.0f }
#define M_SHADE_OF(top, bottom) { M_RGB(top), M_RGB(bottom) }

// The palettes the engine draws the digits in, from the top of a character to
// the bottom.
static const M_SHADE m_White = M_SHADE_OF(0xFFFFFF, 0xFFFFFF);
static const M_SHADE m_Neutral = M_SHADE_OF(0xFFFFFF, 0x404040);
static const M_SHADE m_Grey = M_SHADE_OF(0x808080, 0x1A1A1A);
static const M_SHADE m_Green = M_SHADE_OF(0x59BF33, 0x1A4000);
static const M_SHADE m_Red = M_SHADE_OF(0xE63300, 0x4D0000);
static const M_SHADE m_Pink = M_SHADE_OF(0xFF00FF, 0x400040);

static struct {
    bool on;
    int32_t timer_slot;
    int32_t lap_slot;
} m_Assault = { .timer_slot = -1, .lap_slot = -1 };

// What one frame of the readouts shows, polled as the frame is reserved.
static struct {
    bool on_screen;
    bool penalty_shown[2];
    bool lap_shown;
    bool has_best;
    bool is_best;
    char clock[16];
    char penalty[2][32];
    char last[16];
    char best[16];
} m_Readout;

static M_GLYPH M_GlyphOf(const char c)
{
    if (c >= '0' && c <= '9') {
        return (M_GLYPH) { .sprite = c - '0', .advance = M_DIGIT_ADVANCE };
    }
    switch (c) {
    case ':':
        return (M_GLYPH) { .sprite = 10, .offset = -6, .advance = 14 };
    case '.':
        return (M_GLYPH) { .sprite = 11, .offset = -6, .advance = 14 };
    case 'T':
        return (M_GLYPH) {
            .sprite = 12, .offset = -6, .advance = 16, .drop = 1, .mark = true
        };
    case 's':
        return (M_GLYPH) {
            .sprite = 13, .offset = -6, .advance = 14, .nudge = -4
        };
    case ' ':
        return (M_GLYPH) { .sprite = -1, .advance = 8 };
    default:
        return (M_GLYPH) { .sprite = -1, .advance = M_DIGIT_ADVANCE };
    }
}

static const SPRITE_TEXTURE *M_DigitSprite(const M_GLYPH glyph)
{
    if (glyph.sprite < 0 || glyph.sprite >= M_SpriteCount(O_ASSAULT_DIGITS)) {
        return nullptr;
    }
    return Output_GetSpriteTexture(
        Object_Get(O_ASSAULT_DIGITS)->mesh_idx + glyph.sprite);
}

// The characters sit around the point each is drawn at; the highest decides
// where a line of them starts from the top of its box.
static float M_DigitsRise(const char *const text)
{
    float rise = 0.0f;
    for (const char *c = text; *c != '\0'; c++) {
        const SPRITE_TEXTURE *const sprite = M_DigitSprite(M_GlyphOf(*c));
        if (sprite != nullptr) {
            rise = fminf(rise, sprite->y0);
        }
    }
    return rise;
}

static void M_MeasureDigits(
    const char *const text, float *const w, float *const h)
{
    const float scale = UI_Scaler_GetTextScale();
    float width = 0.0f;
    float bottom = 0.0f;
    for (const char *c = text; *c != '\0'; c++) {
        const M_GLYPH glyph = M_GlyphOf(*c);
        width += glyph.offset + glyph.advance;
        const SPRITE_TEXTURE *const sprite = M_DigitSprite(glyph);
        if (sprite != nullptr) {
            bottom = fmaxf(bottom, sprite->y1 + glyph.drop);
        }
    }
    *w = width * scale;
    *h = (bottom - M_DigitsRise(text)) * scale;
}

static void M_DrawDigits(
    const char *const text, const float x, const float y, const M_SHADE color,
    const M_SHADE mark)
{
    const float scale = UI_Scaler_GetTextScale();
    const float top = y - M_DigitsRise(text) * scale;
    const int32_t base = Object_Get(O_ASSAULT_DIGITS)->mesh_idx;
    float pen = x;
    for (const char *c = text; *c != '\0'; c++) {
        const M_GLYPH glyph = M_GlyphOf(*c);
        pen += glyph.offset * scale;
        if (M_DigitSprite(glyph) != nullptr) {
            const M_SHADE shade = glyph.mark ? mark : color;
            M_DrawSprite(
                base + glyph.sprite, pen + glyph.nudge * scale,
                top + glyph.drop * scale, scale, shade.top, shade.bottom);
        }
        pen += glyph.advance * scale;
    }
}

// The clock as the game writes it: TR2 to a tenth of a second, TR3 to a
// hundredth. A lap not run yet shows dashes.
static void M_FormatTime(
    char *const out, const size_t size, const int32_t frames,
    const bool placeholder)
{
    const bool is_tr2 = g_TRVersion < 3;
    if (placeholder && frames <= 0) {
        snprintf(out, size, "%s", is_tr2 ? "--:--.-" : "--:--.--");
        return;
    }
    const int32_t seconds = frames / LOGIC_FPS;
    const int32_t fraction =
        frames % LOGIC_FPS * (is_tr2 ? 10 : 100) / LOGIC_FPS;
    snprintf(
        out, size, is_tr2 ? "%d:%02d.%d" : "%d:%02d.%02d", (int)seconds / 60,
        (int)seconds % 60, (int)fraction);
}

static void M_FormatPenalty(
    char *const out, const size_t size, const int32_t frames,
    const bool is_target)
{
    const int32_t seconds = frames / LOGIC_FPS;
    snprintf(
        out, size, is_target ? "T %d:%02d s" : "%d:%02d s", (int)seconds / 60,
        (int)seconds % 60);
}

static int32_t M_RunFrames(void)
{
    const GF_LEVEL *const level = Game_GetCurrentLevel();
    const RESUME_INFO *const resume =
        level == nullptr ? nullptr : SG_Resume_GetEntry(level);
    const int32_t frames = resume == nullptr ? 0 : (int32_t)resume->stats.timer;
    // The first two frames read as zero, as in the original games.
    return frames < 2 ? 0 : frames;
}

static int32_t M_BestFrames(const GYM_TRACK_TYPE track)
{
    const GYM_TRACK_STATS *const stats = Gym_TrackManager_GetStats(track);
    return stats != nullptr && stats->total_attempts > 0
        ? (int32_t)stats->entries[0].time
        : 0;
}

static void M_PollAssault(void)
{
    const bool playing = Game_IsPlaying();
    const GYM_TRACK_TYPE active = Gym_TrackManager_GetActiveTrackType();
    // The lap times take the top middle to themselves while they are up.
    const bool lap_up =
        playing && Gym_TrackManager_GetLapTimeDisplayTimer(GYM_TRACK_QUAD) > 0;
    const int32_t last = Gym_TrackManager_GetLapTime(GYM_TRACK_QUAD);
    m_Readout.lap_shown = lap_up && last > 0;
    m_Readout.on_screen = playing && !lap_up && active != GYM_TRACK_NONE
        && Gym_TrackManager_IsTimerDisplay(active);

    if (m_Readout.on_screen) {
        M_FormatTime(
            m_Readout.clock, sizeof(m_Readout.clock), M_RunFrames(), false);
        for (int32_t i = 0; i < 2; i++) {
            const int32_t frames = i == 0
                ? Gym_TrackManager_GetPenaltyFrames(active)
                : Gym_TrackManager_GetTargetPenaltyFrames(active);
            m_Readout.penalty_shown[i] =
                Gym_TrackManager_GetPenaltyDisplayTimer(active) > 0
                && frames > 0;
            M_FormatPenalty(
                m_Readout.penalty[i], sizeof(m_Readout.penalty[i]), frames,
                i == 1);
        }
    }
    if (m_Readout.lap_shown) {
        const int32_t best = M_BestFrames(GYM_TRACK_QUAD);
        m_Readout.has_best = best > 0;
        m_Readout.is_best = best > 0 && last == best;
        M_FormatTime(m_Readout.last, sizeof(m_Readout.last), last, true);
        M_FormatTime(m_Readout.best, sizeof(m_Readout.best), best, true);
    }
}

// The penalties shown, one above the other and right aligned, so that both
// end in the unit and the target mark stands out to the left.
static void M_MeasurePenalties(float *const w, float *const h)
{
    const float gap = M_PENALTY_SPACING * UI_Scaler_GetTextScale();
    *w = 0.0f;
    *h = 0.0f;
    for (int32_t i = 0; i < 2; i++) {
        if (!m_Readout.penalty_shown[i]) {
            continue;
        }
        float pw;
        float ph;
        M_MeasureDigits(m_Readout.penalty[i], &pw, &ph);
        *h += (*h > 0.0f ? gap : 0.0f) + ph;
        *w = fmaxf(*w, pw);
    }
}

// The penalties take the left of the clock, and as much room again is held
// open on its right, so that the clock stands in the middle.
static void M_MeasureTimer(float *const w, float *const h, float *const pw)
{
    float cw;
    float ch;
    float ph;
    M_MeasureDigits(m_Readout.clock, &cw, &ch);
    M_MeasurePenalties(pw, &ph);
    const float gap = M_PENALTY_GAP * UI_Scaler_GetTextScale();
    *w = *pw + gap + cw + gap + *pw;
    *h = fmaxf(ch, ph);
}

static void M_PaintTimer(const float x, const float y)
{
    const M_SHADE plain = g_TRVersion < 3 ? m_White : m_Neutral;
    float w;
    float h;
    float pw;
    M_MeasureTimer(&w, &h, &pw);
    const float scale = UI_Scaler_GetTextScale();
    float at = y;
    for (int32_t i = 0; i < 2; i++) {
        if (!m_Readout.penalty_shown[i]) {
            continue;
        }
        float dw;
        float dh;
        M_MeasureDigits(m_Readout.penalty[i], &dw, &dh);
        M_DrawDigits(m_Readout.penalty[i], x + pw - dw, at, m_Pink, plain);
        at += dh + M_PENALTY_SPACING * scale;
    }
    M_DrawDigits(
        m_Readout.clock, x + pw + M_PENALTY_GAP * scale, y, plain, plain);
}

static void M_MeasureLap(float *const w, float *const h)
{
    float bw = 0.0f;
    float bh = 0.0f;
    M_MeasureDigits(m_Readout.last, w, h);
    if (m_Readout.has_best) {
        M_MeasureDigits(m_Readout.best, &bw, &bh);
        *w += M_LAP_SPACING * UI_Scaler_GetTextScale() + bw;
        *h = fmaxf(*h, bh);
    }
}

// A lap that matches the record is green on both sides; otherwise the lap is
// red against a record and neutral without one. A circuit with no record
// shows the lap alone.
static void M_PaintLap(const float x, const float y)
{
    const M_SHADE last = m_Readout.is_best ? m_Green
        : m_Readout.has_best               ? m_Red
                                           : m_Neutral;
    M_DrawDigits(m_Readout.last, x, y, last, last);
    if (m_Readout.has_best) {
        float lw;
        float lh;
        M_MeasureDigits(m_Readout.last, &lw, &lh);
        const M_SHADE best = m_Readout.is_best ? m_Green : m_Grey;
        M_DrawDigits(
            m_Readout.best,
            x + lw + M_LAP_SPACING * UI_Scaler_GetTextScale(), y, best, best);
    }
}

void DC_NoLua_SetupAssaultHUD(void)
{
    m_Assault.on = true;
}

static void M_ReserveAssault(void)
{
    m_Assault.timer_slot = -1;
    m_Assault.lap_slot = -1;
    // The digits come from a sprite object only the gym levels load.
    if (!m_Assault.on || M_SpriteCount(O_ASSAULT_DIGITS) == 0) {
        return;
    }
    M_PollAssault();
    float w;
    float h;
    float pw;
    if (m_Readout.on_screen) {
        M_MeasureTimer(&w, &h, &pw);
        m_Assault.timer_slot = UI_Region_Reserve(UI_REGION_TOP_CENTER, w, h);
    }
    if (m_Readout.lap_shown) {
        M_MeasureLap(&w, &h);
        m_Assault.lap_slot = UI_Region_Reserve(UI_REGION_TOP_CENTER, w, h);
    }
}

static void M_PaintAssault(void)
{
    float x;
    float y;
    float w;
    float h;
    if (UI_Region_GetSlotBox(m_Assault.timer_slot, &x, &y, &w, &h)) {
        M_PaintTimer(x, y);
    }
    if (UI_Region_GetSlotBox(m_Assault.lap_slot, &x, &y, &w, &h)) {
        M_PaintLap(x, y);
    }
}

// --- Events ---

static bool M_OnTick(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    M_TickFades();
    M_TickCaptions();
    M_TickPickups();
    return false;
}

static bool M_OnReserve(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    M_ReserveAssault();
    M_ReserveCaptions();
    return false;
}

static bool M_OnPaint(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    M_PaintAssault();
    M_PaintCaptions();
    M_PaintPickups();
    return false;
}

static bool M_OnPaintFade(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    M_PaintFades(ev == LUA_EVENT_UI_PAINT_EXIT);
    return false;
}

// Runs before the level scripts' own game start (nolua_levels.c listens
// later), so an assault level turns its readouts back on after this.
static bool M_OnGameStart(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (M_IsLevelType(GFL_CUTSCENE)) {
        M_DropFades(M_DROP_CUTSCENE);
    } else if (M_IsLevelType(GFL_DEMO)) {
        M_DropFades(M_DROP_DEMO);
    }
    M_LevelGameStart();
    M_DemoGameStart();
    // These stand for the level unload this build does not fire yet.
    m_PickupCount = 0;
    m_Assault.on = false;
    return false;
}

static bool M_OnTitleStart(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    M_DropFades(M_DROP_TITLE);
    M_DropLevel();
    if (m_Captions.demo) {
        m_Captions.demo = false;
        DC_NoLua_SetCaption(nullptr, false, false);
    }
    return false;
}

static bool M_OnLevelUnload(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    M_DropFades(M_DROP_UNLOAD);
    M_DropLevel();
    m_Captions.demo = false;
    m_Captions.arrows = 0;
    DC_NoLua_SetCaption(nullptr, false, false);
    m_PickupCount = 0;
    m_Assault.on = false;
    return false;
}

void DC_NoLua_SetupUI(void)
{
    static bool done = false;
    if (done) {
        // The engine started over for another game: the exit fade of the
        // last one (black, held) must not come along onto the new screen.
        memset(m_Fades, 0, sizeof(m_Fades));
        M_DropLevel();
        M_UpdateHold();
        return;
    }
    done = true;
    m_Captions.caption_slot = -1;
    m_Captions.version_slot = -1;
    for (int32_t i = 0; i < DC_ARROW_CAPTION_LEFT; i++) {
        m_Captions.arrow_slots[i] = -1;
    }
    DC_Events_Listen(LUA_EVENT_TICK, M_OnTick);
    DC_Events_Listen(LUA_EVENT_UI_RESERVE_ALL, M_OnReserve);
    DC_Events_Listen(LUA_EVENT_UI_PAINT, M_OnPaint);
    DC_Events_Listen(LUA_EVENT_UI_PAINT_FADE, M_OnPaintFade);
    DC_Events_Listen(LUA_EVENT_UI_PAINT_EXIT, M_OnPaintFade);
    DC_Events_Listen(LUA_EVENT_SCREEN_CHANGE, M_OnScreenChange);
    DC_Events_Listen(LUA_EVENT_EXIT, M_OnExit);
    DC_Events_Listen(LUA_EVENT_GAME_START, M_OnGameStart);
    DC_Events_Listen(LUA_EVENT_TITLE_START, M_OnTitleStart);
    DC_Events_Listen(LUA_EVENT_LEVEL_UNLOAD, M_OnLevelUnload);
    DC_Events_Listen(LUA_EVENT_BEFORE_CONTROL, M_OnBeforeControl);
    DC_Events_Listen(LUA_EVENT_SHOW_PICKUP, M_OnShowPickup);
}
