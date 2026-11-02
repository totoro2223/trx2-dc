// The passport without Lua: what ui/screens/passport (init, pages, book,
// rows) and ui/widgets/requester.lua did. The passport entry of the ring
// turns its pages and opens the list of the page it rests on; the quick save
// and load screens and the save crystal show one page alone.
//
// shortcut: the lists have no hold-to-delete button and no "play previous
// levels", "story so far" or mod switch; the Dreamcast build never offered
// the last two, and deleting a save waits for the settings dialogs.
#include "nolua.h"

#include <platform/dreamcast/dc_games.h>
#include <platform/dreamcast/vmu.h>
#include <trx/config.h>
#include <trx/core/json/util/file.h>
#include <trx/core/json/util/read_io.h>
#include <trx/core/log.h>
#include <trx/game/game.h>
#include <trx/game/game_flow.h>
#include <trx/game/game_flow/common.h>
#include <trx/game/game_strings/entries.h>
#include <trx/game/input.h>
#include <trx/game/inventory.h>
#include <trx/game/lua/events/ui.h>
#include <trx/game/objects/names.h>
#include <trx/game/paths.h>
#include <trx/game/savegame.h>
#include <trx/game/sound.h>
#include <trx/game/ui/common.h>
#include <trx/game/ui/draw.h>
#include <trx/game/ui/scaler.h>
#include <trx/game/ui/text.h>
#include <trx/version.h>

#include <stdio.h>
#include <string.h>

// --- Menu colors (cfg/ui.json5, trx/internal/themes.lua) ---

static UI_MENU_COLORS m_Colors[2];

static RGBA_8888 M_ParseColor(const char *const text)
{
    unsigned r = 0;
    unsigned g = 0;
    unsigned b = 0;
    unsigned a = 255;
    if (text != nullptr && text[0] == '#') {
        if (sscanf(text + 1, "%2x%2x%2x%2x", &r, &g, &b, &a) < 3) {
            r = g = b = 0;
        }
    }
    return (RGBA_8888) { r, g, b, a };
}

static RESULT M_ReadColor(
    JSON_READ_IO *const io, const char *const key, RGBA_8888 *const out)
{
    const char *text = nullptr;
    MUST(JSON_READ_OPT(io, key, &text));
    if (text != nullptr) {
        *out = M_ParseColor(text);
    }
    return OK;
}

static RESULT M_ReadPair(
    JSON_READ_IO *const io, const char *const key, RGBA_8888 out[2])
{
    if (!JSON_ReadIO_HasKey(io, key)) {
        return OK;
    }
    MUST(JSON_PUSH(io, key));
    for (int32_t i = 0; i < 2 && i < JSON_ReadIO_GetArrayLength(io); i++) {
        const char *text = nullptr;
        MUST(JSON_ReadIO_ReadIndex(
            io, (size_t)i, &text, JSON_READ_IO_READ_FN_FOR(&text)));
        out[i] = M_ParseColor(text);
    }
    return JSON_POP(io);
}

static RESULT M_ReadMenuColors(JSON_READ_IO *const io)
{
    char game[8];
    snprintf(game, sizeof(game), "tr%d", (int)g_TRVersion);
    MUST(JSON_PUSH(io, "ui"));
    MUST(JSON_PUSH(io, game));
    if (JSON_ReadIO_HasKey(io, "pc")) {
        UI_MENU_COLORS_PC *const c = &m_Colors[UI_STYLE_PC].pc;
        MUST(JSON_PUSH(io, "pc"));
        MUST(M_ReadPair(io, "background", c->background));
        MUST(M_ReadPair(io, "background_heavy", c->background_heavy));
        MUST(M_ReadColor(io, "outline_light", &c->outline_light));
        MUST(M_ReadColor(io, "outline_dark", &c->outline_dark));
        MUST(JSON_POP(io));
    }
    if (JSON_ReadIO_HasKey(io, "ps1")) {
        UI_MENU_COLORS_PS1 *const c = &m_Colors[UI_STYLE_PS1].ps1;
        MUST(JSON_PUSH(io, "ps1"));
#define M_C(name) MUST(M_ReadColor(io, #name, &c->name))
        M_C(background_edge);
        M_C(background_center);
        M_C(background_heavy_edge);
        M_C(background_heavy_center);
        M_C(heading_edge);
        M_C(heading_center);
        M_C(requested_edge);
        M_C(requested_center);
        M_C(requested_outline_ch);
        M_C(requested_outline_cv);
        M_C(requested_outline_edge);
        M_C(outline_tl);
        M_C(outline_tr);
        M_C(outline_bl);
        M_C(outline_br);
        M_C(heading_outline);
#undef M_C
        MUST(JSON_POP(io));
    }
    MUST(JSON_POP(io));
    return JSON_POP(io);
}

static void M_LoadMenuColors(void)
{
    const char *path = nullptr;
    if (!IS_OK(GamePath_Resolve(
            GAME_DYNAMIC_PATH_COMMON_CONFIG, "ui.json5", &path))) {
        return;
    }
    JSON_VALUE *root = nullptr;
    if (!IS_OK(JSONFile_ReadRequired(path, &root))) {
        return;
    }
    JSON_READ_IO *const io = JSON_ReadIO_Create(root, 0, path);
    if (!IS_OK(M_ReadMenuColors(io))) {
        LOG_WARNING("passport: the menu colors of ui.json5 could not be read");
    }
    JSON_ReadIO_Destroy(io);
    JSON_ValueFree(root);
}

// A box the game draws behind a dialog, in canvas units, in the style the
// player chose: filled, or the outline alone.
void DC_UI_Panel(
    const float x, const float y, const float w, const float h,
    const int32_t z, const TEXT_STYLE look, const bool fill)
{
    const UI_STYLE style = g_Config.ui.menu_style;
    const UI_MENU_COLORS *const colors = &m_Colors[style == UI_STYLE_PC ? 0 : 1];
    const int32_t x0 = UI_ScaleX(x);
    const int32_t y0 = UI_ScaleY(y);
    const int32_t sw = UI_ScaleX(w);
    const int32_t sh = UI_ScaleY(h);
    if (fill) {
        UI_ScheduleDrawTextBackground(style, colors, x0, y0, z, sw, sh, look);
    }
    UI_ScheduleDrawTextOutline(style, colors, x0, y0, z, sw, sh, look);
}

// --- The list dialog (widgets/requester.lua) ---

#define M_TEXT_HEIGHT 15
#define M_BODY_PAD_Y 4
#define M_TITLE_PAD_X 10
#define M_TITLE_SPACING 3
#define M_HINT_HEIGHT 7
#define M_MAX_VISIBLE 10
#define M_PASSPORT_WIDTH 300.0f
#define M_MESSAGE_PAD 8

static float M_OuterPad(void)
{
    return g_TRVersion == 1 ? 2.0f : 3.0f;
}

static float M_BodyPadX(void)
{
    return g_TRVersion == 1 ? 8.0f : 4.0f;
}

static float M_TitlePadY(void)
{
    return g_TRVersion == 1 ? 2.0f : 1.0f;
}

static float M_RowSpacing(void)
{
    return g_TRVersion == 1 ? 2.0f : 3.0f;
}

static void M_Measure(const char *const text, float *const w, float *const h)
{
    UI_Text_Measure(text, w, h, (UI_TEXT_SETTINGS) { .scale = 1.0f });
}

static void M_Text(
    const char *const text, const float x, const float y, const float scale)
{
    UI_Text_Draw(text, x, y, (UI_TEXT_SETTINGS) { .scale = scale });
}

void DC_Req_Init(
    DC_REQ *const req, const char *const title, const float min_w,
    const float anchor)
{
    *req = (DC_REQ) { .min_w = min_w, .anchor = anchor };
    snprintf(req->title, sizeof(req->title), "%s", title != nullptr ? title : "");
}

DC_REQ_ROW *DC_Req_Add(DC_REQ *const req, const char *const text)
{
    if (req->count >= DC_REQ_MAX_ROWS) {
        return nullptr;
    }
    DC_REQ_ROW *const row = &req->rows[req->count++];
    *row = (DC_REQ_ROW) {};
    snprintf(row->text, sizeof(row->text), "%s", text);
    return row;
}

void DC_Req_Select(DC_REQ *const req, const int32_t index)
{
    if (req->count == 0) {
        return;
    }
    req->selected = MAX(0, MIN(index, req->count - 1));
    if (req->selected < req->scroll) {
        req->scroll = req->selected;
    } else if (req->selected >= req->scroll + M_MAX_VISIBLE) {
        req->scroll = req->selected - M_MAX_VISIBLE + 1;
    }
}

int32_t DC_Req_Control(DC_REQ *const req)
{
    if (g_InputDB.menu_back) {
        return DC_REQ_CANCELLED;
    }
    if (req->count == 0) {
        return DC_REQ_NONE;
    }
    if (g_InputDB.menu_up && req->selected > 0) {
        DC_Req_Select(req, req->selected - 1);
    } else if (g_InputDB.menu_down && req->selected < req->count - 1) {
        DC_Req_Select(req, req->selected + 1);
    } else if (g_InputDB.menu_confirm) {
        return req->selected;
    }
    return DC_REQ_NONE;
}

// The dialog's size and place in canvas units: every list rests on the same
// line, the given fraction of the way down the safe area, whatever its own
// height.
static void M_ReqBox(
    const DC_REQ *const req, float *const x, float *const y, float *const w,
    float *const h)
{
    const float s = UI_Scaler_GetTextScale();
    float width = req->min_w;
    for (int32_t i = 0; i < req->count; i++) {
        float tw;
        float th;
        M_Measure(req->rows[i].text, &tw, &th);
        float rw = tw / s;
        if (req->rows[i].right[0] != '\0') {
            M_Measure(req->rows[i].right, &tw, &th);
            rw += tw / s + 20.0f;
        }
        width = MAX(width, rw + 2 * M_BodyPadX());
    }
    const int32_t visible = MIN(req->count, M_MAX_VISIBLE);
    const bool scrolls = req->count > M_MAX_VISIBLE;
    float height = 2 * (M_OuterPad() + M_BODY_PAD_Y)
        + visible * M_TEXT_HEIGHT + MAX(visible - 1, 0) * M_RowSpacing();
    if (req->title[0] != '\0') {
        height += M_TEXT_HEIGHT + 2 * M_TitlePadY() + M_TITLE_SPACING;
    }
    if (scrolls) {
        height += 2 * M_HINT_HEIGHT;
    }
    // The standard page: a titled list long enough to scroll.
    const float page = 2 * (M_OuterPad() + M_BODY_PAD_Y) + M_TEXT_HEIGHT
        + 2 * M_TitlePadY() + M_TITLE_SPACING + 2 * M_HINT_HEIGHT
        + M_MAX_VISIBLE * M_TEXT_HEIGHT + (M_MAX_VISIBLE - 1) * M_RowSpacing();
    const float sw = width * s;
    const float top = UI_GetSafeCanvasTopFor(sw);
    const float avail = UI_GetSafeCanvasHeightFor(sw) / s;
    const float line = (avail - page - 5.0f) * req->anchor + page / 2.0f;
    *w = sw;
    *h = height * s;
    *x = (UI_GetCanvasWidth() - sw) / 2.0f;
    *y = top + line * s - *h / 2.0f;
}

void DC_Req_Paint(const DC_REQ *const req)
{
    const float s = UI_Scaler_GetTextScale();
    float x;
    float y;
    float w;
    float h;
    M_ReqBox(req, &x, &y, &w, &h);
    DC_UI_Panel(x, y, w, h, 0, TS_BACKGROUND, true);
    const float pad = M_OuterPad() * s;
    float at = y + pad;
    if (req->title[0] != '\0') {
        const float hh = (M_TEXT_HEIGHT + 2 * M_TitlePadY()) * s;
        DC_UI_Panel(x + pad, at, w - 2 * pad, hh, 0, TS_HEADING, true);
        float tw;
        float th;
        M_Measure(req->title, &tw, &th);
        M_Text(req->title, x + (w - tw) / 2.0f, at + (hh - th) / 2.0f, 1.0f);
        at += hh + M_TITLE_SPACING * s;
    }
    at += M_BODY_PAD_Y * s;
    const bool scrolls = req->count > M_MAX_VISIBLE;
    if (scrolls) {
        if (req->scroll > 0) {
            float tw;
            float th;
            M_Measure("\\{arrow up}", &tw, &th);
            M_Text("\\{arrow up}", x + (w - tw * 0.7f) / 2.0f, at, 0.7f);
        }
        at += M_HINT_HEIGHT * s;
    }
    const float left = x + pad + M_BodyPadX() * s;
    const float right = x + w - pad - M_BodyPadX() * s;
    const int32_t end = MIN(req->count, req->scroll + M_MAX_VISIBLE);
    for (int32_t i = req->scroll; i < end; i++) {
        const DC_REQ_ROW *const row = &req->rows[i];
        const float rh = M_TEXT_HEIGHT * s;
        if (i == req->selected) {
            DC_UI_Panel(
                left - 2 * s, at - s, right - left + 4 * s, rh + 2 * s, 0,
                TS_REQUESTED, true);
        }
        float tw;
        float th;
        M_Measure(row->text, &tw, &th);
        const float ty = at + (rh - th) / 2.0f;
        if (row->right[0] != '\0') {
            M_Text(row->text, left, ty, 1.0f);
            float rw;
            M_Measure(row->right, &rw, &th);
            M_Text(row->right, right - rw, ty, 1.0f);
        } else {
            M_Text(row->text, x + (w - tw) / 2.0f, ty, 1.0f);
        }
        at += rh + M_RowSpacing() * s;
    }
    if (scrolls && end < req->count) {
        float tw;
        float th;
        M_Measure("\\{arrow down}", &tw, &th);
        M_Text("\\{arrow down}", x + (w - tw * 0.7f) / 2.0f, at, 0.7f);
    }
}

// A message in a box of its own, its middle the given fraction of the way
// down the canvas.
void DC_UI_Message(const char *const text, const float anchor)
{
    const float s = UI_Scaler_GetTextScale();
    float tw;
    float th;
    M_Measure(text, &tw, &th);
    const float w = tw + 2 * M_MESSAGE_PAD * s;
    const float h = th + 2 * M_MESSAGE_PAD * s;
    const float x = (UI_GetCanvasWidth() - w) / 2.0f;
    const float y = UI_GetCanvasHeight() * anchor - h / 2.0f;
    DC_UI_Panel(x, y, w, h, 0, TS_BACKGROUND, true);
    M_Text(text, x + M_MESSAGE_PAD * s, y + M_MESSAGE_PAD * s, 1.0f);
}

// --- Pages (passport/pages.lua) ---

#define M_FRAMES_PER_PAGE 5
#define M_PAGES 3
// The box that says how the save went holds for a moment before the
// passport closes.
#define M_RESULT_HOLD 60

typedef enum {
    M_PAGE_NONE,
    M_PAGE_LOAD_GAME,
    M_PAGE_SAVE_GAME,
    M_PAGE_NEW_GAME,
    M_PAGE_SELECT_LEVEL,
    M_PAGE_RESTART_LEVEL,
    M_PAGE_EXIT_TO_TITLE,
    M_PAGE_EXIT_GAME,
} M_PAGE;

typedef enum {
    M_LIST_PAGE,
    M_LIST_LEVEL_MODE,
} M_LIST;

typedef enum {
    M_CHOICE_NEW_GAME,
    M_CHOICE_NEW_GAME_PLUS,
    M_CHOICE_DC_EXPANSION,
} M_CHOICE;

typedef struct {
    M_PAGE role;
    bool available;
} M_BOOK_PAGE;

static const char *M_CaptionKey(const M_PAGE page)
{
    switch (page) {
    case M_PAGE_LOAD_GAME:
        return "general/passport/load_game";
    case M_PAGE_SAVE_GAME:
        return "general/passport/save_game";
    case M_PAGE_NEW_GAME:
        return "general/passport/new_game";
    case M_PAGE_SELECT_LEVEL:
        return "general/passport/select_level";
    case M_PAGE_RESTART_LEVEL:
        return "general/passport/restart_level";
    case M_PAGE_EXIT_TO_TITLE:
        return "general/passport/exit_to_title";
    case M_PAGE_EXIT_GAME:
        return "general/passport/exit_game";
    default:
        return "";
    }
}

static bool M_IsFlat(const M_PAGE page)
{
    return page == M_PAGE_RESTART_LEVEL || page == M_PAGE_EXIT_TO_TITLE
        || page == M_PAGE_EXIT_GAME;
}

static const char *M_GS(const char *const key, const char *const fallback)
{
    const char *const text = GameString_Get(key);
    return text != nullptr && strcmp(text, key) != 0 ? text : fallback;
}

static bool M_IsTitle(void)
{
    const GF_LEVEL *const level = Game_GetCurrentLevel();
    return level == nullptr || level->type == GFL_TITLE;
}

static bool M_IsInGym(void)
{
    const GF_LEVEL *const level = Game_GetCurrentLevel();
    return level != nullptr && level->type == GFL_GYM;
}

static bool M_SavingEnabled(void)
{
    return SG_Manager_GetSlotCount(SAVEGAME_SLOT_POOL_NORMAL) > 0
        && !g_Config.flow.load_save_disabled;
}

static bool M_GameModesAvailable(void)
{
    switch (g_Config.gameplay.game_modes_policy) {
    case GAME_MODES_POLICY_NEVER:
        return false;
    case GAME_MODES_POLICY_ALWAYS:
        return true;
    default:
        return g_Config.profile.new_game_plus_unlock;
    }
}

// The quick saves first, then the numbered slots.
static int32_t M_SlotCount(const SAVEGAME_SLOT_POOL pool)
{
    return pool == SAVEGAME_SLOT_POOL_QUICK ? SG_Manager_GetQuickVisualCount()
                                            : SG_Manager_GetSlotCount(pool);
}

static SAVEGAME_SLOT_REF M_Slot(
    const int32_t index, const SAVEGAME_SLOT_POOL pool)
{
    return pool == SAVEGAME_SLOT_POOL_QUICK
        ? SG_Manager_QuickFromVisualIndex(index)
        : SG_Manager_NormalSlot(index);
}

static bool M_HasSave(const SAVEGAME_SLOT_REF slot)
{
    return SG_Manager_IsValidSlotRef(slot) && !SG_Manager_IsSlotFree(slot);
}

static bool M_HasSaves(void)
{
    for (int32_t p = 0; p < 2; p++) {
        const SAVEGAME_SLOT_POOL pool =
            p == 0 ? SAVEGAME_SLOT_POOL_QUICK : SAVEGAME_SLOT_POOL_NORMAL;
        for (int32_t i = 0; i < M_SlotCount(pool); i++) {
            if (M_HasSave(M_Slot(i, pool))) {
                return true;
            }
        }
    }
    return false;
}

static bool M_CanRestartLevel(void)
{
    const GF_LEVEL *const level = Game_GetCurrentLevel();
    return level != nullptr
        && (level->type == GFL_NORMAL || level->type == GFL_GYM
            || level->type == GFL_BONUS)
        && Savegame_RestartAvailable(SG_Manager_GetBoundSlot());
}

static void M_DeterminePages(
    const INVENTORY_MODE mode, M_BOOK_PAGE pages[M_PAGES])
{
    for (int32_t i = 0; i < M_PAGES; i++) {
        pages[i] = (M_BOOK_PAGE) {};
    }
#define M_SET(n, r, a) (pages[(n) - 1] = (M_BOOK_PAGE) { (r), (a) })
    const bool saves = M_HasSaves() && M_SavingEnabled();
    const bool can_restart = M_CanRestartLevel();
    switch (mode) {
    case INV_TITLE_MODE:
        M_SET(1, M_PAGE_LOAD_GAME, saves);
        M_SET(2, M_PAGE_NEW_GAME, true);
        M_SET(3, M_PAGE_EXIT_GAME, true);
        break;
    case INV_GAME_MODE:
        if (!M_SavingEnabled()) {
            M_SET(2, M_PAGE_RESTART_LEVEL, can_restart);
        } else {
            M_SET(1, M_PAGE_LOAD_GAME, saves);
            M_SET(2, M_PAGE_SAVE_GAME, true);
        }
        M_SET(3, M_PAGE_EXIT_TO_TITLE, true);
        break;
    case INV_LOAD_MODE:
        if (!M_SavingEnabled()) {
            M_SET(2, M_PAGE_RESTART_LEVEL, can_restart);
        } else if (saves) {
            M_SET(1, M_PAGE_LOAD_GAME, true);
        } else {
            M_SET(2, M_PAGE_SAVE_GAME, true);
        }
        break;
    case INV_SAVE_MODE:
    case INV_SAVE_CRYSTAL_MODE:
        if (!M_SavingEnabled()) {
            M_SET(2, M_PAGE_RESTART_LEVEL, can_restart);
        } else {
            M_SET(2, M_PAGE_SAVE_GAME, true);
        }
        break;
    case INV_DEATH_MODE:
        M_SET(1, M_PAGE_LOAD_GAME, saves);
        M_SET(2, M_PAGE_RESTART_LEVEL, can_restart);
        M_SET(3, M_PAGE_EXIT_TO_TITLE, true);
        break;
    default:
        break;
    }
#undef M_SET

    // The gym offers a new game in place of a save. A level that forbids
    // manual saves offers a restart, or nothing.
    for (int32_t i = 0; i < M_PAGES; i++) {
        if (pages[i].role != M_PAGE_SAVE_GAME) {
            continue;
        }
        if (M_IsInGym()) {
            pages[i].role = M_PAGE_NEW_GAME;
        } else if (
            !Savegame_IsManualSaveAllowed() && mode != INV_SAVE_CRYSTAL_MODE) {
            if (can_restart) {
                pages[i].role = M_PAGE_RESTART_LEVEL;
            } else {
                pages[i].available = false;
            }
        }
    }
    if (g_Config.flow.play_any_level) {
        for (int32_t i = 0; i < M_PAGES; i++) {
            if (pages[i].role == M_PAGE_NEW_GAME) {
                pages[i].role = M_PAGE_SELECT_LEVEL;
            }
        }
    }

    // An exit on the last page when no page is available, so that the
    // player always has a way out.
    for (int32_t i = 0; i < M_PAGES; i++) {
        if (pages[i].available) {
            return;
        }
    }
    pages[2] = (M_BOOK_PAGE) {
        mode == INV_TITLE_MODE ? M_PAGE_EXIT_GAME : M_PAGE_EXIT_TO_TITLE,
        true,
    };
}

static int32_t M_FirstAvailable(const M_BOOK_PAGE pages[M_PAGES])
{
    for (int32_t i = 0; i < M_PAGES; i++) {
        if (pages[i].available) {
            return i;
        }
    }
    return -1;
}

// TR1 opens a page only after the player confirms it; the later games open
// a page as soon as the book stops on it.
static bool M_BrowsesFirst(const INVENTORY_MODE mode)
{
    return g_TRVersion == 1
        && (mode == INV_TITLE_MODE || mode == INV_GAME_MODE
            || mode == INV_DEATH_MODE);
}

// --- Rows (passport/rows.lua) ---

static void M_SlotRows(DC_REQ *const req, const bool hide_quick)
{
    for (int32_t p = 0; p < 2; p++) {
        const SAVEGAME_SLOT_POOL pool =
            p == 0 ? SAVEGAME_SLOT_POOL_QUICK : SAVEGAME_SLOT_POOL_NORMAL;
        if (hide_quick && pool == SAVEGAME_SLOT_POOL_QUICK) {
            continue;
        }
        for (int32_t i = 0; i < M_SlotCount(pool); i++) {
            const SAVEGAME_SLOT_REF slot = M_Slot(i, pool);
            char text[64];
            const SAVEGAME_INFO *const info =
                M_HasSave(slot) ? SG_Manager_GetSavegameInfo(slot) : nullptr;
            if (info == nullptr) {
                snprintf(
                    text, sizeof(text), GS("general/misc/empty_slot_fmt"),
                    i + 1);
            } else if (info->counter <= 0) {
                snprintf(text, sizeof(text), "%s", info->level_title);
            } else if (g_TRVersion == 1) {
                snprintf(
                    text, sizeof(text), "%s %d%s", info->level_title,
                    (int)info->counter, info->is_quick ? " (QS)" : "");
            } else {
                snprintf(text, sizeof(text), "%s", info->level_title);
            }
            DC_REQ_ROW *const row = DC_Req_Add(req, text);
            if (row == nullptr) {
                return;
            }
            row->a = i;
            row->b = pool;
            if (info != nullptr && info->counter > 0 && g_TRVersion != 1) {
                if (info->is_quick) {
                    snprintf(
                        row->right, sizeof(row->right), "QS %d",
                        (int)info->counter);
                } else {
                    snprintf(
                        row->right, sizeof(row->right), "%d", (int)info->counter);
                }
            }
        }
    }
}

// The cursor on the save that the game last used.
static void M_SelectRecentSlot(DC_REQ *const req)
{
    SAVEGAME_SLOT_REF slot = SG_Manager_GetMostRecentlyUsedSlot();
    if (!SG_Manager_IsValidSlotRef(slot)) {
        slot = SG_Manager_GetMostRecentlyCreatedSlot();
    }
    if (!SG_Manager_IsValidSlotRef(slot)) {
        return;
    }
    const int32_t index = slot.pool == SAVEGAME_SLOT_POOL_QUICK
        ? SG_Manager_QuickToVisualIndex(slot)
        : slot.index;
    for (int32_t i = 0; i < req->count; i++) {
        if (req->rows[i].a == index && req->rows[i].b == (int32_t)slot.pool) {
            DC_Req_Select(req, i);
            return;
        }
    }
}

static void M_ModeRows(DC_REQ *const req)
{
    DC_REQ_ROW *row = DC_Req_Add(
        req, M_GS("general/passport/mode_new_game", "New Game"));
    row->a = M_CHOICE_NEW_GAME;
    if (M_GameModesAvailable()) {
        row = DC_Req_Add(
            req, M_GS("general/passport/mode_new_game_plus", "New Game+"));
        row->a = M_CHOICE_NEW_GAME_PLUS;
    }
}

static void M_NewGameRows(DC_REQ *const req)
{
    M_ModeRows(req);
    // A Dreamcast disc with the expansion of this game.
    if (DC_Games_ExpansionAvailable()) {
        DC_REQ_ROW *const row = DC_Req_Add(
            req, M_GS("general/passport/dc_expansion", "Play Expansion"));
        row->a = M_CHOICE_DC_EXPANSION;
        row->rule = true;
    }
}

static void M_AllLevelRows(DC_REQ *const req)
{
    const GF_LEVEL_TABLE *const table = GF_GetLevelTable(GFLT_MAIN);
    for (int32_t i = 0; i < table->count; i++) {
        const GF_LEVEL *const level = &table->levels[i];
        if (level->type == GFL_GYM || level->type == GFL_DUMMY
            || level->type == GFL_CURRENT) {
            continue;
        }
        DC_REQ_ROW *const row = DC_Req_Add(req, level->title);
        if (row == nullptr) {
            return;
        }
        row->a = level->num;
    }
}

// --- The book (passport/book.lua) ---

static struct {
    bool open;
    UI_TAKEOVER screen;
    INVENTORY_MODE mode;
    M_BOOK_PAGE pages[M_PAGES];
    int32_t index;
    bool standalone;
    bool crystal;
    bool browse;
    // The page the list was built for, M_PAGE_NONE for none yet.
    M_PAGE opened;
    // A new game page with one choice shows no list and starts the game.
    bool bare;
    bool has_page;
    DC_REQ page;
    bool has_list;
    M_LIST list_role;
    DC_REQ list;
    int32_t chosen_level;
    const char *message;
    int32_t save_ticks;
    int32_t close_ticks;
    SAVEGAME_SLOT_REF save_slot;
    bool closing;
} m_Book;

static void M_Override(const GF_COMMAND command)
{
    GF_OverrideCommand(command, !UI_Takeover_IsAnyHeld());
}

static int32_t M_PageAt(const int32_t frame)
{
    int32_t f;
    int32_t goal;
    int32_t open_frame;
    int32_t count;
    if (frame < 0 || !DC_Ring_GetSelectionAnim(&f, &goal, &open_frame, &count)) {
        return -1;
    }
    const int32_t offset = frame - open_frame;
    if (offset < 0 || offset % M_FRAMES_PER_PAGE != 0) {
        return -1;
    }
    return offset / M_FRAMES_PER_PAGE;
}

static void M_TurnTo(const int32_t index)
{
    int32_t frame;
    int32_t goal;
    int32_t open_frame;
    int32_t count;
    if (!DC_Ring_GetSelectionAnim(&frame, &goal, &open_frame, &count)) {
        return;
    }
    const int32_t target = open_frame + M_FRAMES_PER_PAGE * index;
    if (goal == target) {
        return;
    }
    DC_Ring_AnimateSelection(target, target > goal ? 1 : -1);
    Sound_Effect(SFX_MENU_PASSPORT, nullptr, SPM_ALWAYS);
}

static int32_t M_Neighbour(const int32_t step)
{
    for (int32_t i = m_Book.index + step; i >= 0 && i < M_PAGES; i += step) {
        if (m_Book.pages[i].available) {
            return i;
        }
    }
    return -1;
}

static bool M_CanFlip(void)
{
    return m_Book.browse || g_TRVersion != 1;
}

static bool M_CanBackOut(void)
{
    return DC_Ring_GetMode() != INV_DEATH_MODE || m_Book.standalone;
}

// Names the page the book rests on, or the list over it, and shows where
// the book turns to; the caption hides while the book turns.
static void M_SyncCaption(void)
{
    if (m_Book.standalone) {
        return;
    }
    const bool resting = M_PageAt(DC_Ring_GetRestFrame()) == m_Book.index;
    const char *key = M_CaptionKey(m_Book.pages[m_Book.index].role);
    if (m_Book.has_list && m_Book.list_role == M_LIST_LEVEL_MODE) {
        key = "general/passport/select_level";
    }
    DC_NoLua_SetCaption(resting ? GS(key) : nullptr, false, false);
    const bool alone = !m_Book.has_list;
    DC_NoLua_ShowArrow(
        DC_ARROW_CAPTION_LEFT,
        resting && alone && M_CanFlip() && M_Neighbour(-1) >= 0);
    DC_NoLua_ShowArrow(
        DC_ARROW_CAPTION_RIGHT,
        resting && alone && M_CanFlip() && M_Neighbour(1) >= 0);
}

static void M_ClearCaption(void)
{
    DC_NoLua_SetCaption(nullptr, false, false);
    DC_NoLua_ShowArrow(DC_ARROW_CAPTION_LEFT, false);
    DC_NoLua_ShowArrow(DC_ARROW_CAPTION_RIGHT, false);
}

// Shuts the book towards the nearer cover. A confirmed close leaves the
// ring; otherwise the entry goes back into the ring.
static void M_Close(const bool confirmed)
{
    if (!m_Book.standalone) {
        int32_t frame;
        int32_t goal;
        int32_t open_frame;
        int32_t count;
        if (DC_Ring_GetSelectionAnim(&frame, &goal, &open_frame, &count)) {
            if (m_Book.index == M_PAGES - 1) {
                DC_Ring_AnimateSelection(count - 1, 1);
            } else {
                DC_Ring_AnimateSelection(0, -1);
            }
        }
    }
    const UI_TAKEOVER screen = m_Book.screen;
    DC_Screens_Close(
        screen,
        confirmed && (!m_Book.standalone || m_Book.crystal)
            ? UI_TAKEOVER_CHOICE_CONFIRM
            : UI_TAKEOVER_CHOICE_CANCEL);
}

static bool M_OpenPage(const M_PAGE role)
{
    m_Book.bare = false;
    m_Book.has_page = false;
    const float anchor = M_IsTitle() ? 0.98f : 0.67f;
    if (role == M_PAGE_NEW_GAME) {
        DC_Req_Init(
            &m_Book.page, M_GS("general/passport/select_mode", "Select Mode"),
            0.0f, anchor);
        M_NewGameRows(&m_Book.page);
        if (m_Book.page.count == 1) {
            m_Book.bare = true;
            return true;
        }
    } else if (role == M_PAGE_LOAD_GAME || role == M_PAGE_SAVE_GAME) {
        DC_Req_Init(&m_Book.page, GS(M_CaptionKey(role)), M_PASSPORT_WIDTH, anchor);
        // The one quick save (L + R + Start) is not a slot to save to.
        M_SlotRows(&m_Book.page, role == M_PAGE_SAVE_GAME);
        M_SelectRecentSlot(&m_Book.page);
    } else if (role == M_PAGE_SELECT_LEVEL) {
        DC_Req_Init(
            &m_Book.page, GS("general/passport/select_level"), M_PASSPORT_WIDTH,
            anchor);
        M_AllLevelRows(&m_Book.page);
    } else {
        return false;
    }
    m_Book.has_page = true;
    return true;
}

static void M_ClearPage(void)
{
    m_Book.has_page = false;
    m_Book.opened = M_PAGE_NONE;
    m_Book.bare = false;
}

static bool M_Flip(const int32_t step)
{
    const int32_t next = M_Neighbour(step);
    if (next < 0) {
        return false;
    }
    m_Book.index = next;
    M_ClearPage();
    return true;
}

static void M_FlipKeys(void)
{
    if (!M_CanFlip()) {
        return;
    }
    if (g_InputDB.menu_left) {
        M_Flip(-1);
    } else if (g_InputDB.menu_right) {
        M_Flip(1);
    }
}

static void M_Say(const char *const text)
{
    m_Book.message = text;
}

static void M_DoSave(void)
{
    bool ok;
    if (m_Book.crystal) {
        // The crystal is spent before the save, so that the save does not
        // hold it, and given back where the save fails.
        Inv_RemoveItem(O_SAVE_CRYSTAL_ITEM);
        ok = Savegame_Save(m_Book.save_slot);
        if (!ok) {
            Inv_AddItem(O_SAVE_CRYSTAL_ITEM);
        }
    } else {
        ok = Savegame_Save(m_Book.save_slot);
    }
    static const char *const results[] = {
        "",
        "Saved to the VMU",
        "Could not save to the VMU",
        "No VMU found: saved until the console is switched off",
    };
    const int result = DC_VMU_LastResult();
    M_Say(
        ok && result > 0 && result < 4 ? results[result]
            : ok                       ? "Saved"
                                       : "Could not save the game");
    m_Book.close_ticks = M_RESULT_HOLD;
}

// The Dreamcast writes the save to the memory card with the game stopped,
// so the box that says so is drawn first: the save follows two ticks later,
// and the passport closes after the result has shown.
static void M_SaveWithNotice(const SAVEGAME_SLOT_REF slot)
{
    m_Book.save_slot = slot;
    M_Say("Saving to VMU... Do not remove the card");
    m_Book.save_ticks = 2;
}

static void M_StartNewGame(const bool ng_plus)
{
    M_Override((GF_COMMAND) {
        .action = GF_NEW_GAME,
        .param = ng_plus ? GBF_NGPLUS : GBF_NONE,
    });
}

// Starts a level the way the level select does: Lara's inventory is rebuilt
// to what she would carry on reaching it.
static void M_PlayLevel(const int32_t level_num, const bool ng_plus)
{
    Game_SetBonusFlag(ng_plus ? GBF_NGPLUS : GBF_NONE);
    SG_Manager_UnbindSlot();
    M_Override((GF_COMMAND) {
        .action = GF_SELECT_GAME,
        .param = level_num,
    });
}

static void M_Choose(const M_LIST list, const DC_REQ_ROW *const row)
{
    const M_PAGE role = m_Book.pages[m_Book.index].role;
    if (list == M_LIST_LEVEL_MODE) {
        M_PlayLevel(m_Book.chosen_level, row->a == M_CHOICE_NEW_GAME_PLUS);
        M_Close(true);
    } else if (role == M_PAGE_NEW_GAME) {
        if (row->a == M_CHOICE_DC_EXPANSION) {
            // Ends the game: the disc starts over in the expansion.
            if (DC_Games_ChooseExpansion(-2)) {
                GF_OverrideCommand(
                    (GF_COMMAND) { .action = GF_EXIT_GAME }, true);
            }
            return;
        }
        M_StartNewGame(row->a == M_CHOICE_NEW_GAME_PLUS);
        M_Close(true);
    } else if (role == M_PAGE_LOAD_GAME) {
        const SAVEGAME_SLOT_REF slot = M_Slot(row->a, row->b);
        if (!M_HasSave(slot)) {
            return;
        }
        M_Override((GF_COMMAND) {
            .action = GF_START_SAVED_GAME,
            .param = SG_Manager_SlotToParam(slot),
        });
        M_Close(true);
    } else if (role == M_PAGE_SAVE_GAME) {
        // Quick saves are listed but are not save targets.
        if (row->b != SAVEGAME_SLOT_POOL_NORMAL) {
            return;
        }
        M_SaveWithNotice(M_Slot(row->a, row->b));
    } else if (role == M_PAGE_SELECT_LEVEL) {
        if (M_GameModesAvailable()) {
            m_Book.chosen_level = row->a;
            DC_Req_Init(
                &m_Book.list,
                M_GS("general/passport/select_mode", "Select Mode"), 0.0f,
                M_IsTitle() ? 0.98f : 0.67f);
            M_ModeRows(&m_Book.list);
            m_Book.list_role = M_LIST_LEVEL_MODE;
            m_Book.has_list = true;
            return;
        }
        M_PlayLevel(row->a, false);
        M_Close(true);
    }
}

// Leaves an open page. TR1 goes back to turning the pages; the later games
// and the quick save and load screens put the passport away, except when
// Lara is dead.
static void M_BackOut(void)
{
    if (!m_Book.standalone && M_BrowsesFirst(DC_Ring_GetMode())) {
        m_Book.browse = true;
        M_ClearPage();
    } else if (M_CanBackOut()) {
        M_Close(false);
    }
}

static bool M_ControlPage(void)
{
    const int32_t picked = DC_Req_Control(&m_Book.page);
    if (picked == DC_REQ_CANCELLED) {
        M_BackOut();
        return true;
    } else if (picked >= 0) {
        M_Choose(M_LIST_PAGE, &m_Book.page.rows[picked]);
        return true;
    }
    return false;
}

static void M_RunFlat(const M_PAGE role)
{
    if (m_Book.bare) {
        M_StartNewGame(false);
    } else if (role == M_PAGE_RESTART_LEVEL) {
        const GF_LEVEL *const level = Game_GetCurrentLevel();
        M_Override((GF_COMMAND) {
            .action = GF_RESTART_GAME,
            .param = level != nullptr ? level->num : 0,
        });
    } else if (role == M_PAGE_EXIT_TO_TITLE) {
        M_Override((GF_COMMAND) { .action = GF_EXIT_TO_TITLE });
    } else {
        M_Override((GF_COMMAND) { .action = GF_EXIT_GAME });
    }
    M_Close(true);
}

static void M_ControlBook(void)
{
    const M_PAGE role = m_Book.pages[m_Book.index].role;

    // Input turns the pages during a turn only with the responsive passport
    // setting.
    const int32_t showing = M_PageAt(DC_Ring_GetRestFrame());
    if (showing < 0) {
        if (g_Config.input.enable_responsive_passport) {
            M_FlipKeys();
        }
        return;
    }
    if (showing != m_Book.index) {
        M_TurnTo(m_Book.index);
        return;
    }

    bool confirmed = false;
    if (m_Book.browse) {
        if (g_InputDB.menu_confirm) {
            confirmed = true;
            m_Book.browse = false;
            // A flat page has no list to open, so the same press acts on it
            // below.
            if (!M_IsFlat(role)) {
                return;
            }
        } else {
            if (g_InputDB.menu_back) {
                if (M_CanBackOut()) {
                    M_Close(false);
                }
            } else {
                M_FlipKeys();
            }
            return;
        }
    }

    // Builds the page once, when the book stops on it, because the page
    // reads every save slot.
    if (m_Book.opened != role) {
        m_Book.opened = role;
        if (!M_IsFlat(role) && !M_OpenPage(role)) {
            M_Close(false);
            return;
        }
        // On the TR1 title screen, a new game page with one choice starts
        // the game on the press that opened the page.
        if (m_Book.bare && M_BrowsesFirst(DC_Ring_GetMode()) && M_IsTitle()) {
            M_RunFlat(role);
            return;
        }
    }

    if (M_IsFlat(role) || m_Book.bare) {
        if (confirmed || g_InputDB.menu_confirm) {
            M_RunFlat(role);
            return;
        }
        if (m_Book.bare && g_InputDB.menu_back) {
            M_BackOut();
            return;
        }
    } else if (m_Book.has_page && M_ControlPage()) {
        return;
    }

    if (g_InputDB.menu_back) {
        if (M_CanBackOut()) {
            M_Close(false);
        }
    } else {
        M_FlipKeys();
    }
}

static void M_Control(void)
{
    if (m_Book.save_ticks > 0) {
        if (--m_Book.save_ticks == 0) {
            M_DoSave();
        }
        return;
    }
    if (m_Book.close_ticks > 0) {
        if (--m_Book.close_ticks == 0) {
            m_Book.message = nullptr;
            M_Close(true);
        }
        return;
    }
    if (m_Book.message != nullptr) {
        return;
    }
    if (m_Book.has_list) {
        const int32_t picked = DC_Req_Control(&m_Book.list);
        if (picked == DC_REQ_CANCELLED) {
            m_Book.has_list = false;
        } else if (picked >= 0) {
            M_Choose(m_Book.list_role, &m_Book.list.rows[picked]);
        }
    } else if (m_Book.standalone) {
        M_ControlPage();
    } else {
        M_ControlBook();
    }
}

static bool M_OnTick(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (!m_Book.open) {
        return false;
    }
    M_Control();
    if (m_Book.open) {
        M_SyncCaption();
    }
    return false;
}

static bool M_OnPaint(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (!m_Book.open) {
        return false;
    }
    if (m_Book.has_list) {
        DC_Req_Paint(&m_Book.list);
    } else if (m_Book.has_page
               && (m_Book.standalone
                   || M_PageAt(DC_Ring_GetRestFrame()) == m_Book.index)) {
        DC_Req_Paint(&m_Book.page);
    }
    if (m_Book.message != nullptr) {
        DC_UI_Message(m_Book.message, 0.82f);
    }
    return false;
}

static void M_Release(const UI_TAKEOVER screen)
{
    if (m_Book.open && m_Book.screen == screen) {
        m_Book.open = false;
        M_ClearCaption();
    }
}

static void M_Reset(const UI_TAKEOVER screen, const INVENTORY_MODE mode)
{
    m_Book = (typeof(m_Book)) {
        .screen = screen,
        .mode = mode,
        .opened = M_PAGE_NONE,
    };
}

static bool M_OpenPassport(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    const INVENTORY_MODE mode = DC_Ring_GetMode();
    M_Reset(screen, mode);
    M_DeterminePages(mode, m_Book.pages);
    m_Book.index = M_FirstAvailable(m_Book.pages);
    if (m_Book.index < 0) {
        return false;
    }
    m_Book.browse = M_BrowsesFirst(mode);
    m_Book.open = true;
    // The book names its own pages, so the name the ring gave the entry
    // comes down.
    M_SyncCaption();
    return true;
}

// The save page of the save crystal entry: the crystal saves only in the
// save pickup mode, and only from the ring that the player opened.
static bool M_OpenCrystal(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    if (g_Config.gameplay.save_crystal_mode != SAVE_CRYSTAL_SAVE_PICKUP
        || DC_Ring_GetMode() != INV_GAME_MODE) {
        return false;
    }
    M_Reset(screen, INV_GAME_MODE);
    m_Book.pages[0] = (M_BOOK_PAGE) { M_PAGE_SAVE_GAME, true };
    m_Book.standalone = true;
    m_Book.crystal = true;
    if (!M_OpenPage(M_PAGE_SAVE_GAME)) {
        return false;
    }
    m_Book.open = true;
    const char *const name = GS("objects/save_crystal_item/name");
    static char caption[64];
    snprintf(caption, sizeof(caption), "%.*s", (int)strcspn(name, "|"), name);
    DC_NoLua_SetCaption(caption, true, false);
    return true;
}

static bool M_OpenRingEntry(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    if (args->object_id == O_PASSPORT_OPTION
        || args->object_id == O_PASSPORT_CLOSED) {
        return M_OpenPassport(screen, args);
    } else if (args->object_id == O_SAVE_CRYSTAL_OPTION) {
        return M_OpenCrystal(screen, args);
    }
    return DC_NoLua_OpenSettingsEntry(screen, args);
}

// The page of the quick save and load screens. A page with no list leaves
// the screen closed.
static bool M_OpenSaveLoad(
    const UI_TAKEOVER screen, const UI_TAKEOVER_ARGS *const args)
{
    M_Reset(screen, args->mode);
    M_DeterminePages(args->mode, m_Book.pages);
    m_Book.index = M_FirstAvailable(m_Book.pages);
    if (m_Book.index < 0 || M_IsFlat(m_Book.pages[m_Book.index].role)) {
        return false;
    }
    m_Book.standalone = true;
    if (!M_OpenPage(m_Book.pages[m_Book.index].role) || m_Book.bare) {
        return false;
    }
    m_Book.open = true;
    DC_NoLua_SetCaption(
        GS(args->mode == INV_LOAD_MODE ? "general/passport/load_game"
                                       : "general/passport/save_game"),
        false, false);
    return true;
}

static void M_ReleaseRingEntry(const UI_TAKEOVER screen)
{
    if (m_Book.open) {
        M_Release(screen);
    } else {
        DC_NoLua_ReleaseSettingsEntry(screen);
    }
}

void DC_NoLua_SetupPassport(void)
{
    M_LoadMenuColors();
    DC_Screens_Define(
        UI_TAKEOVER_RING_ENTRY, M_OpenRingEntry, M_ReleaseRingEntry);
    DC_Screens_Define(UI_TAKEOVER_SAVE_LOAD, M_OpenSaveLoad, M_Release);
    DC_Events_Listen(LUA_EVENT_TICK, M_OnTick);
    DC_Events_Listen(LUA_EVENT_UI_PAINT, M_OnPaint);
}
