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

// What the level scripts did (nolua_levels.c): the level's entry takes over
// until it is dropped.
void DC_NoLua_RunLevelScript(const GF_LEVEL *level);
void DC_NoLua_DropLevelScript(void);
