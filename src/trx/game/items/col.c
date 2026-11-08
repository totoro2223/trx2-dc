#include <trx/game/items/col.h>

#include <trx/config.h>
#include <trx/core/log.h>
#include <trx/core/utils.h>
#include <string.h>

#include <trx/game/objects.h>
#include <trx/game/rooms.h>

static BOUNDS_16 m_NullBounds = {};
static BOUNDS_16 m_InterpolatedBounds = {};

int32_t Item_GetHeight(const ITEM *const item)
{
    int16_t room_num = item->room_num;
    const SECTOR *const sector = Room_GetSector(item->pos, &room_num);
    return Room_GetHeightEx(
        sector, item->pos,
        &(ROOM_HEIGHT_CTX) {
            .fix_tilts = g_Config.gameplay.fix_wall_geometry,
            .subject = item,
        });
}

int32_t Item_GetDistance(const ITEM *const item, const XYZ_32 target)
{
    return XYZ_32_GetDistance(item->pos, target);
}

void Item_Translate(
    ITEM *const item, const int32_t x, const int32_t y, const int32_t z)
{
    item->pos = XYZ_32_OffsetLocalYaw(
        item->pos, (XYZ_32) { .x = x, .z = z }, item->rot.y);
    item->pos.y += y;
}

bool Item_Test3DRange(
    const int32_t x, const int32_t y, const int32_t z, const int32_t range)
{
    return ABS(x) < range && ABS(y) < range && ABS(z) < range
        && (SQUARE(x) + SQUARE(y) + SQUARE(z) < SQUARE(range));
}

bool Item_IsNearby(
    const ITEM *const item_1, const ITEM *const item_2, const int32_t distance)
{
    return XYZ_32_IsNearby(item_1->pos, item_2->pos, distance);
}

bool Item_TestBoundsCollide(
    const ITEM *const src_item, const ITEM *const dst_item,
    const int32_t radius)
{
    const ANIM_FRAME *const src_frame = Item_GetBestFrame(src_item);
    const ANIM_FRAME *const dst_frame = Item_GetBestFrame(dst_item);
    if (src_frame == nullptr || dst_frame == nullptr) {
        return false;
    }

    const COLL_ITEM src = {
        .bounds = src_frame->bounds,
        .pos = src_item->pos,
        .rot = src_item->rot,
    };
    const COLL_ITEM dst = {
        .bounds = dst_frame->bounds,
        .pos = dst_item->pos,
        .rot = dst_item->rot,
    };
    return Collide_TestBoundsCollide(&src, &dst, radius);
}

bool Item_TestStatic3DBoundsCollide(
    const STATIC_MESH *const mesh, const ITEM *const dst_item,
    const int32_t radius)
{
    const COLL_ITEM src = {
        .bounds = Object_Get3DStatic(mesh->static_num)->collision_bounds,
        .pos = mesh->pos,
        .rot = { .y = mesh->rot.y },
    };
    const COLL_ITEM dst = {
        .bounds = Item_GetBestFrame(dst_item)->bounds,
        .pos = dst_item->pos,
        .rot = dst_item->rot,
    };
    return Collide_TestBoundsCollide(&src, &dst, radius);
}

static void M_BoundsLerpC(
    BOUNDS_16 *const result, const BOUNDS_16 *const b1,
    const BOUNDS_16 *const b2, const int32_t frac, const int32_t rate)
{
#define CALC(target, p1, p2, prop)                                                 target->prop = (p1)->prop + ((((p2)->prop - (p1)->prop) * frac) / rate);
    CALC(result, b1, b2, min.x);
    CALC(result, b1, b2, max.x);
    CALC(result, b1, b2, min.y);
    CALC(result, b1, b2, max.y);
    CALC(result, b1, b2, min.z);
    CALC(result, b1, b2, max.z);
#undef CALC
}

#if defined(__DREAMCAST__)
extern uint32_t g_DC_AsmOff;
extern void dc_bounds_lerp(
    int16_t *out, const int16_t *b1, const int16_t *b2, const int32_t *p);
#endif

const BOUNDS_16 *Item_GetBoundsAccurate(const ITEM *const item)
{
    int32_t rate;
    ANIM_FRAME *frames[2];
    const int32_t frac = Item_GetFrames(item, frames, &rate);
    if (frames[0] == nullptr) {
        return &m_NullBounds;
    }

    if (frac == 0) {
        return &frames[0]->bounds;
    }

    BOUNDS_16 *const result = &m_InterpolatedBounds;
#if defined(__DREAMCAST__)
    // In assembly (game_sh4.S, dc_bounds_lerp): the six divisions by the
    // reciprocal. ASMOFF bit 2147483648 turns it off.
    if ((g_DC_AsmOff & 2147483648u) == 0 && rate > 0) {
        int32_t p[3] = { frac, rate, (int32_t)(0xFFFFFFFFu / (uint32_t)rate) };
    #if defined(DC_EMIT_VERIFY)
        BOUNDS_16 ref;
        M_BoundsLerpC(&ref, &frames[0]->bounds, &frames[1]->bounds, frac, rate);
        dc_bounds_lerp(
            (int16_t *)result, (const int16_t *)&frames[0]->bounds,
            (const int16_t *)&frames[1]->bounds, p);
        static uint32_t runs;
        static uint32_t bad;
        if (memcmp(&ref, result, sizeof(BOUNDS_16)) != 0) {
            bad++;
            LOG_WARNING("[bounds-verify] MISMATCH");
        }
        if (runs++ == 0 || runs % 5000 == 0) {
            LOG_WARNING(
                "[bounds-verify] %u runs, %u bad", (unsigned)runs,
                (unsigned)bad);
        }
    #else
        dc_bounds_lerp(
            (int16_t *)result, (const int16_t *)&frames[0]->bounds,
            (const int16_t *)&frames[1]->bounds, p);
    #endif
        return result;
    }
#endif
    M_BoundsLerpC(result, &frames[0]->bounds, &frames[1]->bounds, frac, rate);

    return result;
}

BOUNDS_16 Item_RotateBounds(const ITEM *const item, const int16_t rot_y)
{
    const BOUNDS_16 *bounds = &Item_GetBestFrame(item)->bounds;
    BOUNDS_16 rot_bounds = {};
    if (bounds == nullptr) {
        return rot_bounds;
    }

    switch (rot_y) {
    case 0:
    default:
        rot_bounds = *bounds;
        break;
    case DEG_90:
        rot_bounds.min.x = bounds->min.z;
        rot_bounds.max.x = bounds->max.z;
        rot_bounds.min.z = -bounds->max.x;
        rot_bounds.max.z = -bounds->min.x;
        break;
    case -DEG_180:
        rot_bounds.min.x = -bounds->max.x;
        rot_bounds.max.x = -bounds->min.x;
        rot_bounds.min.z = -bounds->max.z;
        rot_bounds.max.z = -bounds->min.z;
        break;
    case -DEG_90:
        rot_bounds.min.x = -bounds->max.z;
        rot_bounds.max.x = -bounds->min.z;
        rot_bounds.min.z = bounds->min.x;
        rot_bounds.max.z = bounds->max.x;
        break;
    }
    return rot_bounds;
}
