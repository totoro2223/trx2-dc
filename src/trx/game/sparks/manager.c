#include <trx/game/sparks/manager.h>

#include <trx/config.h>
#include <trx/core/handle.h>
#include <trx/core/json/util/read_io.h>
#include <trx/core/json/util/write_io.h>
#include <trx/core/memory.h>
#include <trx/core/utils.h>
#include <trx/debug.h>
#include <trx/game/effects.h>
#include <trx/game/lara.h>
#include <trx/game/objects.h>
#include <trx/game/output/lights.h>
#include <trx/game/output/sources/poly_fx.h>
#include <trx/game/random.h>
#include <trx/version.h>

#if defined(__DREAMCAST__)
    // TR3's own pool held 256; 400 were 15K more of RAM.
    #define M_MAX_SPARKS 256
#else
    #define M_MAX_SPARKS 400
#endif
#define M_MAX_SPARK_DYNAMICS 32

// Half a turn in the 4096-unit angle space of the TR3 wind.
#define M_TR3_WIND_START_ANGLE 2048

typedef struct {
    bool on;
    uint8_t falloff;
    RGB_888 color;
    uint8_t flags;
} M_SPARK_DYNAMIC;

// Made at each level load, in the level's memory, not kept in .bss.
static SPARK *m_Sparks = nullptr;
static uint32_t m_SparkGens[M_MAX_SPARKS];
static HANDLE_REGISTRY m_SparkHandles = {
    .gens = m_SparkGens,
    .capacity = M_MAX_SPARKS,
};
static M_SPARK_DYNAMIC m_Dynamics[M_MAX_SPARK_DYNAMICS];
static int32_t m_NextSpark = 0;
static XZ_32 m_SmokeWind = {};
static int32_t m_HairWindZ = 0;
static int32_t m_TR3Wind = 0;
static int32_t m_TR3WindAngle = M_TR3_WIND_START_ANGLE;
static int32_t m_TR3DWindAngle = M_TR3_WIND_START_ANGLE;
static bool m_TR3WindHeld = false;

static const BITE m_NodeOffsets[16] = {
    { .pos = { 0, 340, 64 }, .mesh_num = 7 },
    { .pos = { 0, 0, -96 }, .mesh_num = 10 },
    { .pos = { 16, 48, 320 }, .mesh_num = 13 },
    { .pos = { 0, -256, 0 }, .mesh_num = 5 },
    { .pos = { 0, 64, 0 }, .mesh_num = 10 },
    { .pos = { 0, 64, 0 }, .mesh_num = 13 },
    { .pos = { -32, -16, -192 }, .mesh_num = 13 },
    { .pos = { -64, 410, 0 }, .mesh_num = 20 },
    { .pos = { 64, 410, 0 }, .mesh_num = 23 },
    { .pos = { -160, -8, 16 }, .mesh_num = 5 },
    { .pos = { -160, -8, 16 }, .mesh_num = 9 },
    { .pos = { -160, -8, 16 }, .mesh_num = 13 },
    { .pos = { 0, 0, 0 }, .mesh_num = 0 },
    { .pos = { 0, 0, 0 }, .mesh_num = 0 },
    { .pos = { 0, 0, 0 }, .mesh_num = 0 },
    { .pos = { 0, 0, 0 }, .mesh_num = 0 },
};

static int32_t M_GetFreeSparkIndex(void)
{
    int32_t idx = m_NextSpark;
    for (int32_t i = 0; i < M_MAX_SPARKS; i++) {
        if (!m_Sparks[idx].on) {
            m_NextSpark = (idx + 1) & 0xBF;
            Handle_RegistryBump(&m_SparkHandles, idx);
            return idx;
        }
        idx = idx == (M_MAX_SPARKS - 1) ? 0 : idx + 1;
    }

    int32_t free = 0;
    int32_t min_life = INT32_MAX;
    for (int32_t i = 0; i < M_MAX_SPARKS; i++) {
        const SPARK *const spark = &m_Sparks[i];
        if ((int32_t)spark->life < min_life && spark->dynamic == -1
            && ((spark->flags & SPARK_F_BLOOD) == 0U || (i & 1) != 0)) {
            free = i;
            min_life = (int32_t)spark->life;
        }
    }

    m_NextSpark = (free + 1) & 0xBF;
    Handle_RegistryBump(&m_SparkHandles, free);
    return free;
}

static SPARK *M_GetFreeSpark(void)
{
    const int32_t idx = M_GetFreeSparkIndex();
    return &m_Sparks[idx];
}

static void M_UpdateWind(void)
{
    if (g_Config.visuals.breeze_mode == BREEZE_MODE_OFF) {
        m_SmokeWind = (XZ_32) {};
        m_HairWindZ = 0;
        return;
    }

    if (g_Config.visuals.breeze_mode == BREEZE_MODE_TR2) {
        const ITEM *const lara_item = Lara_GetItem();
        if (lara_item == nullptr) {
            m_HairWindZ = 0;
            return;
        }

        const ROOM *const room = Room_Get(lara_item->room_num);
        if (room == nullptr || !room->flags.wind) {
            m_HairWindZ = 0;
            return;
        }

        const int32_t random = Random_GetDraw() & 7;
        if (random != 0) {
            m_HairWindZ += random - 4;
            if (m_HairWindZ < 0) {
                m_HairWindZ = 0;
            } else if (m_HairWindZ >= 8) {
                m_HairWindZ--;
            }
        }
        m_SmokeWind = (XZ_32) {
            .x = 0,
            .z = m_HairWindZ << 1,
        };
        return;
    }

    if (m_TR3WindHeld) {
        m_TR3WindHeld = false;
        m_SmokeWind = (XZ_32) {};
        m_HairWindZ = 0;
        return;
    }

    // TR3 wind logic: a small random wind magnitude with a slowly-changing
    // direction, biased to the [90°, 270°] range.
    m_TR3Wind += (Random_GetControl() & 7) - 3;
    if (m_TR3Wind <= -2) {
        m_TR3Wind++;
    } else if (m_TR3Wind >= 9) {
        m_TR3Wind--;
    }

    // Original TR3 uses a 0..4095 angle space; keep the calculations faithful.
    m_TR3DWindAngle =
        (m_TR3DWindAngle + (((Random_GetControl() & 0x3F) - 32) * 2)) & 0x1FFE;

    if (m_TR3DWindAngle < 1024) { // DEG_90
        m_TR3DWindAngle += (1024 - m_TR3DWindAngle) << 1;
    } else if (m_TR3DWindAngle > 3072) { // DEG_270
        m_TR3DWindAngle -= (m_TR3DWindAngle - 3072) << 1;
    }
    m_TR3DWindAngle &= 0x1FFE;
    m_TR3WindAngle =
        (m_TR3WindAngle + ((m_TR3DWindAngle - m_TR3WindAngle) >> 3)) & 0x1FFE;

    // Promote to DEG_360 for Math_Sin/Cos just at the end.
    const XYZ_32 wind =
        XYZ_32_RotateYaw((XYZ_32) { .z = m_TR3Wind }, m_TR3WindAngle << 3);
    m_SmokeWind = (XZ_32) { .x = wind.x, .z = wind.z };

    m_HairWindZ = 0;
}

void Sparks_SaveSpark(JSON_WRITE_IO *const io, const SPARK *const spark)
{
    JSONW_WRITE(io, "s_life", spark->s_life);
    JSONW_WRITE(io, "life", spark->life);
    JSONW_WRITE(io, "pos", spark->pos);
    JSONW_WRITE(io, "vel", spark->vel);
    JSONW_WRITE(io, "src_width", spark->src_size.width);
    JSONW_WRITE(io, "src_height", spark->src_size.height);
    JSONW_WRITE(io, "dst_width", spark->dst_size.width);
    JSONW_WRITE(io, "dst_height", spark->dst_size.height);
    JSONW_WRITE(io, "width", spark->size.width);
    JSONW_WRITE(io, "height", spark->size.height);
    JSONW_WRITE(io, "src_color", spark->src_color);
    JSONW_WRITE(io, "dst_color", spark->dst_color);
    JSONW_WRITE(io, "color", spark->color);
    JSONW_WRITE(io, "scalar", spark->scalar);
    JSONW_WRITE(io, "col_fade_speed", spark->col_fade_speed);
    JSONW_WRITE(io, "fade_to_black", spark->fade_to_black);
    JSONW_WRITE(io, "gravity", spark->gravity);
    JSONW_WRITE(io, "max_y_vel", spark->max_y_vel);
    JSONW_WRITE(io, "friction", spark->friction);
    JSONW_WRITE(io, "flags", spark->flags);
    JSONW_WRITE(io, "room_num", spark->room_num);
    JSONW_WRITE(io, "node_num", spark->node_num);
    JSONW_WRITE(io, "extras", spark->extras);
    JSONW_WRITE(io, "dynamic", spark->dynamic);
    JSONW_WRITE(io, "rot_angle", spark->rot_angle);
    JSONW_WRITE(io, "rot_add", spark->rot_add);
    JSONW_WRITE(io, "draw_type", (int32_t)spark->draw_type);
    JSONW_WRITE(io, "context", (int32_t)spark->context);

    if ((spark->flags & SPARK_F_SPRITE) != 0U) {
        const OBJECT *const obj = Object_Get(spark->sprite_obj_id);
        JSONW_WRITE(io, "sprite_obj_id", Object_IDToSlot(spark->sprite_obj_id));
        JSONW_WRITE(io, "sprite_offset", spark->sprite_idx - obj->mesh_idx);
    }

    if ((spark->flags & SPARK_F_ITEM) != 0U) {
        JSONW_WRITE(io, "item_num", spark->item_num);
    } else if ((spark->flags & SPARK_F_FX) != 0U) {
        JSONW_WRITE(io, "effect_num", Effect_GetInOrderNum(spark->effect_num));
    }
}

RESULT Sparks_LoadSpark(JSON_READ_IO *const io, SPARK *const spark)
{
    MUST(JSON_READ(io, "s_life", &spark->s_life));
    MUST(JSON_READ(io, "life", &spark->life));
    MUST(JSON_READ(io, "pos", &spark->pos));
    MUST(JSON_READ(io, "vel", &spark->vel));
    MUST(JSON_READ(io, "src_width", &spark->src_size.width));
    MUST(JSON_READ(io, "src_height", &spark->src_size.height));
    MUST(JSON_READ(io, "dst_width", &spark->dst_size.width));
    MUST(JSON_READ(io, "dst_height", &spark->dst_size.height));
    MUST(JSON_READ(io, "width", &spark->size.width));
    MUST(JSON_READ(io, "height", &spark->size.height));
    MUST(JSON_READ(io, "src_color", &spark->src_color));
    MUST(JSON_READ(io, "dst_color", &spark->dst_color));
    MUST(JSON_READ(io, "color", &spark->color));
    MUST(JSON_READ(io, "scalar", &spark->scalar));
    MUST(JSON_READ(io, "col_fade_speed", &spark->col_fade_speed));
    MUST(JSON_READ(io, "fade_to_black", &spark->fade_to_black));
    MUST(JSON_READ(io, "gravity", &spark->gravity));
    MUST(JSON_READ(io, "max_y_vel", &spark->max_y_vel));
    MUST(JSON_READ(io, "friction", &spark->friction));
    MUST(JSON_READ(io, "flags", &spark->flags));
    MUST(JSON_READ(io, "room_num", &spark->room_num));
    MUST(JSON_READ(io, "node_num", &spark->node_num));
    MUST(JSON_READ(io, "extras", &spark->extras));
    MUST(JSON_READ(io, "dynamic", &spark->dynamic));
    MUST(JSON_READ(io, "rot_angle", &spark->rot_angle));
    MUST(JSON_READ(io, "rot_add", &spark->rot_add));
    SHOULD(JSON_READ(io, "context", &spark->context));

    int32_t draw_type = 0;
    MUST(JSON_READ(io, "draw_type", &draw_type));
    spark->draw_type = (DRAW_TYPE)draw_type;

    spark->sprite_obj_id = NO_OBJECT;
    spark->sprite_idx = 0;
    if ((spark->flags & SPARK_F_SPRITE) != 0U) {
        int32_t game_id = 0;
        int32_t sprite_offset = 0;
        MUST(JSON_READ(io, "sprite_obj_id", &game_id));
        MUST(JSON_READ(io, "sprite_offset", &sprite_offset));
        spark->sprite_obj_id = Object_SlotToID(game_id);
        if (spark->sprite_obj_id == NO_OBJECT) {
            return JSON_ReadIO_Fail(io, "unsupported object #%d", game_id);
        }
        const OBJECT *const obj = Object_Get(spark->sprite_obj_id);
        if (!obj->loaded || sprite_offset < 0
            || sprite_offset >= ABS(obj->mesh_count)) {
            return JSON_ReadIO_Fail(
                io, "sprite #%d is not in object #%d", sprite_offset, game_id);
        }
        spark->sprite_idx = obj->mesh_idx + sprite_offset;
    }

    if ((spark->flags & SPARK_F_ITEM) != 0U) {
        MUST(JSON_READ(io, "item_num", &spark->item_num));
    } else if ((spark->flags & SPARK_F_FX) != 0U) {
        MUST(JSON_READ(io, "effect_num", &spark->effect_num));
        if (Effect_GetInOrderNum(spark->effect_num) == NO_EFFECT) {
            return JSON_ReadIO_Fail(
                io, "no effect #%d to attach to", spark->effect_num);
        }
    }

    return OK;
}

// While the sparks are staged for drawing the items stand still: a joint (an
// item's node) is worked out once for all the sparks that hang from it, not
// once each (a torch's flame is a score of them).
static struct {
    bool on;
    struct {
        bool valid;
        int16_t item;
        XYZ_32 pos;
    } joint[16];
} m_JointCache;

void Sparks_JointCache(const bool on)
{
    memset(&m_JointCache, 0, sizeof(m_JointCache));
    m_JointCache.on = on;
}

XYZ_32 Sparks_GetWorldPos(const SPARK *const spark)
{
    if (spark == nullptr) {
        return (XYZ_32) { 0, 0, 0 };
    }

    if ((spark->flags & SPARK_F_FX) != 0U) {
        const EFFECT *const effect = Effect_Get(spark->effect_num);
        if ((spark->flags & SPARK_F_ATTACHED_POS) != 0U) {
            return effect->pos;
        }
        return (XYZ_32) {
            .x = effect->pos.x + spark->pos.x,
            .y = effect->pos.y + spark->pos.y,
            .z = effect->pos.z + spark->pos.z,
        };
    }

    if ((spark->flags & SPARK_F_ITEM) != 0U) {
        const ITEM *const item = Item_Get(spark->item_num);
        if ((spark->flags & SPARK_F_ATTACHED_POS) != 0U) {
            return item->pos;
        }

        if ((spark->flags & SPARK_F_ATTACHED_NODE) != 0U) {
            XYZ_32 joint_pos;
            const int32_t node = spark->node_num & 0xF;
            if (m_JointCache.on && m_JointCache.joint[node].valid
                && m_JointCache.joint[node].item == spark->item_num) {
                joint_pos = m_JointCache.joint[node].pos;
            } else {
                joint_pos = m_NodeOffsets[node].pos;
                Collide_GetJointAbsPosition(
                    item, &joint_pos, m_NodeOffsets[node].mesh_num);
                if (m_JointCache.on) {
                    m_JointCache.joint[node].valid = true;
                    m_JointCache.joint[node].item = spark->item_num;
                    m_JointCache.joint[node].pos = joint_pos;
                }
            }
            return (XYZ_32) {
                .x = joint_pos.x + spark->pos.x,
                .y = joint_pos.y + spark->pos.y,
                .z = joint_pos.z + spark->pos.z,
            };
        }

        return (XYZ_32) {
            .x = item->pos.x + spark->pos.x,
            .y = item->pos.y + spark->pos.y,
            .z = item->pos.z + spark->pos.z,
        };
    }

    return spark->pos;
}

SPARK *Sparks_GetSpark(const int32_t idx)
{
    ASSERT(idx >= 0 && idx < M_MAX_SPARKS);
    return &m_Sparks[idx];
}

int32_t Sparks_GetMaxCount(void)
{
    return M_MAX_SPARKS;
}

TRX_HANDLE Sparks_GetHandle(const SPARK *const spark)
{
    if (spark == nullptr) {
        return (TRX_HANDLE) { .id = -1 };
    }
    return Handle_RegistryMint(&m_SparkHandles, (int32_t)(spark - m_Sparks));
}

SPARK *Sparks_FromHandle(const TRX_HANDLE handle)
{
    if (!Handle_RegistryIsLive(&m_SparkHandles, handle)) {
        return nullptr;
    }
    SPARK *const spark = &m_Sparks[handle.id];
    return spark->on ? spark : nullptr;
}

SPARK *Sparks_InitialiseSpark(const SPARK_CONTEXT context)
{
    SPARK *const spark = M_GetFreeSpark();
    spark->on = true;
    spark->context = context;
    return spark;
}

SPARK *Sparks_InitialiseSpriteSpark(
    const SPARK_SPRITE_TYPE type, const SPARK_CONTEXT context)
{
    const int32_t sprite_idx = Sparks_GetSpriteIndex(type);
    if (sprite_idx == NO_ITEM) {
        return nullptr;
    }

    SPARK *const spark = Sparks_InitialiseSpark(context);
    spark->sprite_idx = sprite_idx;
    spark->sprite_obj_id = O_SPARKS_GFX;
    return spark;
}

int32_t Sparks_GetSpriteIndex(const SPARK_SPRITE_TYPE type)
{
    OBJECT *const obj = Object_Get(O_SPARKS_GFX);
    if (obj == nullptr || !obj->loaded) {
        return NO_ITEM;
    }
    if (type < 0 || (int32_t)type >= ABS(obj->mesh_count)) {
        return NO_ITEM;
    }
    return obj->mesh_idx + type;
}

void Sparks_Save(JSON_WRITE_IO *const io)
{
    if (!g_Config.gameplay.enable_enhanced_saves) {
        return;
    }

    JSONW_PUSH_ARRAY(io);
    for (int32_t i = 0; i < M_MAX_SPARKS; i++) {
        const SPARK *const spark = &m_Sparks[i];
        if (!spark->on) {
            continue;
        }
        if ((spark->flags & SPARK_F_FX) != 0U
            && Effect_GetInOrderNum(spark->effect_num) == NO_EFFECT) {
            continue;
        }
        JSONW_PUSH_OBJECT(io);
        Sparks_SaveSpark(io, spark);
        JSONW_POP_AND_APPEND(io);
    }
    JSONW_POP_AND_SET(io, "sparks");
}

RESULT Sparks_Load(JSON_READ_IO *const io)
{
    if (!g_Config.gameplay.enable_enhanced_saves) {
        return OK;
    }

    if (!JSON_ReadIO_HasKey(io, "sparks")) {
        return OK;
    }
    MUST(JSON_PUSH(io, "sparks"));

    const int32_t count = JSON_ARRAY_LEN(io);
    for (int32_t i = 0; i < count; i++) {
        if (i >= M_MAX_SPARKS) {
            LOG_WARNING(
                "Malformed save: too many sparks. Extra sparks will be "
                "ignored.");
            break;
        }
        SPARK *const spark = &m_Sparks[i];
        MUST(JSON_PUSH_INDEX(io, i));
        MUST(Sparks_LoadSpark(io, spark));
        MUST(JSON_POP(io));
        spark->on = true;
        Sparks_Sync(spark);
    }
    m_NextSpark = count < M_MAX_SPARKS ? count : 0;

    MUST(JSON_POP(io));
    return OK;
}

void Sparks_Sync(SPARK *const spark)
{
    if (spark == nullptr) {
        return;
    }

    const XYZ_32 world_pos = Sparks_GetWorldPos(spark);
    spark->prev_pos = spark->pos;
    spark->prev_world_pos = world_pos;
    spark->prev_color = spark->color;
    spark->prev_size = spark->size;
    spark->prev_rot_angle = spark->rot_angle;
}

void Sparks_FinishSetup(SPARK *const spark)
{
    if (spark == nullptr) {
        return;
    }

    spark->color = spark->src_color;
    Sparks_Sync(spark);
}

int8_t Sparks_AllocDynamic(const uint8_t flags)
{
    for (int32_t i = 0; i < M_MAX_SPARK_DYNAMICS; i++) {
        if (!m_Dynamics[i].on) {
            m_Dynamics[i].on = true;
            m_Dynamics[i].falloff = 4;
            m_Dynamics[i].flags = flags;
            m_Dynamics[i].color = COLOR_RGB_888_BLACK;
            return (int8_t)i;
        }
    }
    return -1;
}

void Sparks_FreeDynamic(const int8_t idx)
{
    if (idx < 0 || idx >= M_MAX_SPARK_DYNAMICS) {
        return;
    }
    m_Dynamics[idx].on = false;
}

void Sparks_Reset(void)
{
    Memory_FreePointer(&m_Sparks);
    m_Sparks = Memory_Alloc(sizeof(SPARK) * M_MAX_SPARKS);
    for (int32_t i = 0; i < M_MAX_SPARKS; i++) {
        m_Sparks[i] = (SPARK) {};
        m_Sparks[i].dynamic = -1;
    }
    for (int32_t i = 0; i < M_MAX_SPARK_DYNAMICS; i++) {
        m_Dynamics[i] = (M_SPARK_DYNAMIC) {};
    }
    Handle_RegistryBumpAll(&m_SparkHandles);
    m_NextSpark = 0;
    m_SmokeWind = (XZ_32) {};
    m_HairWindZ = 0;
    Sparks_ResetWind();
}

void Sparks_ResetWind(void)
{
    m_TR3Wind = 0;
    m_TR3WindAngle = M_TR3_WIND_START_ANGLE;
    m_TR3DWindAngle = M_TR3_WIND_START_ANGLE;
    m_TR3WindHeld = true;
}

XZ_32 Sparks_GetSmokeWind(void)
{
    return m_SmokeWind;
}

void Sparks_SetSmokeWind(const XZ_32 wind)
{
    m_SmokeWind = wind;
}

int32_t Sparks_GetHairWindZ(void)
{
    return m_HairWindZ;
}

void Sparks_UpdateWind(void)
{
    M_UpdateWind();
}

// The rare paths of a spark's tick, which the assembly (sparks_ctl_sh4.S)
// calls back into: the explosion a spark sets off at its time, and the
// detaching of a spark hanging from a node.
void dc_spark_extras(SPARK *const spark)
{
    int32_t uw = 0;
    if ((spark->flags & SPARK_F_UNDERWATER) != 0U) {
        uw = 1;
    } else if ((spark->flags & SPARK_F_GREEN) != 0U) {
        uw = 2;
    }

    const XYZ_32 spark_pos = Sparks_GetWorldPos(spark);

    for (int32_t j = 0; j < (int32_t)(spark->extras & 7U); j++) {
        Sparks_TriggerExplosionSparks(
            spark_pos, (int32_t)(spark->extras & 7U) - 1,
            spark->dynamic, uw, spark->room_num);
        spark->dynamic = -1;
    }

    if ((spark->flags & SPARK_F_UNDERWATER) != 0U) {
        Sparks_TriggerExplosionBubble(spark_pos, spark->room_num);
    }

    spark->extras = 0;
}

void dc_spark_detach(SPARK *const spark, const int32_t lived)
{
    const int32_t b = spark->node_num == 3 ? (Random_GetDraw() & 3) + 12
                                           : (Random_GetDraw() & 3) + 8;
    if (lived > b) {
        spark->pos = Sparks_GetWorldPos(spark);
        spark->flags &= ~(SPARK_F_ATTACHED_NODE | SPARK_F_ITEM);
        Sparks_Sync(spark);
    }
}

// The spark loop of a tick, in C: the reference of the assembly (a build
// with DC_EMIT_VERIFY compares them) and the other platforms' code.
[[maybe_unused]] static void M_ControlC(SPARK *const sparks)
{
    for (int32_t i = 0; i < M_MAX_SPARKS; i++) {
        SPARK *const spark = &sparks[i];
        if (!spark->on) {
            continue;
        }

        if ((spark->flags & SPARK_F_ATTACHED_POS) == 0U || spark->life > 16) {
            if (spark->life > 0) {
                spark->life--;
            }
        }

        if (spark->life == 0) {
            if (spark->dynamic != -1) {
                m_Dynamics[(uint8_t)spark->dynamic].on = false;
                spark->dynamic = -1;
            }

            spark->on = false;
            continue;
        }

        const int32_t lived = (int32_t)spark->s_life - (int32_t)spark->life;

        spark->prev_pos = spark->pos;
        spark->prev_world_pos = Sparks_GetWorldPos(spark);
        spark->prev_color = spark->color;
        spark->prev_size = spark->size;
        spark->prev_rot_angle = spark->rot_angle;

        // Color fade: src -> dst, then fade-to-black.
        if (lived < (int32_t)spark->col_fade_speed
            && spark->col_fade_speed != 0U) {
            const float fade = lived / (float)spark->col_fade_speed;
            spark->color.r = LERP(
                (int32_t)spark->src_color.r, (int32_t)spark->dst_color.r, fade);
            spark->color.g = LERP(
                (int32_t)spark->src_color.g, (int32_t)spark->dst_color.g, fade);
            spark->color.b = LERP(
                (int32_t)spark->src_color.b, (int32_t)spark->dst_color.b, fade);
        } else if (
            spark->life < spark->fade_to_black && spark->fade_to_black != 0U) {
            const float fade = spark->life / (float)spark->fade_to_black;
            spark->color.r = spark->dst_color.r * fade;
            spark->color.g = spark->dst_color.g * fade;
            spark->color.b = spark->dst_color.b * fade;
        } else {
            spark->color = spark->dst_color;
        }

        if (spark->life == spark->fade_to_black
            && (spark->flags & SPARK_F_UNDERWATER) != 0U) {
            spark->dst_size.width >>= 2;
            spark->dst_size.height >>= 2;
        }

        if ((spark->flags & SPARK_F_ROTATE) != 0U) {
            spark->rot_angle = (spark->rot_angle + spark->rot_add) & 0xFFF;
        }

        if ((spark->flags & SPARK_F_ALT_SPRITE) != 0U) {
            const int32_t base = Sparks_GetSpriteIndex(SPARK_TYPE_EXPLOSION);
            if (base != NO_ITEM) {
                spark->sprite_obj_id = O_SPARKS_GFX;
                if (spark->color.r < 16 && spark->color.g < 16
                    && spark->color.b < 16) {
                    spark->sprite_idx = base + 3;
                } else if (
                    spark->color.r < 64 && spark->color.g < 64
                    && spark->color.b < 64) {
                    spark->sprite_idx = base + 2;
                } else if (
                    spark->color.r < 96 && spark->color.g < 96
                    && spark->color.b < 96) {
                    spark->sprite_idx = base + 1;
                } else {
                    spark->sprite_idx = base;
                }
            }
        }

        if (lived == (int32_t)(spark->extras >> 3)
            && (spark->extras & 7U) != 0U) {
            dc_spark_extras(spark);
        }

        // Physics
        spark->vel.y += spark->gravity;
        if (spark->max_y_vel != 0) {
            const int32_t limit = (int32_t)spark->max_y_vel * (1 << 5);
            if ((spark->vel.y < 0 && spark->vel.y < limit)
                || (spark->vel.y > 0 && spark->vel.y > limit)) {
                spark->vel.y = limit;
            }
        }

        if ((spark->friction & 0x0FU) != 0U) {
            spark->vel.x -= spark->vel.x >> (spark->friction & 0x0FU);
            spark->vel.z -= spark->vel.z >> (spark->friction & 0x0FU);
        }

        if ((spark->friction & 0xF0U) != 0U) {
            spark->vel.y -= spark->vel.y >> (spark->friction >> 4);
        }

        spark->pos.x += spark->vel.x >> 5;
        spark->pos.y += spark->vel.y >> 5;
        spark->pos.z += spark->vel.z >> 5;

        if ((spark->flags & SPARK_F_OUTSIDE) != 0U) {
            spark->pos.x += m_SmokeWind.x >> 1;
            spark->pos.z += m_SmokeWind.z >> 1;
        }

        // Size lerp across lifetime.
        if (spark->s_life != 0U) {
            const float fade = lived / (float)spark->s_life;
            spark->size.width = LERP(
                (int32_t)spark->src_size.width, (int32_t)spark->dst_size.width,
                fade);
            spark->size.height = LERP(
                (int32_t)spark->src_size.height,
                (int32_t)spark->dst_size.height, fade);
        } else {
            spark->size = spark->src_size;
        }

        // If attached to a node, detach after a short random delay for some
        // node types.
        if ((spark->flags & (SPARK_F_ITEM | SPARK_F_ATTACHED_NODE))
                == (SPARK_F_ITEM | SPARK_F_ATTACHED_NODE)
            && (spark->node_num == 2 || spark->node_num == 3)) {
            dc_spark_detach(spark, lived);
        }
    }
}

#if defined(__DREAMCAST__)
// What sparks_ctl_sh4.S takes (the layout is its own: see the header of that
// file).
typedef struct {
    SPARK *sparks;
    int32_t count;
    M_SPARK_DYNAMIC *dyn;
    int32_t base;
    int32_t wind_x;
    int32_t wind_z;
    void (*world)(const SPARK *, int32_t *);
    void (*extras)(SPARK *);
    void (*detach)(SPARK *, int32_t);
    int32_t gfx;
    uint32_t *rand;
    void (*light)(const int32_t *);
} M_SPARKCTL;
_Static_assert(sizeof(M_SPARK_DYNAMIC) == 6, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(M_SPARK_DYNAMIC, on) == 0, "sparks_ctl_sh4.S layout");
_Static_assert(sizeof(SPARK) == 112, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, s_life) == 1, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, life) == 2, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, pos) == 4, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, prev_pos) == 16, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, prev_world_pos) == 28, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, vel) == 40, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, src_size) == 52, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, dst_size) == 54, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, size) == 56, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, prev_size) == 58, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, src_color) == 60, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, dst_color) == 63, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, color) == 66, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, prev_color) == 69, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, scalar) == 72, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, col_fade_speed) == 73, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, fade_to_black) == 74, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, gravity) == 76, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, max_y_vel) == 78, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, friction) == 79, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, flags) == 80, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, node_num) == 85, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, extras) == 86, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, dynamic) == 87, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, rot_angle) == 88, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, prev_rot_angle) == 90, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, rot_add) == 92, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, sprite_idx) == 96, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(SPARK, sprite_obj_id) == 100, "sparks_ctl_sh4.S layout");
_Static_assert(NO_ITEM == -1, "sparks_ctl_sh4.S: no sprites is -1");
_Static_assert(offsetof(M_SPARKCTL, gfx) == 36, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(M_SPARKCTL, rand) == 40, "sparks_ctl_sh4.S layout");
_Static_assert(offsetof(M_SPARKCTL, light) == 44, "sparks_ctl_sh4.S layout");
_Static_assert(
    SPARK_F_ATTACHED_POS == 0x400 && SPARK_F_UNDERWATER == 0x800
        && SPARK_F_ALT_SPRITE == 0x200 && SPARK_F_OUTSIDE == 0x100,
    "sparks_ctl_sh4.S flags");

extern void dc_spark_control(M_SPARKCTL *ctl);
extern void dc_spark_world(const SPARK *spark, int32_t *out);

static void M_Control(SPARK *const sparks)
{
    M_SPARKCTL ctl = {
        .sparks = sparks,
        .count = M_MAX_SPARKS,
        .dyn = m_Dynamics,
        .base = Sparks_GetSpriteIndex(SPARK_TYPE_EXPLOSION),
        .wind_x = m_SmokeWind.x,
        .wind_z = m_SmokeWind.z,
        .world = dc_spark_world,
        .extras = dc_spark_extras,
        .detach = dc_spark_detach,
        .gfx = O_SPARKS_GFX,
    };
    #if defined(DC_EMIT_VERIFY)
    // A tick that sets off an explosion or detaches a spark takes its own
    // paths through the engine: only the others are compared with the C code.
    static SPARK scratch[M_MAX_SPARKS];
    M_SPARK_DYNAMIC dyn0[M_MAX_SPARK_DYNAMICS];
    M_SPARK_DYNAMIC dyn_asm[M_MAX_SPARK_DYNAMICS];
    bool rare = false;
    for (int32_t i = 0; i < M_MAX_SPARKS && !rare; i++) {
        const SPARK *const sp = &sparks[i];
        rare = sp->on
            && ((sp->extras & 7U) != 0U
                || ((sp->flags & (SPARK_F_ITEM | SPARK_F_ATTACHED_NODE))
                        == (SPARK_F_ITEM | SPARK_F_ATTACHED_NODE)
                    && (sp->node_num == 2 || sp->node_num == 3)));
    }
    if (!rare) {
        memcpy(scratch, sparks, sizeof(scratch));
        memcpy(dyn0, m_Dynamics, sizeof(dyn0));
    }
    dc_spark_control(&ctl);
    if (!rare) {
        memcpy(dyn_asm, m_Dynamics, sizeof(dyn_asm));
        memcpy(m_Dynamics, dyn0, sizeof(dyn0));
        M_ControlC(scratch);
        static uint32_t m_Ticks;
        static uint32_t m_Bad;
        m_Ticks++;
        int32_t bad_spark = -1;
        for (int32_t i = 0; i < M_MAX_SPARKS; i++) {
            if (memcmp(&sparks[i], &scratch[i], sizeof(SPARK)) != 0) {
                bad_spark = i;
                break;
            }
        }
        const bool dyn_bad = memcmp(dyn_asm, m_Dynamics, sizeof(dyn_asm)) != 0;
        if (bad_spark >= 0 || dyn_bad) {
            m_Bad++;
            if (m_Bad <= 8) {
                const uint8_t *const a = (const uint8_t *)&sparks[MAX(bad_spark, 0)];
                const uint8_t *const b = (const uint8_t *)&scratch[MAX(bad_spark, 0)];
                char diff[200] = "";
                int32_t len = 0;
                for (size_t k = 0; k < sizeof(SPARK) && len < 170; k++) {
                    if (a[k] != b[k]) {
                        len += snprintf(
                            diff + len, sizeof(diff) - len, " +%u:%02x/%02x",
                            (unsigned)k, a[k], b[k]);
                    }
                }
                LOG_WARNING(
                    "[spark-ctl-verify] MISMATCH spark %d dyn %d, asm/C:%s",
                    (int)bad_spark, (int)dyn_bad, diff);
            }
        }
        if (m_Ticks % 300 == 0) {
            LOG_WARNING(
                "[spark-ctl-verify] %u ticks, %u bad", (unsigned)m_Ticks,
                (unsigned)m_Bad);
        }
    }
    #else
    dc_spark_control(&ctl);
    #endif
}
#else
static void M_Control(SPARK *const sparks)
{
    M_ControlC(sparks);
}
#endif

// A light the dynamic pass adds: x, y, z, falloff, r, g, b.
static void M_AddLight(const int32_t *const a)
{
    Output_AddDynamicLightRGB(
        (XYZ_32) { a[0], a[1], a[2] }, a[3],
        (RGB_888) { (uint8_t)a[4], (uint8_t)a[5], (uint8_t)a[6] });
}

#if defined(__DREAMCAST__)
void dc_spark_light(const int32_t *const a)
{
    M_AddLight(a);
}
#endif

// The dynamic light pass of a tick, in C: every spark with a light that is on
// lights its place, the light fading as the spark does. The reference of the
// assembly (a build with DC_EMIT_VERIFY compares them) and the other
// platforms' code.
[[maybe_unused]] static void M_LightsC(
    SPARK *const sparks, void (*const emit)(const int32_t *))
{
    for (int32_t i = 0; i < M_MAX_SPARKS; i++) {
        const SPARK *const spark = &sparks[i];
        if (!spark->on || spark->dynamic == -1) {
            continue;
        }

        M_SPARK_DYNAMIC *const dl = &m_Dynamics[(uint8_t)spark->dynamic];
        if (!dl->on) {
            continue;
        }

        const XYZ_32 world_pos = Sparks_GetWorldPos(spark);
        const int32_t rnd = Random_GetControl();
        XYZ_32 pos = {
            .x = world_pos.x + ((rnd & 0xF) << 4),
            .y = world_pos.y + (rnd & 0xF0),
            .z = world_pos.z + ((rnd >> 4) & 0xF0),
        };

        int32_t falloff = (int32_t)spark->s_life - (int32_t)spark->life - 1;
        int32_t r = 0;
        int32_t g = 0;
        int32_t b = 0;

        if (falloff < 2) {
            if (dl->falloff < 28) {
                dl->falloff = (uint8_t)MIN((int32_t)dl->falloff + 6, 255);
            }
            r = 255 - (rnd & 0x1F) - (falloff << 3);
            g = 255 - (rnd & 0x1F) - (falloff << 4);
            b = 255 - (rnd & 0x1F) - (falloff << 6);
        } else if (falloff < 4) {
            if (dl->falloff < 28) {
                dl->falloff = (uint8_t)MIN((int32_t)dl->falloff + 6, 255);
            }
            r = 255 - (rnd & 0x1F) - (falloff << 3);
            g = 128 - (falloff << 3);
            b = (4 - falloff) << 2;
            if (b < 0) {
                b = 0;
            } else {
                b <<= 3;
            }
        } else {
            if (dl->falloff != 0U) {
                dl->falloff--;
            }
            r = (rnd & 0x1F) + 224;
            g = ((rnd >> 4) & 0x1F) + 128;
            b = (rnd >> 8) & 0x3F;
        }

        falloff = (int32_t)dl->falloff;
        if (falloff > 31) {
            falloff = 31;
        }

        const int32_t light[7] = {
            pos.x, pos.y, pos.z, falloff,
            (spark->flags & SPARK_F_GREEN) != 0U ? b : r,
            (spark->flags & SPARK_F_GREEN) != 0U ? r : g,
            (spark->flags & SPARK_F_GREEN) != 0U ? g : b,
        };
        emit(light);
    }
}

#if defined(__DREAMCAST__) && defined(DC_EMIT_VERIFY)
static int32_t asm_lights[M_MAX_SPARKS][7];
static int32_t c_lights[M_MAX_SPARKS][7];
static int32_t asm_n;
static int32_t c_n;

static void M_RecordAsm(const int32_t *const a)
{
    if (asm_n < M_MAX_SPARKS) {
        memcpy(asm_lights[asm_n++], a, sizeof(int32_t) * 7);
    }
}

static void M_RecordC(const int32_t *const a)
{
    if (c_n < M_MAX_SPARKS) {
        memcpy(c_lights[c_n++], a, sizeof(int32_t) * 7);
    }
}
#endif

#if defined(__DREAMCAST__)
extern void dc_spark_lights(M_SPARKCTL *ctl);

static void M_Lights(SPARK *const sparks)
{
    M_SPARKCTL ctl = {
        .sparks = sparks,
        .count = M_MAX_SPARKS,
        .dyn = m_Dynamics,
        .world = dc_spark_world,
        .rand = Random_GetControlState(),
        .light = dc_spark_light,
    };
    #if defined(DC_EMIT_VERIFY)
    // Both passes record the lights they would add instead of adding them; the
    // two lists, the random state and the dynamic lights must come out alike.
    M_SPARK_DYNAMIC dyn0[M_MAX_SPARK_DYNAMICS];
    M_SPARK_DYNAMIC dyn_asm[M_MAX_SPARK_DYNAMICS];
    memcpy(dyn0, m_Dynamics, sizeof(dyn0));
    const uint32_t seed0 = *Random_GetControlState();
    asm_n = 0;
    ctl.light = M_RecordAsm;
    dc_spark_lights(&ctl);
    const uint32_t seed_asm = *Random_GetControlState();
    memcpy(dyn_asm, m_Dynamics, sizeof(dyn_asm));
    memcpy(m_Dynamics, dyn0, sizeof(dyn0));
    *Random_GetControlState() = seed0;
    c_n = 0;
    M_LightsC(sparks, M_RecordC);
    static uint32_t m_Ticks;
    static uint32_t m_Bad;
    m_Ticks++;
    const bool bad = asm_n != c_n || seed_asm != *Random_GetControlState()
        || memcmp(dyn_asm, m_Dynamics, sizeof(dyn_asm)) != 0
        || memcmp(asm_lights, c_lights, sizeof(int32_t) * 7 * c_n) != 0;
    if (bad) {
        m_Bad++;
        if (m_Bad <= 8) {
            LOG_WARNING(
                "[spark-light-verify] MISMATCH asm %d lights, C %d, seed "
                "%08x/%08x, first light asm %d %d %d %d %d %d %d | C %d %d %d "
                "%d %d %d %d",
                (int)asm_n, (int)c_n, (unsigned)seed_asm,
                (unsigned)*Random_GetControlState(), (int)asm_lights[0][0],
                (int)asm_lights[0][1], (int)asm_lights[0][2],
                (int)asm_lights[0][3], (int)asm_lights[0][4],
                (int)asm_lights[0][5], (int)asm_lights[0][6],
                (int)c_lights[0][0], (int)c_lights[0][1], (int)c_lights[0][2],
                (int)c_lights[0][3], (int)c_lights[0][4], (int)c_lights[0][5],
                (int)c_lights[0][6]);
        }
    }
    if (m_Ticks % 300 == 0) {
        LOG_WARNING(
            "[spark-light-verify] %u ticks, %u bad", (unsigned)m_Ticks,
            (unsigned)m_Bad);
    }
    for (int32_t i = 0; i < c_n; i++) {
        M_AddLight(c_lights[i]);
    }
    #else
    dc_spark_lights(&ctl);
    #endif
}
#else
static void M_Lights(SPARK *const sparks)
{
    M_LightsC(sparks, M_AddLight);
}
#endif

void Sparks_Control(void)
{
    if (g_TRVersion != 3) {
        M_UpdateWind();
    }

    // The items are done moving for the tick: the joints the sparks hang from
    // are worked out once each, not once a spark and pass.
    Sparks_JointCache(true);
    M_Control(m_Sparks);

    M_Lights(m_Sparks);
    Sparks_JointCache(false);
}

void Sparks_Draw(void)
{
#if defined(__DREAMCAST__)
    OutputSource_PolyFX_StageSparks(m_Sparks, M_MAX_SPARKS);
#else
    for (int32_t i = 0; i < M_MAX_SPARKS; i++) {
        SPARK *const spark = &m_Sparks[i];
        if (!spark->on) {
            continue;
        }

        OutputSource_PolyFX_StageSpark(spark);
    }
#endif
}

void Sparks_DetachEffect(const int16_t effect_num)
{
    for (int32_t i = 0; i < M_MAX_SPARKS; i++) {
        SPARK *const spark = &m_Sparks[i];
        if (!spark->on) {
            continue;
        }

        if ((spark->flags & SPARK_F_FX) != 0U
            && spark->effect_num == effect_num) {
            if ((spark->flags & SPARK_F_ATTACHED_POS) != 0U) {
                spark->on = false;
                continue;
            }

            const EFFECT *const effect = Effect_Get(effect_num);
            spark->pos.x += effect->pos.x;
            spark->pos.y += effect->pos.y;
            spark->pos.z += effect->pos.z;
            spark->flags &= ~SPARK_F_FX;
        }
    }
}

void Sparks_DetachItem(const int16_t item_num)
{
    for (int32_t i = 0; i < M_MAX_SPARKS; i++) {
        SPARK *const spark = &m_Sparks[i];
        if (!spark->on) {
            continue;
        }

        if ((spark->flags & SPARK_F_ITEM) != 0U
            && spark->item_num == item_num) {
            if ((spark->flags & SPARK_F_ATTACHED_POS) != 0U) {
                spark->on = false;
                continue;
            }

            const ITEM *const item = Item_Get(item_num);
            spark->pos.x += item->pos.x;
            spark->pos.y += item->pos.y;
            spark->pos.z += item->pos.z;
            spark->flags &= ~SPARK_F_ITEM;
            spark->flags &= ~SPARK_F_ATTACHED_NODE;
        }
    }
}
