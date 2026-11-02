// The graphics entry of the menu ring, without Lua: the rendering page of
// ui/settings, which is all the settings the console has.
//
// shortcut: no descriptions, no mouse and no "reset to default"; left and
// right (or confirm) step a row's value, back leaves. The names are the
// English fallbacks of the strings files.
#include "nolua.h"

#include "dc_mode.h"

#include <trx/config.h>
#include <trx/game/game.h>
#include <trx/game/game_flow.h>
#include <trx/game/game_flow/common.h>
#include <trx/game/game_strings/entries.h>
#include <trx/game/input.h>
#include <trx/game/lua/events/ui.h>
#include <trx/game/objects/ids.h>

#include <stdio.h>
#include <string.h>

typedef enum {
    M_ROW_MODE,
    M_ROW_RESTART,
    M_ROW_FPS,
    M_ROW_CEL,
    M_ROW_SHADOW,
    M_ROW_PICKUPS,
    M_ROW_BUMP,
    M_ROW_ROOM_CEL,
    M_ROW_FPS_COUNTER,
    M_ROW_TEXTURE_FILTER,
    M_ROW_UI_FILTER,
    M_ROW_NUMBER_OF,
} M_ROW;

static struct {
    bool open;
    DC_REQ req;
    int32_t rows[M_ROW_NUMBER_OF];
    int32_t count;
} m_Page;

static const char *M_GS(const char *const key, const char *const fallback)
{
    const char *const text = GameString_Get(key);
    return text != nullptr && strcmp(text, key) != 0 ? text : fallback;
}

static const char *M_OnOff(const bool on)
{
    return on ? M_GS("general/on", "On") : M_GS("general/off", "Off");
}

static const char *M_FilterName(const TEXTURE_FILTER filter)
{
    return filter == TEXTURE_FILTER_BILINEAR
        ? M_GS("general/settings/filter/bilinear", "Bilinear")
        : M_GS("general/settings/filter/point", "Point");
}

static const char *M_ModeName(const DC_MODE mode)
{
    static const char *const names[] = {
        "Graphics", "Performance", "Original", "Experimental 60"
    };
    return mode >= 0 && mode < DC_MODE_NUMBER_OF ? names[mode] : "";
}

static const char *M_FpsName(const DC_FPS fps)
{
    static const char *const names[] = { "30", "60", "60 adaptive" };
    return fps >= 0 && fps < DC_FPS_NUMBER_OF ? names[fps] : "";
}

static const char *M_ShadowName(const SHADOW_TYPE type)
{
    switch (type) {
    case SHADOW_TYPE_OCTAGON:
        return "Octagon";
    case SHADOW_TYPE_CIRCLE:
        return "Circle";
    case SHADOW_TYPE_SPRITE:
        return "Sprite";
    case SHADOW_TYPE_VOLUME:
        return "Volume";
    case SHADOW_TYPE_SOFT:
        return "Soft";
    case SHADOW_TYPE_SOFT_HIGH:
        return "Soft (high)";
    default:
        return "";
    }
}

static bool M_CanRestart(void)
{
    const GF_LEVEL *const level = Game_GetCurrentLevel();
    return level != nullptr
        && (level->type == GFL_NORMAL || level->type == GFL_GYM
            || level->type == GFL_BONUS);
}

static const char *M_Name(const M_ROW row)
{
    switch (row) {
    case M_ROW_MODE:
        return M_GS("settings/rendering.dc_mode/title", "Mode");
    case M_ROW_RESTART:
        return M_GS("settings/rendering.dc_restart_level/title", "Restart level");
    case M_ROW_FPS:
        return M_GS("settings/rendering.dc_fps/title", "Frame rate");
    case M_ROW_CEL:
        return M_GS("settings/rendering.dc_cel_shading/title", "Cel shading");
    case M_ROW_SHADOW:
        return M_GS("settings/visuals.shadow_type/title", "Shadows");
    case M_ROW_PICKUPS:
        return M_GS("settings/visuals.enable_3d_pickups/title", "3D pickups");
    case M_ROW_BUMP:
        return M_GS("settings/rendering.dc_bump_mapping/title", "Bump mapping");
    case M_ROW_ROOM_CEL:
        return M_GS("settings/rendering.dc_room_cel/title", "Room cel shading");
    case M_ROW_FPS_COUNTER:
        return M_GS("settings/ui.enable_fps_counter/title", "FPS counter");
    case M_ROW_TEXTURE_FILTER:
        return M_GS("settings/rendering.texture_filter/title", "Texture filter");
    default:
        return M_GS("settings/rendering.ui_filter/title", "UI filter");
    }
}

static const char *M_Value(const M_ROW row)
{
    switch (row) {
    case M_ROW_MODE:
        return M_ModeName(g_Config.rendering.dc_mode);
    case M_ROW_RESTART:
        return "";
    case M_ROW_FPS:
        return M_FpsName(g_Config.rendering.dc_fps);
    case M_ROW_CEL:
        return M_OnOff(g_Config.rendering.dc_cel_shading);
    case M_ROW_SHADOW:
        return M_ShadowName(g_Config.visuals.shadow_type);
    case M_ROW_PICKUPS:
        return M_OnOff(g_Config.visuals.enable_3d_pickups);
    case M_ROW_BUMP:
        return M_OnOff(g_Config.rendering.dc_bump_mapping);
    case M_ROW_ROOM_CEL:
        return M_OnOff(g_Config.rendering.dc_room_cel);
    case M_ROW_FPS_COUNTER:
        return M_OnOff(g_Config.ui.enable_fps_counter);
    case M_ROW_TEXTURE_FILTER:
        return M_FilterName(g_Config.rendering.texture_filter);
    default:
        return M_FilterName(g_Config.rendering.ui_filter);
    }
}

// Rebuilds the rows, keeping the one the player is on.
static void M_Fill(void)
{
    const int32_t selected = m_Page.req.selected;
    DC_Req_Init(
        &m_Page.req,
        M_GS("general/settings/graphic_settings/title", "Graphics"), 290.0f,
        0.5f);
    m_Page.count = 0;
    for (int32_t i = 0; i < M_ROW_NUMBER_OF; i++) {
        if (i == M_ROW_RESTART && !M_CanRestart()) {
            continue;
        }
        DC_REQ_ROW *const row = DC_Req_Add(&m_Page.req, M_Name((M_ROW)i));
        snprintf(row->right, sizeof(row->right), "%s", M_Value((M_ROW)i));
        m_Page.rows[m_Page.count++] = i;
    }
    DC_Req_Select(&m_Page.req, selected > 0 ? selected : 0);
}

static void M_Step(const M_ROW row, const int32_t rate)
{
    switch (row) {
    case M_ROW_MODE:
        CONFIG_CYCLE(g_Config.rendering.dc_mode, rate, DC_MODE_NUMBER_OF);
        // The mode picks the other options; the player may change them after.
        DC_Mode_ApplyDefaults();
        break;
    case M_ROW_RESTART: {
        const GF_LEVEL *const level = Game_GetCurrentLevel();
        GF_OverrideCommand(
            (GF_COMMAND) { .action = GF_RESTART_GAME, .param = level->num },
            false);
        break;
    }
    case M_ROW_FPS:
        CONFIG_CYCLE(g_Config.rendering.dc_fps, rate, DC_FPS_NUMBER_OF);
        break;
    case M_ROW_CEL:
        CONFIG_TOGGLE(g_Config.rendering.dc_cel_shading);
        break;
    case M_ROW_SHADOW:
        CONFIG_CYCLE(g_Config.visuals.shadow_type, rate, SHADOW_TYPE_NUMBER_OF);
        break;
    case M_ROW_PICKUPS:
        CONFIG_TOGGLE(g_Config.visuals.enable_3d_pickups);
        break;
    case M_ROW_BUMP:
        CONFIG_TOGGLE(g_Config.rendering.dc_bump_mapping);
        break;
    case M_ROW_ROOM_CEL:
        CONFIG_TOGGLE(g_Config.rendering.dc_room_cel);
        break;
    case M_ROW_FPS_COUNTER:
        CONFIG_TOGGLE(g_Config.ui.enable_fps_counter);
        break;
    case M_ROW_TEXTURE_FILTER:
        CONFIG_CYCLE(
            g_Config.rendering.texture_filter, rate, TEXTURE_FILTER_NUMBER_OF);
        break;
    default:
        CONFIG_CYCLE(
            g_Config.rendering.ui_filter, rate, TEXTURE_FILTER_NUMBER_OF);
        break;
    }
}

static void M_Leave(void)
{
    int32_t frame;
    int32_t goal;
    int32_t open_frame;
    int32_t count;
    if (DC_Ring_GetSelectionAnim(&frame, &goal, &open_frame, &count)) {
        DC_Ring_AnimateSelection(0, -1);
    }
    DC_Screens_Close(UI_TAKEOVER_RING_ENTRY, UI_TAKEOVER_CHOICE_CANCEL);
}

static bool M_OnTick(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    // The page waits for the entry to finish opening.
    if (!m_Page.open || DC_Ring_GetRestFrame() < 0) {
        return false;
    }
    const int32_t picked = DC_Req_Control(&m_Page.req);
    if (picked == DC_REQ_CANCELLED) {
        M_Leave();
        return false;
    }
    const M_ROW row = (M_ROW)m_Page.rows[m_Page.req.selected];
    int32_t rate = 0;
    if (picked >= 0 || g_InputDB.menu_right) {
        rate = 1;
    } else if (g_InputDB.menu_left) {
        rate = -1;
    }
    if (rate != 0 && (row != M_ROW_RESTART || picked >= 0)) {
        M_Step(row, rate);
        M_Fill();
    }
    return false;
}

static bool M_OnPaint(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (m_Page.open && DC_Ring_GetRestFrame() >= 0) {
        DC_Req_Paint(&m_Page.req);
    }
    return false;
}

bool DC_NoLua_OpenSettingsEntry(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    if (args->object_id != O_GRAPHICS_OPTION) {
        return false;
    }
    static bool listening = false;
    if (!listening) {
        listening = true;
        DC_Events_Listen(LUA_EVENT_TICK, M_OnTick);
        DC_Events_Listen(LUA_EVENT_UI_PAINT, M_OnPaint);
    }
    m_Page.req.selected = 0;
    M_Fill();
    m_Page.open = true;
    return true;
}

void DC_NoLua_ReleaseSettingsEntry(const UI_TAKEOVER screen)
{
    m_Page.open = false;
}
