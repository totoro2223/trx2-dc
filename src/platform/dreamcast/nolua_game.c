// The game rules the shipped modules set up, without Lua: ngplus.lua,
// save_crystal.lua and secret_music.lua.
#include "nolua.h"

#include <trx/config.h>
#include <trx/game/catalog/manager.h>
#include <trx/game/game/state.h>
#include <trx/game/game_flow/common.h>
#include <trx/game/inventory.h>
#include <trx/game/music.h>
#include <trx/game/objects/families.h>
#include <trx/game/rules.h>

// A new game plus doubles Lara's health; a demo plays the original, and rules
// outlive the game they were set in.
static void M_NGPlus(void)
{
    const GF_LEVEL *const level = GF_GetCurrentLevel();
    if (Game_IsBonusFlagSet(GBF_NGPLUS)) {
        g_Rules.health.scale = 2;
    } else if (level != nullptr && level->type == GFL_DEMO) {
        Rules_ResetOne(Rules_GetByName("health.scale"));
    }
}

// TR3 PS1 gave Lara a save crystal on starting the game.
static void M_SaveCrystal(const bool is_save)
{
    const GF_LEVEL *const level = GF_GetCurrentLevel();
    if (!is_save && level != nullptr && level->type == GFL_NORMAL
        && level->num == 1
        && g_Config.gameplay.save_crystal_mode == SAVE_CRYSTAL_SAVE_PICKUP) {
        Inv_AddItem(O_SAVE_CRYSTAL_ITEM);
    }
}

static bool M_OnGameStart(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    M_NGPlus();
    M_SaveCrystal(arg_count > 0 && args[0].value.b);
    return false;
}

// The secret music plays when a secret Lara collects is announced; the fix
// plays it over the level's own music rather than in its place.
static bool M_OnShowPickup(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (arg_count < 1
        || !ObjectFamily_Has(args[0].value.i32, OBJ_FAMILY_SECRET)) {
        return false;
    }
    const CATALOG_ID track =
        Catalog_KeyToID(CATALOG_MUSIC, "secret", NO_CATALOG_ID);
    if (track != NO_CATALOG_ID) {
        Music_Play(
            track,
            g_Config.audio.fix_secrets_killing_music ? MPM_OVERLAY : MPM_ONCE);
    }
    return false;
}

void DC_NoLua_SetupGame(void)
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    DC_Events_Listen(LUA_EVENT_GAME_START, M_OnGameStart);
    DC_Events_Listen(LUA_EVENT_SHOW_PICKUP, M_OnShowPickup);
}
