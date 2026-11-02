// The weapons of the game's weapons.json5, declared without Lua: what
// scripts/weapons.lua and trx/game/lua/capi/weapons_spec.c do, reading the
// file directly. A weapon the catalog does not hold is passed over, as the
// script did.
#include "nolua.h"

#include <trx/core/enum_map.h>
#include <trx/core/json/util/file.h>
#include <trx/core/json/util/read_io.h>
#include <trx/core/log.h>
#include <trx/core/math/const.h>
#include <trx/game/catalog/manager.h>
#include <trx/game/const.h>
#include <trx/game/gun/registry.h>
#include <trx/game/gun/routines.h>
#include <trx/game/objects/names.h>
#include <trx/game/paths.h>

// Names a routine of the given kind, and reports a name no module registers.
#define M_ROUTINE(io_, kind_, key_, target_)                                   \
    do {                                                                       \
        const char *name_ = nullptr;                                           \
        MUST(JSON_READ_OPT((io_), (key_), &name_));                            \
        if (name_ != nullptr && name_[0] != '\0') {                            \
            const typeof(target_) func_ =                                      \
                CONCAT(Gun_Routines_Get, kind_)(name_);                        \
            if (func_ == nullptr) {                                            \
                return JSON_ReadIO_Fail(                                       \
                    (io_), "there is no %s routine '%s'", (key_), name_);      \
            }                                                                  \
            (target_) = func_;                                                 \
        }                                                                      \
    } while (0)

// Names one of an enumeration, and reports a name the enumeration lacks.
#define M_ENUM(io_, enum_name_, key_, target_)                                 \
    do {                                                                       \
        const char *name_ = nullptr;                                           \
        MUST(JSON_READ_OPT((io_), (key_), &name_));                            \
        if (name_ != nullptr && name_[0] != '\0') {                            \
            const int32_t found_ = ENUM_MAP_GET(enum_name_, name_, -1);        \
            if (found_ < 0) {                                                  \
                return JSON_ReadIO_Fail(                                       \
                    (io_), "there is no %s '%s'", (key_), name_);              \
            }                                                                  \
            (target_) = found_;                                                \
        }                                                                      \
    } while (0)

#define M_INT(io_, key_, target_)                                              \
    do {                                                                       \
        int32_t value_ = (int32_t)(target_);                                   \
        MUST(JSON_READ_OPT((io_), (key_), &value_));                           \
        (target_) = value_;                                                    \
    } while (0)

#define M_NUM(io_, key_, target_)                                              \
    do {                                                                       \
        double value_ = (double)(target_);                                     \
        MUST(JSON_READ_OPT((io_), (key_), &value_));                           \
        (target_) = value_;                                                    \
    } while (0)

typedef RESULT (*M_GROUP_READER)(JSON_READ_IO *io, WEAPON_INFO *w);

typedef struct {
    const char *key;
    M_GROUP_READER read;
} M_GROUP;

static RESULT M_ReadObjectId(
    JSON_READ_IO *const io, const char *const key, OBJECT_ID *const target)
{
    const char *name = nullptr;
    MUST(JSON_READ_OPT(io, key, &name));
    if (name == nullptr || name[0] == '\0') {
        return OK;
    }
    const OBJECT_ID object_id = Object_IdFromKey(name);
    if (object_id == NO_OBJECT) {
        return JSON_ReadIO_Fail(io, "there is no object '%s'", name);
    }
    *target = object_id;
    return OK;
}

static RESULT M_ReadSample(
    JSON_READ_IO *const io, const char *const key, SAMPLE_ID *const target)
{
    const char *name = nullptr;
    MUST(JSON_READ_OPT(io, key, &name));
    if (name == nullptr || name[0] == '\0') {
        return OK;
    }
    const CATALOG_ID sample_id =
        Catalog_KeyToID(CATALOG_SAMPLES, name, NO_CATALOG_ID);
    if (sample_id == NO_CATALOG_ID) {
        LOG_WARNING("there is no sample '%s'", name);
        return OK;
    }
    *target = sample_id;
    return OK;
}

static RESULT M_ReadKeptString(
    JSON_READ_IO *const io, const char *const key, const char **const target)
{
    const char *name = nullptr;
    MUST(JSON_READ_OPT(io, key, &name));
    if (name != nullptr && name[0] != '\0') {
        *target = Gun_Registry_KeepString(name);
    }
    return OK;
}

// Four angles in whole degrees, as a list or by name.
static RESULT M_ReadAngles(
    JSON_READ_IO *const io, const char *const key,
    WEAPON_AIM_LIMITS *const limits)
{
    static const char *const names[] = { "min_yaw", "max_yaw", "min_pitch",
                                         "max_pitch" };
    int16_t *const angles[] = { &limits->min_yaw, &limits->max_yaw,
                                &limits->min_pitch, &limits->max_pitch };
    if (!JSON_ReadIO_HasKey(io, key)) {
        return OK;
    }
    MUST(JSON_PUSH(io, key));
    double values[4];
    RESULT result = OK;
    const JSON_ARRAY *const arr =
        JSON_ValueAsArray(JSON_ReadIO_GetCurrentValue(io));
    const int32_t len = arr != nullptr ? (int32_t)arr->length : -1;
    for (int32_t i = 0; IS_OK(result) && i < 4; i++) {
        if (len >= 0) {
            result = len == 4 ? JSON_READ_A(io, i, &values[i])
                              : JSON_ReadIO_Fail(io, "must hold four angles");
        } else {
            result = JSON_READ(io, names[i], &values[i]);
        }
    }
    MUST(JSON_POP(io));
    MUST(result);
    for (int32_t i = 0; i < 4; i++) {
        *angles[i] = values[i] * DEG_1;
    }
    return OK;
}

// Set the mesh source for Lara while she holds this weapon. An undeclared
// source uses the outfit's gun-swap object.
static RESULT M_ReadMeshes(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    OBJECT_ID object_id = NO_OBJECT;
    MUST(M_ReadObjectId(io, "object", &object_id));
    if (object_id == NO_OBJECT) {
        return OK;
    }
    w->meshes = (WEAPON_MESH_INFO) {
        .is_declared = true,
        .object_id = object_id,
        .hand_r = -1,
        .hand_l = -1,
        .torso = -1,
        .thigh_r = -1,
        .thigh_l = -1,
    };
    M_INT(io, "hand_r", w->meshes.hand_r);
    M_INT(io, "hand_l", w->meshes.hand_l);
    M_INT(io, "torso", w->meshes.torso);
    M_INT(io, "thigh_r", w->meshes.thigh_r);
    M_INT(io, "thigh_l", w->meshes.thigh_l);
    return OK;
}

static RESULT M_ReadObjects(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    MUST(M_ReadObjectId(io, "pickup", &w->gun_object_id));
    MUST(M_ReadObjectId(io, "ammo", &w->ammo_object_id));
    MUST(M_ReadObjectId(io, "anim", &w->anim_object_id));
    MUST(M_ReadObjectId(io, "shell", &w->shell_object_id));
    MUST(M_ReadObjectId(io, "projectile", &w->projectile_object_id));
    return OK;
}

static RESULT M_ReadAmmo(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    WEAPON_AMMO_INFO *const ammo = &w->ammo;
    M_INT(io, "initial_shots", ammo->initial_shots);
    M_INT(io, "box_shots", ammo->box_shots);
    M_INT(io, "box_label_qty", ammo->box_label_qty);
    M_INT(io, "rounds_per_shot", ammo->rounds_per_shot);
    MUST(JSON_READ_OPT(io, "infinite", &ammo->infinite));
    MUST(M_ReadKeptString(io, "icon", &w->ammo_icon));
    return OK;
}

static RESULT M_ReadAim(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    int32_t speed = w->aim_speed / DEG_1;
    int32_t accuracy = w->shot_accuracy / DEG_1;
    double target_dist = w->target_dist / (double)WALL_L;
    MUST(JSON_READ_OPT(io, "speed", &speed));
    MUST(JSON_READ_OPT(io, "accuracy", &accuracy));
    MUST(JSON_READ_OPT(io, "target_dist", &target_dist));
    w->aim_speed = speed * DEG_1;
    w->shot_accuracy = accuracy * DEG_1;
    w->target_dist = target_dist * WALL_L;
    MUST(M_ReadAngles(io, "lock", &w->lock));
    MUST(M_ReadAngles(io, "left", &w->left_arm));
    MUST(M_ReadAngles(io, "right", &w->right_arm));
    return OK;
}

static RESULT M_ReadAnim(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    WEAPON_ANIM_INFO *const anim = &w->anim;
    M_INT(io, "equip", anim->equip_anim_idx);
    M_INT(io, "draw_frame", anim->draw_frame);
    M_INT(io, "undraw_frame", anim->undraw_frame);
    M_INT(io, "recoil_frame", anim->recoil_frame);
    M_INT(io, "shell_frame", anim->shell_frame);
    M_ROUTINE(io, ReadyAnim, "ready", w->ready_anim_func);
    return OK;
}

static RESULT M_ReadFlash(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    WEAPON_FLASH_INFO *const flash = &w->flash;
    M_INT(io, "time", flash->time);
    M_INT(io, "shade", flash->shade);
    MUST(JSON_READ_OPT(io, "color", &flash->color));
    MUST(JSON_READ_OPT(io, "pos", &flash->pos.right));
    MUST(JSON_READ_OPT(io, "pos_alt", &flash->pos.left));
    MUST(JSON_READ_OPT(io, "lights_room", &w->flash_lights_room));
    MUST(JSON_READ_OPT(io, "is_optional", &w->flash_is_optional));
    M_ROUTINE(io, Flash, "routine", w->flash_func);
    return OK;
}

static RESULT M_ReadGlow(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    WEAPON_GLOW_INFO *const glow = &w->glow;
    MUST(JSON_READ_OPT(io, "color", &glow->color));
    MUST(JSON_READ_OPT(io, "pos", &glow->pos));
    M_NUM(io, "scale", glow->scale);
    MUST(JSON_READ_OPT(io, "flicker", &glow->flicker));
    return OK;
}

static RESULT M_ReadMuzzle(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    MUST(JSON_READ_OPT(io, "pos", &w->muzzle_pos.right));
    MUST(JSON_READ_OPT(io, "pos_alt", &w->muzzle_pos.left));
    return OK;
}

static RESULT M_ReadSmoke(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    MUST(JSON_READ_OPT(io, "pos", &w->smoke_pos.right));
    MUST(JSON_READ_OPT(io, "pos_alt", &w->smoke_pos.left));
    MUST(JSON_READ_OPT(io, "tip", &w->smoke_tip.right));
    MUST(JSON_READ_OPT(io, "tip_alt", &w->smoke_tip.left));
    M_INT(io, "count", w->smoke_count);
    M_ROUTINE(io, SmokeSize, "size", w->smoke_size_func);
    return OK;
}

static RESULT M_ReadShell(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    MUST(JSON_READ_OPT(io, "pos", &w->shell_pos.right));
    MUST(JSON_READ_OPT(io, "pos_alt", &w->shell_pos.left));
    MUST(JSON_READ_OPT(io, "throws_forward", &w->shell_throws_forward));
    M_INT(io, "angle", w->shell_angle);
    M_INT(io, "min_speed", w->shell_min_speed);
    return OK;
}

static RESULT M_ReadSound(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    MUST(M_ReadSample(io, "fire", &w->sample_num));
    MUST(M_ReadSample(io, "overlay", &w->sample_overlay_num));
    M_INT(io, "overlay_pitch", w->sample_overlay_pitch);
    MUST(JSON_READ_OPT(io, "alternating", &w->has_alternating_fire_sound));
    M_ROUTINE(io, Sound, "rapid_fire", w->rapid_fire_sound_func);
    return OK;
}

static RESULT M_ReadStow(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    M_INT(io, "order", w->stow_order);
    M_ENUM(io, STOW_PLACE, "place", w->stow_place);
    return OK;
}

static RESULT M_ReadSave(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    MUST(M_ReadKeptString(io, "ammo_key", &w->save_ammo_key));
    MUST(M_ReadKeptString(io, "resume_has_key", &w->save_resume_has_key));
    MUST(M_ReadKeptString(io, "resume_ammo_key", &w->save_resume_ammo_key));
    MUST(JSON_READ_OPT(io, "required", &w->save_keys_required));
    return OK;
}

static RESULT M_ReadCheat(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    M_INT(io, "ammo", w->cheat_ammo);
    M_INT(io, "key_ammo", w->cheat_key_ammo);
    return OK;
}

// clang-format off
static const M_GROUP m_Groups[] = {
    { "objects", M_ReadObjects },
    { "meshes",  M_ReadMeshes },
    { "ammo",    M_ReadAmmo },
    { "aim",     M_ReadAim },
    { "anim",    M_ReadAnim },
    { "flash",   M_ReadFlash },
    { "glow",    M_ReadGlow },
    { "muzzle",  M_ReadMuzzle },
    { "smoke",   M_ReadSmoke },
    { "shell",   M_ReadShell },
    { "sound",   M_ReadSound },
    { "stow",    M_ReadStow },
    { "save",    M_ReadSave },
    { "cheat",   M_ReadCheat },
};
// clang-format on

// Reads the weapon this weapon is based on and keeps its own settings where
// they differ.
static RESULT M_ReadBase(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    const char *key = nullptr;
    MUST(JSON_READ_OPT(io, "base", &key));
    if (key == nullptr || key[0] == '\0') {
        return OK;
    }
    const CATALOG_ID base_id =
        Catalog_KeyToID(CATALOG_WEAPONS, key, NO_CATALOG_ID);
    if (base_id == NO_CATALOG_ID || !Gun_Registry_IsValidType(base_id)) {
        return JSON_ReadIO_Fail(io, "there is no weapon '%s'", key);
    }
    if (base_id == w->gun_type) {
        return JSON_ReadIO_Fail(io, "a weapon cannot be based on itself");
    }

    const LARA_GUN_TYPE gun_type = w->gun_type;
    const bool was_declared = w->is_declared;
    const WEAPON_INFO own = *w;
    *w = *Gun_Registry_Get(base_id);
    w->gun_type = gun_type;
    w->skin_source = base_id;
    w->given_in_ngplus = own.given_in_ngplus;
    w->is_declared = was_declared;
    w->equip_input_role = was_declared ? own.equip_input_role : (INPUT_ROLE)-1;
    w->save_ammo_key = was_declared ? own.save_ammo_key : nullptr;
    w->save_resume_has_key = was_declared ? own.save_resume_has_key : nullptr;
    w->save_resume_ammo_key = was_declared ? own.save_resume_ammo_key : nullptr;
    w->save_keys_required = was_declared && own.save_keys_required;
    w->is_claimed = true;
    return OK;
}

// Names what drives the weapon, which declares it where the engine has an
// implementation for that kind and leaves it inert where it has none.
static RESULT M_ReadKind(JSON_READ_IO *const io, WEAPON_INFO *const w)
{
    const char *name = nullptr;
    MUST(JSON_READ_OPT(io, "kind", &name));
    if (name == nullptr || name[0] == '\0') {
        return OK;
    }
    const int32_t kind = ENUM_MAP_GET(WEAPON_TYPE, name, -1);
    if (kind < 0) {
        return JSON_ReadIO_Fail(io, "there is no weapon kind '%s'", name);
    }
    if (w->is_declared) {
        return Gun_Registry_SetKind((WEAPON_TYPE)kind, w);
    }
    w->is_claimed = true;
    if (Gun_Routines_GetKind(kind) == nullptr) {
        w->type = (WEAPON_TYPE)kind;
        return OK;
    }
    return Gun_Registry_Declare(w->gun_type, (WEAPON_TYPE)kind, w);
}

static RESULT M_ReadFlat(
    JSON_READ_IO *const io, WEAPON_INFO *const w, INPUT_ROLE *const out_role)
{
    M_INT(io, "damage", w->damage);
    M_INT(io, "fire_delay", w->fire_delay);
    M_INT(io, "gun_height", w->gun_height);
    MUST(JSON_READ_OPT(io, "is_available", &w->is_available));
    MUST(JSON_READ_OPT(io, "given_in_ngplus", &w->given_in_ngplus));
    MUST(JSON_READ_OPT(io, "is_default", &w->is_default));
    MUST(JSON_READ_OPT(io, "is_remembered", &w->is_remembered));
    MUST(JSON_READ_OPT(io, "is_launcher", &w->is_launcher));
    MUST(JSON_READ_OPT(io, "is_machine_gun", &w->is_machine_gun));
    MUST(JSON_READ_OPT(io, "is_usable_underwater", &w->is_usable_underwater));
    MUST(JSON_READ_OPT(io, "wants_combat_camera", &w->wants_combat_camera));
    MUST(JSON_READ_OPT(io, "unaims_on_release", &w->unaims_on_release));
    M_ROUTINE(io, Fire, "fire", w->fire_func);
    M_ENUM(io, INPUT_ROLE, "equip_key", *out_role);
    return OK;
}

// A spec that fails leaves the weapon as it was.
static RESULT M_ReadSpec(JSON_READ_IO *const io, WEAPON_INFO *const weapon)
{
    WEAPON_INFO w = *weapon;
    INPUT_ROLE role = (INPUT_ROLE)-1;
    MUST(M_ReadBase(io, &w));
    MUST(M_ReadKind(io, &w));
    for (size_t i = 0; i < ARRAY_SIZE(m_Groups); i++) {
        if (!JSON_ReadIO_HasKey(io, m_Groups[i].key)) {
            continue;
        }
        MUST(JSON_PUSH(io, m_Groups[i].key));
        const RESULT result = m_Groups[i].read(io, &w);
        MUST(JSON_POP(io));
        MUST(result, "'%s'", m_Groups[i].key);
    }
    MUST(M_ReadFlat(io, &w, &role));
    *weapon = w;
    if ((int32_t)role >= 0) {
        Gun_Registry_SetInputRole(weapon->gun_type, role);
    }
    return OK;
}

static RESULT M_ReadWeapons(JSON_READ_IO *const io)
{
    const int32_t count = JSON_ReadIO_GetKeyCount(io);
    if (count < 0) {
        return JSON_ReadIO_Fail(io, "the file must hold a dictionary");
    }
    for (int32_t i = 0; i < count; i++) {
        const char *const key = JSON_ReadIO_GetKeyAt(io, i);
        const CATALOG_ID id =
            Catalog_KeyToID(CATALOG_WEAPONS, key, NO_CATALOG_ID);
        if (id == NO_CATALOG_ID || !Gun_Registry_IsValidType(id)) {
            LOG_WARNING("unknown weapon '%s'", key);
            continue;
        }
        MUST(JSON_PUSH(io, key));
        const RESULT result = M_ReadSpec(io, Gun_Registry_Get(id));
        MUST(JSON_POP(io));
        MUST(result, "'%s'", key);
    }
    return OK;
}

RESULT DC_NoLua_DeclareWeapons(void)
{
    const char *path = nullptr;
    MUST(GamePath_Resolve(GAME_DYNAMIC_PATH_COMMON_CONFIG, "weapons.json5", &path));
    JSON_VALUE *root = nullptr;
    MUST(JSONFile_ReadRequired(path, &root));
    JSON_READ_IO *const io = JSON_ReadIO_Create(root, 0, path);
    const RESULT result = M_ReadWeapons(io);
    JSON_ReadIO_Destroy(io);
    JSON_ValueFree(root);
    return result;
}
