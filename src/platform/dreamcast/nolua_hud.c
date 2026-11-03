// The in-game overlay without Lua: what ui/hud/overlay/bars.lua, ammo.lua and
// readouts.lua drew. Photo mode's panel is left out; the pickups are
// nolua_ui.c's.
//
// The tick reads Lara and works out what shows; the reserve asks each region
// for room, and the paint draws into the box it got. Each region holds one
// stack, as the Lua regions did, so what nolua_ui.c places there stacks
// beside it rather than over it.
#include "nolua.h"

#include <trx/config.h>
#include <trx/game/creature/common.h>
#include <trx/game/cutseq/playback.h>
#include <trx/game/game/state.h>
#include <trx/game/gun/common.h>
#include <trx/game/gun/registry.h>
#include <trx/game/inventory.h>
#include <trx/game/lara/common.h>
#include <trx/game/lara/const.h>
#include <trx/game/lara/vehicle.h>
#include <trx/game/output/func.h>
#include <trx/game/rooms/common.h>
#include <trx/game/rules.h>
#include <trx/game/ui/common.h>
#include <trx/game/ui/draw.h>
#include <trx/game/ui/regions.h>
#include <trx/game/ui/scaler.h>
#include <trx/game/ui/text.h>
#include <trx/version.h>

#include <arch/arch.h>
#include <stdarg.h>
#include <string.h>
#include <dc/pvr.h>
#include <dc/sound/sound.h>
#include <malloc.h>
#include <math.h>
#include <stddef.h>
#include <unistd.h>

void *sbrk(ptrdiff_t increment);
#include <stdio.h>

// A bar's size before the player's bar scale, in canvas units, and the bands
// of its fill.
#define M_BAR_W 208.0f
#define M_BAR_H 18.0f
#define M_BAR_STEPS 5
// The gap between two widgets in one region, at the default text size.
#define M_SPACING 3.0f
// In UI ticks: a low bar's blink, a hit, a level's opening.
#define M_BLINK_PERIOD 10
#define M_HIT_TIME 40
#define M_OPENING_TIME 100
#define M_AMMO_SCALE 1.5f
#define M_MAX_WIDGETS 6

#define M_RGB(hex)                                                             \
    { ((hex) >> 16) & 0xFF, ((hex) >> 8) & 0xFF, (hex) & 0xFF, 0xFF }
#define M_RAMP(a, b, c, d, e) { M_RGB(a), M_RGB(b), M_RGB(c), M_RGB(d), M_RGB(e) }

typedef enum {
    M_BAR_HP,
    M_BAR_HP_POISON,
    M_BAR_AIR,
    M_BAR_SPRINT,
    M_BAR_EXPOSURE,
    M_BAR_ENEMY,
    M_BAR_ALLY,
    M_BAR_NUMBER_OF,
} M_BAR;

// A look from cfg/ui.json5, with the color each bar takes by default. A PC
// look fills each band with one color; a PS1 look shades each band from its
// left ramp towards its right one, as far as the bar is full.
typedef struct {
    bool ps1;
    RGBA_8888 border[4]; // PC: light, dark; PS1: tl, tr, bl, br
    RGBA_8888 ramp[M_BAR_NUMBER_OF][2][M_BAR_STEPS];
} M_LOOK;

// ponytail: the default look of each game only (tr1_pc, tr2_ps1, tr3_ps1,
// all at scale 1, flat bands, no PS1 side borders); the bar look, color,
// smooth-bar and side-border settings come with phase 5.
#define M_PS1_BORDER                                                           \
    { M_RGB(0x508282), M_RGB(0xA0A0A0), M_RGB(0xA0A0A0), M_RGB(0x508282) }
#define M_PS1_YELLOW                                                           \
    { M_RAMP(0x606000, 0xB0B000, 0xF0F000, 0xB0B000, 0x606000),                \
      M_RAMP(0x606000, 0xB0B000, 0xF0F000, 0xB0B000, 0x606000) }
#define M_PS1_TEAL_GREEN                                                       \
    { M_RAMP(0x00717B, 0x00929A, 0x00717B, 0x005D6B, 0x004A5B),                \
      M_RAMP(0x007100, 0x009200, 0x007100, 0x005D00, 0x004A00) }
#define M_PS1_RED_YELLOW                                                       \
    { M_RAMP(0x900000, 0xC00000, 0xF00000, 0xC00000, 0x900000),                \
      M_RAMP(0x909000, 0xC0C000, 0xF0F000, 0xC0C000, 0x909000) }
#define M_PS1_DARK_BLUE_RED                                                    \
    { M_RAMP(0x000060, 0x000080, 0x0000A0, 0x000080, 0x000060),                \
      M_RAMP(0x600000, 0x800000, 0xA00000, 0x800000, 0x600000) }
#define M_PS1_ORANGE_RED                                                       \
    { M_RAMP(0xB64C00, 0xED6400, 0xB64C00, 0x843700, 0x6D2D00),                \
      M_RAMP(0xC00100, 0xF00100, 0xC00100, 0x900000, 0x600000) }
#define M_PS1_YELLOW_GREEN                                                     \
    { M_RAMP(0xB78900, 0xEEB300, 0xB78900, 0x846300, 0x6D5200),                \
      M_RAMP(0x01B900, 0x03FA00, 0x01B900, 0x018800, 0x015A00) }

static const M_LOOK m_Looks[3] = {
    // tr1_pc
    {
        .ps1 = false,
        .border = { M_RGB(0x353535), M_RGB(0x353535) },
        .ramp = {
            [M_BAR_HP] = { M_RAMP(0xA0281C, 0xB82C20, 0xA0281C, 0x7C2020, 0x541420) },
            [M_BAR_HP_POISON] = { M_RAMP(0xB9B723, 0xD6D629, 0xB9B723, 0x9B981E, 0x7E7218) },
            [M_BAR_AIR] = { M_RAMP(0x3D717B, 0x65929A, 0x3D717B, 0x1F5D6B, 0x004A5B) },
            [M_BAR_SPRINT] = { M_RAMP(0x239117, 0x33B020, 0x239117, 0x217210, 0x23540B) },
            [M_BAR_EXPOSURE] = { M_RAMP(0x20B3BC, 0x25D1DA, 0x20B3BC, 0x1B949E, 0x166F80) },
            [M_BAR_ENEMY] = { M_RAMP(0x586458, 0x748474, 0x586458, 0x4C504C, 0x303030) },
            [M_BAR_ALLY] = { M_RAMP(0x14BE6E, 0x1EE682, 0x14BE6E, 0x0F964B, 0x0A6E28) },
        },
    },
    // tr2_ps1
    {
        .ps1 = true,
        .border = M_PS1_BORDER,
        .ramp = {
            [M_BAR_HP] = {
                M_RAMP(0xB80000, 0xF00000, 0xB80000, 0x850000, 0x6E0000),
                M_RAMP(0x00BC00, 0x00FF00, 0x00BC00, 0x008C00, 0x005C00),
            },
            [M_BAR_HP_POISON] = M_PS1_YELLOW,
            [M_BAR_AIR] = M_PS1_TEAL_GREEN,
            [M_BAR_SPRINT] = M_PS1_RED_YELLOW,
            [M_BAR_EXPOSURE] = M_PS1_DARK_BLUE_RED,
            [M_BAR_ENEMY] = M_PS1_ORANGE_RED,
            [M_BAR_ALLY] = M_PS1_YELLOW_GREEN,
        },
    },
    // tr3_ps1
    {
        .ps1 = true,
        .border = M_PS1_BORDER,
        .ramp = {
            [M_BAR_HP] = {
                M_RAMP(0x400000, 0x600000, 0x800000, 0x600000, 0x400000),
                M_RAMP(0x008000, 0x00C000, 0x00FF00, 0x00C000, 0x008000),
            },
            [M_BAR_HP_POISON] = M_PS1_YELLOW,
            [M_BAR_AIR] = M_PS1_TEAL_GREEN,
            [M_BAR_SPRINT] = M_PS1_RED_YELLOW,
            [M_BAR_EXPOSURE] = M_PS1_DARK_BLUE_RED,
            [M_BAR_ENEMY] = M_PS1_ORANGE_RED,
            [M_BAR_ALLY] = M_PS1_YELLOW_GREEN,
        },
    },
};

// One thing a region shows: a bar (bar >= 0) or a line of text.
typedef struct {
    int32_t bar;
    float fill;
    const char *text;
    float scale;
    float w;
    float h;
} M_WIDGET;

typedef struct {
    UI_REGION region;
    bool right;
    int32_t count;
    M_WIDGET widgets[M_MAX_WIDGETS];
    int32_t slot;
} M_STACK;

enum { M_TOP_LEFT, M_TOP_RIGHT, M_BOTTOM_LEFT, M_STACK_COUNT };

static M_STACK m_Stacks[M_STACK_COUNT] = {
    [M_TOP_LEFT] = { .region = UI_REGION_TOP_LEFT, .slot = -1 },
    [M_TOP_RIGHT] = { .region = UI_REGION_TOP_RIGHT, .right = true, .slot = -1 },
    [M_BOTTOM_LEFT] = { .region = UI_REGION_BOTTOM_LEFT, .slot = -1 },
};

static struct {
    bool had_lara;
    int16_t hp;
    int32_t hit;
    int32_t opening;
    int32_t blink_ticks;
    bool blink;
} m_Clock;

#define M_MSG_LINES 4
static struct {
    char text[40];
    int32_t ticks;
} m_Msgs[M_MSG_LINES];

void DC_Msg(const char *const fmt, ...)
{
    memmove(&m_Msgs[0], &m_Msgs[1], sizeof(m_Msgs[0]) * (M_MSG_LINES - 1));
    va_list args;
    va_start(args, fmt);
    vsnprintf(m_Msgs[M_MSG_LINES - 1].text, sizeof(m_Msgs[0].text), fmt, args);
    va_end(args);
    m_Msgs[M_MSG_LINES - 1].ticks = LOGIC_FPS * 5;
}

static char m_AmmoText[32];
static char m_FPSText[16];
static int32_t m_FPS = -1;

static const M_LOOK *M_GetLook(void)
{
    return &m_Looks[g_TRVersion <= 1 ? 0 : g_TRVersion == 2 ? 1 : 2];
}

static float M_Fraction(const int32_t value, const int32_t full)
{
    if (full <= 0 || value <= 0) {
        return 0.0f;
    }
    return value >= full ? 1.0f : (float)value / (float)full;
}

// How low a bar has to be before it flashes, which TR1 puts lower.
static bool M_IsLow(const float fill)
{
    return fill <= (g_TRVersion == 1 ? 0.2f : 0.25f);
}

static void M_Push(
    const int32_t stack, const int32_t bar, const float fill,
    const char *const text, const float scale)
{
    M_STACK *const s = &m_Stacks[stack];
    if (s->count < M_MAX_WIDGETS) {
        s->widgets[s->count++] = (M_WIDGET) {
            .bar = bar, .fill = fill, .text = text, .scale = scale };
    }
}

static void M_PushBar(const int32_t stack, const M_BAR bar, const float fill)
{
    M_Push(stack, bar, fill, nullptr, 1.0f);
}

// The shots left for what Lara holds, or for the gun on her vehicle; false
// where there is no count to show.
static bool M_FormatAmmo(const LARA_INFO *const lara)
{
    LARA_GUN_TYPE gun = Lara_Vehicle_GetGunType();
    const bool from_vehicle = gun != LGT_UNARMED;
    if (!from_vehicle) {
        if (lara->gun_status != LGS_READY) {
            return false;
        }
        gun = lara->gun_type;
    }
    if (!Gun_Registry_IsValidType(gun)) {
        return false;
    }
    const WEAPON_INFO *const weapon = Gun_Registry_Get(gun);
    if (!weapon->is_declared || Gun_HasInfiniteAmmo(gun)) {
        return false;
    }
    if (!from_vehicle && Gun_GetAmmoObject(gun) == NO_OBJECT) {
        return false;
    }
    const int32_t per_shot = Gun_GetRoundsPerShot(gun);
    const int32_t shots =
        Inv_State_GetAmmo(Inv_GetState(), gun) / (per_shot > 0 ? per_shot : 1);
    const char *const icon = g_TRVersion == 1 ? weapon->ammo_icon : nullptr;
    if (icon != nullptr) {
        snprintf(
            m_AmmoText, sizeof(m_AmmoText), "\\{small}%6d %s", (int)shots,
            icon);
    } else {
        snprintf(m_AmmoText, sizeof(m_AmmoText), "\\{small}%6d", (int)shots);
    }
    return true;
}

static bool M_IsInSwamp(const ITEM *const item)
{
    const ROOM *const room = Room_Get(item->room_num);
    return room != nullptr && room->flags.swamp;
}

static bool M_IsInUPV(void)
{
    const ITEM *const vehicle = Lara_Vehicle_GetItem();
    return vehicle != nullptr && vehicle->object_id == O_UPV;
}

// Counts the timers and works out what each region shows until the next
// tick. The show_bars, enable_bar_flashing and enemy_healthbar_show_mode
// settings were Lua's alone; their defaults (on, on, always) are built in.
static bool M_OnTick(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    for (int32_t i = 0; i < M_STACK_COUNT; i++) {
        m_Stacks[i].count = 0;
    }
    if (m_Clock.hit > 0) {
        m_Clock.hit--;
    }
    if (m_Clock.opening > 0) {
        m_Clock.opening--;
    }

    const bool ui_enabled = g_Config.ui.enable_game_ui;
    const bool playing = Game_IsPlaying();
    const ITEM *const item = Lara_GetItem();
    const LARA_INFO *const lara = Lara_GetLaraInfo();

    // A level opens on Lara's health bar; any change to her health counts
    // as a hit, as the Lua signal did.
    if (item != nullptr && !m_Clock.had_lara) {
        m_Clock.opening = M_OPENING_TIME;
    }
    m_Clock.had_lara = item != nullptr;
    if (item != nullptr && item->hit_points != m_Clock.hp) {
        m_Clock.hp = item->hit_points;
        m_Clock.hit = M_HIT_TIME;
    }

    const bool armed = item != nullptr && lara->gun_status == LGS_READY;
    float health = 1.0f;
    float air = 1.0f;
    float exposure = 1.0f;
    bool poisoned = false;
    if (item != nullptr) {
        health = M_Fraction(item->hit_points, item->max_hit_points);
        air = M_Fraction(lara->air, LARA_MAX_AIR);
        exposure = M_Fraction(lara->exposure_timer, g_Rules.exposure.max);
        poisoned = lara->poison.value > 0;
    }
    const bool health_flashing = M_IsLow(health) || poisoned;
    const bool air_flashing = M_IsLow(air);
    const bool exposure_flashing = M_IsLow(exposure);

    // The blink runs only while some bar flashes, shown or not.
    if (item != nullptr
        && (health_flashing || air_flashing || exposure_flashing)) {
        if (++m_Clock.blink_ticks >= M_BLINK_PERIOD) {
            m_Clock.blink_ticks = 0;
            m_Clock.blink = !m_Clock.blink;
        }
    } else {
        m_Clock.blink_ticks = 0;
        m_Clock.blink = false;
    }

    // Lara's bars show while she answers to the player outside a cutscene.
    // The ring shows the health bar while it rests on a medipack.
    const bool lara_bars = item != nullptr && ui_enabled
        && Lara_IsControllable() && !CutSeq_IsActive() && playing;
    if (item != nullptr && ui_enabled && DC_Ring_ForcesHealthBar()) {
        M_PushBar(
            M_TOP_LEFT, poisoned ? M_BAR_HP_POISON : M_BAR_HP,
            health_flashing && m_Clock.blink ? 0.0f : health);
    } else if (lara_bars
        && (m_Clock.opening > 0 || m_Clock.hit > 0 || health <= 0.0f || armed
            || poisoned || health_flashing)) {
        M_PushBar(
            M_TOP_LEFT, poisoned ? M_BAR_HP_POISON : M_BAR_HP,
            health_flashing && m_Clock.blink ? 0.0f : health);
    }

    if (lara_bars) {
        const LARA_WATER_STATE water = lara->water_status;
        if (water == LWS_UNDERWATER || water == LWS_SURFACE
            || (air < 1.0f && M_IsInSwamp(item))
            || (water == LWS_ABOVE_WATER && M_IsInUPV())) {
            M_PushBar(
                M_TOP_RIGHT, M_BAR_AIR,
                air_flashing && m_Clock.blink ? 0.0f : air);
        }
        // The sprint never flashes.
        const float sprint = M_Fraction(lara->sprint_timer, LARA_MAX_SPRINT);
        if (sprint < 1.0f) {
            M_PushBar(M_TOP_RIGHT, M_BAR_SPRINT, sprint);
        }
        if (exposure < 1.0f) {
            M_PushBar(
                M_TOP_RIGHT, M_BAR_EXPOSURE,
                exposure_flashing && m_Clock.blink ? 0.0f : exposure);
        }
    }

    if (ui_enabled && playing && item != nullptr && M_FormatAmmo(lara)) {
        M_Push(M_TOP_RIGHT, -1, 0.0f, m_AmmoText, M_AMMO_SCALE);
    }

    // What she aims at, an ally or an enemy.
    const ITEM *const target = item != nullptr ? lara->target : nullptr;
    if (ui_enabled && playing && armed && target != nullptr) {
        M_PushBar(
            M_BOTTOM_LEFT, Creature_IsAlly(target) ? M_BAR_ALLY : M_BAR_ENEMY,
            M_Fraction(target->hit_points, target->max_hit_points));
    }

    // How full the console's three memories are (render option, off by default),
    // measured once a second.
    if (ui_enabled && playing && g_Config.rendering.dc_memory_hud) {
        static int32_t countdown = 0;
        static char text[3][24];
        if (countdown-- <= 0) {
            countdown = LOGIC_FPS;
            const struct mallinfo mem = mallinfo();
            const size_t ram_free = mem.fordblks
                + ((uintptr_t)_arch_mem_top - (uintptr_t)sbrk(0));
            snprintf(
                text[0], sizeof(text[0]), "RAM %u%%",
                (unsigned)(100 - ram_free * 100 / (16u << 20)));
            snprintf(
                text[1], sizeof(text[1]), "VRAM %u%%",
                (unsigned)(100 - pvr_mem_available() * 100 / (8u << 20)));
            snprintf(
                text[2], sizeof(text[2]), "AICA %u%%",
                (unsigned)(100 - snd_mem_available() * 100 / (2u << 20)));
        }
        for (int32_t i = 0; i < 3; i++) {
            M_Push(M_TOP_RIGHT, -1, 0.0f, text[i], 1.0f);
        }
    }

    // The message console: the newest lines DC_Msg was given, oldest on top.
    for (int32_t i = 0; i < M_MSG_LINES; i++) {
        if (m_Msgs[i].ticks > 0) {
            m_Msgs[i].ticks--;
            if (ui_enabled) {
                M_Push(M_BOTTOM_LEFT, -1, 0.0f, m_Msgs[i].text, 1.0f);
            }
        }
    }

    // The position, animation, camera and invulnerability readouts are off
    // by default (debug.enable_debug_*) and are not drawn here; the frame
    // counter is on by default on the Dreamcast.
    if (ui_enabled && g_Config.ui.enable_fps_counter) {
        const int32_t fps = Output_GetMeasuredFPS();
        if (fps != m_FPS) {
            m_FPS = fps;
            snprintf(m_FPSText, sizeof(m_FPSText), "%d FPS", (int)fps);
        }
        M_Push(M_TOP_LEFT, -1, 0.0f, m_FPSText, 1.0f);
    }
    return false;
}

// A new level counts as Lara entering the world even where her item is
// where the last level left it.
static bool M_OnGameStart(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    m_Clock.had_lara = false;
    return false;
}

static bool M_OnReserve(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    const float bar_scale = UI_Scaler_GetScale(UI_SCALER_TARGET_BAR);
    const float spacing = M_SPACING * UI_Scaler_GetTextScale();
    for (int32_t i = 0; i < M_STACK_COUNT; i++) {
        M_STACK *const s = &m_Stacks[i];
        s->slot = -1;
        float w = 0.0f;
        float h = 0.0f;
        for (int32_t j = 0; j < s->count; j++) {
            M_WIDGET *const wd = &s->widgets[j];
            if (wd->bar >= 0) {
                wd->w = M_BAR_W * bar_scale;
                wd->h = M_BAR_H * bar_scale;
            } else {
                UI_Text_Measure(
                    wd->text, &wd->w, &wd->h,
                    (UI_TEXT_SETTINGS) { .scale = wd->scale });
            }
            w = wd->w > w ? wd->w : w;
            h += wd->h + (j > 0 ? spacing : 0.0f);
        }
        if (w > 0.0f && h > 0.0f) {
            s->slot = UI_Region_Reserve(s->region, w, h);
        }
    }
    return false;
}

static int32_t M_Px(const float v)
{
    return (int32_t)floorf(UI_ScaleY(v) + 0.5f);
}

static RGBA_8888 M_Mix(const RGBA_8888 a, const RGBA_8888 b, const float r)
{
    return (RGBA_8888) {
        .r = (uint8_t)floorf(a.r + (b.r - a.r) * r),
        .g = (uint8_t)floorf(a.g + (b.g - a.g) * r),
        .b = (uint8_t)floorf(a.b + (b.b - a.b) * r),
        .a = (uint8_t)floorf(a.a + (b.a - a.a) * r),
    };
}

// Draws a bar as widgets/bar.lua did: in whole screen pixels, its height cut
// into a row per fill band plus a frame and a padding row on either side.
static void M_DrawBar(
    const M_BAR bar, float fill, const float x, const float y, const float w,
    const float h)
{
    static const RGBA_8888 black = { 0, 0, 0, 0xFF };
    const M_LOOK *const look = M_GetLook();
    const int32_t x0 = M_Px(x);
    const int32_t y0 = M_Px(y);
    const int32_t x_end = M_Px(x + w);
    const int32_t y_end = M_Px(y + h);
    const int32_t edge = (y_end - y0) / (M_BAR_STEPS + 4);
    const int32_t x1 = x0 + edge;
    const int32_t y1 = y0 + edge;

    if (look->ps1) {
        UI_ScheduleDrawScreenGradientQuad(
            x0, y0, 0, x_end - x0, y_end - y0, look->border[0],
            look->border[1], look->border[2], look->border[3]);
    } else {
        UI_ScheduleDrawScreenFlatQuad(
            x0, y0, 0, x_end - x0, y_end - y0, look->border[0]);
        // The dark side runs to the far edge, as the engine drew it.
        UI_ScheduleDrawScreenFlatQuad(
            x1, y1, 0, x_end - x1, y_end - y1, look->border[1]);
    }
    UI_ScheduleDrawScreenFlatQuad(
        x1, y1, 0, x_end - edge - x1, y_end - edge - y1, black);

    fill = floorf((fill < 0.0f ? 0.0f : fill > 1.0f ? 1.0f : fill) * 100.0f)
        / 100.0f;
    if (fill <= 0.0f) {
        return;
    }
    const int32_t fx = x1 + edge;
    const int32_t fy = y1 + edge;
    const int32_t fw = (int32_t)lroundf((x_end - 2 * edge - fx) * fill);
    const int32_t fh = y_end - y0 - 4 * edge;
    for (int32_t i = 0; i < M_BAR_STEPS; i++) {
        const int32_t top = fy + fh * i / M_BAR_STEPS;
        const int32_t bottom = fy + fh * (i + 1) / M_BAR_STEPS;
        const RGBA_8888 left = look->ramp[bar][0][i];
        if (look->ps1) {
            const RGBA_8888 right = M_Mix(left, look->ramp[bar][1][i], fill);
            UI_ScheduleDrawScreenGradientQuad(
                fx, top, 0, fw, bottom - top, left, right, left, right);
        } else {
            UI_ScheduleDrawScreenFlatQuad(fx, top, 0, fw, bottom - top, left);
        }
    }
}

static bool M_OnPaint(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    const float spacing = M_SPACING * UI_Scaler_GetTextScale();
    for (int32_t i = 0; i < M_STACK_COUNT; i++) {
        const M_STACK *const s = &m_Stacks[i];
        float bx;
        float by;
        float bw;
        float bh;
        if (s->slot < 0 || !UI_Region_GetSlotBox(s->slot, &bx, &by, &bw, &bh)) {
            continue;
        }
        for (int32_t j = 0; j < s->count; j++) {
            const M_WIDGET *const wd = &s->widgets[j];
            const float x = s->right ? bx + bw - wd->w : bx;
            if (wd->bar >= 0) {
                M_DrawBar(wd->bar, wd->fill, x, by, wd->w, wd->h);
            } else {
                UI_Text_Draw(
                    wd->text, x, by,
                    (UI_TEXT_SETTINGS) { .scale = wd->scale, .z = 0 });
            }
            by += wd->h + spacing;
        }
    }
    return false;
}

void DC_NoLua_SetupHUD(void)
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    DC_Events_Listen(LUA_EVENT_TICK, M_OnTick);
    DC_Events_Listen(LUA_EVENT_GAME_START, M_OnGameStart);
    DC_Events_Listen(LUA_EVENT_UI_RESERVE_ALL, M_OnReserve);
    DC_Events_Listen(LUA_EVENT_UI_PAINT, M_OnPaint);
}
