// The engine without Lua (dc_lua=false): nolua.c answers the LUA_* calls the
// engine makes, and what the scripts did is done by C handlers listening to
// the same events.
#pragma once

#include <trx/core/result.h>
#include <trx/game/game_flow/types.h>
#include <trx/game/lua/events/manager.h>

#include <stdbool.h>
#include <stdint.h>

// A handler answers true to take over an event that carries a default, as a
// Lua listener returning true did.
typedef bool (*DC_EVENT_HANDLER)(
    LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *args, int32_t arg_count);

void DC_Events_Listen(LUA_EVENT_TYPE ev, DC_EVENT_HANDLER handler);

// What the startup scripts did (nolua_*.c).
RESULT DC_NoLua_DeclareWeapons(void);
void DC_NoLua_SetupGame(void);

// The interface (phase 2): the overlay (nolua_hud.c) and what is around it,
// such as fades, captions and pickups (nolua_ui.c). Called once.
void DC_NoLua_SetupHUD(void);
void DC_NoLua_SetupUI(void);

// The assault course timer and lap times, for the level that has a course
// (nolua_levels.c); they go with the level.
void DC_NoLua_SetupAssaultHUD(void);

// What the screens ask of the fades (fade.lua), in ticks: fade to black and
// rest there before the next screen starts, unless it blends in; or do it
// now, whatever comes next. The engine waits for both.
void DC_NoLua_FadeHandOver(int32_t ticks, int32_t hold);
void DC_NoLua_FadeLeaveToBlack(int32_t ticks, int32_t hold);

// A loading picture ending hands what is on screen to the level that starts
// (level_start.lua). The picture's screen confirms itself after the call.
void DC_NoLua_LevelHandOver(int32_t fade_out_ticks);

// The caption at the bottom of the screen and the arrows the game shows over
// play (trx.overlay). A null text takes the caption down; a text that is not
// plain is written as an object name.
typedef enum {
    DC_ARROW_TOP_LEFT,
    DC_ARROW_TOP_RIGHT,
    DC_ARROW_BOTTOM_LEFT,
    DC_ARROW_BOTTOM_RIGHT,
    DC_ARROW_CAPTION_LEFT,
    DC_ARROW_CAPTION_RIGHT,
} DC_ARROW;

void DC_NoLua_SetCaption(const char *text, bool plain, bool flashing);
void DC_NoLua_ShowArrow(DC_ARROW arrow, bool shown);
void DC_NoLua_ShowVersion(bool shown);

// What the level scripts did (nolua_levels.c): the level's entry takes over
// until it is dropped.
void DC_NoLua_RunLevelScript(const GF_LEVEL *level);
void DC_NoLua_DropLevelScript(void);
