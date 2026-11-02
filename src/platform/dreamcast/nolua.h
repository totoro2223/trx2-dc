// The engine without Lua (dc_lua=false): nolua.c answers the LUA_* calls the
// engine makes, and what the scripts did is done by C handlers listening to
// the same events.
#pragma once

#include <trx/core/result.h>
#include <trx/game/game_flow/types.h>
#include <trx/game/lua/events/manager.h>
#include <trx/game/output/types.h>
#include <trx/game/ui/takeover.h>

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

// The fade layers of nolua_ui.c for the screens: a black layer at an opacity,
// which the title, a level unloading and a demo close; a move to an opacity
// over ticks, closing the layer at rest where asked, with what runs once it
// ends. FadeOpen answers 0 where there is no room for another layer.
int32_t DC_NoLua_FadeOpen(float opacity, bool holds);
void DC_NoLua_FadeMove(
    int32_t id, float target, int32_t ticks, bool close,
    void (*on_done)(int32_t id));
void DC_NoLua_FadeClose(int32_t id);
bool DC_NoLua_FadesEnabled(void);

// The screens without Lua (nolua.c). A C screen takes the engine's screen as
// it is offered, or the screen the ring opens for what the player does with
// an entry (UI_TAKEOVER_RING_ENTRY, _EXAMINE, _COMBINE). A screen ends itself
// with DC_Screens_Close; whoever opened it may cancel it, which releases it
// without its close callback.
typedef bool (*DC_SCREEN_OPEN)(UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *args);
void DC_Screens_Define(
    UI_TAKEOVER screen, DC_SCREEN_OPEN open, void (*release)(UI_TAKEOVER));
bool DC_Screens_OpenEntry(
    UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *args,
    void (*on_close)(bool confirmed));
void DC_Screens_Close(UI_TAKEOVER screen, UI_TAKEOVER_CHOICE choice);
void DC_Screens_Cancel(UI_TAKEOVER screen);
bool DC_Screens_IsOpen(UI_TAKEOVER screen);

// The inventory ring (nolua_ring.c), and what the screens of its entries
// read and steer of the entry the player picked.
void DC_NoLua_SetupRing(void);
bool DC_Ring_IsOpen(void);
INVENTORY_MODE DC_Ring_GetMode(void);
bool DC_Ring_ForcesHealthBar(void);
void DC_Ring_Request(OBJECT_ID object);
void DC_Ring_SetDarkened(bool darkened);
void DC_Ring_TurnSelection(int32_t yaw, int32_t pitch);
bool DC_Ring_GetSelectionAnim(
    int32_t *frame, int32_t *goal, int32_t *open_frame, int32_t *frame_count);
void DC_Ring_AnimateSelection(int32_t goal, int32_t direction);
int32_t DC_Ring_GetRestFrame(void);
void DC_Ring_SetHints(const char *const *parts, int32_t count);

// The game's list dialog (nolua_passport.c, widgets/requester.lua): a frame,
// a heading and rows, one of them picked, resting the given fraction of the
// way down the safe area. Control answers the row confirmed, or one of the
// DC_REQ_* below.
#define DC_REQ_MAX_ROWS 40
#define DC_REQ_NONE (-1)
#define DC_REQ_CANCELLED (-2)

typedef struct {
    char text[64];
    char right[16];
    int32_t a;
    int32_t b;
    bool rule;
} DC_REQ_ROW;

typedef struct {
    char title[64];
    DC_REQ_ROW rows[DC_REQ_MAX_ROWS];
    int32_t count;
    int32_t selected;
    int32_t scroll;
    float min_w;
    float anchor;
} DC_REQ;

void DC_Req_Init(DC_REQ *req, const char *title, float min_w, float anchor);
DC_REQ_ROW *DC_Req_Add(DC_REQ *req, const char *text);
void DC_Req_Select(DC_REQ *req, int32_t index);
int32_t DC_Req_Control(DC_REQ *req);
void DC_Req_Paint(const DC_REQ *req);

// A box of the game's menus in canvas units, and a message in a box of its
// own, its middle the given fraction of the way down the canvas.
void DC_UI_Panel(
    float x, float y, float w, float h, int32_t z, TEXT_STYLE look, bool fill);
void DC_UI_Message(const char *text, float anchor);

// The passport and the quick save and load screens (nolua_passport.c), and
// the entries of the menu ring it does not draw: the settings
// (nolua_settings.c).
void DC_NoLua_SetupPassport(void);
bool DC_NoLua_OpenSettingsEntry(
    UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *args);
void DC_NoLua_ReleaseSettingsEntry(UI_TAKEOVER screen);

// The other screens the engine offers (nolua_screens.c): pause, statistics,
// examine, combine, pictures, the level view and the globe.
void DC_NoLua_SetupScreens(void);

// What the level scripts did (nolua_levels.c): the level's entry takes over
// until it is dropped.
void DC_NoLua_RunLevelScript(const GF_LEVEL *level);
void DC_NoLua_DropLevelScript(void);
