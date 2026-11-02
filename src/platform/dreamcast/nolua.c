// The LUA_* functions the engine calls, without Lua: events go to C handlers
// (DC_Events_Listen), scripts and the store do nothing, and eval says no.
#include "nolua.h"

#include "dc_lua_alloc.h"

#include <trx/core/memory.h>
#include <trx/debug.h>
#include <trx/game/game_flow/common.h>
#include <trx/game/lua/common.h>
#include <trx/game/lua/events/scene.h>
#include <trx/game/lua/events/ui.h>
#include <trx/game/lua/guard.h>
#include <trx/game/lua/hooks/common.h>
#include <trx/game/lua/scripts.h>
#include <trx/game/lua/startup.h>
#include <trx/game/lua/store.h>
#include <trx/game/lua/utils/level.h>
#include <trx/game/ui/common.h>
#include <trx/game/ui/regions.h>
#include <trx/game/ui/takeover.h>

#define M_MAX_HANDLERS 8

static DC_EVENT_HANDLER m_Handlers[LUA_EVENT_NUMBER_OF][M_MAX_HANDLERS];
static int32_t m_Tick = 0;
static void (*m_TickHook)(void) = nullptr;
static bool m_HideNextFrame = false;
static bool m_IsFrameHidden = false;
static bool m_Reserving = false;
static bool m_Painting = false;

void DC_Events_Listen(const LUA_EVENT_TYPE ev, const DC_EVENT_HANDLER handler)
{
    for (int32_t i = 0; i < M_MAX_HANDLERS; i++) {
        if (m_Handlers[ev][i] == nullptr) {
            m_Handlers[ev][i] = handler;
            return;
        }
    }
    ASSERT_FAIL_FMT("nolua: too many handlers for event %d", (int)ev);
}

bool LUA_FireEventEx(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    bool taken = false;
    for (int32_t i = 0; i < M_MAX_HANDLERS && m_Handlers[ev][i] != nullptr;
         i++) {
        taken |= m_Handlers[ev][i](ev, args, arg_count);
    }
    return taken;
}

bool LUA_FireEvent(const LUA_EVENT_TYPE ev)
{
    return LUA_FireEventEx(ev, nullptr, 0);
}

bool LUA_FireEventInt32(const LUA_EVENT_TYPE ev, const int32_t arg)
{
    const LUA_EVENT_ARG a = { .type = LUA_EVENT_ARG_INT32, .value.i32 = arg };
    return LUA_FireEventEx(ev, &a, 1);
}

bool LUA_FireEventBool(const LUA_EVENT_TYPE ev, const bool arg)
{
    const LUA_EVENT_ARG a = { .type = LUA_EVENT_ARG_BOOL, .value.b = arg };
    return LUA_FireEventEx(ev, &a, 1);
}

void LUA_Tick(void)
{
    m_Tick++;
    LUA_FireEvent(LUA_EVENT_TICK);
    if (m_TickHook != nullptr) {
        m_TickHook();
    }
}

int32_t LUA_GetTick(void)
{
    return m_Tick;
}

void LUA_SetTickHook(void (*const hook)(void))
{
    m_TickHook = hook;
}

void LUA_Scene_Paint(void)
{
    LUA_FireEvent(LUA_EVENT_SCENE_PAINT);
}

void LUA_UI_SetReserving(const bool reserving)
{
    m_Reserving = reserving;
}

bool LUA_UI_IsReserving(void)
{
    return m_Reserving;
}

void LUA_UI_SetPainting(const bool painting)
{
    m_Painting = painting;
}

bool LUA_UI_IsPainting(void)
{
    return m_Painting;
}

// The C fades (nolua_ui.c) say while one holds the screen: the engine waits on
// it to start the next screen and to exit.
static bool m_HoldingScreen = false;

void LUA_UI_SetHoldingScreen(const bool holding)
{
    m_HoldingScreen = holding;
}

bool LUA_UI_IsHoldingScreen(void)
{
    return m_HoldingScreen;
}

void LUA_UI_HideNextFrame(void)
{
    m_HideNextFrame = true;
}

void LUA_UI_ReserveRegions(void)
{
    m_IsFrameHidden = m_HideNextFrame;
    m_HideNextFrame = false;
    if (m_IsFrameHidden) {
        return;
    }
    m_Reserving = true;
    LUA_FireEvent(LUA_EVENT_UI_RESERVE_ALL);
    m_Reserving = false;
}

static void M_Paint(const UI_LAYER layer, const LUA_EVENT_TYPE ev)
{
    if (m_IsFrameHidden && layer == UI_LAYER_UNDER) {
        return;
    }
    UI_BeginLayer(layer);
    m_Painting = true;
    LUA_FireEvent(ev);
    m_Painting = false;
    UI_EndLayer(layer);
}

void LUA_UI_EndScene(void)
{
    UI_Region_Layout();
    M_Paint(UI_LAYER_BACKDROP, LUA_EVENT_UI_PAINT_BACKDROP);
    M_Paint(UI_LAYER_UNDER, LUA_EVENT_UI_PAINT);
    M_Paint(UI_LAYER_FADE, LUA_EVENT_UI_PAINT_FADE);
    M_Paint(UI_LAYER_CONSOLE, LUA_EVENT_UI_PAINT_CONSOLE);
    M_Paint(UI_LAYER_EXIT, LUA_EVENT_UI_PAINT_EXIT);
}

// --- Screens ---

// The C screen for each engine screen, and whether it is open: for the
// engine, which takes its choice, or for the ring, which hears it close.
static struct {
    DC_SCREEN_OPEN open;
    void (*release)(UI_TAKEOVER screen);
    bool is_open;
    bool for_engine;
    void (*on_close)(bool confirmed);
} m_Screens[UI_TAKEOVER_NUMBER_OF];

void DC_Screens_Define(
    const UI_TAKEOVER screen, const DC_SCREEN_OPEN open,
    void (*const release)(UI_TAKEOVER))
{
    m_Screens[screen].open = open;
    m_Screens[screen].release = release;
}

static bool M_OpenScreen(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args,
    const bool for_engine, void (*const on_close)(bool))
{
    if (m_Screens[screen].open == nullptr || m_Screens[screen].is_open) {
        return false;
    }
    m_Screens[screen].is_open = true;
    m_Screens[screen].for_engine = for_engine;
    m_Screens[screen].on_close = on_close;
    if (!m_Screens[screen].open(screen, args)) {
        m_Screens[screen].is_open = false;
        return false;
    }
    return true;
}

bool DC_Screens_OpenEntry(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args,
    void (*const on_close)(bool))
{
    return M_OpenScreen(screen, args, false, on_close);
}

bool DC_Screens_IsOpen(const UI_TAKEOVER screen)
{
    return m_Screens[screen].is_open;
}

void DC_Screens_Close(
    const UI_TAKEOVER screen, const UI_TAKEOVER_CHOICE choice)
{
    if (!m_Screens[screen].is_open) {
        return;
    }
    m_Screens[screen].is_open = false;
    if (m_Screens[screen].release != nullptr) {
        m_Screens[screen].release(screen);
    }
    if (m_Screens[screen].for_engine) {
        UI_Takeover_Close(screen, choice);
    } else if (m_Screens[screen].on_close != nullptr) {
        m_Screens[screen].on_close(choice == UI_TAKEOVER_CHOICE_CONFIRM);
    }
}

void DC_Screens_Cancel(const UI_TAKEOVER screen)
{
    if (!m_Screens[screen].is_open) {
        return;
    }
    m_Screens[screen].is_open = false;
    if (m_Screens[screen].release != nullptr) {
        m_Screens[screen].release(screen);
    }
}

static bool M_TakeoverOffer(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    return M_OpenScreen(screen, args, true, nullptr);
}

static void M_TakeoverRelease(const UI_TAKEOVER screen)
{
    DC_Screens_Cancel(screen);
}

RESULT LUA_Init(void)
{
    UI_Takeover_SetHooks((UI_TAKEOVER_HOOKS) {
        .offer = M_TakeoverOffer,
        .release = M_TakeoverRelease,
    });
    return OK;
}

void LUA_Shutdown(void)
{
}

RESULT LUA_RunStartupScripts(void)
{
    return DC_NoLua_DeclareWeapons();
}

RESULT LUA_RunGameScript(void)
{
    DC_NoLua_SetupGame();
    // Before the captions, so that its hints stand over the caption.
    DC_NoLua_SetupRing();
    DC_NoLua_SetupPassport();
    DC_NoLua_SetupScreens();
    DC_NoLua_SetupHUD();
    DC_NoLua_SetupUI();
    return OK;
}

static bool m_LevelLive = false;

void LUA_RunLevelScript(const GF_LEVEL *const level)
{
    m_LevelLive = true;
    DC_NoLua_RunLevelScript(level);
}

// The level stays where it is: its script starts over, state and all.
void LUA_ReloadLevelScript(void)
{
    const GF_LEVEL *const level = GF_GetCurrentLevel();
    if (level == nullptr) {
        return;
    }
    LUA_DropLevelScript();
    LUA_RunLevelScript(level);
}

// The level's modules hear it unload before its script goes, as in
// lua/scripts.c.
void LUA_DropLevelScript(void)
{
    if (m_LevelLive) {
        m_LevelLive = false;
        LUA_FireEvent(LUA_EVENT_LEVEL_UNLOAD);
    }
    DC_NoLua_DropLevelScript();
}

LUA_RESULT LUA_Eval(const char *const code)
{
    return (LUA_RESULT) {
        .code = LUA_ERRRUN,
        .message = Memory_DupStr("no Lua in this build"),
    };
}

void LUA_FreeResult(LUA_RESULT *const result)
{
    Memory_FreePointer(&result->message);
    result->code = LUA_OK;
}

// A save written with Lua keeps its store section; this build reads past it.
RESULT LUA_Store_Load(JSON_READ_IO *const io)
{
    return OK;
}

void LUA_Store_Dump(JSON_WRITE_IO *const io)
{
}

void LUA_Store_ClearLevel(void)
{
}

void LUA_Store_ClearGame(void)
{
}

void LUA_Rooms_ApplyFlipGroups(void)
{
}

void LUA_Config_FlushPendingWatchers(void)
{
}

void LUA_Guard_Heartbeat(void)
{
}

bool LUA_Hooks_CallBoolEx(
    const LUA_HOOK_TYPE hook, const int32_t key, const bool fallback,
    const LUA_HOOK_ARG *const args, const int32_t arg_count)
{
    return fallback;
}

// The hook arguments' push functions are inline in hooks/common.h and taken by
// address where the engine calls a hook; nothing ever runs them.
void lua_pushboolean(lua_State *const L, const int b)
{
}

void LUA_PushLevel(lua_State *const L, const GF_LEVEL *const level)
{
}

void DC_LuaAlloc_GetStats(DC_LUA_ALLOC_STATS *const out)
{
    *out = (DC_LUA_ALLOC_STATS) {};
}

int32_t LUA_DC_GetMemoryKB(void)
{
    return 0;
}

int32_t LUA_DC_CollectGarbage(void)
{
    return 0;
}
