// The entries of the menu ring that open the settings, without Lua: on the
// Dreamcast the graphics entry and its rendering page (ui/settings).
//
// shortcut: no settings page yet (phase 5 of PLAN_SIN_LUA.md); the graphics
// entry opens nothing, so the ring puts it back on any key.
#include "nolua.h"

bool DC_NoLua_OpenSettingsEntry(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    return false;
}

void DC_NoLua_ReleaseSettingsEntry(const UI_TAKEOVER screen)
{
}
