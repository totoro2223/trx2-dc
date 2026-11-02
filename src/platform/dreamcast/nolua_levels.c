// What the per-level scripts (games/tr*/scripts/<level>.lua) did, without
// Lua. A level runs the entry for its game and key, the way the engine picks
// scripts/<key>.lua; an expansion has no entries of its own, so it falls back
// to the game it extends, as the script path resolves.
#include "nolua.h"

#include <trx/config.h>
#include <trx/core/log.h>
#include <trx/game/camera/cinematic.h>
#include <trx/game/const.h>
#include <trx/game/creature/common.h>
#include <trx/game/fx/gun_flash.h>
#include <trx/game/game/state.h>
#include <trx/game/game_flow/common.h>
#include <trx/game/gun/common.h>
#include <trx/game/gun/registry.h>
#include <trx/game/gym.h>
#include <trx/game/inventory.h>
#include <trx/game/items.h>
#include <trx/game/lara/common.h>
#include <trx/game/lara/skin/common.h>
#include <trx/game/lara/skin/storage.h>
#include <trx/game/music.h>
#include <trx/game/objects/common.h>
#include <trx/game/objects/general/pickup.h>
#include <trx/game/objects/property.h>
#include <trx/game/rooms/common.h>
#include <trx/game/rooms/const.h>
#include <trx/version.h>

#include <string.h>

typedef struct {
    int32_t game;
    const char *key;
    void (*game_start)(bool is_save);
    void (*after_control)(void);
    // Taking this object ends the level. O_LARA (0) for none: Lara is never
    // picked up.
    OBJECT_ID end_on_pickup;
    // Answers true for an effect number the level claims, whose stock effect
    // then does not run.
    bool (*flip_effect)(int32_t effect_num);
} M_SCRIPT;

// What a level script kept between frames; cleared each time one starts, as
// re-running the Lua file did.
static struct {
    bool outfit_changed;
    bool equipment_set;
    bool fan_zone;
    int16_t fan_room;
    XYZ_32 fan_corner;
    bool fan_inside;
} m_State;

static const M_SCRIPT *m_Current = nullptr;

static void M_SetObjectProp(
    const OBJECT_ID object_id, const char *const name, const TRX_VALUE value)
{
    const char *const err =
        ObjectProperty_SetObjectValueRaw(Object_Get(object_id), name, value);
    if (err != nullptr) {
        LOG_WARNING("level script: object %d '%s': %s", object_id, name, err);
    }
}

static void M_SetItemProp(
    const int16_t item_num, const char *const name, const TRX_VALUE value)
{
    if (item_num >= Item_GetTotalCount()) {
        LOG_WARNING("level script: no item %d", item_num);
        return;
    }
    const char *const err =
        ObjectProperty_SetItemValueRaw(Item_Get(item_num), name, value);
    if (err != nullptr) {
        LOG_WARNING("level script: item %d '%s': %s", item_num, name, err);
    }
}

// A shoal swims its range in quarter tiles.
static void M_Shoal(
    const int16_t item_num, const int32_t x, const int32_t y, const int32_t z)
{
    M_SetItemProp(
        item_num, "range",
        Value_Make_TVT_XYZ_32((XYZ_32) { .x = x, .y = y, .z = z }));
}

static void M_ShoalSprite(const int16_t item_num)
{
    M_SetItemProp(item_num, "sprite_offset", Value_Make_TVT_S32(1));
}

// The TR3 artefacts glow green and turn slowly, with no twinkle over them.
static void M_Artefact(const OBJECT_ID object_id)
{
    M_SetObjectProp(
        object_id, "glow_color",
        Value_Make_TVT_RGB_888((RGB_888) { .r = 0x00, .g = 0xF8, .b = 0x7C }));
    M_SetObjectProp(object_id, "rotation", Value_Make_TVT_S32(DEG_90 / 16));
    M_SetObjectProp(object_id, "show_pickup_aid", Value_Make_TVT_BOOL(false));
}

static void M_PlinthScion(const int16_t item_num)
{
    M_SetItemProp(
        item_num, "pickup_mode", Value_Make_TVT_S32(PICKUP_MODE_PLINTH_SCION));
}

// trx.lara.has_pistol_weapon (capi/lara.c).
static bool M_HasPistolWeapon(void)
{
    for (int32_t i = 0; i < Gun_Registry_GetCount(); i++) {
        const WEAPON_INFO *const weapon = Gun_Registry_GetByIndex(i);
        if ((weapon->type == WEAPON_TYPE_DUAL_PISTOLS
             || weapon->type == WEAPON_TYPE_SINGLE_PISTOL)
            && Inv_HasItem(Gun_GetGunObject(weapon->gun_type))) {
            return true;
        }
    }
    return false;
}

// Lara wears holsters at home only where she has pistols to put in them.
static void M_Holsters(void)
{
    Lara_Skin_SetHolstersVisible(M_HasPistolWeapon());
}

static void M_Rooms(const bool cold)
{
    for (int32_t i = 0; i < Room_GetCount(); i++) {
        ROOM *const room = Room_Get(i);
        room->flags.damaging = room->flags.underwater;
        room->flags.cold = cold;
    }
}

// trx.items.query:of_object(id):first() and trx.fx.gun_flash with its
// defaults: the flash modelled pointing down, from the gun flash object.
static void M_GunFlash(
    const ITEM *const item, const int32_t mesh_num, const XYZ_32 pos)
{
    if (item == nullptr) {
        return;
    }
    const BITE bite = { .pos = pos, .mesh_num = mesh_num };
    FX_GunFlash_SpawnAt(item, bite, O_GUN_FLASH, -DEG_90);
}

static int32_t M_CutsceneFrame(void)
{
    const GF_LEVEL *const level = GF_GetCurrentLevel();
    if (level == nullptr || level->type != GFL_CUTSCENE) {
        return -1;
    }
    return Camera_GetCineData()->frame_idx;
}

// --- TR1 ---

static void M_TR1_Gym(const bool is_save)
{
    M_Holsters();
}

static void M_TR1_Level10b(const bool is_save)
{
    M_SetObjectProp(O_BACON_LARA, "anchor_room", Value_Make_TVT_S32(10));
}

static void M_TR1_Level3b(const bool is_save)
{
    M_PlinthScion(20);
}

static void M_TR1_Level8c(const bool is_save)
{
    M_PlinthScion(73);
}

// --- TR2 ---

static void M_TR2_Assault(const bool is_save)
{
    DC_NoLua_SetupAssaultHUD();
    M_Holsters();
    if (is_save) {
        return;
    }
    // A course run before plays the fanfare; the first visit, the house theme.
    // Without a record table there is no music, as the Lua call raised there.
    const GYM_TRACK_STATS *const stats =
        Gym_TrackManager_GetStats(GYM_TRACK_ASSAULT);
    if (stats == nullptr || !Gym_TrackManager_HasStats(GYM_TRACK_ASSAULT)) {
        return;
    }
    int32_t records = 0;
    while (records < MAX_ASSAULT_TIMES && stats->entries[records].time != 0) {
        records++;
    }
    const MUSIC_SLOT track = records > 1 ? 22 : 5;
    if (Music_IsTrackAvailableBySlot(track)) {
        Music_PlayBySlot(track, MPM_ONCE);
    }
}

// Bartoli shoots the monk from the ledge and Lara answers; the scene carries no
// effect command for it, so the flashes go on the frames the shots land on.
static void M_TR2_Cut3(void)
{
    const ITEM *const lara_item = Lara_GetItem();
    if (lara_item == nullptr) {
        return;
    }
    if (!m_State.outfit_changed
        && Item_GetRelativeObjAnim(lara_item, Lara_GetAnimationObject())
            >= 7) {
        m_State.outfit_changed = true;
        const LARA_SKIN_TYPE outfit =
            Lara_Skin_FindOutfitByName("tr2_diving_suit");
        if (Lara_Skin_IsOutfitAvailable(outfit)) {
            Lara_Skin_SetType(outfit);
        } else {
            LOG_WARNING("level script: no outfit tr2_diving_suit");
        }
    }

    const int32_t frame = M_CutsceneFrame();
    if (frame < 0) {
        return;
    }
    switch (frame) {
    case 3374:
    case 3383:
    case 3536:
    case 3544:
        // His gun is at the end of the arm he points with, joint 17.
        M_GunFlash(Item_Find(O_PLAYER_6), 17, (XYZ_32) { .y = 120 });
        break;
    }

    int32_t hand = -1;
    switch (frame) {
    case 3387:
    case 3396:
    case 3412:
    case 3548:
    case 3575:
    case 3607:
        hand = 10;
        break;
    case 3402:
    case 3414:
    case 3613:
        hand = 13;
        break;
    }
    if (hand >= 0) {
        M_GunFlash(
            lara_item, hand, Gun_Registry_Get(LGT_PISTOLS)->flash.pos.right);
    }
}

// The level gives its second and third secret each other's sprite.
static void M_TR2_Floating(const bool is_save)
{
    const OBJECT *const obj1 = Object_Get(O_SECRET_2);
    const OBJECT *const obj2 = Object_Get(O_SECRET_3);
    if (!obj1->loaded || !obj2->loaded) {
        return;
    }
    if (obj1->mesh_count >= 0 || obj2->mesh_count >= 0) {
        LOG_WARNING("level script: both objects must be drawn as sprites");
        return;
    }
    Object_SwapSprite(O_SECRET_2, O_SECRET_3);
}

static void M_TR2_House(const bool is_save)
{
    M_Holsters();
    // The Lua compares trx.lara.extra_anim, a boolean, to -1, which is never
    // true; this is what it meant: the dagger on her hips unless an extra
    // animation holds her.
    if (!Lara_GetLaraInfo()->extra_anim) {
        Lara_Skin_SetExtraEquipment(LM_HIPS, EXTRA_MESH_DAGGER_HIPS);
    }
}

static void M_TR2_Monastry(const bool is_save)
{
    Creature_AddAlly(O_MONK_1);
    Creature_AddAlly(O_MONK_2);
    Creature_AddAllyTargetingEnemy(O_BANDIT_1);
    Creature_AddAllyTargetingEnemy(O_BANDIT_2);
}

// --- TR3 ---

static void M_TR3_Antarc(const bool is_save)
{
    M_Rooms(true);
}

static void M_TR3_Area51(const bool is_save)
{
    M_SetObjectProp(
        O_STROBE_LIGHT, "requires_alarm_active", Value_Make_TVT_BOOL(true));
    Creature_AddAlly(O_PRISONER);
    M_Artefact(O_QUEST_ITEM_2);

    // Element 115 stays put until a trigger brings it.
    if (44 < Item_GetTotalCount() && Item_GetTriggerMask(Item_Get(44)) == 0) {
        Item_Deactivate(44);
    }
}

static void M_TR3_Chamber(const bool is_save)
{
    M_Artefact(O_QUEST_ITEM_1);
    M_Artefact(O_QUEST_ITEM_2);
    M_Artefact(O_QUEST_ITEM_3);
    M_Artefact(O_QUEST_ITEM_4);
}

static void M_TR3_City(const bool is_save)
{
    for (int32_t i = 0; i < Item_GetTotalCount(); i++) {
        if (Item_Get(i)->object_id == O_ANIMATING_EXT_1) {
            M_SetItemProp(i, "kill_on_trigger", Value_Make_TVT_BOOL(true));
        }
    }
}

static void M_TR3_Compound(const bool is_save)
{
    Creature_AddAlly(O_PRISONER);
}

static void M_TR3_Crash(const bool is_save)
{
    Creature_AddAlly(O_STHPAC_MERCENARY);
    M_Shoal(114, 22, 6, 6);
}

// Willard fires twice at the start; the scene carries no effect command for
// it. His gun is part of his hand mesh, barrel along the joint's Y axis.
static void M_TR3_Cut3(void)
{
    const int32_t frame = M_CutsceneFrame();
    if (frame == 3147 || frame == 3155) {
        M_GunFlash(
            Item_Find(O_PLAYER_2), 13, (XYZ_32) { .x = 0, .y = 220, .z = 45 });
    }
}

// Lara takes the can in her hand on the frame she drinks from it.
static void M_TR3_Cut8(void)
{
    const ITEM *const lara_item = Lara_GetItem();
    if (lara_item == nullptr || m_State.equipment_set
        || Item_GetRelativeFrame(lara_item) < 737) {
        return;
    }
    Lara_Skin_SetExtraEquipment(LM_HAND_R, EXTRA_MESH_DRINK_CAN);
    m_State.equipment_set = true;
}

static void M_TR3_House(const bool is_save)
{
    DC_NoLua_SetupAssaultHUD();
    M_Shoal(55, 22, 2, 10);
    M_Shoal(56, 18, 1, 10);
    M_Shoal(57, 10, 1, 14);
    M_Shoal(58, 18, 2, 10);
    M_Shoal(59, 26, 2, 10);
    M_Shoal(77, 10, 2, 18);
    M_ShoalSprite(56);
    M_ShoalSprite(57);
    M_ShoalSprite(59);
}

// Track 2 bypasses the regular one-shot setup and always plays in the gym.
static bool M_TR3_HouseFlipEffect(const int32_t effect_num)
{
    if (effect_num != 200) {
        return false;
    }
    if (Music_IsTrackAvailableBySlot(2)) {
        Music_PlayBySlot(2, MPM_OVERLAY);
    }
    return true;
}

static void M_TR3_Jungle(const bool is_save)
{
    Creature_AddAlly(O_MONKEY);
    M_Shoal(133, 10, 3, 22);
}

static void M_TR3_Mines(const bool is_save)
{
    Creature_AddAlly(O_RX_WORKER_3);
    M_Rooms(true);
}

static void M_TR3_Nevada(const bool is_save)
{
    M_SetObjectProp(O_COBRA, "alert_radius", Value_Make_TVT_FLOAT(1.25f));
    M_SetObjectProp(O_COBRA, "attack_radius", Value_Make_TVT_FLOAT(0.666667f));
    M_SetObjectProp(O_COBRA, "forget_radius", Value_Make_TVT_FLOAT(2.5f));
}

static void M_TR3_Office(const bool is_save)
{
    M_Artefact(O_QUEST_ITEM_3);
}

static void M_TR3_Quadchas(const bool is_save)
{
    M_SetObjectProp(O_QUAD_BIKE, "track_1", Value_Make_TVT_S32(9));
    M_SetObjectProp(O_QUAD_BIKE, "track_2", Value_Make_TVT_S32(12));
    M_SetObjectProp(O_QUAD_BIKE, "track_3", Value_Make_TVT_S32(4));
    M_SetObjectProp(O_QUAD_BIKE, "track_4", Value_Make_TVT_S32(12));
    M_Shoal(5, 6, 1, 14);
    M_Shoal(7, 2, 2, 14);
    M_Shoal(28, 10, 2, 6);
    M_Shoal(70, 6, 1, 10);
    M_Shoal(85, 6, 2, 18);
    M_Shoal(86, 6, 1, 26);
    M_Shoal(89, 14, 1, 6);
    M_Shoal(93, 18, 1, 6);
}

static void M_TR3_Rapids(const bool is_save)
{
    M_Shoal(15, 6, 5, 10);
    M_Shoal(51, 18, 8, 18);
    M_SetObjectProp(O_FLAME_EMITTER_SIDE, "interval", Value_Make_TVT_S32(4));
}

static void M_TR3_Roofs(const bool is_save)
{
    M_SetObjectProp(O_PROPELLER_2, "damage", Value_Make_TVT_S32(1000));
    M_SetObjectProp(O_PROPELLER_3, "damage", Value_Make_TVT_S32(1000));

    // trx.zones.tile: the sector under this point, in the room holding it, at
    // any height.
    const XYZ_32 pad = { .x = 28160, .y = -8192, .z = 53760 };
    const int16_t room_num = Room_GetIndexFromPos(pad);
    if (g_Config.gameplay.fix_floor_data_issues && room_num != NO_ROOM) {
        m_State.fan_zone = true;
        m_State.fan_room = room_num;
        m_State.fan_corner = (XYZ_32) {
            .x = pad.x / WALL_L * WALL_L,
            .z = pad.z / WALL_L * WALL_L,
        };
    }
}

// Slow down the fans in room 77 before Lara re-enters the water, but only if
// she has completed the required puzzle: on stepping onto the fan pad.
static void M_TR3_RoofsControl(void)
{
    const ITEM *const lara_item = Lara_GetItem();
    if (!m_State.fan_zone || lara_item == nullptr) {
        return;
    }
    const XYZ_32 c = m_State.fan_corner;
    const bool inside = lara_item->room_num == m_State.fan_room
        && lara_item->pos.x >= c.x && lara_item->pos.x < c.x + WALL_L
        && lara_item->pos.z >= c.z && lara_item->pos.z < c.z + WALL_L;
    const bool entered = inside && !m_State.fan_inside;
    m_State.fan_inside = inside;
    if (!entered || Room_GetFlipStatus() || 96 >= Item_GetTotalCount()
        || Item_Get(94)->trigger.spent
        || Item_GetTriggerMask(Item_Get(96)) != TRIGGER_MASK_ALL) {
        return;
    }
    const ITEM_TRIGGER trigger = {
        .kind = ITEM_TRIGGER_NORMAL,
        .mask = TRIGGER_MASK_ALL,
        .one_shot = true,
    };
    Item_Trigger(94, &trigger);
    Item_Trigger(95, &trigger);
}

static void M_TR3_Sewer(const bool is_save)
{
    M_SetObjectProp(O_CEILING_SPIKES, "speed", Value_Make_TVT_S32(10));
}

static void M_TR3_Shore(const bool is_save)
{
    M_Shoal(8, 14, 6, 22);
    M_Shoal(12, 14, 6, 14);
    M_Shoal(14, 22, 8, 6);
    M_ShoalSprite(12);
    M_ShoalSprite(14);
}

static void M_TR3_Temple(const bool is_save)
{
    M_Shoal(0, 6, 3, 30);
    M_Shoal(19, 6, 2, 18);
    M_Shoal(25, 6, 2, 6);
}

static void M_TR3_Tonyboss(const bool is_save)
{
    M_Artefact(O_QUEST_ITEM_1);
}

static void M_TR3_Tower(const bool is_save)
{
    Creature_AddAlly(O_PUNK_1);
}

static void M_TR3_Triboss(const bool is_save)
{
    M_Artefact(O_QUEST_ITEM_4);
}

// clang-format off
static const M_SCRIPT m_Scripts[] = {
    { 1, "gym",      .game_start = M_TR1_Gym },
    { 1, "level10b", .game_start = M_TR1_Level10b },
    { 1, "level3b",  .game_start = M_TR1_Level3b },
    { 1, "level8c",  .game_start = M_TR1_Level8c },

    { 2, "assault",  .game_start = M_TR2_Assault },
    { 2, "cut3",     .after_control = M_TR2_Cut3 },
    { 2, "floating", .game_start = M_TR2_Floating },
    { 2, "house",    .game_start = M_TR2_House },
    { 2, "monastry", .game_start = M_TR2_Monastry },

    { 3, "antarc",   .game_start = M_TR3_Antarc },
    { 3, "area51",   .game_start = M_TR3_Area51,
                     .end_on_pickup = O_QUEST_ITEM_2 },
    { 3, "chamber",  .game_start = M_TR3_Chamber },
    { 3, "city",     .game_start = M_TR3_City },
    { 3, "compound", .game_start = M_TR3_Compound },
    { 3, "crash",    .game_start = M_TR3_Crash },
    { 3, "cut3",     .after_control = M_TR3_Cut3 },
    { 3, "cut8",     .after_control = M_TR3_Cut8 },
    { 3, "house",    .game_start = M_TR3_House,
                     .flip_effect = M_TR3_HouseFlipEffect },
    { 3, "jungle",   .game_start = M_TR3_Jungle },
    { 3, "mines",    .game_start = M_TR3_Mines },
    { 3, "nevada",   .game_start = M_TR3_Nevada },
    { 3, "office",   .game_start = M_TR3_Office,
                     .end_on_pickup = O_QUEST_ITEM_3 },
    { 3, "quadchas", .game_start = M_TR3_Quadchas },
    { 3, "rapids",   .game_start = M_TR3_Rapids },
    { 3, "roofs",    .game_start = M_TR3_Roofs,
                     .after_control = M_TR3_RoofsControl },
    { 3, "sewer",    .game_start = M_TR3_Sewer },
    { 3, "shore",    .game_start = M_TR3_Shore },
    { 3, "temple",   .game_start = M_TR3_Temple },
    { 3, "tonyboss", .game_start = M_TR3_Tonyboss,
                     .end_on_pickup = O_QUEST_ITEM_1 },
    { 3, "tower",    .game_start = M_TR3_Tower },
    { 3, "triboss",  .game_start = M_TR3_Triboss,
                     .end_on_pickup = O_QUEST_ITEM_4 },
};
// clang-format on

static bool M_OnGameStart(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (m_Current != nullptr && m_Current->game_start != nullptr) {
        m_Current->game_start(arg_count > 0 && args[0].value.b);
    }
    return false;
}

static bool M_OnAfterControl(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (m_Current != nullptr && m_Current->after_control != nullptr) {
        m_Current->after_control();
    }
    return false;
}

static bool M_OnPickup(
    const LUA_EVENT_TYPE ev, const LUA_EVENT_ARG *const args,
    const int32_t arg_count)
{
    if (m_Current == nullptr || m_Current->end_on_pickup == O_LARA
        || arg_count < 1) {
        return false;
    }
    const int32_t item_num = args[0].value.i32;
    if (item_num >= 0 && item_num < Item_GetTotalCount()
        && Item_Get(item_num)->object_id == m_Current->end_on_pickup) {
        Game_SetIsLevelComplete(true);
    }
    return false;
}

static bool M_InterceptFlipEffect(
    const int32_t effect_num, const int32_t timer, const int16_t item_num)
{
    return m_Current != nullptr && m_Current->flip_effect != nullptr
        && m_Current->flip_effect(effect_num);
}

void DC_NoLua_RunLevelScript(const GF_LEVEL *const level)
{
    static bool listening = false;
    if (!listening) {
        listening = true;
        DC_Events_Listen(LUA_EVENT_GAME_START, M_OnGameStart);
        DC_Events_Listen(LUA_EVENT_AFTER_CONTROL, M_OnAfterControl);
        DC_Events_Listen(LUA_EVENT_PICKUP, M_OnPickup);
        ItemAction_SetInterceptor(M_InterceptFlipEffect);
    }

    memset(&m_State, 0, sizeof(m_State));
    m_Current = nullptr;
    if (level->key == nullptr) {
        return;
    }
    for (size_t i = 0; i < sizeof(m_Scripts) / sizeof(m_Scripts[0]); i++) {
        if (m_Scripts[i].game == g_TRVersion
            && strcmp(m_Scripts[i].key, level->key) == 0) {
            m_Current = &m_Scripts[i];
            return;
        }
    }
}

void DC_NoLua_DropLevelScript(void)
{
    m_Current = nullptr;
}
